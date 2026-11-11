#include "ir.h"

static const char *fmt_off(int64_t off) {
  return temp_sprintf("^%" PRId64, off);
}

char *fmt_op(Operation op) {
  switch (op.kind) {
  case OP_MOV:
    return temp_sprintf("MOV    %5" PRId64 " %5s %5s %5s", op.as.mov.summand,
                        "", "", "");
  case OP_ADD:
    return temp_sprintf("ADD    %5s %5" PRId64 " %5s %5s",
                        fmt_off(op.as.add.offset), op.as.add.summand, "", "");
  case OP_NEG:
    return temp_sprintf("NEG    %5s %5s %5s %5s", fmt_off(op.as.neg.offset), "",
                        "", "");
  case OP_CLR:
    return temp_sprintf("CLR    %5s %5s %5s %5s", fmt_off(op.as.clr.offset), "",
                        "", "");
  case OP_MAD:
    return temp_sprintf("MAD    %5s %5s %5" PRId64 " %5s",
                        fmt_off(op.as.mad.dst_offset),
                        fmt_off(op.as.mad.src_offset), op.as.mad.factor, "");
  case OP_MUL:
    return temp_sprintf("MUL    %5s %5s %5s %5" PRId64,
                        fmt_off(op.as.mul.dst_offset),
                        fmt_off(op.as.mul.src1_offset),
                        fmt_off(op.as.mul.src2_offset), op.as.mul.factor);
  case OP_PCH:
    return temp_sprintf("PCH    %5s %5s %5s %5s", fmt_off(op.as.pch.offset), "",
                        "", "");
  case OP_GCH:
    return temp_sprintf("GCH    %5s %5s %5s %5s", fmt_off(op.as.gch.offset), "",
                        "", "");
  case OP_LSTART:
    return temp_sprintf("LSTART %5s %5s %5s %5s", "", "", "", "");
  case OP_LEND:
    return temp_sprintf("LEND   %5s %5s %5s %5s", "", "", "", "");
  case OP_NOP:
    return temp_sprintf("NOP    %5s %5s %5s %5s", "", "", "", "");
  }
  return NULL;
}

void fmt_ir(String_Builder *sb, Operation *ops, size_t count) {
  for (size_t i = 0; i < count; i++) {
    size_t mark = temp_save();
    sb_appendf(sb, "%s\n", fmt_op(ops[i]));
    temp_rewind(mark);
  }
}

void debug_cmp_segments(CS_Array *seg1, CS_Array *seg2) {
  for (size_t i = 0; i < seg2->count; i++) {
    Code_Segment old_segment = seg1->items[i];
    Code_Segment new_segment = seg2->items[i];
    fprintf(stderr,
            "\n________________________"
            "Segment_%0*zu"
            "___________________________\n",
            6, i);
    size_t count = old_segment.count > new_segment.count ? old_segment.count
                                                         : new_segment.count;
    for (size_t j = 0; j < count; j++) {
      size_t mark = temp_save();
      if (j < old_segment.count) {
        Operation op = old_segment.items[j];
        fprintf(stderr, "%s |   ", fmt_op(op));
      } else {
        fprintf(stderr, "%*s |   ", OP_FMT_LEN, "");
      }

      if (j < new_segment.count) {
        Operation op = new_segment.items[j];
        fprintf(stderr, "%s\n", fmt_op(op));
      } else {
        fprintf(stderr, "\n");
      }
      temp_rewind(mark);
    }
  }
}

void ir_optimize(Intermediate *ir) {
  Intermediate ir_buf = {0};
  Intermediate *new_ir = &ir_buf;
  CS_Array segments = {0};
  CS_Array new_segments = {0};

  size_t pass = 0;
  bool changed = false;
  do {
    if (DEBUG_INFO) {
      fprintf(stderr, "\n\nOptimization pass %zu\n", pass);
    }
    new_ir->count = 0;
    segments.count = 0;

    ir_segment(ir, &segments);
    for (size_t i = 0; i < segments.count; i++) {
      cs_canonicalize(segments.items[i], new_ir);
    }

    if (DEBUG_INFO) {
      new_segments.count = 0;
      ir_segment(new_ir, &new_segments);
      debug_cmp_segments(&segments, &new_segments);
    }

    changed = eliminate_loops(new_ir);

    Intermediate temp = *new_ir;
    *new_ir = *ir;
    *ir = temp;
    pass++;
  } while (changed);

  da_free(new_segments);
  da_free(segments);
  da_free(*new_ir);
}

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
    TODO("mul");
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
  case OP_LSTART:
  case OP_LEND:
    UNREACHABLE("loop instructions have no well defined footprint");
  }
  return f;
}

bool fp_touches(Footprint fp, int64_t offset) {
  return (fp.has_write && fp.write_offset == offset) ||
         (fp.has_read && fp.read_offset == offset) ||
         (fp.has_add && fp.add_offset == offset);
}
