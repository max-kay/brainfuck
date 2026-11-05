#ifndef COMMON_H
#define COMMON_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define MIN_TAPE_LEN ((size_t)(1 << 12))

typedef enum {
  Bf_R_Ok = 0,
  Bf_R_Underflow = 1,
  Bf_R_Overflow = 2,
  Bf_R_Could_Not_Parse,
  Bf_R_Unknown,
} Bf_Result;

typedef struct {
  size_t *items;
  size_t count;
  size_t capacity;
} Index_Array;

typedef enum {
  OP_NOP = 0,
  // operand0 signed amout to move
  OP_MOV,
  // operand0 amount to add
  // operand1 offset
  OP_ADD,
  // operand0 offset
  OP_CLR,
  // operand0 target_offset
  // operand1 offset_source (factor)
  // operand2
  OP_MAD,
  // operand0 repetitions
  OP_PCH,
  // operand0 repetitions
  OP_GCH,
  // no operands
  OP_LSTART,
  // no operands
  OP_LEND,
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
    } clr;
    struct {
      int64_t dst_offset;
      int64_t src_offset;
      int64_t factor;
    } mad;
    struct {
      int64_t offset;
    } gch;
    struct {
      int64_t offset;
    } pch;
    struct {
    } lstart;
    struct {
    } lend;
    struct {
      int64_t offset;
    } get_offset;
  } as;
} Operation;

typedef struct {
  Operation *items;
  size_t count;
  size_t capacity;
} Intermediate;

extern Bf_Result RESULT;

typedef struct {
  void *ptr;
  size_t size;
} Code;

typedef int (*putchar_type)(int);
typedef int (*getchar_type)(void);
typedef void (*jit_fn_type)(uint8_t *, putchar_type, getchar_type);

#endif // COMMON_H
