#include "ir.h"

static bool op_commute(Operation a, Operation b) {
  switch (a.kind) {
  case OP_LSTART:
  case OP_LEND:
  case OP_MOV:
    return false;
  case OP_NOP:
    return true;
  default:
    break;
  }
  switch (b.kind) {
  case OP_LSTART:
  case OP_LEND:
  case OP_MOV:
    return false;
  case OP_NOP:
    return true;
  default:
    break;
  }
  Footprint fa = op_footprint(a);
  Footprint fb = op_footprint(b);
  if (fa.io && fb.io)
    return false;
  if (fa.has_write && fp_touches(fb, fa.write_offset))
    return false;
  if (fa.has_read && fp_touches(fb, fa.read_offset))
    return false;
  if (fa.has_add) {
    if ((fb.has_write && fb.write_offset == fa.add_offset) ||
        (fb.has_read && fb.read_offset == fa.add_offset)) {
      return false;
    }
  }
  return true;
}

static bool op_greater(Operation a, Operation b) {
  if (a.kind == OP_NOP)
    return true;
  if (a.as.offset > b.as.offset) {
    return true;
  }
  if (a.as.offset == b.as.offset) {
    return (a.kind > b.kind);
  }
  return false;
}

static void sort_ops(Operation *items, size_t count) {
  if (count == 0)
    return;
  for (size_t i = count - 1; i > 0; i--) {
    bool swapped = false;
    for (size_t j = 0; j < i; j++) {
      Operation *a = &items[j];
      Operation *b = &items[j + 1];
      if (op_greater(*a, *b) && op_commute(*a, *b)) {
        Operation temp = *a;
        *a = *b;
        *b = temp;
        swapped = true;
      }
    }
    if (!swapped)
      break;
  }
}

static int64_t collect_moves(Operation *items, size_t count,
                             Intermediate *new_ir) {
  int64_t current_offset = 0;
  for (size_t i = 0; i < count; i++) {
    Operation op = items[i];
    switch (op.kind) {
    case OP_NOP:
      break;
    case OP_MOV:
      current_offset += op.as.mov.summand;
      break;
    case OP_NEG:
      op.as.neg.offset += current_offset;
      da_append(new_ir, op);
      break;
    case OP_ADD:
      op.as.add.offset += current_offset;
      da_append(new_ir, op);
      break;
    case OP_CLR:
      op.as.clr.offset += current_offset;
      da_append(new_ir, op);
      break;
    case OP_MUL:
      op.as.mul.dst_offset += current_offset;
      op.as.mul.src1_offset += current_offset;
      op.as.mul.src2_offset += current_offset;
      break;
    case OP_MAD:
      op.as.mad.dst_offset += current_offset;
      op.as.mad.src_offset += current_offset;
      da_append(new_ir, op);
      break;
    case OP_PCH:
      op.as.pch.offset += current_offset;
      da_append(new_ir, op);
      break;
    case OP_GCH:
      op.as.gch.offset += current_offset;
      da_append(new_ir, op);
      break;
    case OP_LSTART:
    case OP_LEND:
      UNREACHABLE("never in basic block");
    }
  }
  return current_offset;
}

// Try to merge `b` (executed after `a`, same dst offset) into `a`.
// Returns true if `a` was overwritten.
static bool op_combine(Operation *a, Operation *b) {
  switch (a->kind) {
  case OP_ADD:
    switch (b->kind) {
    case OP_ADD:
      a->as.add.summand = (uint8_t)(a->as.add.summand + b->as.add.summand);
      if (a->as.add.summand == 0)
        a->kind = OP_NOP;
      return true;
    case OP_CLR: // add is overwritten
    case OP_GCH:
      *a = *b;
      return true;
    default:
      return false;
    }
  case OP_NEG:
    switch (b->kind) {
    case OP_NEG: // -(-x) == x
      a->kind = OP_NOP;
      return true;
    case OP_CLR:
    case OP_GCH:
      *a = *b;
      return true;
    default:
      return false;
    }
  case OP_CLR:
    switch (b->kind) {
    case OP_CLR:
    case OP_NEG:
      return true;
    case OP_GCH:
      *a = *b;
      return true;
    default:
      return false;
    }
  case OP_MAD:
    if (b->kind == OP_MAD && a->as.mad.dst_offset == b->as.mad.dst_offset &&
        a->as.mad.src_offset == b->as.mad.src_offset &&
        a->as.mad.dst_offset !=
            a->as.mad.src_offset) { // dst += dst * f is not additive in f
      a->as.mad.factor = (uint8_t)(a->as.mad.factor + b->as.mad.factor);
      if (a->as.mad.factor == 0)
        a->kind = OP_NOP;
      return true;
    }
    return false;
  case OP_MUL:
    if (b->kind == OP_MUL && a->as.mul.dst_offset == b->as.mul.dst_offset &&
        a->as.mul.src1_offset == b->as.mul.src1_offset &&
        a->as.mul.src2_offset == b->as.mul.src2_offset &&
        a->as.mul.dst_offset !=
            a->as.mul.src1_offset && // dst must not feed itself
        a->as.mul.dst_offset != a->as.mul.src2_offset) {
      a->as.mul.factor = (uint8_t)(a->as.mul.factor + b->as.mul.factor);
      if (a->as.mul.factor == 0)
        a->kind = OP_NOP;
      return true;
    }
    return false;
  case OP_PCH: // never merged: every output is observable
  case OP_GCH: // never removed: every input byte must be consumed
    return false;
  case OP_NOP:
  case OP_MOV:
  case OP_LSTART:
  case OP_LEND:
    UNREACHABLE("invalid operation while combining operations");
  }
  return false;
}

// combines sorted operations and returns the count of
// non-nop operations after combining them
static size_t combine_ops(Operation *items, size_t count) {
  if (count == 0)
    return 0;
  if (count == 1)
    return items[0].kind == OP_NOP ? 0 : 1;

  size_t idx_a = 0;
  size_t idx_b = 1;
  while (idx_b < count) {
    Operation *opa = &items[idx_a];
    Operation *opb = &items[idx_b];
    if (opb->kind == OP_NOP) {
      idx_b++;
      continue;
    }
    if (opa->kind == OP_NOP) {
      *opa = *opb;
      opb->kind = OP_NOP;
      idx_b++;
      continue;
    }
    if (opa->as.offset == opb->as.offset && op_combine(opa, opb)) {
      opb->kind = OP_NOP;
      idx_b++;
      continue;
    }
    idx_a++;
    if (idx_a != idx_b) { // keep kept ops contiguous
      items[idx_a] = *opb;
      opb->kind = OP_NOP;
    }
    idx_b++;
  }

  return idx_a + (items[idx_a].kind == OP_NOP ? 0 : 1);
}

void cs_canonicalize(Code_Segment segment, Intermediate *new_ir) {
  if (!segment.is_basicblock) {
    da_append_many(new_ir, segment.items, segment.count);
    return;
  }

  size_t start = new_ir->count;

  size_t offset = collect_moves(segment.items, segment.count, new_ir);

  sort_ops(&new_ir->items[start], new_ir->count - start);
  size_t new_count = combine_ops(&new_ir->items[start], new_ir->count - start);
  new_ir->count = start + new_count;

  if (offset != 0) {
    Operation move = (Operation){.kind = OP_MOV, .as.mov.summand = offset};
    da_append(new_ir, move);
  }
}

void ir_segment(Intermediate *ir, CS_Array *segments) {
  size_t current_start = 0;

  Code_Segment segment;

  for (size_t i = 0; i < ir->count; i++) {
    Operation op = ir->items[i];
    switch (op.kind) {
    case OP_LSTART:
    case OP_LEND:
      if (i - current_start != 0) {
        segment = (Code_Segment){
            .items = &ir->items[current_start],
            .count = i - current_start,
            .is_basicblock = true,

        };
        da_append(segments, segment);
      }
      segment = (Code_Segment){
          .items = &ir->items[i],
          .count = 1,
          .is_basicblock = false,
      };
      da_append(segments, segment);
      current_start = i + 1;
      break;
    default:
      break;
    }
  }
  if (current_start != ir->count) {
    segment = (Code_Segment){
        .items = &ir->items[current_start],
        .count = ir->count - current_start,
        .is_basicblock = true,
    };
    da_append(segments, segment);
  }
}
