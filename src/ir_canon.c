#include "common.h"
#include "ir.h"

typedef struct {
  bool has_read, has_read2, has_write, has_add, io;
  int64_t read_offset, read2_offset, write_offset, add_offset;
} Footprint;

Footprint op_footprint(Operation op) {
  Footprint f = {0};
  switch (op.kind) {
  case OP_NOP:
    break;
  case OP_ADD:
    f.has_add = true;
    f.add_offset = op.as.add.offset;
    break;
  case OP_NEG:
    f.has_read = true;
    f.read_offset = op.as.neg.offset;
    f.has_write = true;
    f.write_offset = op.as.neg.offset;
    break;
  case OP_CLR:
    f.has_write = true;
    f.write_offset = op.as.clr.offset;
    break;
  case OP_MUL:
    f.has_read = true;
    f.read_offset = op.as.mul.src1_offset;
    f.has_read2 = true;
    f.read2_offset = op.as.mul.src2_offset;
    f.has_add = true;
    f.add_offset = op.as.mul.dst_offset;
    break;
    break;
  case OP_MAD:
    f.has_read = true;
    f.read_offset = op.as.mad.src_offset;
    f.has_add = true;
    f.add_offset = op.as.mad.dst_offset;
    break;
  case OP_PCH:
    f.has_read = true;
    f.read_offset = op.as.pch.offset;
    f.io = true;
    break;
  case OP_GCH:
    f.has_write = true;
    f.write_offset = op.as.gch.offset;
    f.io = true;
    break;
  case OP_MOV:
    UNREACHABLE("mov instructions have no well defined footprint");
  case OP_CENTR:
  case OP_CEXIT:
    UNREACHABLE("conditionals instructions have no well defined footprint");
  case OP_LENTR:
  case OP_LEXIT:
    UNREACHABLE("loop instructions have no well defined footprint");
  }
  return f;
}

static bool fp_touches(Footprint fp, int64_t offset) {
  return (fp.has_write && fp.write_offset == offset) ||
         (fp.has_read && fp.read_offset == offset) ||
         (fp.has_read2 && fp.read2_offset == offset) ||
         (fp.has_add && fp.add_offset == offset);
}

static bool op_commute(Operation a, Operation b) {
  switch (a.kind) {
  case OP_CENTR:
  case OP_CEXIT:
  case OP_LENTR:
  case OP_LEXIT:
  case OP_MOV:
    return false;
  case OP_NOP:
    return true;
  default:
    break;
  }
  switch (b.kind) {
  case OP_CENTR:
  case OP_CEXIT:
  case OP_LENTR:
  case OP_LEXIT:
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
  if (fa.has_read && fp_touches(fb, fa.read_offset))
    return false;
  if (fa.has_add) {
    if ((fb.has_write && fb.write_offset == fa.add_offset) ||
        (fb.has_read && fb.read_offset == fa.add_offset) ||
        (fb.has_read2 && fb.read2_offset == fa.add_offset)) {
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

typedef struct {
  size_t old_ir_idx, new_ir_idx;
  int64_t offset;
} Fallback;

typedef struct {
  Fallback *items;
  size_t count;
  size_t capacity;
} Fallback_Array;

static void collect_moves(Intermediate const *old_ir, Intermediate *new_ir) {
  size_t i = 0;
  int64_t current_offset = 0;
  Fallback fb = {0};
  Fallback_Array fallbacks = {0};
  Operation op = {0};
  while (i < old_ir->count) {
  loop_head:
    op = old_ir->items[i];
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
    case OP_CENTR:
      fb = (Fallback){
          .offset = current_offset,
          .new_ir_idx = new_ir->count,
          .old_ir_idx = i,
      };
      da_append(&fallbacks, fb);
      op.as.centr.offset += current_offset;
      da_append(new_ir, op);
      break;
    case OP_CEXIT:
      fb = da_pop(&fallbacks);
      if (current_offset != fb.offset) {
        i = fb.old_ir_idx;
        new_ir->count = fb.new_ir_idx;
        op = (Operation){
            .kind = OP_MOV,
            .as.mov.summand = fb.offset,
        };
        current_offset = 0;
        goto loop_head;
      }
      da_append(new_ir, op);
      break;
    // TODO: think about the implication of adding offset to LENTR
    case OP_LENTR:
      if (fallbacks.count > 0) {
        fb = fallbacks.items[0];
        i = fb.old_ir_idx;
        new_ir->count = fb.new_ir_idx;
        op = (Operation){
            .kind = OP_MOV,
            .as.mov.summand = fb.offset,
        };
        current_offset = 0;
        goto loop_head;
      }
      if (current_offset != 0) {
        Operation mov_op = (Operation){
            .kind = OP_MOV,
            .as.mov.summand = current_offset,
        };
        da_append(new_ir, mov_op);
        current_offset = 0;
      }
      da_append(new_ir, op);
      break;
    case OP_LEXIT:
      NOB_ASSERT(fallbacks.count == 0);
      if (current_offset != 0) {
        Operation mov_op = (Operation){
            .kind = OP_MOV,
            .as.mov.summand = current_offset,
        };
        da_append(new_ir, mov_op);
        current_offset = 0;
      }
      da_append(new_ir, op);
    }

    i++;
  }
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
  case OP_LENTR:
  case OP_LEXIT:
  case OP_CENTR:
  case OP_CEXIT:
    return false;
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

void ir_segment(Intermediate *ir, CS_Array *segments) {
  size_t current_start = 0;

  Code_Segment segment;

  for (size_t i = 0; i < ir->count; i++) {
    Operation op = ir->items[i];
    switch (op.kind) {
    case OP_LENTR:
    case OP_LEXIT:
    case OP_CENTR:
    case OP_CEXIT:
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

void canonicalize(Intermediate *old_ir, Intermediate *new_ir) {
  new_ir->count = 0;
  size_t start = new_ir->count;

  collect_moves(old_ir, new_ir);

  // TODO: do the rest in segments;
  sort_ops(new_ir->items, new_ir->count);
  size_t new_count = combine_ops(&new_ir->items[start], new_ir->count - start);
  new_ir->count = start + new_count;
}
