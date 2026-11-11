#include "frontend.h"
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
