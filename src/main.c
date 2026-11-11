#define NOB_IMPLEMENTATION
#include <errno.h>
#include <inttypes.h>
#include <string.h>
#include <termios.h>
#include <unistd.h>

#include "backend.h"
#include "common.h"
#include "frontend.h"
#include "interp.h"
#include "ir.h"
#include "memory.h"

// COMMANDLINE CONFIG
bool TEST_OUTPUT = false;
bool RUN_JIT = true;
bool RUN_IR = false;
bool STATS = false;
bool EMIT_IR = false;
bool OPTIMIZE = false;
bool DEBUG_INFO = false;
size_t TAPE_PAGES = 1;

Bf_Result RESULT = Bf_R_Ok;

static void print_result(void) {
  switch (RESULT) {
  case Bf_R_Ok:
    break;
  case Bf_R_Underflow:
    if (TEST_OUTPUT) {
      fprintf(stderr, "UNDERFLOW\n");
    } else {
      fprintf(stderr, "brainfuck encountered underflow\n");
    }
    break;
  case Bf_R_Overflow:
    if (TEST_OUTPUT) {
      fprintf(stderr, "OVERFLOW\n");
    } else {
      fprintf(stderr,
              "brainfuck encountered overflow (increase tape length)\n");
    }
    break;
  case Bf_R_Could_Not_Parse:
    if (TEST_OUTPUT) {
      fprintf(stderr, "COULD NOT PARSE\n");
    } else {
      fprintf(stderr, "could not parse the input file\n");
    }
    break;
  case Bf_R_Unknown:
    if (TEST_OUTPUT) {
      fprintf(stderr, "UNKNOWN\n");
    } else {
      fprintf(stderr, "an unknown error occured\n");
    }
    break;
  }
}

static struct termios orig_termios;

static void init_terminal(void) {
  struct termios raw;
  tcgetattr(STDIN_FILENO, &orig_termios);
  raw = orig_termios;
  raw.c_lflag &= ~(ICANON);
  tcsetattr(STDIN_FILENO, TCSANOW, &raw);
}

static void reset_terminal(void) {
  tcsetattr(STDIN_FILENO, TCSANOW, &orig_termios);
}

static void run_on_jit(Code code) {
  jit_fn_type code_ptr = copy_to_jit_region(code.ptr, code.size);

  get_tape_region(TAPE_PAGES);

  code_ptr(TAPE_PTR, putchar, getchar);

  free_tape_region();
  free_jit_region(code_ptr, code.size);
}

static void print_usage(char *pname) {
  fprintf(stderr, "Usage %s FLAGS <filename.bf>\n", pname);
  fprintf(stderr, "   --interpret-ir   -i     interpret ir directly\n");
  fprintf(stderr, "   --optimize       -o     optimize code\n");
  fprintf(stderr, "   --stats          -s     print statistics\n");
  fprintf(stderr,
          "   --tape-len              number of pages used for tape (pagesize: "
          "%d on this machine)\n",
          getpagesize());
  fprintf(stderr,
          "   --emit-ir        -e     emit intermediate representation\n");
  fprintf(stderr, "   --test-output           output for tests\n");
  fprintf(stderr, "   --debug          -d     print debug output\n");
}

static bool parse_size(char *s, size_t *num) {
  char *endptr;
  errno = 0;

  unsigned long val = strtoul(s, &endptr, 10);

  if ((errno == ERANGE && (val == ULONG_MAX)) || (errno != 0 && val == 0)) {
    return false;
  }
  if (endptr == s) {
    return false;
  }
  if (*endptr != '\0') {
    return false;
  }

  *num = (size_t)val;
  return true;
}

static char *parse_args(int argc, char **argv) {
  char *p_name = *argv;
  shift(argv, argc);

  if (argc == 0) {
    print_usage(p_name);
    exit(-1);
  }
  while (argc > 0) {
    char *arg = *argv;
    if (!(arg[0] == '-')) {
      break;
    }
    if (arg[1] == '-') {
      arg += 2;
      if (strcmp(arg, "test-output") == 0) {
        TEST_OUTPUT = true;
      } else if (strcmp(arg, "interpret-ir") == 0) {
        RUN_JIT = false;
        RUN_IR = true;
      } else if (strcmp(arg, "stats") == 0) {
        STATS = true;
      } else if (strcmp(arg, "optimize") == 0) {
        OPTIMIZE = true;
      } else if (strcmp(arg, "emit-ir") == 0) {
        EMIT_IR = true;
      } else if (strcmp(arg, "debug") == 0) {
        DEBUG_INFO = true;
      } else if (strcmp(arg, "tape-len") == 0) {
        shift(argv, argc);
        size_t len = 0;
        if (argc == 0 || !parse_size(*argv, &len)) {
          fprintf(stderr, "could not parse --tape-len/-t argument\n");
          exit(-1);
        }
        if (len == 0) {
          fprintf(stderr, "provided tape len 0 is invalid\n");
          exit(-1);
        }
        TAPE_PAGES = len;
      } else {
        fprintf(stderr, "unrecognized option `%s`\n", *argv);
        print_usage(p_name);
        exit(-1);
      }
    } else {
      arg++;
      while (*arg) {
        if (*arg == 'i') {
          RUN_JIT = false;
          RUN_IR = true;
        } else if (*arg == 's') {
          STATS = true;
        } else if (*arg == 'o') {
          OPTIMIZE = true;
        } else if (*arg == 'e') {
          EMIT_IR = true;
        } else if (*arg == 'd') {
          DEBUG_INFO = true;
        } else {
          fprintf(stderr, "unrecognized flag `%s`\n", *argv);
          print_usage(p_name);
          exit(-1);
        }
        arg++;
      }
    }
    shift(argv, argc);
  }

  if (argc != 1) {
    fprintf(stderr, "could not parse args\n");
    fprintf(stderr, "provide flags before single file\n");
    print_usage(p_name);
    exit(-1);
  }
  return *argv;
}

static void pprint_clock(clock_t elapsed) {
  if (elapsed >= 60 * CLOCKS_PER_SEC) {
    size_t mins = elapsed / (60 * CLOCKS_PER_SEC);
    size_t secs = (elapsed - mins * (60 * CLOCKS_PER_SEC)) / CLOCKS_PER_SEC;
    fprintf(stderr, "%zu min %zu s", mins, secs);
    return;
  }
  if (elapsed >= CLOCKS_PER_SEC) {
    fprintf(stderr, "%.3f s", (double)elapsed / (double)CLOCKS_PER_SEC);
    return;
  }
  if (elapsed >= CLOCKS_PER_SEC / 1000) {
    fprintf(stderr, "%.3f ms", (double)elapsed / (double)CLOCKS_PER_SEC * 1000);
    return;
  }
  fprintf(stderr, "%g us", (double)elapsed / (double)CLOCKS_PER_SEC * 1000000);
}

int main(int argc, char **argv) {
  if ((getpagesize() & ((1 << 12) - 1)) != 0) {
    fprintf(stderr, "unsupported page size %d\n", getpagesize());
    return -1;
  }
  char *file_name = parse_args(argc, argv);

  if (!file_exists(file_name)) {
    fprintf(stderr, "could not find file %s\n", file_name);
    return -1;
  }
  String_Builder contents = {0};
  if (!read_entire_file(file_name, &contents)) {
    fprintf(stderr, "could not read file %s\n", file_name);
    return -1;
  }
  init_terminal();

  clock_t start;
  clock_t end;

  if (SETUP_FAULT_JUMP() != 0)
    goto error;

  Intermediate ir = {0};

  start = clock();
  if (!parse_bf(sb_to_sv(contents), &ir)) {
    RESULT = Bf_R_Could_Not_Parse;
    goto error;
  }
  end = clock();

  if (STATS) {
    fprintf(stderr, "Intermediate Representation\n");
    fprintf(stderr, "  %zu instructions\n", ir.count);
    fprintf(stderr, "  ran for ");
    pprint_clock(end - start);
    fprintf(stderr, "\n");
    fprintf(stderr, "\n");
  }

  if (OPTIMIZE) {
    size_t old_count = ir.count;
    start = clock();
    ir_optimize(&ir);
    end = clock();
    if (STATS) {
      fprintf(stderr, "\n");
      fprintf(stderr, "Optimized IR\n");
      fprintf(stderr, "  old instruction count: %*zu\n", 6, old_count);
      fprintf(stderr, "  new instruction count: %*zu\n", 6, ir.count);
      fprintf(stderr, "  ran for ");
      pprint_clock(end - start);
      fprintf(stderr, "\n");

      fprintf(stderr, "\n");
    }
  }

  if (EMIT_IR) {
    printf("\nIR for `%s`\n", file_name);
    String_Builder ir_str = {0};
    fmt_ir(&ir_str, ir.items, ir.count);
    sb_append_null(&ir_str);
    printf("%s", ir_str.items);
  }

  if (RUN_JIT) {
    start = clock();
    Code code = translate_to_code(&ir);
    end = clock();
    if (STATS) {
      fprintf(stderr, "JIT compilation\n");
      // TODO: only valid for aarch64
      fprintf(stderr, "  %zu instructions\n", code.size / 4);
      fprintf(stderr, "  compiled in ");
      pprint_clock(end - start);
      fprintf(stderr, "\n");
      fprintf(stderr, "\n");
    }

    start = clock();
    run_on_jit(code);
    end = clock();
    free(code.ptr);

    if (STATS) {
      fprintf(stderr, "\n");
      fprintf(stderr, "JIT program\n");
      fprintf(stderr, "  ran for ");
      pprint_clock(end - start);
      fprintf(stderr, "\n");
      fprintf(stderr, "\n");
    }
    if (RESULT != Bf_R_Ok) {
      goto error;
    }
  }

  if (RUN_IR) {
    start = clock();
    ir_interpret(&ir);
    end = clock();
    if (STATS) {
      fprintf(stderr, "\n");
      fprintf(stderr, "IR interpretation\n");
      fprintf(stderr, "  ran for ");
      pprint_clock(end - start);
      fprintf(stderr, "\n");
      fprintf(stderr, "\n");
    }

    if (RESULT != Bf_R_Ok) {
      goto error;
    }
  }

  return 0;

error:
  print_result();
  reset_terminal();
  return -1;
}
