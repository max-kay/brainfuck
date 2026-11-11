#ifndef BACKEND_H
#define BACKEND_H

#include "common.h"
#include "ir.h"

typedef struct {
  void *ptr;
  size_t size;
} Code;

Code translate_to_code(Intermediate *ir);

typedef int (*putchar_type)(int);
typedef int (*getchar_type)(void);
typedef void (*jit_fn_type)(uint8_t *, putchar_type, getchar_type);

#endif // BACKEND_H
