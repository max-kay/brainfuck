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

typedef struct {
  OpCode op;
  uint rep_or_jump_len;
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
  JumpForwardIndecies idxs = {0};

  Token prev_token = TOK_START;
  while (prev_token != TOK_END) {
    Token token = get_next(&iter);
    if (token == TOK_JUMP_FOREWARD) {
      da_append(&idxs, code.count);
      Operation op = {.op = OP_JUMP_FOREWARD, .rep_or_jump_len = 0};
      da_append(&code, op);
    }
  }

  return 0;
}
