#include <errno.h>
#include <inttypes.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#include "backend.h"
#include "common.h"
#include "memory.h"

#define NOB_STRIP_PREFIX
#define NOB_IMPLEMENTATION
#include "../nob.h"

// COMMANDLINE CONFIG
static bool TEST_OUTPUT = false;
static bool RUN_JIT = true;
static bool RUN_IR = false;
static bool STATS = false;
static bool EMIT_IR = false;
static bool OPTIMIZE = false;
static bool DEBUG_INFO = false;
static size_t TAPE_PAGES = 1;

Bf_Result RESULT = Bf_R_Ok;

void print_result(void) {
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

static const char *fmt_off(int64_t off) {
  return temp_sprintf("^%" PRId64, off);
}

#define OP_FMT_LEN 30
char *op_fmt(Operation op) {
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

void sb_append_ir(String_Builder *sb, Operation *ops, size_t count) {
  for (size_t i = 0; i < count; i++) {
    size_t mark = temp_save();
    sb_appendf(sb, "%s\n", op_fmt(ops[i]));
    temp_rewind(mark);
  }
}

typedef struct {
  bool has_read, has_write, has_add, io;
  int64_t read_offset, write_offset, add_offset;
} Footprint;

static inline Footprint op_footprint(Operation op) {
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

static inline bool fp_touches(Footprint fp, int64_t offset) {
  return (fp.has_write && fp.write_offset == offset) ||
         (fp.has_read && fp.read_offset == offset) ||
         (fp.has_add && fp.add_offset == offset);
}

static inline bool op_commute(Operation a, Operation b) {
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

bool op_greater(Operation a, Operation b) {
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

bool parse_bf(String_View iter, Intermediate *ir) {
  size_t open_loops = 0;
  Token token;
  Operation operation;
  while (true) {
    token = get_next(&iter);
    operation = (Operation){0};
    switch (token) {
    case TOK_JUMP_FORWARD:
      operation = (Operation){.kind = OP_LSTART};
      da_append(ir, operation);
      open_loops++;
      break;

    case TOK_JUMP_BACKWARD:
      if (open_loops == 0) {
        goto error;
      }
      open_loops--;
      operation = (Operation){.kind = OP_LEND};
      da_append(ir, operation);
      break;

    case TOK_START:
      UNREACHABLE("TOK_START is never returned by get_next");
      break;
    case TOK_END:
      goto finish;
      break;

    case TOK_INC:
      if (ir->count > 0 && da_last(ir).kind == OP_ADD) {
        da_last(ir).as.add.summand += 1;
      } else {
        operation = (Operation){.kind = OP_ADD, .as.add = {.summand = 1}};
        da_append(ir, operation);
      }
      break;
    case TOK_DEC:
      if (ir->count > 0 && da_last(ir).kind == OP_ADD) {
        da_last(ir).as.add.summand -= 1;
      } else {
        operation = (Operation){.kind = OP_ADD, .as.add = {.summand = -1}};
        da_append(ir, operation);
      }
      break;

    case TOK_MOVE_RIGHT:
      if (ir->count > 0 && da_last(ir).kind == OP_MOV) {
        da_last(ir).as.mov.summand += 1;
      } else {
        operation = (Operation){.kind = OP_MOV, .as.mov = {.summand = 1}};
        da_append(ir, operation);
      }
      break;
    case TOK_MOVE_LEFT:
      if (ir->count > 0 && da_last(ir).kind == OP_MOV) {
        da_last(ir).as.mov.summand += -1;
      } else {
        operation = (Operation){.kind = OP_MOV, .as.mov = {.summand = -1}};
        da_append(ir, operation);
      }
      break;

    case TOK_OUTPUT:
      operation = (Operation){.kind = OP_PCH, .as.pch = {.offset = 0}};
      da_append(ir, operation);
      break;
    case TOK_INPUT:
      operation = (Operation){.kind = OP_GCH, .as.gch = {.offset = 0}};
      da_append(ir, operation);
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
} Code_Segment;

typedef struct {
  Code_Segment *items;
  size_t count;
  size_t capacity;
} CS_Array;

void cs_canonicalize(Code_Segment segment, Intermediate *new_ir) {
  if (!segment.is_basicblock) {
    da_append_many(new_ir, segment.items, segment.count);
    return;
  }

  size_t start = new_ir->count;
  int64_t current_offset = 0;
  for (size_t i = 0; i < segment.count; i++) {
    Operation op = segment.items[i];
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
  for (size_t i = new_ir->count; start < i; i--) {
    bool swapped = false;
    for (size_t j = start; j + 1 < i; j++) {
      Operation *a = &new_ir->items[j];
      Operation *b = &new_ir->items[j + 1];
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

  // TODO: try combine instructions

  Operation move =
      (Operation){.kind = OP_MOV, .as.mov.summand = current_offset};
  da_append(new_ir, move);
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

enum LoopKind {
  LK_NONE = 0,
  LK_INC,
  LK_DEC,
};

bool eliminate_innermost_loop(Operation *items, size_t start, size_t end) {
  enum LoopKind loop_kind = LK_NONE;
  for (size_t i = start + 1; i < end; i++) {
    Operation *op = &items[i];
    switch (op->kind) {
    case OP_NOP:
      break;
    case OP_MOV:
      NOB_ASSERT(op->as.mov.summand == 0);
      break;
    case OP_ADD:
      if (op->as.add.offset == 0) {
        if (loop_kind != LK_NONE) {
          return false;
        }

        switch (op->as.add.summand) {
        case -1:
          loop_kind = LK_DEC;
          break;
        case 1:
          loop_kind = LK_INC;
          break;
        case 0:
          break;
        default:
          return false;
        }
      }
      break;
    case OP_NEG:
      // TODO: think about this case
      // cannot optimize this any further
      // maybe loop kind once?
      return false;
    case OP_CLR:
      if (op->as.clr.offset == 0) {
        // TODO: think about this case
        // cannot optimize this any further
        // maybe loop kind once?
        return false;
      }
      break;
    case OP_MUL:
      // TODO: think about this case
      // cannot optimize this any further
      return false;
      break;
    case OP_MAD:
      // TODO: think about this case
      // cannot optimize this any further
      return false;
      break;
    case OP_PCH:
    case OP_GCH:
    case OP_LSTART:
    case OP_LEND:
      NOB_UNREACHABLE("cannot eliminate loop with this instruction");
    }
  }
  if (loop_kind == LK_NONE) {
    return false;
  }
  if (loop_kind == LK_INC) {
    items[start] = (Operation){
        .kind = OP_NEG,
        .as.neg = {.offset = 0},
    };
  } else {
    items[start].kind = OP_NOP;
  }
  items[end] = (Operation){
      .kind = OP_CLR,
      .as.clr = {.offset = 0},
  };

  for (size_t i = start + 1; i < end; i++) {
    Operation *op = &items[i];
    switch (op->kind) {
    case OP_NOP:
      break;
    case OP_MOV:
      NOB_ASSERT(op->as.mov.summand == 0);
      break;
    case OP_ADD:
      if (op->as.add.offset == 0) {
        op->kind = OP_NOP;
      } else {
        *op = (Operation){
            .kind = OP_MAD,
            .as.mad = {.src_offset = 0,
                       .dst_offset = op->as.add.offset,
                       .factor = op->as.add.summand},
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
    case OP_PCH:
    case OP_GCH:
    case OP_LSTART:
    case OP_LEND:
      NOB_UNREACHABLE("cannot eliminate loop with this instruction");
    }
  }
  return true;
}

bool eliminate_loops(Intermediate *ir) {
  // FIXME: when eliminating the loop the zero check is removed
  // which is fine for instruction which are nop for ^0 == 0
  // but if this is not true the op is run irregardless of wether ^0 is zero
  size_t eliminations = 0;
  size_t l_start = SIZE_MAX;
  String_Builder debug_output = {0};
  for (size_t i = 0; i < ir->count; i++) {
    Operation op = ir->items[i];
    switch (op.kind) {
    case OP_LSTART:
      l_start = i;
      break;
    case OP_LEND:
      if (l_start != SIZE_MAX) {
        NOB_ASSERT(ir->items[i - 1].kind == OP_MOV);

        if (ir->items[i - 1].as.mov.summand == 0) {
          if (DEBUG_INFO) {
            debug_output.count = 0;
            sb_appendf(&debug_output, "\nElimination %zu\n", eliminations);
            sb_append_ir(&debug_output, &ir->items[l_start], i - l_start + 1);
          }
          if (eliminate_innermost_loop(ir->items, l_start, i)) {
            eliminations++;
            if (DEBUG_INFO) {
              sb_appendf(&debug_output, "transformed to\n");
              sb_append_ir(&debug_output, &ir->items[l_start], i - l_start + 1);
              fprintf(stderr, SV_Fmt, SV_Arg(sb_to_sv(debug_output)));
            }
          }
        }
        l_start = SIZE_MAX;
      }
      break;

    case OP_PCH:
    case OP_GCH:
      l_start = SIZE_MAX;
      break;
    case OP_NOP:
    case OP_MOV:
    case OP_ADD:
    case OP_CLR:
    case OP_MUL:
    case OP_MAD:
    case OP_NEG:
      break;
    }
  }
  if (DEBUG_INFO) {
    fprintf(stderr, "\nhad %zu eliminations\n", eliminations);
  }
  sb_free(debug_output);
  return eliminations != 0;
}

void debug_cmp_segments(CS_Array *seg1, CS_Array *seg2) {
  for (size_t i = 0; i < seg2->count; i++) {
    Code_Segment old_segment = seg1->items[i];
    Code_Segment new_segment = seg2->items[i];
    fprintf(stderr,
            "\n__________________"
            "Segment_%0*zu"
            "_____________________\n",
            6, i);
    size_t count = old_segment.count > new_segment.count ? old_segment.count
                                                         : new_segment.count;
    for (size_t j = 0; j < count; j++) {
      size_t mark = temp_save();
      if (j < old_segment.count) {
        Operation op = old_segment.items[j];
        fprintf(stderr, "%s |   ", op_fmt(op));
      } else {
        fprintf(stderr, "%*s |   ", OP_FMT_LEN, "");
      }

      if (j < new_segment.count) {
        Operation op = new_segment.items[j];
        fprintf(stderr, "%s\n", op_fmt(op));
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
  bool changed;
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

    if (pass != 0) {
      // FIXME: see other fixme
      break;
      Intermediate temp = *new_ir;
      *new_ir = *ir;
      *ir = temp;
    }
    changed = eliminate_loops(new_ir);

    Intermediate temp = *new_ir;
    *new_ir = *ir;
    *ir = temp;
    pass++;
  } while (changed);

  da_free(new_segments);
  da_free(segments);
  da_free(ir_buf);
}

void ir_interpret(Intermediate *ir) {
  size_t instruction_ptr = 0;
  int64_t tape_ptr = 0;
  get_tape_region(TAPE_PAGES);
  Index_Array jump_back = {0};

  while (instruction_ptr < ir->count) {
    Operation op = ir->items[instruction_ptr];
    switch (op.kind) {
    case OP_NOP:
      instruction_ptr++;
      break;
    case OP_ADD:
      TAPE_PTR[tape_ptr + op.as.add.offset] += op.as.add.summand;
      instruction_ptr++;
      break;

    case OP_NEG:
      TAPE_PTR[tape_ptr + op.as.clr.offset] =
          (uint8_t)(0x100 - (uint16_t)TAPE_PTR[tape_ptr + op.as.clr.offset]);
      instruction_ptr++;
      break;
    case OP_CLR:
      TAPE_PTR[tape_ptr + op.as.clr.offset] = 0;
      instruction_ptr++;
      break;
    case OP_MAD:
      TAPE_PTR[tape_ptr + op.as.mad.dst_offset] +=
          TAPE_PTR[tape_ptr + op.as.mad.src_offset] * op.as.mad.factor;
      instruction_ptr++;
      break;
    case OP_MUL:
      TAPE_PTR[tape_ptr + op.as.mul.dst_offset] +=
          TAPE_PTR[tape_ptr + op.as.mul.src1_offset] *
          TAPE_PTR[tape_ptr + op.as.mul.src2_offset] * op.as.mul.factor;
      instruction_ptr++;
      break;

    case OP_MOV:
      tape_ptr += op.as.mov.summand;
      instruction_ptr++;
      break;

    case OP_GCH:
      TAPE_PTR[tape_ptr + op.as.gch.offset] = getchar();
      instruction_ptr++;
      break;
    case OP_PCH:
      putchar(TAPE_PTR[tape_ptr + op.as.pch.offset]);
      instruction_ptr++;
      break;

    case OP_LSTART:
      if (TAPE_PTR[tape_ptr] == 0) {
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
      if (TAPE_PTR[tape_ptr] != 0) {
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

void run_on_jit(Code code) {
  jit_fn_type code_ptr = copy_to_jit_region(code.ptr, code.size);

  get_tape_region(TAPE_PAGES);

  code_ptr(TAPE_PTR, putchar, getchar);

  free_tape_region();
  free_jit_region(code_ptr, code.size);
}

void print_usage(char *pname) {
  fprintf(stderr, "Usage %s FLAGS <filename.bf>\n", pname);
  fprintf(stderr, "   --interpret-ir   -i     interpret ir directly\n");
  fprintf(stderr, "   --optimize       -o     optimize code\n");
  fprintf(stderr, "   --stats          -s     print statistics\n");
  fprintf(stderr,
          "   --tape-len              number of pages used for tape (pagesize: "
          "%d on this machine)\n",
          getpagesize());
  fprintf(stderr,
          "   --emit-ir        -e     emit intermediate representation\n");
  fprintf(stderr, "   --test-output           output for tests\n");
  fprintf(stderr, "   --debug          -d     print debug output\n");
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
    if (arg[1] == '-') {
      arg += 2;
      if (strcmp(arg, "test-output") == 0) {
        TEST_OUTPUT = true;
      } else if (strcmp(arg, "interpret-ir") == 0) {
        RUN_JIT = false;
        RUN_IR = true;
      } else if (strcmp(arg, "stats") == 0) {
        STATS = true;
      } else if (strcmp(arg, "optimize") == 0) {
        OPTIMIZE = true;
      } else if (strcmp(arg, "emit-ir") == 0) {
        EMIT_IR = true;
      } else if (strcmp(arg, "debug") == 0) {
        DEBUG_INFO = true;
      } else if (strcmp(arg, "tape-len") == 0) {
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
    } else {
      arg++;
      while (*arg) {
        if (*arg == 'i') {
          RUN_JIT = false;
          RUN_IR = true;
        } else if (*arg == 's') {
          STATS = true;
        } else if (*arg == 'o') {
          OPTIMIZE = true;
        } else if (*arg == 'e') {
          EMIT_IR = true;
        } else if (*arg == 'd') {
          DEBUG_INFO = true;
        } else {
          fprintf(stderr, "unrecognized flag `%s`\n", *argv);
          print_usage(p_name);
          exit(-1);
        }
        arg++;
      }
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
  if ((getpagesize() & ((1 << 12) - 1)) != 0) {
    fprintf(stderr, "unsupported page size %d\n", getpagesize());
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

  if (SETUP_FAULT_JUMP() != 0)
    goto error;

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
    size_t old_count = ir.count;
    start = clock();
    ir_optimize(&ir);
    end = clock();
    if (STATS) {
      fprintf(stderr, "\n");
      fprintf(stderr, "Optimized IR\n");
      fprintf(stderr, "  ran for ");
      pprint_clock(end - start);
      fprintf(stderr, "\n");
      fprintf(stderr, "  old instruction count: %*zu\n", 6, old_count);
      fprintf(stderr, "  new instruction count: %*zu\n", 6, ir.count);
      fprintf(stderr, "\n");
    }
  }

  if (EMIT_IR) {
    printf("\nIR for `%s`\n", file_name);
    String_Builder ir_str = {0};
    sb_append_ir(&ir_str, ir.items, ir.count);
    sb_append_null(&ir_str);
    printf("%s", ir_str.items);
  }

  if (RUN_JIT) {
    if (DEBUG_INFO) {
      fprintf(stderr, "\nstarting JIT interpretation\n");
    }
    start = clock();
    Code code = translate_to_code(&ir);
    end = clock();
    if (STATS) {
      fprintf(stderr, "JIT compilation\n");
      // TODO: only valid for aarch64
      fprintf(stderr, "  %zu instructions\n", code.size / 4);
      fprintf(stderr, "  compiled in ");
      pprint_clock(end - start);
      fprintf(stderr, "\n");
      fprintf(stderr, "\n");
    }

    start = clock();
    run_on_jit(code);
    end = clock();
    free(code.ptr);

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
    if (DEBUG_INFO) {
      fprintf(stderr, "\nstarting IR interpretation\n");
    }
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
