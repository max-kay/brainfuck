#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <termios.h>

#define NOB_STRIP_PREFIX
#define NOB_IMPLEMENTATION
#include "nob.h"

// #define DEBUG
//
#define TAPE_LEN 1024 * 4

static struct termios orig_termios;

void init_terminal(void) {
  struct termios raw;
  tcgetattr(STDIN_FILENO, &orig_termios);
  raw = orig_termios;
  raw.c_lflag &= ~(ICANON | ECHO);
  tcsetattr(STDIN_FILENO, TCSANOW, &raw);
}

void reset_terminal(void) { tcsetattr(STDIN_FILENO, TCSANOW, &orig_termios); }

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
    return "L";
  case OP_MOVE_RIGHT:
    return "R";
  case OP_INC:
    return "A";
  case OP_DEC:
    return "S";
  case OP_OUTPUT:
    return "O";
  case OP_INPUT:
    return "I";
  case OP_JUMP_FOREWARD:
    return "F";
  case OP_JUMP_BACKWARD:
    return "B";
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

bool parse_bf(String_View iter, Intermediate *ir) {
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
        da_last(ir).rep_or_jump_idx += 1;
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
  return true;

error:
  ir->count = 0;
  da_free(idxs);
  return false;
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
    size_t old_count = tape->count;
    da_resize(tape, idx + 1);
    memset(&tape->items[old_count], 0, idx + 1 - old_count);
  }
  tape->items[idx] = val;
}

bool ir_interpret(Intermediate *ir) {
  init_terminal();

  size_t instruction_ptr = 0;
  size_t tape_ptr = 0;
  Tape tape = {0};
  while (instruction_ptr < ir->count) {
    Operation op = ir->items[instruction_ptr];
    uint8_t tape_val = tape_read(&tape, tape_ptr);

#ifdef DEBUG
    fprintf(stderr, "instruction_ptr: %zu\n", instruction_ptr);
    fprintf(stderr, "tape[%zu] = %d\n", tape_ptr, tape_val);
    fprintf(stderr,
            "instruction: Operation { op = `%s`, rep_or_jump = %zu }\n\n",
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
      if (tape_ptr >= TAPE_LEN) {
        fprintf(stderr, "OVERFLOW");
        goto error;
      }
      instruction_ptr++;
      break;
    case OP_MOVE_LEFT:
      if (op.rep_or_jump_idx > tape_ptr) {
        fprintf(stderr, "UNDERFLOW");
        goto error;
      }
      tape_ptr -= op.rep_or_jump_idx;
      instruction_ptr++;
      break;

    case OP_INPUT:
      for (size_t i = 0; i < op.rep_or_jump_idx; i++)
        tape_write(&tape, tape_ptr, getchar());
      instruction_ptr++;
      break;
    case OP_OUTPUT:
      for (size_t i = 0; i < op.rep_or_jump_idx; i++)
        putchar(tape_val);
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
  reset_terminal();
  return true;
error:
  da_free(tape);
  reset_terminal();
  return false;
}

void append_ir(String_Builder *sb, Intermediate *ir) {
  for (size_t i = 0; i < ir->count; i++) {
    Operation op = ir->items[i];
    sb_appendf(sb, "%0*zu: %s %*zu\n", 3, i, op_as_str(op.op), 4,
               op.rep_or_jump_idx);
  }
}

void append_freestanding_main(String_Builder *sb) {
  sb_appendf(sb, ".global _main\n");
  sb_appendf(sb, ".align 2\n");

  sb_appendf(sb, "_main:\n");

  sb_appendf(sb, "  stp fp, lr, [sp, -16]!\n");
  sb_appendf(sb, "  mov fp, sp\n");

  sb_appendf(sb, "  mov x0, %d\n", TAPE_LEN);
  sb_appendf(sb, "  bl _malloc\n");
  sb_appendf(sb, "  cbz x0, exit\n");
  sb_appendf(sb, "  bl _bf_assembled\n");

  sb_appendf(sb, "exit:\n");
  sb_appendf(sb, "  ldp fp, lr, [sp], 16\n");
  sb_appendf(sb, "  sub x0, x0, #1\n");
  sb_appendf(sb, "  bl _exit\n");
}

// generates the function:
// bool brainfuck_function(void *tape_ptr)
void translate_to_asm(String_Builder *sb, Intermediate *ir) {
  char *tape_base = "x20";
  char *tape_index = "x21";

  // Prelude
  sb_appendf(sb, ".globl _bf_assembled\n");
  sb_appendf(sb, "_bf_assembled:\n");
  sb_appendf(sb, "  sub sp, sp, 32\n");
  sb_appendf(sb, "  stp x29, x30, [sp, 16]\n");
  sb_appendf(sb, "  stp %s, %s, [sp]\n", tape_base, tape_index);
  sb_appendf(sb, "  mov x29, sp\n");
  sb_appendf(sb, "  mov %s, x0\n", tape_base);
  sb_appendf(sb, "  mov %s, xzr\n", tape_index);
  for (size_t i = 0; i < ir->count; i++) {
    Operation op = ir->items[i];
    switch (op.op) {
    case OP_MOVE_RIGHT:
      // sb_appendf(sb, "  brk #0xf000\n");
      sb_appendf(sb, "  add %s, %s, %zu\n", tape_index, tape_index,
                 op.rep_or_jump_idx);
      sb_appendf(sb, "  cmp %s, %d\n", tape_index, TAPE_LEN);
      sb_appendf(sb, "  b.hs bf_overflow\n");
      break;
    case OP_MOVE_LEFT:
      // sb_appendf(sb, "  brk #0xf000\n");
      sb_appendf(sb, "  subs %s, %s, %zu\n", tape_index, tape_index,
                 op.rep_or_jump_idx);
      sb_appendf(sb, "  b.lo bf_underflow\n");
      break;

    case OP_INC:
      sb_appendf(sb, "  ldrb w0, [%s, %s]\n", tape_base, tape_index);
      sb_appendf(sb, "  add w1, w0, %zu\n", op.rep_or_jump_idx % 256);
      sb_appendf(sb, "  strb w1, [%s, %s]\n", tape_base, tape_index);
      break;
    case OP_DEC:
      sb_appendf(sb, "  ldrb w0, [%s, %s]\n", tape_base, tape_index);
      sb_appendf(sb, "  add w1, w0, %zu\n", 256 - (op.rep_or_jump_idx % 256));
      sb_appendf(sb, "  strb w1, [%s, %s]\n", tape_base, tape_index);
      break;

    case OP_OUTPUT:
      for (size_t j = 0; j < op.rep_or_jump_idx; j++) {
        sb_appendf(sb, "  ldrb w0, [%s, %s]\n", tape_base, tape_index);
        sb_appendf(sb, "  bl _putchar\n");
      }
      break;
    case OP_INPUT:
      TODO("input not yet valid we need to set up the terminal for this");
      for (size_t j = 0; j < op.rep_or_jump_idx; j++) {
        sb_appendf(sb, "  bl _getchar\n");
      }
      sb_appendf(sb, "  strb w0, [%s, %s]\n", tape_base, tape_index);
      break;

    case OP_JUMP_FOREWARD:
      sb_appendf(sb, "  ldrb w0, [%s, %s]\n", tape_base, tape_index);
      sb_appendf(sb, "  cbz w0, label_f_%zu\n", op.rep_or_jump_idx);
      sb_appendf(sb, "label_b_%zu:\n", i);
      break;
    case OP_JUMP_BACKWARD:
      sb_appendf(sb, "  ldrb w0, [%s, %s]\n", tape_base, tape_index);
      sb_appendf(sb, "  cbnz w0, label_b_%zu\n", op.rep_or_jump_idx);
      sb_appendf(sb, "label_f_%zu:\n", i);
      break;
    }
  }

  // Postlude return success
  sb_appendf(sb, "  mov x0, 1\n"); // true for success
  // Postlude
  sb_appendf(sb, "bf_postlude:\n");
  sb_appendf(sb, "  ldp x29, x30, [sp, 16]\n");
  sb_appendf(sb, "  ldp %s, %s, [sp]\n", tape_base, tape_index);
  sb_appendf(sb, "  add sp, sp, 32\n");
  sb_appendf(sb, "  ret\n");

  sb_appendf(sb, "bf_overflow:\n");
  sb_appendf(sb, "  adr x1, overflow_msg\n");
  sb_appendf(sb, "  b bf_exit_err\n");

  sb_appendf(sb, "bf_underflow:\n");
  sb_appendf(sb, "  adr x1, underflow_msg\n");

  sb_appendf(sb, "bf_exit_err:\n");
  sb_appendf(sb, "  mov x0, 2\n");
  sb_appendf(sb, "  bl _fprintf\n");
  sb_appendf(sb, "  mov x0, 0\n"); // false for error
  sb_appendf(sb, "  b bf_postlude\n");

  sb_appendf(sb, "overflow_msg:\n");
  sb_appendf(sb, "  .asciz \"OVERFLOW\\n\"\n");
  sb_appendf(sb, "underflow_msg:\n");
  sb_appendf(sb, "  .asciz \"UNDERFLOW\\n\"\n");
}

int main(int argc, char **argv) {
  char *p_name = *argv;
  shift(argv, argc);

  if (argc == 0 | argc > 1) {
    printf("Usage %s <filename.bf>\n", p_name);
    return -1;
  }

  String_Builder contents = {0};
  if (!read_entire_file(*argv, &contents)) {
    fprintf(stderr, "could not find file %s\n", *argv);
    return -1;
  }

  Intermediate ir = {0};

  if (!parse_bf(sb_to_sv(contents), &ir))
    return -1;

  String_Builder asm_buf = {0};
  append_freestanding_main(&asm_buf);
  translate_to_asm(&asm_buf, &ir);
  sb_append_null(&asm_buf);

  char *asm_file = "bf-main.s";
  FILE *file = fopen(asm_file, "w");
  if (file == NULL) {
    fprintf(stderr, "could not open %s\n", asm_file);
  }
  fprintf(file, "%s", asm_buf.items);
  fclose(file);

  Cmd cmd = {0};
  cmd_append(&cmd, "clang", "-arch", "arm64", "-c", "-o", "bf-main.o",
             "bf-main.s");
  if (!cmd_run(&cmd)) {
    fprintf(stderr, "could not compile to object\n");
    return -1;
  }
  cmd_append(&cmd, "clang", "-arch", "arm64", "-o", "bf-main", "bf-main.o");
  if (!cmd_run(&cmd)) {
    fprintf(stderr, "could not compile to executable\n");
    return -1;
  }
  return 0;

  if (!ir_interpret(&ir)) {
    fprintf(stderr, "Encountered underflow");
    return -1;
  }

  return 0;
}
