#include <errno.h>
#include <inttypes.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#include "backend.h"
#include "common.h"
#include "memory.h"

#define NOB_STRIP_PREFIX
#define NOB_IMPLEMENTATION
#include "nob.h"

// COMMANDLINE CONFIG
static bool TEST_OUTPUT = false;
static bool RUN_JIT = true;
static bool RUN_IR = false;
static bool STATS = false;
static bool EMIT_IR = false;
static bool OPTIMIZE = false;
static bool DEBUG_INFO = false;
static size_t TAPE_PAGES = 1;

Bf_Result RESULT = Bf_R_Ok;

void print_result(void) {
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

static inline void init_terminal(void) {
  struct termios raw;
  tcgetattr(STDIN_FILENO, &orig_termios);
  raw = orig_termios;
  raw.c_lflag &= ~(ICANON);
  tcsetattr(STDIN_FILENO, TCSANOW, &raw);
}

static inline void reset_terminal(void) {
  tcsetattr(STDIN_FILENO, TCSANOW, &orig_termios);
}

typedef enum {
  TOK_MOVE_LEFT,
  TOK_MOVE_RIGHT,
  TOK_INC,
  TOK_DEC,
  TOK_OUTPUT,
  TOK_INPUT,
  TOK_JUMP_FORWARD,
  TOK_JUMP_BACKWARD,
  TOK_START,
  TOK_END,
} Token;

Token get_next(String_View *sv) {
  while ((sv->count--) > 0) {
    switch (*(sv->items++)) {
    case '>':
      return TOK_MOVE_RIGHT;
    case '<':
      return TOK_MOVE_LEFT;
    case '+':
      return TOK_INC;
    case '-':
      return TOK_DEC;
    case '.':
      return TOK_OUTPUT;
    case ',':
      return TOK_INPUT;
    case '[':
      return TOK_JUMP_FORWARD;
    case ']':
      return TOK_JUMP_BACKWARD;
    default:
      continue;
    }
  }
  return TOK_END;
}

static const char *fmt_off(int64_t off) {
  return temp_sprintf("^%" PRId64, off);
}

#define OP_FMT_LEN 24
char *op_fmt(Operation op) {
  switch (op.kind) {
  case OP_MOV:
    return temp_sprintf("MOV    %5" PRId64 " %5s %5s", op.as.mov.summand, "",
                        "");
  case OP_ADD:
    return temp_sprintf("ADD    %5s %5" PRId64 " %5s",
                        fmt_off(op.as.add.offset), op.as.add.summand, "");
  case OP_CLR:
    return temp_sprintf("CLR    %5s %5s %5s", fmt_off(op.as.clr.offset), "",
                        "");
  case OP_MAD:
    return temp_sprintf("MAD    %5s %5s %5" PRId64,
                        fmt_off(op.as.mad.dst_offset),
                        fmt_off(op.as.mad.src_offset), op.as.mad.factor);
  case OP_PCH:
    return temp_sprintf("PCH    %5s %5s %5s", fmt_off(op.as.pch.offset), "",
                        "");
  case OP_GCH:
    return temp_sprintf("GCH    %5s %5s %5s", fmt_off(op.as.gch.offset), "",
                        "");
  case OP_LSTART:
    return temp_sprintf("LSTART %5s %5s %5s", "", "", "");
  case OP_LEND:
    return temp_sprintf("LEND   %5s %5s %5s", "", "", "");
  case OP_NOP:
    return temp_sprintf("NOP    %5s %5s %5s", "", "", "");
  default:
    return NULL;
  }
}

bool parse_bf(String_View iter, Intermediate *ir) {
  size_t open_loops = 0;
  Token token;
  Operation operation;
  while (true) {
    token = get_next(&iter);
    operation = (Operation){0};
    switch (token) {
    case TOK_JUMP_FORWARD:
      operation = (Operation){.kind = OP_LSTART};
      da_append(ir, operation);
      open_loops++;
      break;

    case TOK_JUMP_BACKWARD:
      if (open_loops == 0) {
        goto error;
      }
      open_loops--;
      operation = (Operation){.kind = OP_LEND};
      da_append(ir, operation);
      break;

    case TOK_START:
      UNREACHABLE("TOK_START is never returned by get_next");
      break;
    case TOK_END:
      goto finish;
      break;

    case TOK_INC:
      if (ir->count > 0 && da_last(ir).kind == OP_ADD) {
        da_last(ir).as.add.summand += 1;
      } else {
        operation = (Operation){.kind = OP_ADD, .as.add = {.summand = 1}};
        da_append(ir, operation);
      }
      break;
    case TOK_DEC:
      if (ir->count > 0 && da_last(ir).kind == OP_ADD) {
        da_last(ir).as.add.summand -= 1;
      } else {
        operation = (Operation){.kind = OP_ADD, .as.add = {.summand = -1}};
        da_append(ir, operation);
      }
      break;

    case TOK_MOVE_RIGHT:
      if (ir->count > 0 && da_last(ir).kind == OP_MOV) {
        da_last(ir).as.mov.summand += 1;
      } else {
        operation = (Operation){.kind = OP_MOV, .as.mov = {.summand = 1}};
        da_append(ir, operation);
      }
      break;
    case TOK_MOVE_LEFT:
      if (ir->count > 0 && da_last(ir).kind == OP_MOV) {
        da_last(ir).as.mov.summand += -1;
      } else {
        operation = (Operation){.kind = OP_MOV, .as.mov = {.summand = -1}};
        da_append(ir, operation);
      }
      break;

    case TOK_OUTPUT:
      operation = (Operation){.kind = OP_PCH, .as.pch = {.offset = 0}};
      da_append(ir, operation);
      break;
    case TOK_INPUT:
      operation = (Operation){.kind = OP_GCH, .as.gch = {.offset = 0}};
      da_append(ir, operation);
      break;
    }
  }

finish:
  if (open_loops != 0) {
    goto error;
  }
  return true;

error:
  ir->count = 0;
  return false;
}

typedef struct {
  Operation *items;
  size_t count;
  bool is_basicblock;
} Code_Segment;

typedef struct {
  Code_Segment *items;
  size_t count;
  size_t capacity;
} CS_Array;

void cs_canonicalize(Code_Segment segment, Intermediate *ir) {
  if (!segment.is_basicblock) {
    da_append_many(ir, segment.items, segment.count);
    return;
  }

  ptrdiff_t current_offset = 0;
  for (size_t i = 0; i < segment.count; i++) {
    Operation op = segment.items[i];
    switch (op.kind) {
    case OP_NOP:
      break;
    case OP_MOV:
      current_offset += op.as.mov.summand;
      break;
    case OP_ADD:
      op.as.add.offset += current_offset;
      da_append(ir, op);
      break;
    case OP_CLR:
      op.as.clr.offset += current_offset;
      da_append(ir, op);
      break;
    case OP_MAD:
      op.as.mad.dst_offset += current_offset;
      op.as.mad.src_offset += current_offset;
      da_append(ir, op);
      break;
    case OP_PCH:
      op.as.pch.offset += current_offset;
      da_append(ir, op);
      break;
    case OP_GCH:
      op.as.gch.offset += current_offset;
      da_append(ir, op);
      break;
    case OP_LSTART:
    case OP_LEND:
      UNREACHABLE("never in basic block");
    }
  }
  // TODO: sort in some predicable correct way
  Operation move =
      (Operation){.kind = OP_MOV, .as.mov.summand = current_offset};
  da_append(ir, move);
}

void ir_segment(Intermediate *ir, CS_Array *segments) {
  size_t current_start = 0;

  Code_Segment segment;

  for (size_t i = 0; i < ir->count; i++) {
    Operation op = ir->items[i];
    switch (op.kind) {
    case OP_LSTART:
    case OP_LEND:
    case OP_GCH:
    case OP_PCH:
      if (i - current_start != 0) {
        segment = (Code_Segment){
            .items = &ir->items[current_start],
            .count = i - current_start,
            .is_basicblock = true,

        };
        da_append(segments, segment);
      }
      segment = (Code_Segment){
          .items = &ir->items[i],
          .count = 1,
          .is_basicblock = false,
      };
      da_append(segments, segment);
      current_start = i + 1;
      break;
    default:
      break;
    }
  }
  if (current_start != ir->count) {
    segment = (Code_Segment){
        .items = &ir->items[current_start],
        .count = ir->count - current_start,
    };
    da_append(segments, segment);
  }
}

void ir_optimize(Intermediate *old_ir, Intermediate *new_ir) {
  new_ir->count = 0;
  CS_Array segments = {0};

  ir_segment(old_ir, &segments);
  for (size_t i = 0; i < segments.count; i++) {
    Code_Segment segment = segments.items[i];
    cs_canonicalize(segment, new_ir);
  }

  CS_Array new_segments = {0};
  ir_segment(new_ir, &new_segments);
  if (DEBUG_INFO) {
    for (size_t i = 0; i < new_segments.count; i++) {
      Code_Segment old_segment = segments.items[i];
      Code_Segment new_segment = new_segments.items[i];
      fprintf(stderr,
              "\n________________________"
              "Segment_%0*zu"
              "_________________________\n",
              6, i);
      size_t count = old_segment.count > new_segment.count ? old_segment.count
                                                           : new_segment.count;
      for (size_t j = 0; j < count; j++) {
        size_t mark = temp_save();
        if (j < old_segment.count) {
          Operation op = old_segment.items[j];
          fprintf(stderr, "%*zu: %s |   ", 6,
                  ((Operation *)(&old_segment.items[j]) - old_ir->items),
                  op_fmt(op));
        } else {
          fprintf(stderr, "%*s  %*s |   ", 6, "", OP_FMT_LEN, "");
        }

        if (j < new_segment.count) {
          Operation op = new_segment.items[j];
          fprintf(stderr, "%*zu: %s\n", 6,
                  ((Operation *)(&new_segment.items[j]) - new_ir->items),
                  op_fmt(op));
        } else {
          fprintf(stderr, "                             \n");
        }
        temp_rewind(mark);
      }
    }
  }

  da_free(new_segments);
  da_free(segments);
}

void ir_interpret(Intermediate *ir) {
  size_t instruction_ptr = 0;
  ptrdiff_t tape_ptr = 0;
  get_tape_region(TAPE_PAGES);
  Index_Array jump_back = {0};

  while (instruction_ptr < ir->count) {
    Operation op = ir->items[instruction_ptr];
    switch (op.kind) {
    case OP_NOP:
      break;
    case OP_ADD:
      TAPE_PTR[tape_ptr + op.as.add.offset] += op.as.add.summand;
      instruction_ptr++;
      break;

    case OP_CLR:
      TAPE_PTR[tape_ptr + op.as.clr.offset] = 0;
      break;
    case OP_MAD:
      TAPE_PTR[tape_ptr + op.as.mad.dst_offset] =
          TAPE_PTR[tape_ptr + op.as.mad.src_offset] * op.as.mad.factor;
      break;

    case OP_MOV:
      tape_ptr += op.as.mov.summand;
      instruction_ptr++;
      break;

    case OP_GCH:
      TAPE_PTR[tape_ptr + op.as.gch.offset] = getchar();
      instruction_ptr++;
      break;
    case OP_PCH:
      putchar(TAPE_PTR[tape_ptr + op.as.pch.offset]);
      instruction_ptr++;
      break;

    case OP_LSTART:
      if (TAPE_PTR[tape_ptr] == 0) {
        size_t lstarts = 1;
        while (lstarts != 0) {
          instruction_ptr++;
          if (ir->items[instruction_ptr].kind == OP_LSTART) {
            lstarts++;
          } else if (ir->items[instruction_ptr].kind == OP_LEND) {
            lstarts--;
          }
        }
        instruction_ptr++;
      } else {
        instruction_ptr++;
        da_append(&jump_back, instruction_ptr);
      }
      break;
    case OP_LEND:
      if (TAPE_PTR[tape_ptr] != 0) {
        instruction_ptr = da_last(&jump_back);
      } else {
        UNUSED(da_pop(&jump_back));
        instruction_ptr++;
      }
      break;
    }
  }

  free_tape_region();
}

void sb_append_ir(String_Builder *sb, Intermediate *ir) {
  for (size_t i = 0; i < ir->count; i++) {
    Operation op = ir->items[i];
    sb_appendf(sb, "%*zu: %s\n", 6, i, op_fmt(op));
  }
}

void run_on_jit(Code code) {
  jit_fn_type code_ptr = copy_to_jit_region(code.ptr, code.size);

  get_tape_region(TAPE_PAGES);

  code_ptr(TAPE_PTR, putchar, getchar);

  free_tape_region();
  free_jit_region(code_ptr, code.size);
}

void print_usage(char *pname) {
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

bool parse_size(char *s, size_t *num) {
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

char *parse_args(int argc, char **argv) {
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

void pprint_clock(clock_t elapsed) {
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

  if (SETUP_FAULT_JUMP() != 0)
    goto error;

  Intermediate ir = {0};

  clock_t start = clock();

  if (!parse_bf(sb_to_sv(contents), &ir)) {
    RESULT = Bf_R_Could_Not_Parse;
    goto error;
  }

  clock_t end = clock();

  if (STATS) {
    fprintf(stderr, "Intermediate Representation\n");
    fprintf(stderr, "  %zu instructions\n", ir.count);
    fprintf(stderr, "  ran for ");
    pprint_clock(end - start);
    fprintf(stderr, "\n");
    fprintf(stderr, "\n");
  }

  if (OPTIMIZE) {
    Intermediate new_ir = {0};

    start = clock();
    ir_optimize(&ir, &new_ir);
    end = clock();
    if (STATS) {
      fprintf(stderr, "\n");
      fprintf(stderr, "Optimized IR\n");
      fprintf(stderr, "  ran for ");
      pprint_clock(end - start);
      fprintf(stderr, "\n");
      fprintf(stderr, "old instruction count: %*zu\n", 6, ir.count);
      fprintf(stderr, "new instruction count: %*zu\n", 6, new_ir.count);
      fprintf(stderr, "\n");
    }

    swap(Intermediate, ir, new_ir);
    da_free(new_ir);
  }

  if (EMIT_IR) {
    printf("\nIR for `%s`\n", file_name);
    String_Builder ir_str = {0};
    sb_append_ir(&ir_str, &ir);
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
