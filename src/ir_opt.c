#include "common.h"
#include "ir.h"
#include <stddef.h>
#include <stdint.h>

enum LoopKind {
  LK_NONE = 0,
  LK_INC,
  LK_DEC,
};

// This function assumes the code segment
//   - start is LENTR
//   - end is LEXIT
//   - does not contain PCH or GCH or MOV
//   - the op at start+1 < i < end is not LENTR nor LEXIT
//   - the segment is canonicalized
//
static bool eliminate_innermost_loop(Operation *items, size_t start,
                                     size_t end) {
  NOB_ASSERT(items[start].kind = OP_LENTR);
  NOB_ASSERT(items[end].kind = OP_LEXIT);
  enum LoopKind loop_kind = LK_NONE;
  size_t counter_instruction = SIZE_MAX;
  for (size_t i = start + 1; i < end; i++) {
    Operation *op = &items[i];
    switch (op->kind) {
    default:
      break;
    }
    switch (op->kind) {
    case OP_ADD:
      if (op->as.add.offset == 0) {
        if (loop_kind != LK_NONE) {
          return false;
        }

        switch (op->as.add.summand) {
        case -1:
          loop_kind = LK_DEC;
          counter_instruction = i;
          break;
        case 1:
          loop_kind = LK_INC;
          counter_instruction = i;
          break;
        case 0:
          UNREACHABLE("by canonicalization");
          break;
        default:
          return false;
        }
      }
      break;
    case OP_NEG:
      return false;
      break;
    case OP_CLR:
      return false;
      break;
    case OP_MUL:
      return false;
      break;
    case OP_MAD:
      return false;
      break;
    case OP_CENTR:
    case OP_CEXIT:
      return false;
      break;
    case OP_PCH:
    case OP_GCH:
    case OP_MOV:
    case OP_LENTR:
    case OP_LEXIT:
      UNREACHABLE("sanity check!");
    case OP_NOP:
      UNREACHABLE("unreachable because canonicalized!");
    }
  }
  if (loop_kind == LK_NONE) {
    return false;
  }

  if (loop_kind == LK_INC) {
    // TODO: increment loop
    return false;
  }

  NOB_ASSERT(counter_instruction != SIZE_MAX);

  // create spot for CLR operation
  for (size_t i = counter_instruction; i + 1 < end; i++) {
    items[i] = items[i + 1];
  }
  items[end - 1].kind = OP_NOP;

  items[start] = (Operation){
      .kind = OP_CENTR,
      .as.centr = {.offset = 0},
  };

  items[end] = (Operation){
      .kind = OP_CEXIT,
      .as.cexit = {},
  };

  for (size_t i = start + 1; i < end; i++) {
    Operation *op = &items[i];
    switch (op->kind) {
    case OP_NOP:
      break;
    case OP_ADD:
      if (op->as.add.offset == 0) {
        op->kind = OP_NOP;
      } else {
        *op = (Operation){
            .kind = OP_MAD,
            .as.mad =
                {
                    .src_offset = 0,
                    .dst_offset = op->as.add.offset,
                    .factor = op->as.add.summand,
                },
        };
      }
      break;
    case OP_NEG:
      TODO("neg");
      break;
    case OP_CLR:
      TODO("clr");
      break;
    case OP_MAD:
      TODO("mad");
      break;
    case OP_MUL:
      TODO("mul");
      break;
    case OP_CENTR:
    case OP_CEXIT:
      TODO("conditinals");
      break;
    case OP_MOV:
    case OP_PCH:
    case OP_GCH:
    case OP_LENTR:
    case OP_LEXIT:
      NOB_UNREACHABLE("sanity check 2!");
    }
  }

  items[end - 1] = (Operation){
      .kind = OP_CLR,
      .as.clr = {.offset = 0},
  };

  return true;
}

size_t eliminate_loops(Intermediate *ir) {
  size_t eliminations = 0;
  size_t l_entr = SIZE_MAX;
  String_Builder debug_output = {0};
  for (size_t i = 0; i < ir->count; i++) {
    Operation op = ir->items[i];
    switch (op.kind) {
    case OP_LENTR:
      l_entr = i;
      break;
    case OP_LEXIT:
      if (l_entr != SIZE_MAX) {
        if (DEBUG_INFO) {
          debug_output.count = 0;
          sb_appendf(&debug_output, "\nElimination %zu\n", eliminations);
          fmt_ir(&debug_output, &ir->items[l_entr], i - l_entr + 1);
        }

        if (eliminate_innermost_loop(ir->items, l_entr, i)) {
          eliminations++;
          if (DEBUG_INFO) {
            sb_appendf(&debug_output, "transformed to\n");
            fmt_ir(&debug_output, &ir->items[l_entr], i - l_entr + 1);
            fprintf(stderr, SV_Fmt, SV_Arg(sb_to_sv(debug_output)));
          }
        }
      }
      l_entr = SIZE_MAX;
      break;

    case OP_PCH:
    case OP_GCH:
    case OP_MOV:
      // Do not optimize a loop containing PCH, GCH or MOV
      l_entr = SIZE_MAX;
      break;
    default:
      break;
    }
  }
  if (DEBUG_INFO) {
    fprintf(stderr, "\nhad %zu eliminations\n", eliminations);
  }
  sb_free(debug_output);
  return eliminations;
}
