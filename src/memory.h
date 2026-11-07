#ifndef MEMORY_H
#define MEMORY_H

#include <setjmp.h>
#include <stddef.h>
#include <stdint.h>

extern uint8_t *TAPE_PTR;

extern sigjmp_buf jump_env;

#define SETUP_FAULT_JUMP() sigsetjmp(jump_env, 1)

void *copy_to_jit_region(void *code, size_t size);
void free_jit_region(void *ptr, size_t size);

void get_tape_region(size_t pages);
void free_tape_region(void);

#endif // MEMORY_H
