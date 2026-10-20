#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <termios.h>
#include <time.h>

#if defined(__APPLE__)
#include <libkern/OSCacheControl.h>
#include <pthread.h>
#else  // defined(__APPLE__)
#endif // defined(__APPLE__)

#define NOB_STRIP_PREFIX
#define NOB_IMPLEMENTATION
#include "nob.h"

#if defined(__APPLE__)
#define SYM(x) "_" #x // Mach-O prepends an underscore
#else                 // defined(__APPLE__)
#define SYM(x) #x
#endif // defined(__APPLE__)

#define ENCODE_ASM(text, startptr, endptr)                                     \
  __asm__(".text\n"                                                            \
          ".balign 4\n"                                                        \
          ".globl " SYM(startptr) "\n" SYM(                                    \
              startptr) ":\n" text "\n"                                        \
                        ".globl " SYM(endptr) "\n" SYM(endptr) ":\n");         \
  extern const uint32_t startptr[];                                            \
  extern const uint32_t endptr[];

#define ENCODE_INSTR(insn)                                                     \
  ({                                                                           \
    uint32_t _enc;                                                             \
    __asm__ volatile("b 1f\n"                                                  \
                     "2: " insn "\n"                                           \
                     "1: ldr %w0, 2b\n"                                        \
                     : "=r"(_enc));                                            \
    _enc;                                                                      \
  })

#define INDEX_REG (uint32_t)19
#define INDEX_REG_STR "x19"
#define BASE_REG (uint32_t)20
#define BASE_REG_STR "x20"
#define PUTC_REG (uint32_t)21
#define PUTC_REG_STR "x21"
#define GETC_REG (uint32_t)22
#define GETC_REG_STR "x22"

#define MIN_TAPE_LEN ((size_t)(1 << 12))

static struct termios orig_termios;

void init_terminal(void) {
  struct termios raw;
  tcgetattr(STDIN_FILENO, &orig_termios);
  raw = orig_termios;
  raw.c_lflag &= ~(ICANON);
  tcsetattr(STDIN_FILENO, TCSANOW, &raw);
}

void reset_terminal(void) { tcsetattr(STDIN_FILENO, TCSANOW, &orig_termios); }

// COMMANDLINE CONFIG
static bool TEST_OUTPUT = false;
static bool RUN_JIT = true;
static bool RUN_IR = false;
static bool STATS = false;
static bool EMIT_IR = false;
static size_t TAPE_LEN = 1024 * 4;

#if defined(__APPLE__)
static inline void *get_jit_region(size_t size) {
  void *ptr = mmap(NULL, size, PROT_READ | PROT_WRITE | PROT_EXEC,
                   MAP_PRIVATE | MAP_ANON | MAP_JIT, -1, 0);
  NOB_ASSERT(ptr != MAP_FAILED);
  return ptr;
}

static inline uint8_t *get_tape_region() {
  void *ptr = mmap(NULL, TAPE_LEN, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANON, -1, 0);
  NOB_ASSERT(ptr != MAP_FAILED);
  return ptr;
}
#else  // defined(__APPLE__)
static inline void *get_jit_region(size_t size) {
  void *ptr = mmap(NULL, size, PROT_READ | PROT_WRITE | PROT_EXEC,
                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  NOB_ASSERT(ptr != MAP_FAILED);
  return ptr;
}

static inline uint8_t *get_tape_region(void) {
  void *ptr = mmap(NULL, TAPE_LEN, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  NOB_ASSERT(ptr != MAP_FAILED);
  return ptr;
}
#endif // defined(__APPLE__)

static inline void free_jit_region(void *ptr, size_t size) {
  NOB_ASSERT(munmap(ptr, size) == 0);
}

static inline void free_tape_region(void *ptr) {
  NOB_ASSERT(munmap(ptr, TAPE_LEN) == 0);
}

typedef enum {
  Bf_R_Ok = 0,
  Bf_R_Underflow = 1,
  Bf_R_Overflow = 2,
  Bf_R_Could_Not_Parse,
} Bf_Result;

void print_result(Bf_Result res) {
  switch (res) {
  case Bf_R_Ok:
    break;
  case Bf_R_Underflow:
    if (TEST_OUTPUT) {
      fprintf(stderr, "UNDERFLOW\n");
    } else {
      fprintf(stderr, "brainfuck encountered underflow");
    }
    break;
  case Bf_R_Overflow:
    if (TEST_OUTPUT) {
      fprintf(stderr, "OVERFLOW\n");
    } else {
      fprintf(stderr, "brainfuck encountered overflow increase tape length");
    }
    break;
  case Bf_R_Could_Not_Parse:
    if (TEST_OUTPUT) {
      fprintf(stderr, "COULD NOT PARSE\n");
    } else {
      fprintf(stderr, "could not parse the input file");
    }
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

char *token_as_str(Token t) {
  switch (t) {
  case TOK_MOVE_LEFT:
    return "MOVE_LEFT";
  case TOK_MOVE_RIGHT:
    return "MOVE_RIGHT";
  case TOK_INC:
    return "INC";
  case TOK_DEC:
    return "DEC";
  case TOK_OUTPUT:
    return "OUTPUT";
  case TOK_INPUT:
    return "INPUT";
  case TOK_JUMP_FORWARD:
    return "JUMP_FORWARD";
  case TOK_JUMP_BACKWARD:
    return "JUMP_BACKWARD";
  case TOK_START:
    return "START";
  case TOK_END:
    return "END";
  default:
    UNREACHABLE("");
  }
}

typedef enum {
  OP_MOVE_LEFT,
  OP_MOVE_RIGHT,
  OP_INC,
  OP_DEC,
  OP_OUTPUT,
  OP_INPUT,
  OP_JUMP_FORWARD,
  OP_JUMP_BACKWARD,
} OpCode;

char *op_as_str(OpCode t) {
  switch (t) {
  case OP_MOVE_LEFT:
    return "L";
  case OP_MOVE_RIGHT:
    return "R";
  case OP_INC:
    return "A";
  case OP_DEC:
    return "S";
  case OP_OUTPUT:
    return "O";
  case OP_INPUT:
    return "I";
  case OP_JUMP_FORWARD:
    return "F";
  case OP_JUMP_BACKWARD:
    return "B";
  default:
    UNREACHABLE("");
  }
}

OpCode token_as_op_code(Token t) {
  switch (t) {
  case TOK_MOVE_LEFT:
    return OP_MOVE_LEFT;
  case TOK_MOVE_RIGHT:
    return OP_MOVE_RIGHT;
  case TOK_INC:
    return OP_INC;
  case TOK_DEC:
    return OP_DEC;
  case TOK_OUTPUT:
    return OP_OUTPUT;
  case TOK_INPUT:
    return OP_INPUT;
  case TOK_JUMP_FORWARD:
    return OP_JUMP_FORWARD;
  case TOK_JUMP_BACKWARD:
    return OP_JUMP_BACKWARD;
  case TOK_START:
    UNREACHABLE("TOK_START is no Operation");
  case TOK_END:
    UNREACHABLE("TOK_END is no Operation");
  default:
    UNREACHABLE("");
  }
}

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

typedef struct {
  OpCode op;
  size_t rep_or_jump_idx;
} Operation;

typedef struct {
  Operation *items;
  size_t count;
  size_t capacity;
} Intermediate;

typedef struct {
  size_t *items;
  size_t count;
  size_t capacity;
} Index_Array;

bool parse_bf(String_View iter, Intermediate *ir) {
  Index_Array idxs = {0};

  Token prev_token = TOK_START;
  while (true) {
    Token token = get_next(&iter);
    Operation current_op = {0};
    switch (token) {
    case TOK_JUMP_FORWARD:
      da_append(&idxs, ir->count);
      current_op = (Operation){.op = OP_JUMP_FORWARD, .rep_or_jump_idx = 0};
      da_append(ir, current_op);
      break;

    case TOK_JUMP_BACKWARD:
      if (idxs.count == 0) {
        goto error;
      }
      size_t last_idx = da_pop(&idxs);
      ir->items[last_idx].rep_or_jump_idx = ir->count;
      current_op =
          (Operation){.op = OP_JUMP_BACKWARD, .rep_or_jump_idx = last_idx};
      da_append(ir, current_op);
      break;

    case TOK_START:
      UNREACHABLE("TOK_START is never returned by get_next");
      break;
    case TOK_END:
      goto finish;
      break;

    default:
      if (token == prev_token) {
        da_last(ir).rep_or_jump_idx += 1;
      } else {
        current_op =
            (Operation){.op = token_as_op_code(token), .rep_or_jump_idx = 1};
        da_append(ir, current_op);
      }
    }
    prev_token = token;
  }

finish:
  if (idxs.count != 0) {
    goto error;
  }
  da_free(idxs);
  return true;

error:
  ir->count = 0;
  da_free(idxs);
  return false;
}

Bf_Result ir_interpret(Intermediate *ir) {
  init_terminal();

  size_t instruction_ptr = 0;
  size_t tape_ptr = 0;
  uint8_t *tape = get_tape_region();
  Bf_Result result = Bf_R_Ok;
  while (instruction_ptr < ir->count) {
    Operation op = ir->items[instruction_ptr];
    uint8_t tape_val = tape[tape_ptr];
    switch (op.op) {
    case OP_INC:
      tape_val += op.rep_or_jump_idx;
      tape[tape_ptr] = tape_val;
      instruction_ptr++;
      break;
    case OP_DEC:
      tape_val -= op.rep_or_jump_idx;
      tape[tape_ptr] = tape_val;
      instruction_ptr++;
      break;

    case OP_MOVE_RIGHT:
      tape_ptr += op.rep_or_jump_idx;
      if (tape_ptr >= TAPE_LEN) {
        result = Bf_R_Overflow;
        goto finish;
      }
      instruction_ptr++;
      break;
    case OP_MOVE_LEFT:
      if (op.rep_or_jump_idx > tape_ptr) {
        result = Bf_R_Underflow;
        goto finish;
      }
      tape_ptr -= op.rep_or_jump_idx;
      instruction_ptr++;
      break;

    case OP_INPUT:
      for (size_t i = 0; i < op.rep_or_jump_idx; i++)
        tape[tape_ptr] = getchar();
      instruction_ptr++;
      break;
    case OP_OUTPUT:
      for (size_t i = 0; i < op.rep_or_jump_idx; i++)
        putchar(tape_val);
      instruction_ptr++;
      break;

    case OP_JUMP_FORWARD:
      if (tape_val == 0) {
        instruction_ptr = op.rep_or_jump_idx + 1;
      } else {
        instruction_ptr++;
      }
      break;
    case OP_JUMP_BACKWARD:
      if (tape_val != 0) {
        instruction_ptr = op.rep_or_jump_idx + 1;
      } else {
        instruction_ptr++;
      }
      break;
    }
  }

finish:
  free_tape_region(tape);
  reset_terminal();
  return result;
}

void append_ir(String_Builder *sb, Intermediate *ir) {
  for (size_t i = 0; i < ir->count; i++) {
    Operation op = ir->items[i];
    sb_appendf(sb, "%0*zu: %s %*zu\n", 3, i, op_as_str(op.op), 4,
               op.rep_or_jump_idx);
  }
}

typedef uint32_t instr;

instr add_idx(size_t n) {
  // [sf=1]00100010[sh=0][imm12][xindx5][xindx5]
  if (n >= (1 << 12)) {
    TODO("allow the use of larger add using lsl 12");
  }
  instr out = ENCODE_INSTR("add " INDEX_REG_STR ", " INDEX_REG_STR ", 0");
  out |= n << 10;
  return out;
}

instr sub_idx_with_flag(size_t n) {
  // [sf=1]11100010[sh=0][imm12][xindx5][xindx5]
  if (n >= (1 << 12)) {
    TODO("allow the use of larger sub using lsl 12");
  }
  instr out = ENCODE_INSTR("subs " INDEX_REG_STR ", " INDEX_REG_STR ", 0");
  out |= n << 10;
  return out;
}

instr add(uint8_t dst, uint8_t src, size_t n) {
  // [sf=0]00100010[sh=0][imm12][src5][dst5]
  NOB_ASSERT(n < 1 << 12);
  NOB_ASSERT(src < 8);
  NOB_ASSERT(dst < 8);
  instr out = ENCODE_INSTR("add w0, w0, 0");
  out |= dst;
  out |= src << 5;
  out |= n << 10;
  return out;
}

instr mask(uint8_t dst, uint8_t src) {
  instr out = ENCODE_INSTR("and w0, w0, 0xFF");
  NOB_ASSERT(src < 8);
  NOB_ASSERT(dst < 8);
  out |= dst;
  out |= src << 5;
  return out;
}

instr cmp_idx() {
  // [sf=1]11100010[sh=1][imm12][xindx5][xzr5=11111]
  NOB_ASSERT((TAPE_LEN & ((1 << 12) - 1)) == 0);
  size_t shifted = TAPE_LEN >> 12;
  NOB_ASSERT(shifted < 1 << 12);
  instr out = ENCODE_INSTR("cmp " INDEX_REG_STR ", 0, lsl 12");
  out |= shifted << 10;
  return out;
}

instr bhs() { return ENCODE_INSTR("b.hs 0"); }

instr blo() { return ENCODE_INSTR("b.lo 0"); }

instr cbz(uint8_t reg) {
  // [sf=0]0110100[imm19][reg5]
  NOB_ASSERT(reg < 8);
  instr out = ENCODE_INSTR("cbz w0, 0");
  out |= reg;
  return out;
}

instr cbnz(uint8_t reg) {
  // [sf=0]0110101[imm19][reg5]
  NOB_ASSERT(reg < 8);
  instr out = ENCODE_INSTR("cbnz w0, 0");
  out |= reg;
  return out;
}

void link_cond_branch(instr *op, ptrdiff_t offset) {
  // offset is 4 times bytelen or instruction count
  NOB_ASSERT(offset >= -(1 << 18) && offset < (1 << 18));
  *op |= ((uint32_t)offset & 0x7FFFF) << 5;
}

void link_uncond_branch(instr *op, ptrdiff_t offset) {
  NOB_ASSERT(offset >= -(1 << 26) && offset < (1 << 26));
  *op |= ((uint32_t)offset & ((1 << 26) - 1));
}

instr ld_tape(uint8_t reg) {
  // 00111000011[xindx5][option3=011][S=0]10[xbase5][reg5]
  NOB_ASSERT(reg < 8);
  instr out = ENCODE_INSTR("ldrb w0, [" BASE_REG_STR ", " INDEX_REG_STR "]");
  out |= reg;
  return out;
}

instr str_tape(uint8_t reg) {
  // 00111000001[xindx5][option3=011][S=0]10[xbase5][reg5]
  NOB_ASSERT(reg < 8);
  instr out = ENCODE_INSTR("strb w0, [" BASE_REG_STR ", " INDEX_REG_STR "]");
  out |= reg;
  return out;
}

instr blr_putc() { return ENCODE_INSTR("blr " PUTC_REG_STR); }

instr blr_getc() { return ENCODE_INSTR("blr " GETC_REG_STR); }

typedef struct {
  instr *items;
  size_t count;
  size_t capacity;
} Instructions;

typedef int (*putchar_type)(int);
typedef int (*getchar_type)(void);
typedef Bf_Result (*jit_fn_type)(uint8_t *, putchar_type, getchar_type);

ENCODE_ASM("  sub sp, sp, 48\n"
           "  stp x29, x30, [sp, 32]\n"
           "  mov x29, sp\n"
           "  stp " INDEX_REG_STR ", " BASE_REG_STR ", [sp, 16]\n"
           "  stp " PUTC_REG_STR ", " GETC_REG_STR ", [sp]\n"
           "  mov " INDEX_REG_STR ", xzr\n"
           "  mov " BASE_REG_STR ", x0\n"
           "  mov " PUTC_REG_STR ", x1\n"
           "  mov " GETC_REG_STR ", x2\n",
           PRELUDE_START, PRELUDE_END);

ENCODE_ASM("  ldp x29, x30, [sp, 32]\n"
           "  ldp " INDEX_REG_STR ", " BASE_REG_STR ", [sp, 16]\n"
           "  ldp " PUTC_REG_STR ", " GETC_REG_STR ", [sp]\n"
           "  add sp, sp, 48\n"
           "  ret\n",
           POSTLUDE_START, POSTLUDE_END);

typedef struct {
  size_t forward_irop_idx, backward_irop_idx, start_b_index;
} BranchEntry;

typedef struct {
  BranchEntry *items;
  size_t count;
  size_t capacity;
} BranchList;

size_t jl_find(BranchList *jl, size_t ir_op_idx) {
  for (size_t i = 0; i < jl->count; i++) {
    if (jl->items[i].forward_irop_idx == ir_op_idx) {
      return i;
    }
  }
  return SIZE_MAX;
}

void translate_to_code(Instructions *code, Intermediate *ir) {
  Index_Array needs_overflow_update = {0};
  Index_Array needs_underflow_update = {0};
  BranchList branch_list = {0};
  da_append_many(code, PRELUDE_START, PRELUDE_END - PRELUDE_START);

  for (size_t i = 0; i < ir->count; i++) {
    Operation op = ir->items[i];
    switch (op.op) {
    case OP_MOVE_RIGHT:
      da_append(code, add_idx(op.rep_or_jump_idx));
      da_append(code, cmp_idx());
      da_append(&needs_overflow_update, code->count);
      da_append(code, bhs());
      break;
    case OP_MOVE_LEFT:
      da_append(code, sub_idx_with_flag(op.rep_or_jump_idx));
      da_append(&needs_underflow_update, code->count);
      da_append(code, blo());
      break;

    case OP_INC:
      da_append(code, ld_tape(0));
      da_append(code, add(0, 0, op.rep_or_jump_idx % 256));
      da_append(code, str_tape(0));
      break;
    case OP_DEC:
      da_append(code, ld_tape(0));
      da_append(code, add(0, 0, 256 - (op.rep_or_jump_idx % 256)));
      da_append(code, str_tape(0));
      break;

    case OP_OUTPUT:
      for (size_t j = 0; j < op.rep_or_jump_idx; j++) {
        da_append(code, ld_tape(0));
        da_append(code, blr_putc());
      }
      break;
    case OP_INPUT:
      for (size_t j = 0; j < op.rep_or_jump_idx; j++) {
        da_append(code, blr_getc());
      }
      da_append(code, str_tape(0));
      break;

    case OP_JUMP_FORWARD:
      da_append(code, ld_tape(0));
      da_append(code, cbz(0));
      BranchEntry pair = {
          .forward_irop_idx = i,
          .backward_irop_idx = op.rep_or_jump_idx,
          .start_b_index = code->count - 1,
      };
      da_append(&branch_list, pair);
      break;
    case OP_JUMP_BACKWARD:
      da_append(code, ld_tape(0));
      ptrdiff_t upper_b_idx = (ptrdiff_t)code->count;
      da_append(code, cbnz(0));

      size_t idx = jl_find(&branch_list, op.rep_or_jump_idx);
      NOB_ASSERT(idx != SIZE_MAX);
      NOB_ASSERT(branch_list.items[idx].forward_irop_idx == op.rep_or_jump_idx);

      ptrdiff_t lower_b_idx = branch_list.items[idx].start_b_index;

      // link the branches
      link_cond_branch(&code->items[lower_b_idx],
                       (upper_b_idx + 1) - lower_b_idx);
      link_cond_branch(&code->items[upper_b_idx],
                       (lower_b_idx + 1) - upper_b_idx);
      break;
    }
  }
  da_append(code, ENCODE_INSTR("mov x0, 0")); // Bf_R_Ok
  size_t postlude_idx = code->count;
  da_append_many(code, POSTLUDE_START, POSTLUDE_END - POSTLUDE_START);

  size_t underflow_idx = code->count;
  da_append(code, ENCODE_INSTR("mov x0, 1")); // Bf_R_Underflow
  da_append(code, ENCODE_INSTR("b 0"));
  link_uncond_branch(&code->items[code->count - 1],
                     (ptrdiff_t)postlude_idx - ((ptrdiff_t)code->count - 1));

  size_t overflow_idx = code->count;
  da_append(code, ENCODE_INSTR("mov x0, 2")); // Bf_R_Overflow
  da_append(code, ENCODE_INSTR("b 0"));
  link_uncond_branch(&code->items[code->count - 1],
                     (ptrdiff_t)postlude_idx - ((ptrdiff_t)code->count - 1));

  // link underflow return
  for (size_t i = 0; i < needs_underflow_update.count; i++) {
    size_t code_idx = needs_underflow_update.items[i];
    link_cond_branch(&code->items[code_idx],
                     (ptrdiff_t)underflow_idx - (ptrdiff_t)code_idx);
  }

  // link overflow return
  for (size_t i = 0; i < needs_overflow_update.count; i++) {
    size_t code_idx = needs_overflow_update.items[i];
    link_cond_branch(&code->items[code_idx],
                     (ptrdiff_t)overflow_idx - (ptrdiff_t)code_idx);
  }

  da_free(needs_underflow_update);
  da_free(needs_overflow_update);
  da_free(branch_list);
}

Bf_Result run_on_jit(Instructions *code) {
  void *code_ptr = get_jit_region(code->count * sizeof(code->items[0]));

#if defined(__APPLE__)
  pthread_jit_write_protect_np(0);
#endif // defined(__APPLE__)

  memcpy(code_ptr, code->items, code->count * sizeof(code->items[0]));

#if defined(__APPLE__)
  pthread_jit_write_protect_np(1);
  sys_icache_invalidate(code_ptr, code->count * sizeof(code->items[0]));
#endif // defined(__APPLE__)

  uint8_t *tape_ptr = get_tape_region();
  init_terminal();

  Bf_Result res = ((jit_fn_type)code_ptr)(tape_ptr, putchar, getchar);

  reset_terminal();
  free_tape_region(tape_ptr);
  free_jit_region(code_ptr, code->count * sizeof(code->items[0]));

  return res;
}
void print_usage(char *pname) {
  fprintf(stderr, "Usage %s FLAGS <filename.bf>\n", pname);
  fprintf(stderr, "   --interpret-ir  -i     interpret ir directly\n");
  fprintf(stderr, "   --stats          -s     print statistics\n");
  fprintf(stderr, "   --tape-len       -t     set length of tape (multiples of "
                  "2^12 = 4'096)\n");
  fprintf(stderr,
          "   --emit-ir        -e     emit intermediate representation\n");
  fprintf(stderr, "   --test-output           output for tests\n");
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

  while (argc > 0) {
    char *arg = *argv;
    if (!(arg[0] == '-')) {
      break;
    }
    if (strcmp(arg, "--test-output") == 0) {
      TEST_OUTPUT = true;
    } else if (strcmp(arg, "-i") == 0 || strcmp(arg, "--interpret-ir") == 0) {
      RUN_JIT = false;
      RUN_IR = true;
    } else if (strcmp(arg, "-s") == 0 || strcmp(arg, "--stats") == 0) {
      STATS = true;
    } else if (strcmp(arg, "-e") == 0 || strcmp(arg, "--emit-ir") == 0) {
      EMIT_IR = true;
    } else if (strcmp(arg, "-t") == 0 || strcmp(arg, "--tape-len") == 0) {
      shift(argv, argc);
      size_t len = 0;
      if (argc == 0 || !parse_size(*argv, &len)) {
        fprintf(stderr, "could not parse --tape-len/-t argument\n");
        exit(-1);
      }
      if ((len % (1 << 12)) != 0) {
        fprintf(stderr, "provided tape len %zu is not divisible by 2^12\n",
                len);
        len = ((len + MIN_TAPE_LEN - 1) / MIN_TAPE_LEN) * MIN_TAPE_LEN;
        fprintf(stderr, "rounded to %zu\n", len);
      }
      if (len == 0) {
        fprintf(stderr, "provided tape len 0 is invalid\n");
        exit(-1);
      }
      TAPE_LEN = len;
    } else {
      fprintf(stderr, "unrecognized option `%s`\n", *argv);
      print_usage(p_name);
      exit(-1);
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
  char *file_name = parse_args(argc, argv);
  if (!file_exists(file_name)) {
    fprintf(stderr, "could not find file %s\n", *argv);
    return -1;
  }
  String_Builder contents = {0};
  if (!read_entire_file(file_name, &contents)) {
    fprintf(stderr, "could not read file %s\n", *argv);
    return -1;
  }

  Intermediate ir = {0};

  clock_t start = clock();

  if (!parse_bf(sb_to_sv(contents), &ir)) {
    print_result(Bf_R_Could_Not_Parse);
    return -1;
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

  if (EMIT_IR) {
    String_Builder ir_str = {0};
    append_ir(&ir_str, &ir);
    sb_append_null(&ir_str);
    printf("%s", ir_str.items);
  }

  if (RUN_JIT) {
    Instructions code = {0};
    start = clock();
    translate_to_code(&code, &ir);
    end = clock();
    if (STATS) {
      fprintf(stderr, "JIT compilation\n");
      fprintf(stderr, "  %zu instructions\n", code.count);
      fprintf(stderr, "  compiled in ");
      pprint_clock(end - start);
      fprintf(stderr, "\n");
      fprintf(stderr, "\n");
    }

    start = clock();
    Bf_Result res = run_on_jit(&code);
    end = clock();
    if (STATS) {
      fprintf(stderr, "\n");
      fprintf(stderr, "JIT program\n");
      fprintf(stderr, "  ran for ");
      pprint_clock(end - start);
      fprintf(stderr, "\n");
      fprintf(stderr, "\n");
    }
    if (res != Bf_R_Ok) {
      print_result(res);
      return -1;
    }
  }

  if (RUN_IR) {
    start = clock();
    Bf_Result res = ir_interpret(&ir);
    end = clock();
    if (STATS) {
      fprintf(stderr, "\n");
      fprintf(stderr, "IR interpretation\n");
      fprintf(stderr, "  ran for ");
      pprint_clock(end - start);
      fprintf(stderr, "\n");
      fprintf(stderr, "\n");
    }

    if (res != Bf_R_Ok) {
      print_result(res);
      return -1;
    }
  }

  return 0;
}
