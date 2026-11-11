#include "interp.h"
#include "common.h"

void ir_interpret(Intermediate *ir) {
  size_t instruction_ptr = 0;
  Index_Array jump_back = {0};

  get_tape_region(TAPE_PAGES);
  uint8_t *tape_ptr = TAPE_PTR;

  while (instruction_ptr < ir->count) {
    Operation op = ir->items[instruction_ptr];
    switch (op.kind) {
    case OP_NOP:
      instruction_ptr++;
      break;
    case OP_ADD:
      *(tape_ptr + op.as.add.offset) += op.as.add.summand;
      instruction_ptr++;
      break;

    case OP_NEG:
      *(tape_ptr + op.as.neg.offset) = -*(tape_ptr + op.as.neg.offset);
      instruction_ptr++;
      break;
    case OP_CLR:
      *(tape_ptr + op.as.clr.offset) = 0;
      instruction_ptr++;
      break;
    case OP_MAD: {
      uint8_t result = *(tape_ptr + op.as.mad.src_offset) * op.as.mad.factor;
      if (result != 0) {
        *(tape_ptr + op.as.mad.dst_offset) += result;
      }
      instruction_ptr++;
      break;
    }
    case OP_MUL: {
      uint8_t result = *(tape_ptr + op.as.mul.src1_offset) *
                       *(tape_ptr + op.as.mul.src2_offset) * op.as.mul.factor;
      if (result != 0) {
        *(tape_ptr + op.as.mul.dst_offset) += result;
      }
      instruction_ptr++;
      break;
    }

    case OP_MOV:
      tape_ptr += op.as.mov.summand;
      instruction_ptr++;
      break;

    case OP_GCH:
      *(tape_ptr + op.as.gch.offset) = getchar();
      instruction_ptr++;
      break;
    case OP_PCH:
      putchar(*(tape_ptr + op.as.pch.offset));
      instruction_ptr++;
      break;

    case OP_LSTART:
      if (*tape_ptr == 0) {
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
      if (*tape_ptr != 0) {
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
