#ifndef COMMON_H
#define COMMON_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define NOB_STRIP_PREFIX
#include "../nob.h"

#define MIN_TAPE_LEN ((size_t)(1 << 12))

extern bool TEST_OUTPUT;
extern bool RUN_JIT;
extern bool RUN_IR;
extern bool STATS;
extern bool EMIT_IR;
extern bool OPTIMIZE;
extern bool DEBUG_INFO;
extern size_t TAPE_PAGES;

typedef enum {
  Bf_R_Ok = 0,
  Bf_R_Underflow,
  Bf_R_Overflow,
  Bf_R_Could_Not_Parse,
  Bf_R_Unknown,
} Bf_Result;

extern Bf_Result RESULT;

typedef struct {
  size_t *items;
  size_t count;
  size_t capacity;
} Index_Array;

typedef struct {
  void *ptr;
  size_t size;
} Code;

#endif // COMMON_H
