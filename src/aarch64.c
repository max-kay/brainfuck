#if defined(__aarch64__) || defined(_M_ARM64)

#include <stddef.h>
#include <stdint.h>

#include "backend.h"
#include "common.h"

#include "../nob.h"

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

static inline instr add_imm(uint8_t dst, uint8_t src, size_t n) {
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

static inline instr add_reg_lsl(uint8_t dst, uint8_t a, uint8_t b,
                                uint8_t lsl) {
  // [sf=0][op=0][S=0]01011[shift=00]0[Rm5][imm6=n][Rn5][Rd5]
  NOB_ASSERT(dst < 8);
  NOB_ASSERT(a < 8);
  NOB_ASSERT(b < 8);
  NOB_ASSERT(lsl < 8);
  instr out = ENCODE_INSTR("add w0, w0, w0, lsl 0");
  out |= (instr)dst;
  out |= (instr)a << 5;
  out |= (instr)lsl << 10;
  out |= (instr)b << 16;
  return out;
}

static inline instr negate(uint8_t dst, uint8_t src) {
  //[sf=0]1001011[shift2]0[src][imm6=000000]11111[dst5]
  NOB_ASSERT(dst < 8);
  NOB_ASSERT(src < 8);
  instr out = ENCODE_INSTR("neg w0, w0");
  out |= (instr)dst;
  out |= (instr)src << 16;
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

static inline void link_cond_branch(instr *op, int64_t offset) {
  // offset is 4 times bytelen or instruction count
  NOB_ASSERT(offset >= -(1 << 18) && offset < (1 << 18));
  *op |= ((uint32_t)offset & 0x7FFFF) << 5;
}

static inline instr ld_tape(uint8_t reg, int64_t offset) {
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

static inline instr str_tape(uint8_t reg, int64_t offset) {
  // 00111000000[imm9]00[xbase5][reg5]
  NOB_ASSERT(reg < 8);
  if (offset > 255 || offset < -256) {
    TODO("implement str tape with larger offsets");
  }
  instr out = ENCODE_INSTR("sturb w0, [" TAPE_REG_STR ", 0]");
  out |= reg;
  instr off = (instr)offset & 0x1FF;
  out |= (off << 12);
  return out;
}

static inline instr clear_tape(int64_t offset) {
  // 00111000000[imm9]00[wzr5=11111][reg5]
  if (offset > 255 || offset < -256) {
    TODO("implement clear tape with larger offsets");
  }
  instr out = ENCODE_INSTR("sturb wzr, [" TAPE_REG_STR ", 0]");
  instr off = (instr)offset & 0x1FF;
  out |= (off << 12);
  return out;
}

static inline instr zero_reg(uint8_t reg) {
  // [sf=0]0101010000[wzr5=11111]00000011111[reg5];
  NOB_ASSERT(reg < 8);
  instr out = ENCODE_INSTR("mov w0, wzr");
  out |= reg;
  return out;
}

static inline instr blr_putc(void) { return ENCODE_INSTR("blr " PUTC_REG_STR); }

static inline instr blr_getc(void) { return ENCODE_INSTR("blr " GETC_REG_STR); }

typedef struct {
  instr *items;
  size_t count;
  size_t capacity;
} Instructions;

ENCODE_ASM("  stp x29, x30, [sp, -48]!\n"
           "  mov x29, sp\n"
           "  stp " TAPE_REG_STR ", " UNUSED_REG_STR ", [sp, 16]\n"
           "  stp " PUTC_REG_STR ", " GETC_REG_STR ", [sp, 32]\n"
           "  mov " TAPE_REG_STR ", x0\n"
           "  mov " PUTC_REG_STR ", x1\n"
           "  mov " GETC_REG_STR ", x2\n",
           PRELUDE_START, PRELUDE_END);

ENCODE_ASM("  ldp " TAPE_REG_STR ", " UNUSED_REG_STR ", [sp, 16]\n"
           "  ldp " PUTC_REG_STR ", " GETC_REG_STR ", [sp, 32]\n"
           "  ldp x29, x30, [sp], 48\n"
           "  ret\n",
           POSTLUDE_START, POSTLUDE_END);

Code translate_to_code(Intermediate *ir) {
  Instructions code = {0};
  Index_Array loop_enter_idxs = {0};
  da_append_many(&code, PRELUDE_START, PRELUDE_END - PRELUDE_START);

  for (size_t i = 0; i < ir->count; i++) {
    Operation op = ir->items[i];
    switch (op.kind) {
    case OP_NOP:
      break;

    case OP_MOV:
      if (op.as.mov.summand == 0) {
        // DO NOTHING
      } else if (op.as.mov.summand > 0) {
        da_append(&code, add_idx(op.as.mov.summand));
      } else {
        da_append(&code, sub_idx(-op.as.mov.summand));
      }
      break;

    case OP_ADD:
      if ((0xFF & op.as.add.summand) != 0) {
        da_append(&code, ld_tape(0, op.as.add.offset));
        da_append(&code, add_imm(0, 0, (0xFF & op.as.add.summand)));
        da_append(&code, str_tape(0, op.as.add.offset));
      }
      break;

    case OP_PCH:
      da_append(&code, ld_tape(0, op.as.pch.offset));
      da_append(&code, blr_putc());
      break;
    case OP_GCH:
      da_append(&code, blr_getc());
      da_append(&code, str_tape(0, op.as.gch.offset));
      break;

    case OP_NEG:
      da_append(&code, ld_tape(0, op.as.neg.offset));
      da_append(&code, negate(0, 0));
      da_append(&code, str_tape(0, op.as.neg.offset));
      break;
    case OP_CLR:
      da_append(&code, clear_tape(op.as.clr.offset));
      break;

    case OP_MAD: {
      uint8_t factor = (uint8_t)op.as.mad.factor & 0xFF;
      if (factor == 0)
        break;

      // TODO: check if multiplcation using mul is faster
      uint8_t acc = 1;
      uint8_t tape_val = 0;

      da_append(&code, ld_tape(tape_val, op.as.mad.src_offset));
      da_append(&code, zero_reg(acc));
      for (uint8_t lsl = 0; lsl < 8; lsl++) {
        if ((factor >> lsl) & 1) {
          da_append(&code, add_reg_lsl(acc, acc, tape_val, lsl));
        }
      }
      da_append(&code, ld_tape(tape_val, op.as.mad.dst_offset));
      da_append(&code, add_reg_lsl(tape_val, acc, tape_val, 0));
      da_append(&code, str_tape(tape_val, op.as.mad.dst_offset));
      break;
    }

    case OP_MUL:
      TODO("mul");
      break;

    case OP_LSTART:
      da_append(&code, ld_tape(0, 0));
      da_append(&code, cbz(0));
      da_append(&loop_enter_idxs, code.count - 1);
      break;
    case OP_LEND:
      da_append(&code, ld_tape(0, 0));
      int64_t upper_b_idx = (int64_t)code.count;
      da_append(&code, cbnz(0));

      int64_t lower_b_idx = da_pop(&loop_enter_idxs);

      // link the branches
      link_cond_branch(&code.items[lower_b_idx],
                       (upper_b_idx + 1) - lower_b_idx);
      link_cond_branch(&code.items[upper_b_idx],
                       (lower_b_idx + 1) - upper_b_idx);
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
