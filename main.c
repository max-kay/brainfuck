#include <errno.h>
#include <libkern/OSCacheControl.h>
#include <pthread.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <termios.h>

#define NOB_STRIP_PREFIX
#define NOB_IMPLEMENTATION
#include "nob.h"

// #define DEBUG
//
#define TAPE_LEN 1024 * 4

#if defined(__APPLE__)
#define SYM(x) "_" #x // Mach-O prepends an underscore
#else
#define SYM(x) #x
#endif

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
#define PUTC_REG 21
#define PUTC_REG_STR "x21"
#define GETC_REG 22
#define GETC_REG_STR "x22"

static struct termios orig_termios;

void init_terminal(void) {
  struct termios raw;
  tcgetattr(STDIN_FILENO, &orig_termios);
  raw = orig_termios;
  raw.c_lflag &= ~(ICANON | ECHO);
  tcsetattr(STDIN_FILENO, TCSANOW, &raw);
}

void reset_terminal(void) { tcsetattr(STDIN_FILENO, TCSANOW, &orig_termios); }

typedef enum {
  Bf_R_Ok = 0,
  Bf_R_Underflow = 1,
  Bf_R_Overflow = 2,
} Bf_Result;

typedef enum {
  TOK_MOVE_LEFT,
  TOK_MOVE_RIGHT,
  TOK_INC,
  TOK_DEC,
  TOK_OUTPUT,
  TOK_INPUT,
  TOK_JUMP_FOREWARD,
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
  case TOK_JUMP_FOREWARD:
    return "JUMP_FOREWARD";
  case TOK_JUMP_BACKWARD:
    return "JUMP_BACKWARD";
  case TOK_START:
    return "START";
  case TOK_END:
    return "END";
  }
}

typedef enum {
  OP_MOVE_LEFT,
  OP_MOVE_RIGHT,
  OP_INC,
  OP_DEC,
  OP_OUTPUT,
  OP_INPUT,
  OP_JUMP_FOREWARD,
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
  case OP_JUMP_FOREWARD:
    return "F";
  case OP_JUMP_BACKWARD:
    return "B";
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
  case TOK_JUMP_FOREWARD:
    return OP_JUMP_FOREWARD;
  case TOK_JUMP_BACKWARD:
    return OP_JUMP_BACKWARD;
  case TOK_START:
    UNREACHABLE("TOK_START is no Operation");
  case TOK_END:
    UNREACHABLE("TOK_END is no Operation");
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
      return TOK_JUMP_FOREWARD;
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

void *get_jit_region(size_t size) {
  void *ptr = mmap(NULL, size, PROT_READ | PROT_WRITE | PROT_EXEC,
                   MAP_PRIVATE | MAP_ANON | MAP_JIT, -1, 0);

  if (ptr == MAP_FAILED) {
    fprintf(stderr, "could not create memory mapping: %s\n", strerror(errno));
    return NULL;
  }
  return ptr;
}

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
    case TOK_JUMP_FOREWARD:
      da_append(&idxs, ir->count);
      current_op = (Operation){.op = OP_JUMP_FOREWARD, .rep_or_jump_idx = 0};
      da_append(ir, current_op);
      break;

    case TOK_JUMP_BACKWARD:
      if (idxs.count == 0) {
        fprintf(stderr, "unexpected closing parens\n");
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
    fprintf(stderr, "unclosed opening parens\n");
    goto error;
  }
  da_free(idxs);
  return true;

error:
  ir->count = 0;
  da_free(idxs);
  return false;
}

typedef struct {
  uint8_t *items;
  size_t count;
  size_t capacity;
} Tape;

uint8_t tape_read(Tape *tape, size_t idx) {
  if (idx < tape->count) {
    return tape->items[idx];
  } else {
    return 0;
  }
}

void tape_write(Tape *tape, size_t idx, uint8_t val) {
  if (idx >= tape->count) {
    size_t old_count = tape->count;
    da_resize(tape, idx + 1);
    memset(&tape->items[old_count], 0, idx + 1 - old_count);
  }
  tape->items[idx] = val;
}

Bf_Result ir_interpret(Intermediate *ir) {
  init_terminal();

  size_t instruction_ptr = 0;
  size_t tape_ptr = 0;
  Tape tape = {0};
  while (instruction_ptr < ir->count) {
    Operation op = ir->items[instruction_ptr];
    uint8_t tape_val = tape_read(&tape, tape_ptr);

#ifdef DEBUG
    fprintf(stderr, "instruction_ptr: %zu\n", instruction_ptr);
    fprintf(stderr, "tape[%zu] = %d\n", tape_ptr, tape_val);
    fprintf(stderr,
            "instruction: Operation { op = `%s`, rep_or_jump = %zu }\n\n",
            op_as_str(op.op), op.rep_or_jump_idx);
#endif // DEBUG

    switch (op.op) {
    case OP_INC:
      tape_val += op.rep_or_jump_idx;
      tape_write(&tape, tape_ptr, tape_val);
      instruction_ptr++;
      break;
    case OP_DEC:
      tape_val -= op.rep_or_jump_idx;
      tape_write(&tape, tape_ptr, tape_val);
      instruction_ptr++;
      break;

    case OP_MOVE_RIGHT:
      tape_ptr += op.rep_or_jump_idx;
      if (tape_ptr >= TAPE_LEN) {
        goto overflow;
      }
      instruction_ptr++;
      break;
    case OP_MOVE_LEFT:
      if (op.rep_or_jump_idx > tape_ptr) {
        goto underflow;
      }
      tape_ptr -= op.rep_or_jump_idx;
      instruction_ptr++;
      break;

    case OP_INPUT:
      for (size_t i = 0; i < op.rep_or_jump_idx; i++)
        tape_write(&tape, tape_ptr, getchar());
      instruction_ptr++;
      break;
    case OP_OUTPUT:
      for (size_t i = 0; i < op.rep_or_jump_idx; i++)
        putchar(tape_val);
      instruction_ptr++;
      break;

    case OP_JUMP_FOREWARD:
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

  da_free(tape);
  reset_terminal();
  return Bf_R_Ok;
underflow:
  da_free(tape);
  reset_terminal();
  return Bf_R_Underflow;
overflow:
  da_free(tape);
  reset_terminal();
  return Bf_R_Overflow;
}

void append_ir(String_Builder *sb, Intermediate *ir) {
  for (size_t i = 0; i < ir->count; i++) {
    Operation op = ir->items[i];
    sb_appendf(sb, "%0*zu: %s %*zu\n", 3, i, op_as_str(op.op), 4,
               op.rep_or_jump_idx);
  }
}

void append_freestanding_main(String_Builder *sb) {
  sb_appendf(sb, ".global _main\n");
  sb_appendf(sb, ".align 2\n");

  sb_appendf(sb, "_main:\n");

  sb_appendf(sb, "  stp fp, lr, [sp, -16]!\n");
  sb_appendf(sb, "  mov fp, sp\n");

  sb_appendf(sb, "  mov x0, %d\n", TAPE_LEN);
  sb_appendf(sb, "  bl _malloc\n");
  sb_appendf(sb, "  cbz x0, exit\n");
  sb_appendf(sb, "  bl _bf_assembled\n");

  sb_appendf(sb, "exit:\n");
  sb_appendf(sb, "  ldp fp, lr, [sp], 16\n");
  sb_appendf(sb, "  sub x0, x0, #1\n");
  sb_appendf(sb, "  bl _exit\n");
}

// generates the function:
// bool brainfuck_function(void *tape_ptr)
void translate_to_asm(String_Builder *sb, Intermediate *ir) {
  // Prelude
  sb_appendf(sb, ".globl _bf_assembled\n");
  sb_appendf(sb, "_bf_assembled:\n");
  sb_appendf(sb, "  sub sp, sp, 32\n");
  sb_appendf(sb, "  stp x29, x30, [sp, 16]\n");
  sb_appendf(sb, "  stp %s, %s, [sp]\n", BASE_REG_STR, INDEX_REG_STR);
  sb_appendf(sb, "  mov x29, sp\n");
  sb_appendf(sb, "  mov %s, x0\n", BASE_REG_STR);
  sb_appendf(sb, "  mov %s, xzr\n", INDEX_REG_STR);
  for (size_t i = 0; i < ir->count; i++) {
    Operation op = ir->items[i];
    switch (op.op) {
    case OP_MOVE_RIGHT:
      // sb_appendf(sb, "  brk #0xf000\n");
      sb_appendf(sb, "  add %s, %s, %zu\n", INDEX_REG_STR, INDEX_REG_STR,
                 op.rep_or_jump_idx);
      sb_appendf(sb, "  cmp %s, %d\n", INDEX_REG_STR, TAPE_LEN);
      sb_appendf(sb, "  b.hs bf_overflow\n");
      break;
    case OP_MOVE_LEFT:
      // sb_appendf(sb, "  brk #0xf000\n");
      sb_appendf(sb, "  subs %s, %s, %zu\n", INDEX_REG_STR, INDEX_REG_STR,
                 op.rep_or_jump_idx);
      sb_appendf(sb, "  b.lo bf_underflow\n");
      break;

    case OP_INC:
      sb_appendf(sb, "  ldrb w0, [%s, %s]\n", BASE_REG_STR, INDEX_REG_STR);
      sb_appendf(sb, "  add w1, w0, %zu\n", op.rep_or_jump_idx % 256);
      sb_appendf(sb, "  strb w1, [%s, %s]\n", BASE_REG_STR, INDEX_REG_STR);
      break;
    case OP_DEC:
      sb_appendf(sb, "  ldrb w0, [%s, %s]\n", BASE_REG_STR, INDEX_REG_STR);
      sb_appendf(sb, "  add w1, w0, %zu\n", 256 - (op.rep_or_jump_idx % 256));
      sb_appendf(sb, "  strb w1, [%s, %s]\n", BASE_REG_STR, INDEX_REG_STR);
      break;

    case OP_OUTPUT:
      for (size_t j = 0; j < op.rep_or_jump_idx; j++) {
        sb_appendf(sb, "  ldrb w0, [%s, %s]\n", BASE_REG_STR, INDEX_REG_STR);
        sb_appendf(sb, "  bl _putchar\n");
      }
      break;
    case OP_INPUT:
      TODO("input not yet valid we need to set up the terminal for this");
      for (size_t j = 0; j < op.rep_or_jump_idx; j++) {
        sb_appendf(sb, "  bl _getchar\n");
      }
      sb_appendf(sb, "  strb w0, [%s, %s]\n", BASE_REG_STR, INDEX_REG_STR);
      break;

    case OP_JUMP_FOREWARD:
      sb_appendf(sb, "  ldrb w0, [%s, %s]\n", BASE_REG_STR, INDEX_REG_STR);
      sb_appendf(sb, "  cbz w0, label_f_%zu\n", op.rep_or_jump_idx);
      sb_appendf(sb, "label_b_%zu:\n", i);
      break;
    case OP_JUMP_BACKWARD:
      sb_appendf(sb, "  ldrb w0, [%s, %s]\n", BASE_REG_STR, INDEX_REG_STR);
      sb_appendf(sb, "  cbnz w0, label_b_%zu\n", op.rep_or_jump_idx);
      sb_appendf(sb, "label_f_%zu:\n", i);
      break;
    }
  }

  // Postlude return success
  sb_appendf(sb, "  mov x0, %u\n", Bf_R_Ok);
  // Postlude
  sb_appendf(sb, "bf_postlude:\n");
  sb_appendf(sb, "  ldp x29, x30, [sp, 16]\n");
  sb_appendf(sb, "  ldp %s, %s, [sp]\n", BASE_REG_STR, INDEX_REG_STR);
  sb_appendf(sb, "  add sp, sp, 32\n");
  sb_appendf(sb, "  ret\n");

  sb_appendf(sb, "bf_underflow:\n");
  sb_appendf(sb, "  mov x0, %u\n", Bf_R_Underflow);
  sb_appendf(sb, "  b bf_postlude\n");

  sb_appendf(sb, "bf_overflow:\n");
  sb_appendf(sb, "  mov x0, %u\n", Bf_R_Overflow);
  sb_appendf(sb, "  b bf_postlude\n");
}

typedef uint32_t instr;

instr add_idx(size_t n) {
  // [sf=1]00100010[sh=0][imm12][xindx5][xindx5]
  NOB_ASSERT(n < 1 << 12);
  instr out = ENCODE_INSTR("add " INDEX_REG_STR ", " INDEX_REG_STR ", 0");
  out |= n << 10;
  return out;
}

instr sub_idx_with_flag(size_t n) {
  // [sf=1]11100010[sh=0][imm12][xindx5][xindx5]
  NOB_ASSERT(n < 1 << 12);
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
  size_t foreward_irop_idx, backward_irop_idx, start_b_index;
} BranchEntry;

typedef struct {
  BranchEntry *items;
  size_t count;
  size_t capacity;
} BranchList;

size_t jl_find(BranchList *jl, size_t ir_op_idx) {
  for (size_t i = 0; i < jl->count; i++) {
    if (jl->items[i].foreward_irop_idx == ir_op_idx) {
      return i;
    }
  }
  return SIZE_MAX;
}

void translate_to_code(Instructions *code, Intermediate *ir) {
  // TODO: check all branch instructions for of by one errors
  // compare with notes.md
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

    case OP_JUMP_FOREWARD:
      da_append(code, ld_tape(0));
      da_append(code, cbz(0));
      BranchEntry pair = {
          .foreward_irop_idx = i,
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
      NOB_ASSERT(branch_list.items[idx].foreward_irop_idx ==
                 op.rep_or_jump_idx);

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

int main(int argc, char **argv) {
  char *p_name = *argv;
  shift(argv, argc);

  if (argc == 0 | argc > 1) {
    fprintf(stderr, "Usage %s <filename.bf>\n", p_name);
    return -1;
  }

  String_Builder contents = {0};
  if (!read_entire_file(*argv, &contents)) {
    fprintf(stderr, "could not find file %s\n", *argv);
    return -1;
  }

  Intermediate ir = {0};

  if (!parse_bf(sb_to_sv(contents), &ir))
    return -1;

  // String_Builder asm_buf = {0};
  // append_freestanding_main(&asm_buf);
  // translate_to_asm(&asm_buf, &ir);
  // sb_append_null(&asm_buf);
  //
  // char *asm_file = "bf-main.s";
  // FILE *file = fopen(asm_file, "w");
  // if (file == NULL) {
  //   fprintf(stderr, "could not open %s\n", asm_file);
  // }
  // fprintf(file, "%s", asm_buf.items);
  // fclose(file);
  //
  // Cmd cmd = {0};
  // cmd_append(&cmd, "clang", "-arch", "arm64", "-c", "-o", "bf-main.o",
  //            "bf-main.s");
  // if (!cmd_run(&cmd)) {
  //   fprintf(stderr, "could not compile to object\n");
  //   return -1;
  // }
  // cmd_append(&cmd, "clang", "-arch", "arm64", "-o", "bf-main", "bf-main.o");
  // if (!cmd_run(&cmd)) {
  //   fprintf(stderr, "could not compile to executable\n");
  //   return -1;
  // }

  Instructions code = {0};
  translate_to_code(&code, &ir);

  void *code_ptr = get_jit_region(code.count * sizeof(code.items[0]));

  if (code_ptr == NULL) {
    fprintf(stderr, "could not allocate jit memory region\n");
    return 0;
  }

  pthread_jit_write_protect_np(0);
  memcpy(code_ptr, code.items, code.count * sizeof(code.items[0]));
  pthread_jit_write_protect_np(1);
  sys_icache_invalidate(code_ptr, code.count * sizeof(code.items[0]));

  jit_fn_type jit_func = (jit_fn_type)code_ptr;

  uint8_t *tape_ptr = malloc(TAPE_LEN);
  if (tape_ptr == NULL) {
    fprintf(stderr, "could not allocate tape memory region\n");
    return 0;
  }
  memset(tape_ptr, 0, TAPE_LEN);

  switch (jit_func(tape_ptr, putchar, getchar)) {
  case Bf_R_Ok:
    break;
  case Bf_R_Underflow:
    fprintf(stderr, "UNDERFLOW\n");
    break;
  case Bf_R_Overflow:
    fprintf(stderr, "OVERFLOW\n");
    break;
  }
  return 0;

  switch (ir_interpret(&ir)) {
  case Bf_R_Ok:
    break;
  case Bf_R_Underflow:
    fprintf(stderr, "UNDERFLOW\n");
    break;
  case Bf_R_Overflow:
    fprintf(stderr, "OVERFLOW\n");
    break;
  }

  return 0;
}
