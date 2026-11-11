#include "interp.h"
#include "common.h"

void ir_interpret(Intermediate *ir) {
  size_t instruction_ptr = 0;
  Index_Array jump_back = {0};

  size_t instructions_executed = 0;

  get_tape_region(TAPE_PAGES);
  uint8_t *tape_ptr = TAPE_PTR;

  while (instruction_ptr < ir->count) {
    instructions_executed++;
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

    case OP_LENTR:
      if (*tape_ptr == 0) {
        size_t lentrs = 1;
        while (lentrs != 0) {
          instruction_ptr++;
          if (ir->items[instruction_ptr].kind == OP_LENTR) {
            lentrs++;
          } else if (ir->items[instruction_ptr].kind == OP_LEXIT) {
            lentrs--;
          }
        }
        instruction_ptr++;
      } else {
        instruction_ptr++;
        da_append(&jump_back, instruction_ptr);
      }
      break;
    case OP_LEXIT:
      if (*tape_ptr != 0) {
        instruction_ptr = da_last(&jump_back);
      } else {
        UNUSED(da_pop(&jump_back));
        instruction_ptr++;
      }
      break;

    case OP_CENTR:
      if (*tape_ptr == 0) {
        size_t centrs = 1;
        while (centrs != 0) {
          instruction_ptr++;
          if (ir->items[instruction_ptr].kind == OP_CENTR) {
            centrs++;
          } else if (ir->items[instruction_ptr].kind == OP_CEXIT) {
            centrs--;
          }
        }
        instruction_ptr++;
      } else {
        instruction_ptr++;
      }
      break;
    case OP_CEXIT:
      instruction_ptr++;
      break;
    }
  }
  if (STATS) {
    fprintf(stderr, "\n");
    fprintf(stderr, "IR interpretation\n");
    fprintf(stderr, "  instructions executed: %zu\n", instructions_executed);
  }

  NOB_ASSERT(jump_back.count == 0);

  da_free(jump_back);
  free_tape_region();
}
