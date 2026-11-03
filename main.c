#include <errno.h>
#include <setjmp.h>
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

Bf_Result RESULT = Bf_R_Ok;

// COMMANDLINE CONFIG
static bool TEST_OUTPUT = false;
static bool RUN_JIT = true;
static bool RUN_IR = false;
static bool STATS = false;
static bool EMIT_IR = false;
static bool OPTIMIZE = false;
static bool DEBUG_INFO = false;
static size_t TAPE_PAGES = 1;

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

static inline char *op_as_str(OpCode t) {
  switch (t) {
  case OP_MOV:
    return "MOV";
  case OP_ADD:
    return "ADD";
  case OP_PCH:
    return "PCH";
  case OP_GCH:
    return "GCH";
  case OP_LSTART:
    return "LSTART";
  case OP_LEND:
    return "LEND";
  case OP_NOP:
    return "NOP";
  default:
    UNREACHABLE("");
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
      operation = (Operation){.op = OP_LSTART};
      da_append(ir, operation);
      open_loops++;
      break;

    case TOK_JUMP_BACKWARD:
      if (open_loops == 0) {
        goto error;
      }
      open_loops--;
      operation = (Operation){.op = OP_LEND};
      da_append(ir, operation);
      break;

    case TOK_START:
      UNREACHABLE("TOK_START is never returned by get_next");
      break;
    case TOK_END:
      goto finish;
      break;

    case TOK_INC:
      if (ir->count > 0 && da_last(ir).op == OP_ADD) {
        da_last(ir).operand0 += 1;
      } else {
        operation = (Operation){.op = OP_ADD, .operand0 = 1};
        da_append(ir, operation);
      }
      break;
    case TOK_DEC:
      if (ir->count > 0 && da_last(ir).op == OP_ADD) {
        da_last(ir).operand0 -= 1;
      } else {
        operation = (Operation){.op = OP_ADD, .operand0 = -1};
        da_append(ir, operation);
      }
      break;

    case TOK_MOVE_RIGHT:
      if (ir->count > 0 && da_last(ir).op == OP_MOV) {
        da_last(ir).operand0 += 1;
      } else {
        operation = (Operation){.op = OP_MOV, .operand0 = 1};
        da_append(ir, operation);
      }
      break;
    case TOK_MOVE_LEFT:
      if (ir->count > 0 && da_last(ir).op == OP_MOV) {
        da_last(ir).operand0 -= 1;
      } else {
        operation = (Operation){.op = OP_MOV, .operand0 = -1};
        da_append(ir, operation);
      }
      break;

    case TOK_OUTPUT:
      if (ir->count > 0 && da_last(ir).op == OP_PCH) {
        da_last(ir).operand0 += 1;
      } else {
        operation = (Operation){.op = OP_PCH, .operand0 = 1};
        da_append(ir, operation);
      }
      break;
    case TOK_INPUT:
      if (ir->count > 0 && da_last(ir).op == OP_GCH) {
        da_last(ir).operand0 += 1;
      } else {
        operation = (Operation){.op = OP_GCH, .operand0 = 1};
        da_append(ir, operation);
      }
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
} Basic_Block;

typedef struct {
  Basic_Block *items;
  size_t count;
  size_t capacity;
} BB_List;

void bb_simplify(Basic_Block block, Intermediate *ir) {
  if (!block.is_basicblock) {
    da_append_many(ir, block.items, block.count);
    return;
  }

  size_t start = ir->count;
  ptrdiff_t current_offset = 0;
  for (size_t i = 0; i < block.count; i++) {
    Operation op = block.items[i];
    switch (op.op) {
    case OP_MOV:
      current_offset += op.operand0;
      break;
    case OP_ADD:
      op.operand1 = current_offset;
      da_append(ir, op);
      break;
    case OP_NOP:
      break;
    case OP_PCH:
    case OP_GCH:
    case OP_LSTART:
    case OP_LEND:
      UNREACHABLE("never in basic block");
    }
  }
  // TODO: other sorting than bubble sort?
  for (size_t end = ir->count; end > start; end--) {
    bool had_swap = false;
    for (size_t i = start; i < end - 1; i++) {
      if (ir->items[i].operand1 > ir->items[i + 1].operand1) {
        Operation temp = ir->items[i];
        ir->items[i] = ir->items[i + 1];
        ir->items[i + 1] = temp;
        had_swap = true;
      }
    }
    if (!had_swap) {
      break;
    }
  }

  if (current_offset != 0) {
    Operation move = (Operation){.op = OP_MOV, .operand0 = current_offset};
    da_append(ir, move);
  }
}

void bb_segment(Intermediate *ir, BB_List *basic_blocks) {
  size_t current_start = 0;

  Basic_Block block;

  for (size_t i = 0; i < ir->count; i++) {
    Operation op = ir->items[i];
    switch (op.op) {
    case OP_LSTART:
    case OP_LEND:
    case OP_GCH:
    case OP_PCH:
      if (i - current_start != 0) {
        // finish last block
        block = (Basic_Block){
            .items = &ir->items[current_start],
            .count = i - current_start,
            .is_basicblock = true,

        };
        da_append(basic_blocks, block);
      }
      block = (Basic_Block){
          .items = &ir->items[i],
          .count = 1,
          .is_basicblock = false,
      };
      da_append(basic_blocks, block);
      current_start = i + 1;
      break;
    default:
      break;
    }
  }
  if (current_start != ir->count) {
    block = (Basic_Block){
        .items = &ir->items[current_start],
        .count = ir->count - current_start,
    };
    da_append(basic_blocks, block);
  }
}

void ir_optimize(Intermediate *old_ir, Intermediate *new_ir) {
  new_ir->count = 0;
  BB_List basic_blocks = {0};

  bb_segment(old_ir, &basic_blocks);
  for (size_t i = 0; i < basic_blocks.count; i++) {
    Basic_Block block = basic_blocks.items[i];
    bb_simplify(block, new_ir);
  }

  if (DEBUG_INFO) {
    BB_List new_basic_blocks = {0};

    bb_segment(new_ir, &new_basic_blocks);

    for (size_t i = 0; i < new_basic_blocks.count; i++) {
      Basic_Block old_block = basic_blocks.items[i];
      Basic_Block new_block = new_basic_blocks.items[i];
      fprintf(stderr,
              "\n__________________________"
              "Block_%0*zu"
              "__________________________\n",
              6, i);
      size_t count =
          old_block.count > new_block.count ? old_block.count : new_block.count;
      for (size_t j = 0; j < count; j++) {
        if (j < old_block.count) {
          Operation op = old_block.items[j];
          fprintf(stderr, "%*zu: %-6s %*ld %*ld|   ", 6,
                  ((Operation *)(&old_block.items[j]) - old_ir->items),
                  op_as_str(op.op), 7, op.operand0, 7, op.operand1);
        } else {
          fprintf(stderr, "                                 ");
        }

        if (j < new_block.count) {
          Operation op = new_block.items[j];
          fprintf(stderr, "%*zu: %-6s %*ld %*ld\n", 6,
                  ((Operation *)(&new_block.items[j]) - new_ir->items),
                  op_as_str(op.op), 7, op.operand0, 7, op.operand1);
        } else {
          fprintf(stderr, "                             \n");
        }
      }
    }
    da_free(new_basic_blocks);
  }

  da_free(basic_blocks);
}

void ir_interpret(Intermediate *ir) {
  size_t instruction_ptr = 0;
  ptrdiff_t tape_ptr = 0;
  get_tape_region(TAPE_PAGES);
  Index_Array jump_back = {0};

  while (instruction_ptr < ir->count) {
    Operation op = ir->items[instruction_ptr];
    switch (op.op) {
    case OP_ADD:
      TAPE_PTR[tape_ptr + op.operand1] += op.operand0;
      instruction_ptr++;
      break;

    case OP_MOV:
      tape_ptr += op.operand0;
      instruction_ptr++;
      break;

    case OP_GCH:
      for (ptrdiff_t i = 0; i < op.operand0; i++)
        TAPE_PTR[tape_ptr] = getchar();
      instruction_ptr++;
      break;
    case OP_PCH:
      for (ptrdiff_t i = 0; i < op.operand0; i++)
        putchar(TAPE_PTR[tape_ptr]);
      instruction_ptr++;
      break;

    case OP_LSTART:
      if (TAPE_PTR[tape_ptr] == 0) {
        size_t lstarts = 1;
        while (lstarts != 0) {
          instruction_ptr++;
          if (ir->items[instruction_ptr].op == OP_LSTART) {
            lstarts++;
          } else if (ir->items[instruction_ptr].op == OP_LEND) {
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
    case OP_NOP:
      break;
    }
  }

  free_tape_region();
}

void sb_append_ir(String_Builder *sb, Intermediate *ir) {
  for (size_t i = 0; i < ir->count; i++) {
    Operation op = ir->items[i];
    sb_appendf(sb, "%*zu: %-6s %*ld %*ld\n", 6, i, op_as_str(op.op), 7,
               op.operand0, 7, op.operand1);
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

#ifdef __APPLE__
  if (sigsetjmp((int *)&jump_env, 1) != 0)
    goto error;
#else  // __APPLE__
  if (sigsetjmp(jump_env, 1) != 0)
    goto error;
#endif // __APPLE__

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
