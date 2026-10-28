#include <errno.h>
#include <setjmp.h>
#include <signal.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#if defined(__APPLE__)
#include <libkern/OSCacheControl.h>
#include <pthread.h>
#else // defined(__APPLE__)
#error "Compilation for Linux is currently disabled during prototyping."
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

// #define INDEX_REG (uint32_t)19
// #define INDEX_REG_STR "x19"
// #define BASE_REG (uint32_t)20
// #define BASE_REG_STR "x20"
#define TAPE_REG (uint32_t)19
#define TAPE_REG_STR "x19"
#define UNUSED_REG (uint32_t)20
#define UNUSED_REG_STR "x20"
#define PUTC_REG (uint32_t)21
#define PUTC_REG_STR "x21"
#define GETC_REG (uint32_t)22
#define GETC_REG_STR "x22"

#define MIN_TAPE_LEN ((size_t)(1 << 12))

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

// COMMANDLINE CONFIG
static bool TEST_OUTPUT = false;
static bool RUN_JIT = true;
static bool RUN_IR = false;
static bool STATS = false;
static bool EMIT_IR = false;
static size_t TAPE_PAGES = 1;

static size_t PAGE_SIZE = 0;

typedef enum {
  Bf_R_Ok = 0,
  Bf_R_Underflow = 1,
  Bf_R_Overflow = 2,
  Bf_R_Could_Not_Parse,
  Bf_R_Unknown,
} Bf_Result;

static Bf_Result RESULT = Bf_R_Ok;

void print_result() {
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

static uint8_t *TAPE_PTR = NULL;
static uint8_t *LOW_GUARD = NULL;
static uint8_t *HIGH_GUARD = NULL;

#if defined(__APPLE__)
static inline void *get_jit_region(size_t size) {
  void *ptr = mmap(NULL, size, PROT_READ | PROT_WRITE | PROT_EXEC,
                   MAP_PRIVATE | MAP_ANON | MAP_JIT, -1, 0);
  NOB_ASSERT(ptr != MAP_FAILED);
  return ptr;
}

static inline void free_jit_region(void *ptr, size_t size) {
  NOB_ASSERT(munmap(ptr, size) == 0);
}

static sigjmp_buf jump_env;

static void on_fault(int sig, siginfo_t *info, void *uctx) {
  UNUSED(sig);
  UNUSED(uctx);
  uint8_t *addr = (uint8_t *)info->si_addr;
  if (LOW_GUARD <= addr && addr < LOW_GUARD + PAGE_SIZE)
    RESULT = Bf_R_Underflow;
  else if (HIGH_GUARD <= addr && addr < HIGH_GUARD + PAGE_SIZE)
    RESULT = Bf_R_Overflow;
  else
    RESULT = Bf_R_Unknown;
  siglongjmp((int *)jump_env, 1);
}

static inline void get_tape_region() {
  uint8_t *base = mmap(NULL, (2 + TAPE_PAGES) * PAGE_SIZE, PROT_NONE,
                       MAP_PRIVATE | MAP_ANON, -1, 0);
  NOB_ASSERT(base != MAP_FAILED);
  mprotect(base + PAGE_SIZE, TAPE_PAGES * PAGE_SIZE, PROT_READ | PROT_WRITE);

  struct sigaction sa = {0};
  sa.sa_sigaction = on_fault;
  sa.sa_flags = SA_SIGINFO;
  sigemptyset(&sa.sa_mask);
  sigaction(SIGSEGV, &sa, NULL);
  sigaction(SIGBUS, &sa, NULL); // macOS often delivers SIGBUS for these

  LOW_GUARD = base;
  HIGH_GUARD = base + (1 + TAPE_PAGES) * PAGE_SIZE;
  TAPE_PTR = base + PAGE_SIZE;
}

static inline void free_tape_region() {
  NOB_ASSERT(munmap(LOW_GUARD, PAGE_SIZE * (2 + TAPE_PAGES)) == 0);
}

#else  // defined(__APPLE__)
static inline void *get_jit_region(size_t size) {
  void *ptr = mmap(NULL, size, PROT_READ | PROT_WRITE | PROT_EXEC,
                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  NOB_ASSERT(ptr != MAP_FAILED);
  return ptr;
}

static inline void free_jit_region(void *ptr, size_t size) {
  NOB_ASSERT(munmap(ptr, size) == 0);
}

static inline uint8_t *get_tape_region(void) {
  void *ptr = mmap(NULL, TAPE_LEN, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  NOB_ASSERT(ptr != MAP_FAILED);
  return ptr;
}

static inline void free_tape_region(void *ptr) {
  NOB_ASSERT(munmap(ptr - PAGE_SIZE, PAGE_SIZE * (2 + TAPE_PAGES)) == 0);
}
#endif // defined(__APPLE__)

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

typedef enum {
  OP_MOV,
  OP_ADD,
  OP_PCH,
  OP_GCH,
  OP_LSTART,
  OP_LEND,
} OpCode;

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
  ptrdiff_t rep_or_jump_idx;
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

  while (true) {
    Token token = get_next(&iter);
    Operation current_op = {0};
    switch (token) {
    case TOK_JUMP_FORWARD:
      current_op = (Operation){.op = OP_LSTART, .rep_or_jump_idx = 0};
      da_append(ir, current_op);
      da_append(&idxs, ir->count);
      break;

    case TOK_JUMP_BACKWARD:
      if (idxs.count == 0) {
        goto error;
      }
      size_t last_idx = da_pop(&idxs);
      current_op = (Operation){.op = OP_LEND, .rep_or_jump_idx = last_idx};
      da_append(ir, current_op);
      ir->items[last_idx - 1].rep_or_jump_idx = ir->count;
      break;

    case TOK_START:
      UNREACHABLE("TOK_START is never returned by get_next");
      break;
    case TOK_END:
      goto finish;
      break;

    case TOK_INC:
      if (ir->count > 0 && da_last(ir).op == OP_ADD) {
        da_last(ir).rep_or_jump_idx += 1;
      } else {
        current_op = (Operation){.op = OP_ADD, .rep_or_jump_idx = 1};
        da_append(ir, current_op);
      }
      break;
    case TOK_DEC:
      if (ir->count > 0 && da_last(ir).op == OP_ADD) {
        da_last(ir).rep_or_jump_idx -= 1;
      } else {
        current_op = (Operation){.op = OP_ADD, .rep_or_jump_idx = -1};
        da_append(ir, current_op);
      }
      break;

    case TOK_MOVE_RIGHT:
      if (ir->count > 0 && da_last(ir).op == OP_MOV) {
        da_last(ir).rep_or_jump_idx += 1;
      } else {
        current_op = (Operation){.op = OP_MOV, .rep_or_jump_idx = 1};
        da_append(ir, current_op);
      }
      break;
    case TOK_MOVE_LEFT:
      if (ir->count > 0 && da_last(ir).op == OP_MOV) {
        da_last(ir).rep_or_jump_idx -= 1;
      } else {
        current_op = (Operation){.op = OP_MOV, .rep_or_jump_idx = -1};
        da_append(ir, current_op);
      }
      break;

    case TOK_OUTPUT:
      if (ir->count > 0 && da_last(ir).op == OP_PCH) {
        da_last(ir).rep_or_jump_idx += 1;
      } else {
        current_op = (Operation){.op = OP_PCH, .rep_or_jump_idx = 1};
        da_append(ir, current_op);
      }
      break;
    case TOK_INPUT:
      if (ir->count > 0 && da_last(ir).op == OP_GCH) {
        da_last(ir).rep_or_jump_idx += 1;
      } else {
        current_op = (Operation){.op = OP_GCH, .rep_or_jump_idx = 1};
        da_append(ir, current_op);
      }
      break;
    }
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

void ir_interpret(Intermediate *ir) {
  size_t instruction_ptr = 0;
  ptrdiff_t tape_ptr = 0;
  get_tape_region();
  while (instruction_ptr < ir->count) {
    Operation op = ir->items[instruction_ptr];
    uint8_t tape_val = TAPE_PTR[tape_ptr];
    switch (op.op) {
    case OP_ADD:
      tape_val += (0xFF & op.rep_or_jump_idx);
      TAPE_PTR[tape_ptr] = tape_val;
      instruction_ptr++;
      break;

    case OP_MOV:
      tape_ptr += op.rep_or_jump_idx;
      instruction_ptr++;
      break;

    case OP_GCH:
      for (ptrdiff_t i = 0; i < op.rep_or_jump_idx; i++)
        TAPE_PTR[tape_ptr] = getchar();
      instruction_ptr++;
      break;
    case OP_PCH:
      for (ptrdiff_t i = 0; i < op.rep_or_jump_idx; i++)
        putchar(tape_val);
      instruction_ptr++;
      break;

    case OP_LSTART:
      if (tape_val == 0) {
        instruction_ptr = op.rep_or_jump_idx;
      } else {
        instruction_ptr++;
      }
      break;
    case OP_LEND:
      if (tape_val != 0) {
        instruction_ptr = op.rep_or_jump_idx;
      } else {
        instruction_ptr++;
      }
      break;
    }
  }

  free_tape_region();
}

void append_ir(String_Builder *sb, Intermediate *ir) {
  for (size_t i = 0; i < ir->count; i++) {
    Operation op = ir->items[i];
    sb_appendf(sb, "%*zu: %-6s %*ld\n", 5, i, op_as_str(op.op), 7,
               op.rep_or_jump_idx);
  }
}

typedef uint32_t instr;

static inline instr add_idx(size_t n) {
  // [sf=1]00100010[sh=0][imm12][xindx5][xindx5]
  if (n >= (1 << 12)) {
    TODO("allow the use of larger add using lsl 12");
  }
  instr out = ENCODE_INSTR("add " TAPE_REG_STR ", " TAPE_REG_STR ", 0");
  out |= n << 10;
  return out;
}

static inline instr sub_idx(size_t n) {
  // [sf=1]10100010[sh=0][imm12][xindx5][xindx5]
  if (n >= (1 << 12)) {
    TODO("allow the use of larger sub using lsl 12");
  }
  instr out = ENCODE_INSTR("sub " TAPE_REG_STR ", " TAPE_REG_STR ", 0");
  out |= n << 10;
  return out;
}

static inline instr add(uint8_t dst, uint8_t src, size_t n) {
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

// static inline instr mask(uint8_t dst, uint8_t src) {
//   instr out = ENCODE_INSTR("and w0, w0, 0xFF");
//   NOB_ASSERT(src < 8);
//   NOB_ASSERT(dst < 8);
//   out |= dst;
//   out |= src << 5;
//   return out;
// }

static inline instr cbz(uint8_t reg) {
  // [sf=0]0110100[imm19][reg5]
  NOB_ASSERT(reg < 8);
  instr out = ENCODE_INSTR("cbz w0, 0");
  out |= reg;
  return out;
}

static inline instr cbnz(uint8_t reg) {
  // [sf=0]0110101[imm19][reg5]
  NOB_ASSERT(reg < 8);
  instr out = ENCODE_INSTR("cbnz w0, 0");
  out |= reg;
  return out;
}

static inline void link_cond_branch(instr *op, ptrdiff_t offset) {
  // offset is 4 times bytelen or instruction count
  NOB_ASSERT(offset >= -(1 << 18) && offset < (1 << 18));
  *op |= ((uint32_t)offset & 0x7FFFF) << 5;
}

static inline instr ld_tape(uint8_t reg) {
  // 00111000011[xindx5][option3=011][S=0]10[xbase5][reg5]
  NOB_ASSERT(reg < 8);
  instr out = ENCODE_INSTR("ldrb w0, [" TAPE_REG_STR "]");
  out |= reg;
  return out;
}

static inline instr str_tape(uint8_t reg) {
  // 00111000001[xindx5][option3=011][S=0]10[xbase5][reg5]
  NOB_ASSERT(reg < 8);
  instr out = ENCODE_INSTR("strb w0, [" TAPE_REG_STR "]");
  out |= reg;
  return out;
}

static inline instr blr_putc() { return ENCODE_INSTR("blr " PUTC_REG_STR); }

static inline instr blr_getc() { return ENCODE_INSTR("blr " GETC_REG_STR); }

typedef struct {
  instr *items;
  size_t count;
  size_t capacity;
} Instructions;

typedef int (*putchar_type)(int);
typedef int (*getchar_type)(void);
typedef void (*jit_fn_type)(uint8_t *, putchar_type, getchar_type);

ENCODE_ASM("  sub sp, sp, 48\n"
           "  stp x29, x30, [sp, 32]\n"
           "  mov x29, sp\n"
           "  stp " TAPE_REG_STR ", " UNUSED_REG_STR ", [sp, 16]\n"
           "  stp " PUTC_REG_STR ", " GETC_REG_STR ", [sp]\n"
           // "  mov " INDEX_REG_STR ", xzr\n"
           "  mov " TAPE_REG_STR ", x0\n"
           "  mov " PUTC_REG_STR ", x1\n"
           "  mov " GETC_REG_STR ", x2\n",
           PRELUDE_START, PRELUDE_END);

ENCODE_ASM("  ldp x29, x30, [sp, 32]\n"
           "  ldp " TAPE_REG_STR ", " UNUSED_REG_STR ", [sp, 16]\n"
           "  ldp " PUTC_REG_STR ", " GETC_REG_STR ", [sp]\n"
           "  add sp, sp, 48\n"
           "  ret\n",
           POSTLUDE_START, POSTLUDE_END);

typedef struct {
  ptrdiff_t enter_loop_irop, exit_loop_irop, skip_loop_instr_idx;
} Loop_Entry;

typedef struct {
  Loop_Entry *items;
  size_t count;
  size_t capacity;
} Loop_List;

size_t ll_find(Loop_List *jl, ptrdiff_t ir_op_idx) {
  for (size_t i = 0; i < jl->count; i++) {
    if (jl->items[i].enter_loop_irop == ir_op_idx) {
      return i;
    }
  }
  UNREACHABLE("Loop_List is allways valid");
}

void translate_to_code(Instructions *code, Intermediate *ir) {
  Index_Array needs_overflow_update = {0};
  Index_Array needs_underflow_update = {0};
  Loop_List loop_list = {0};
  da_append_many(code, PRELUDE_START, PRELUDE_END - PRELUDE_START);

  for (size_t i = 0; i < ir->count; i++) {
    Operation op = ir->items[i];
    switch (op.op) {
    case OP_MOV:
      if (op.rep_or_jump_idx == 0) {
        // DO NOTHING
      } else if (op.rep_or_jump_idx > 0) {
        da_append(code, add_idx(op.rep_or_jump_idx));
      } else {
        da_append(code, sub_idx(-op.rep_or_jump_idx));
      }
      break;

    case OP_ADD:
      if ((0xFF & op.rep_or_jump_idx) != 0) {
        da_append(code, ld_tape(0));
        da_append(code, add(0, 0, (0xFF & op.rep_or_jump_idx)));
        da_append(code, str_tape(0));
      }
      break;

    case OP_PCH:
      for (ptrdiff_t j = 0; j < op.rep_or_jump_idx; j++) {
        da_append(code, ld_tape(0));
        da_append(code, blr_putc());
      }
      break;
    case OP_GCH:
      for (ptrdiff_t j = 0; j < op.rep_or_jump_idx; j++) {
        da_append(code, blr_getc());
      }
      da_append(code, str_tape(0));
      break;

    case OP_LSTART:
      da_append(code, ld_tape(0));
      da_append(code, cbz(0));
      Loop_Entry pair = {
          .enter_loop_irop = i + 1,
          .exit_loop_irop = op.rep_or_jump_idx,
          .skip_loop_instr_idx = code->count - 1,
      };
      da_append(&loop_list, pair);
      break;
    case OP_LEND:
      da_append(code, ld_tape(0));
      ptrdiff_t upper_b_idx = (ptrdiff_t)code->count;
      da_append(code, cbnz(0));

      Loop_Entry loop_entry = da_pop(&loop_list);
      NOB_ASSERT(loop_entry.enter_loop_irop == (ptrdiff_t)(op.rep_or_jump_idx));
      NOB_ASSERT(loop_entry.exit_loop_irop == (ptrdiff_t)(i + 1));

      ptrdiff_t lower_b_idx = loop_entry.skip_loop_instr_idx;

      // link the branches
      link_cond_branch(&code->items[lower_b_idx],
                       (upper_b_idx + 1) - lower_b_idx);
      link_cond_branch(&code->items[upper_b_idx],
                       (lower_b_idx + 1) - upper_b_idx);
      break;
    }
  }
  da_append_many(code, POSTLUDE_START, POSTLUDE_END - POSTLUDE_START);

  da_free(needs_underflow_update);
  da_free(needs_overflow_update);
  da_free(loop_list);
}

void run_on_jit(Instructions *code) {
  void *code_ptr = get_jit_region(code->count * sizeof(code->items[0]));

#if defined(__APPLE__)
  pthread_jit_write_protect_np(0);
#endif // defined(__APPLE__)

  memcpy(code_ptr, code->items, code->count * sizeof(code->items[0]));

#if defined(__APPLE__)
  pthread_jit_write_protect_np(1);
  sys_icache_invalidate(code_ptr, code->count * sizeof(code->items[0]));
#endif // defined(__APPLE__)

  get_tape_region();

  ((jit_fn_type)code_ptr)(TAPE_PTR, putchar, getchar);

  free_tape_region();
  free_jit_region(code_ptr, code->count * sizeof(code->items[0]));
}

void print_usage(char *pname) {
  fprintf(stderr, "Usage %s FLAGS <filename.bf>\n", pname);
  fprintf(stderr, "   --interpret-ir   -i     interpret ir directly\n");
  fprintf(stderr, "   --stats          -s     print statistics\n");
  fprintf(stderr,
          "   --tape-len       -t     number of pages used for tape (%zu on "
          "this machine)\n",
          PAGE_SIZE);
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

  if (argc == 0) {
    print_usage(p_name);
    exit(-1);
  }
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
  PAGE_SIZE = sysconf(_SC_PAGESIZE);
  if ((PAGE_SIZE & ((1 << 12) - 1)) != 0) {
    fprintf(stderr, "unsupported page size %zu\n", PAGE_SIZE);
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

  if (sigsetjmp((int *)&jump_env, 1) != 0)
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
    run_on_jit(&code);
    end = clock();
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
