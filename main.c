#include <errno.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <sys/types.h>

#define NOB_STRIP_PREFIX
#define NOB_IMPLEMENTATION
#include "nob.h"

typedef enum {
  TOK_MOVE_LEFT,
  TOK_MOVE_RIGHT,
  TOK_INC,
  TOK_DEC,
  TOK_OUTPUT,
  TOK_INPUT,
  TOK_JUMP_FOREWARD,
  TOK_JUMP_BACKWARD,
  TOK_START,
  TOK_END,
} Token;

char *token_as_str(Token t) {
  switch (t) {
  case TOK_MOVE_LEFT:
    return "MOVE_LEFT";
  case TOK_MOVE_RIGHT:
    return "MOVE_RIGHT";
  case TOK_INC:
    return "INC";
  case TOK_DEC:
    return "DEC";
  case TOK_OUTPUT:
    return "OUTPUT";
  case TOK_INPUT:
    return "INPUT";
  case TOK_JUMP_FOREWARD:
    return "JUMP_FOREWARD";
  case TOK_JUMP_BACKWARD:
    return "JUMP_BACKWARD";
  case TOK_START:
    return "START";
  case TOK_END:
    return "END";
  }
}

typedef enum {
  OP_MOVE_LEFT,
  OP_MOVE_RIGHT,
  OP_INC,
  OP_DEC,
  OP_OUTPUT,
  OP_INPUT,
  OP_JUMP_FOREWARD,
  OP_JUMP_BACKWARD,
} OpCode;

char *op_as_str(OpCode t) {
  switch (t) {
  case OP_MOVE_LEFT:
    return "<";
  case OP_MOVE_RIGHT:
    return ">";
  case OP_INC:
    return "+";
  case OP_DEC:
    return "-";
  case OP_OUTPUT:
    return ".";
  case OP_INPUT:
    return ",";
  case OP_JUMP_FOREWARD:
    return "[";
  case OP_JUMP_BACKWARD:
    return "]";
  }
}

OpCode token_as_op_code(Token t) {
  switch (t) {
  case TOK_MOVE_LEFT:
    return OP_MOVE_LEFT;
  case TOK_MOVE_RIGHT:
    return OP_MOVE_RIGHT;
  case TOK_INC:
    return OP_INC;
  case TOK_DEC:
    return OP_DEC;
  case TOK_OUTPUT:
    return OP_OUTPUT;
  case TOK_INPUT:
    return OP_INPUT;
  case TOK_JUMP_FOREWARD:
    return OP_JUMP_FOREWARD;
  case TOK_JUMP_BACKWARD:
    return OP_JUMP_BACKWARD;
  case TOK_START:
    UNREACHABLE("TOK_START is no Operation");
  case TOK_END:
    UNREACHABLE("TOK_END is no Operation");
  }
}

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
      return TOK_JUMP_FOREWARD;
    case ']':
      return TOK_JUMP_BACKWARD;
    default:
      continue;
    }
  }
  return TOK_END;
}

typedef struct {
  OpCode op;
  size_t rep_or_jump_idx;
} Operation;

typedef struct {
  Operation *items;
  size_t count;
  size_t capacity;
} Code;

void *get_jit_region(size_t len) {
  void *ptr = mmap(NULL, len, PROT_WRITE | PROT_EXEC,
                   MAP_PRIVATE | MAP_JIT | MAP_ANON, 0, 0);

  if (ptr == MAP_FAILED) {
    fprintf(stderr, "could not create memory mapping: %s\n", strerror(errno));
    return NULL;
  }
  return ptr;
}

typedef struct {
  size_t *items;
  size_t count;
  size_t capacity;
} JumpForwardIndecies;

int parse_bf(String_View iter, Code *code) {
  JumpForwardIndecies idxs = {0};

  Token prev_token = TOK_START;
  while (true) {
    Token token = get_next(&iter);
    Operation current_op = {0};
    switch (token) {
    case TOK_JUMP_FOREWARD:
      da_append(&idxs, code->count);
      current_op = (Operation){.op = OP_JUMP_FOREWARD, .rep_or_jump_idx = 0};
      da_append(code, current_op);
      break;

    case TOK_JUMP_BACKWARD:
      if (idxs.count == 0) {
        fprintf(stderr, "unexpected closing parens\n");
        goto error;
      }
      size_t last_idx = da_pop(&idxs);
      code->items[last_idx].rep_or_jump_idx = code->count;
      current_op =
          (Operation){.op = OP_JUMP_BACKWARD, .rep_or_jump_idx = last_idx};
      da_append(code, current_op);
      break;

    case TOK_START:
      UNREACHABLE("TOK_START is never return by get_next");
      break;
    case TOK_END:
      goto finish;
      break;

    default:
      if (token == prev_token) {
        code->items[code->count - 1].rep_or_jump_idx += 1;
      } else {
        current_op =
            (Operation){.op = token_as_op_code(token), .rep_or_jump_idx = 1};
        da_append(code, current_op);
      }
    }
    prev_token = token;
  }

finish:
  if (idxs.count != 0) {
    fprintf(stderr, "unclosed opening parens\n");
    goto error;
  }
  da_free(idxs);
  return 0;

error:
  *code = (Code){0};
  da_free(idxs);
  return -1;
}

int main(int argc, char **argv) {
  char *p_name = *argv;
  shift(argv, argc);

  if (argc == 0) {
    printf("Usage %s <filename.bf>\n", p_name);
    printf("No input files were provided\n");
    exit(-1);
  }

  String_Builder contents = {0};
  read_entire_file(*argv, &contents);
  sb_append_null(&contents);
  printf("contents: %s\n", contents.items);
  String_View iter = sb_to_sv(contents);

  Code code = {0};

  parse_bf(iter, &code);

  for (size_t i = 0; i < code.count; i++) {
    Operation op = code.items[i];
    printf("%0*zu: %s %*zu\n", 3, i, op_as_str(op.op), 4, op.rep_or_jump_idx);
  }

  return 0;
}
