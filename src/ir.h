#ifndef IR_H
#define IR_H

#include "common.h"

typedef enum {
  OP_NOP = 0,
  OP_MOV,

  OP_ADD,
  OP_NEG,
  OP_CLR,
  OP_MAD,
  OP_MUL,

  OP_PCH,
  OP_GCH,

  OP_LENTR,
  OP_LEXIT,
  OP_CENTR,
  OP_CEXIT,
} Op_Kind;

typedef struct {
  Op_Kind kind;
  union {
    struct {
    } nop;
    struct {
      int64_t _pad;
      int64_t summand;
    } mov;
    struct {
      int64_t offset;
      int64_t summand;
    } add;
    struct {
      int64_t offset;
    } neg;
    struct {
      int64_t offset;
    } clr;
    struct {
      int64_t dst_offset;
      int64_t src_offset;
      int64_t factor;
    } mad;
    struct {
      int64_t dst_offset;
      int64_t src1_offset;
      int64_t src2_offset;
      int64_t factor;
    } mul;
    struct {
      int64_t offset;
    } gch;
    struct {
      int64_t offset;
    } pch;
    struct {
    } lentr;
    struct {
    } lexit;
    struct {
      int64_t offset;
    } centr;
    struct {
    } cexit;
    int64_t offset;
  } as;
} Operation;

typedef struct {
  Operation *items;
  size_t count;
  size_t capacity;
} Intermediate;

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

#define OP_FMT_LEN 30
char *fmt_op(Operation);

void fmt_ir(String_Builder *sb, Operation *ops, size_t count);

void cs_canonicalize(Code_Segment segment, Intermediate *new_ir);

void ir_segment(Intermediate *ir, CS_Array *segments);

size_t eliminate_loops(Intermediate *ir);

void debug_cmp_segments(CS_Array *seg1, CS_Array *seg2);

void ir_optimize(Intermediate *ir);

#endif // IR_H
