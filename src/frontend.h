#ifndef FRONTEND_H
#define FRONTEND_H

#include "common.h"
#include "ir.h"

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

bool parse_bf(String_View iter, Intermediate *ir);

#endif // FRONTEND_H
