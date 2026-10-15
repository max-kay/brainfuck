#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <sys/types.h>

#define NOB_STRIP_PREFIX
#define NOB_IMPLEMENTATION
#include "nob.h"

// #define DEBUG

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
} Intermediate;

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

int parse_bf(String_View iter, Intermediate *ir) {
  JumpForwardIndecies idxs = {0};

  Token prev_token = TOK_START;
  while (true) {
    Token token = get_next(&iter);
    Operation current_op = {0};
    switch (token) {
    case TOK_JUMP_FOREWARD:
      da_append(&idxs, ir->count);
      current_op = (Operation){.op = OP_JUMP_FOREWARD, .rep_or_jump_idx = 0};
      da_append(ir, current_op);
      break;

    case TOK_JUMP_BACKWARD:
      if (idxs.count == 0) {
        fprintf(stderr, "unexpected closing parens\n");
        goto error;
      }
      size_t last_idx = da_pop(&idxs);
      ir->items[last_idx].rep_or_jump_idx = ir->count;
      current_op =
          (Operation){.op = OP_JUMP_BACKWARD, .rep_or_jump_idx = last_idx};
      da_append(ir, current_op);
      break;

    case TOK_START:
      UNREACHABLE("TOK_START is never returned by get_next");
      break;
    case TOK_END:
      goto finish;
      break;

    default:
      if (token == prev_token) {
        ir->items[ir->count - 1].rep_or_jump_idx += 1;
      } else {
        current_op =
            (Operation){.op = token_as_op_code(token), .rep_or_jump_idx = 1};
        da_append(ir, current_op);
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
  *ir = (Intermediate){0};
  da_free(idxs);
  return -1;
}

typedef struct {
  uint8_t *items;
  size_t count;
  size_t capacity;
} Tape;

uint8_t tape_read(Tape *tape, size_t idx) {
  if (idx < tape->count) {
    return tape->items[idx];
  } else {
    return 0;
  }
}

void tape_write(Tape *tape, size_t idx, uint8_t val) {
  if (idx >= tape->count) {
    while (tape->count <= idx)
      da_append(tape, 0);
  }
  tape->items[idx] = val;
}

int ir_interpret(Intermediate *ir) {
  size_t instruction_ptr = 0;
  size_t tape_ptr = 0;
  Tape tape = {0};
  while (instruction_ptr < ir->count) {
    Operation op = ir->items[instruction_ptr];
    uint8_t tape_val = tape_read(&tape, tape_ptr);

#ifdef DEBUG
    fprintf(stderr, "instruction_ptr: %zu\n", instruction_ptr);
    fprintf(stderr, "tape[%zu] = %d\n", tape_ptr, tape_val);
    fprintf(stderr, "instruction: Operation { op = %s, rep_or_jump = %zu }\n\n",
            op_as_str(op.op), op.rep_or_jump_idx);
#endif // DEBUG

    switch (op.op) {
    case OP_INC:
      tape_val += op.rep_or_jump_idx;
      tape_write(&tape, tape_ptr, tape_val);
      instruction_ptr++;
      break;
    case OP_DEC:
      tape_val -= op.rep_or_jump_idx;
      tape_write(&tape, tape_ptr, tape_val);
      instruction_ptr++;
      break;

    case OP_MOVE_RIGHT:
      tape_ptr += op.rep_or_jump_idx;
      instruction_ptr++;
      break;
    case OP_MOVE_LEFT:
      tape_ptr -= op.rep_or_jump_idx;
      instruction_ptr++;
      break;

    case OP_INPUT:
      TODO("input");
      instruction_ptr++;
      break;
    case OP_OUTPUT:
      for (size_t i = 0; i < op.rep_or_jump_idx; i++)
        printf("%c", tape_val);
      instruction_ptr++;
      break;

    case OP_JUMP_FOREWARD:
      if (tape_val == 0) {
        instruction_ptr = op.rep_or_jump_idx + 1;
      } else {
        instruction_ptr++;
      }
      break;
    case OP_JUMP_BACKWARD:
      if (tape_val != 0) {
        instruction_ptr = op.rep_or_jump_idx + 1;
      } else {
        instruction_ptr++;
      }
      break;
    }
  }

  da_free(tape);
  return 0;
}

void print_ir(Intermediate *ir) {
  for (size_t i = 0; i < ir->count; i++) {
    Operation op = ir->items[i];
    printf("%0*zu: %s %*zu\n", 3, i, op_as_str(op.op), 4, op.rep_or_jump_idx);
  }
}

int main(int argc, char **argv) {
  char *p_name = *argv;
  shift(argv, argc);

  if (argc == 0 | argc > 1) {
    printf("Usage %s <filename.bf>\n", p_name);
    exit(-1);
  }

  String_Builder contents = {0};
  read_entire_file(*argv, &contents);

  Intermediate ir = {0};

  if (parse_bf(sb_to_sv(contents), &ir) != 0)
    exit(-1);

  ir_interpret(&ir);

  return 0;
}
