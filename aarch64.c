#if defined(__aarch64__) || defined(_M_ARM64)

#include <stddef.h>
#include <stdint.h>

#include "backend.h"
#include "common.h"

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

static inline instr blr_putc(void) { return ENCODE_INSTR("blr " PUTC_REG_STR); }

static inline instr blr_getc(void) { return ENCODE_INSTR("blr " GETC_REG_STR); }

typedef struct {
  instr *items;
  size_t count;
  size_t capacity;
} Instructions;

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

Code translate_to_code(Intermediate *ir) {
  Instructions code = {0};
  Index_Array loop_enter_idxs = {0};
  da_append_many(&code, PRELUDE_START, PRELUDE_END - PRELUDE_START);

  for (size_t i = 0; i < ir->count; i++) {
    Operation op = ir->items[i];
    switch (op.op) {
    case OP_MOV:
      if (op.operand0 == 0) {
        // DO NOTHING
      } else if (op.operand0 > 0) {
        da_append(&code, add_idx(op.operand0));
      } else {
        da_append(&code, sub_idx(-op.operand0));
      }
      break;

    case OP_ADD:
      if ((0xFF & op.operand0) != 0) {
        da_append(&code, ld_tape(0, op.operand1));
        da_append(&code, add(0, 0, (0xFF & op.operand0)));
        da_append(&code, str_tape(0, op.operand1));
      }
      break;

    case OP_PCH:
      for (ptrdiff_t j = 0; j < op.operand0; j++) {
        da_append(&code, ld_tape(0, 0));
        da_append(&code, blr_putc());
      }
      break;
    case OP_GCH:
      for (ptrdiff_t j = 0; j < op.operand0; j++) {
        da_append(&code, blr_getc());
      }
      da_append(&code, str_tape(0, 0));
      break;

    case OP_LSTART:
      da_append(&code, ld_tape(0, 0));
      da_append(&code, cbz(0));
      da_append(&loop_enter_idxs, code.count - 1);
      break;
    case OP_LEND:
      da_append(&code, ld_tape(0, 0));
      ptrdiff_t upper_b_idx = (ptrdiff_t)code.count;
      da_append(&code, cbnz(0));

      ptrdiff_t lower_b_idx = da_pop(&loop_enter_idxs);

      // link the branches
      link_cond_branch(&code.items[lower_b_idx],
                       (upper_b_idx + 1) - lower_b_idx);
      link_cond_branch(&code.items[upper_b_idx],
                       (lower_b_idx + 1) - upper_b_idx);
      break;

    case OP_NOP:
      break;
    }
  }
  da_append_many(&code, POSTLUDE_START, POSTLUDE_END - POSTLUDE_START);
  return (Code){
      .ptr = code.items,
      .size = sizeof(code.items[0]) * code.count,
  };
}

#endif // defined(__aarch64__) || defined(_M_ARM64)
