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
static bool OPTIMIZE = false;
static bool DEBUG_INFO = false;
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

static sigjmp_buf jump_env;

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

static void on_fault(int sig, siginfo_t *info, void *uctx) {
  UNUSED(sig);
  UNUSED(uctx);
  if (LOW_GUARD == NULL | TAPE_PTR == NULL | HIGH_GUARD == NULL) {
    RESULT = Bf_R_Unknown;
    siglongjmp((int *)jump_env, 1);
  }
  uint8_t *addr = (uint8_t *)info->si_addr;
  if (LOW_GUARD <= addr && addr < LOW_GUARD + PAGE_SIZE) {
    RESULT = Bf_R_Underflow;
  } else if (HIGH_GUARD <= addr && addr < HIGH_GUARD + PAGE_SIZE) {
    RESULT = Bf_R_Overflow;
  } else {
    RESULT = Bf_R_Unknown;
  }
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
  LOW_GUARD = NULL;
  TAPE_PTR = NULL;
  HIGH_GUARD = NULL;
}

#else // defined(__APPLE__)
static inline void *get_jit_region(size_t size) {
  void *ptr = mmap(NULL, size, PROT_READ | PROT_WRITE | PROT_EXEC,
                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  NOB_ASSERT(ptr != MAP_FAILED);
  return ptr;
}

static inline void free_jit_region(void *ptr, size_t size) {
  NOB_ASSERT(munmap(ptr, size) == 0);
}

static void on_fault(int sig, siginfo_t *info, void *uctx) {
  UNUSED(sig);
  UNUSED(uctx);
  if (LOW_GUARD == NULL | TAPE_PTR == NULL | HIGH_GUARD == NULL) {
    RESULT = Bf_R_Unknown;
    siglongjmp(jump_env, 1);
  }
  uint8_t *addr = (uint8_t *)info->si_addr;
  if (LOW_GUARD <= addr && addr < LOW_GUARD + PAGE_SIZE)
    RESULT = Bf_R_Underflow;
  else if (HIGH_GUARD <= addr && addr < HIGH_GUARD + PAGE_SIZE)
    RESULT = Bf_R_Overflow;
  else
    RESULT = Bf_R_Unknown;
  siglongjmp(jump_env, 1);
}

static inline void get_tape_region() {
  uint8_t *base = mmap(NULL, (2 + TAPE_PAGES) * PAGE_SIZE, PROT_NONE,
                       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  NOB_ASSERT(base != MAP_FAILED);
  mprotect(base + PAGE_SIZE, TAPE_PAGES * PAGE_SIZE, PROT_READ | PROT_WRITE);

  struct sigaction sa = {0};
  sa.sa_sigaction = on_fault;
  sa.sa_flags = SA_SIGINFO;
  sigemptyset(&sa.sa_mask);
  sigaction(SIGSEGV, &sa, NULL);

  LOW_GUARD = base;
  HIGH_GUARD = base + (1 + TAPE_PAGES) * PAGE_SIZE;
  TAPE_PTR = base + PAGE_SIZE;
}

static inline void free_tape_region() {
  NOB_ASSERT(munmap(LOW_GUARD, PAGE_SIZE * (2 + TAPE_PAGES)) == 0);
  LOW_GUARD = NULL;
  TAPE_PTR = NULL;
  HIGH_GUARD = NULL;
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

typedef enum {
  OP_NOP = 0,
  // operand0 signed amout to move
  OP_MOV,
  // operand0 amount to add
  // operand1 offset
  OP_ADD,
  // operand0 repetitions
  OP_PCH,
  // operand0 repetitions
  OP_GCH,
  // no operands
  OP_LSTART,
  // no operands
  OP_LEND,
} OpCode;

typedef struct {
  OpCode op;
  ptrdiff_t operand0;
  ptrdiff_t operand1;
} Operation;

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
  get_tape_region();
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
        da_pop(&jump_back);
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

static inline instr ld_tape(uint8_t reg, ptrdiff_t offset) {
  // 00111000010[imm9]00[xbase5][reg5]
  NOB_ASSERT(reg < 8);
  if (offset > 255 || offset < -256) {
    TODO("implement ld tape with larger offsets");
  }
  instr out = ENCODE_INSTR("ldurb w0, [" TAPE_REG_STR ", 0]");
  out |= reg;
  instr off = (instr)offset & 0x1FF;
  out |= (off << 12);
  return out;
}

static inline instr str_tape(uint8_t reg, ptrdiff_t offset) {
  // 00111000000[imm9]00[xbase5][reg5]
  NOB_ASSERT(reg < 8);
  if (offset > 255 || offset < -256) {
    TODO("implement ld tape with larger offsets");
  }
  instr out = ENCODE_INSTR("sturb w0, [" TAPE_REG_STR ", 0]");
  out |= reg;
  instr off = (instr)offset & 0x1FF;
  out |= (off << 12);
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

void translate_to_code(Instructions *code, Intermediate *ir) {
  Index_Array loop_enter_idxs = {0};
  da_append_many(code, PRELUDE_START, PRELUDE_END - PRELUDE_START);

  for (size_t i = 0; i < ir->count; i++) {
    Operation op = ir->items[i];
    switch (op.op) {
    case OP_MOV:
      if (op.operand0 == 0) {
        // DO NOTHING
      } else if (op.operand0 > 0) {
        da_append(code, add_idx(op.operand0));
      } else {
        da_append(code, sub_idx(-op.operand0));
      }
      break;

    case OP_ADD:
      if ((0xFF & op.operand0) != 0) {
        da_append(code, ld_tape(0, op.operand1));
        da_append(code, add(0, 0, (0xFF & op.operand0)));
        da_append(code, str_tape(0, op.operand1));
      }
      break;

    case OP_PCH:
      for (ptrdiff_t j = 0; j < op.operand0; j++) {
        da_append(code, ld_tape(0, 0));
        da_append(code, blr_putc());
      }
      break;
    case OP_GCH:
      for (ptrdiff_t j = 0; j < op.operand0; j++) {
        da_append(code, blr_getc());
      }
      da_append(code, str_tape(0, 0));
      break;

    case OP_LSTART:
      da_append(code, ld_tape(0, 0));
      da_append(code, cbz(0));
      da_append(&loop_enter_idxs, code->count - 1);
      break;
    case OP_LEND:
      da_append(code, ld_tape(0, 0));
      ptrdiff_t upper_b_idx = (ptrdiff_t)code->count;
      da_append(code, cbnz(0));

      ptrdiff_t lower_b_idx = da_pop(&loop_enter_idxs);

      // link the branches
      link_cond_branch(&code->items[lower_b_idx],
                       (upper_b_idx + 1) - lower_b_idx);
      link_cond_branch(&code->items[upper_b_idx],
                       (lower_b_idx + 1) - upper_b_idx);
      break;

    case OP_NOP:
      break;
    }
  }
  da_append_many(code, POSTLUDE_START, POSTLUDE_END - POSTLUDE_START);
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
#else  // defined(__APPLE__)
  __builtin___clear_cache(code_ptr, (char *)code_ptr +
                                        code->count * sizeof(code->items[0]));
#endif // defined(__APPLE__)

  get_tape_region();

  ((jit_fn_type)code_ptr)(TAPE_PTR, putchar, getchar);

  free_tape_region();
  free_jit_region(code_ptr, code->count * sizeof(code->items[0]));
}

void print_usage(char *pname) {
  fprintf(stderr, "Usage %s FLAGS <filename.bf>\n", pname);
  fprintf(stderr, "   --interpret-ir   -i     interpret ir directly\n");
  fprintf(stderr, "   --optimize       -o     optimize code\n");
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
    } else if (strcmp(arg, "-o") == 0 || strcmp(arg, "--optimize") == 0) {
      OPTIMIZE = true;
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
