#include <setjmp.h>
#include <signal.h>
#include <sys/mman.h>
#include <unistd.h>

#if defined(__APPLE__)
#include <libkern/OSCacheControl.h>
#include <pthread.h>
#else  // defined(__APPLE__)
#endif // defined(__APPLE__)

#include "common.h"
#include "memory.h"

#include "nob.h"

uint8_t *TAPE_PTR = NULL;
static uint8_t *LOW_GUARD = NULL;
static uint8_t *HIGH_GUARD = NULL;
static uint8_t *HIGHEST_GUARD = NULL;

sigjmp_buf jump_env;

#if defined(__APPLE__)
void *get_jit_region(size_t size) {
  void *ptr = mmap(NULL, size, PROT_READ | PROT_WRITE | PROT_EXEC,
                   MAP_PRIVATE | MAP_ANON | MAP_JIT, -1, 0);
  NOB_ASSERT(ptr != MAP_FAILED);
  return ptr;
}

void free_jit_region(void *ptr, size_t size) {
  NOB_ASSERT(munmap(ptr, size) == 0);
}

void on_fault(int sig, siginfo_t *info, void *uctx) {

  UNUSED(sig);
  UNUSED(uctx);
  if (TAPE_PTR == NULL) {
    RESULT = Bf_R_Unknown;
    siglongjmp((int *)jump_env, 1);
  }
  uint8_t *addr = (uint8_t *)info->si_addr;
  if (LOW_GUARD <= addr && addr < TAPE_PTR) {
    RESULT = Bf_R_Underflow;
  } else if (HIGH_GUARD <= addr && addr < HIGHEST_GUARD) {
    RESULT = Bf_R_Overflow;
  } else {
    RESULT = Bf_R_Unknown;
  }
  siglongjmp((int *)jump_env, 1);
}

void get_tape_region(size_t pages) {
  size_t page_size = getpagesize();
  uint8_t *base = mmap(NULL, (2 + pages) * page_size, PROT_NONE,
                       MAP_PRIVATE | MAP_ANON, -1, 0);
  NOB_ASSERT(base != MAP_FAILED);
  mprotect(base + page_size, pages * page_size, PROT_READ | PROT_WRITE);

  struct sigaction sa = {0};
  sa.sa_sigaction = on_fault;
  sa.sa_flags = SA_SIGINFO;
  sigemptyset(&sa.sa_mask);
  sigaction(SIGSEGV, &sa, NULL);
  sigaction(SIGBUS, &sa, NULL); // macOS often delivers SIGBUS for these

  LOW_GUARD = base;
  HIGH_GUARD = base + (1 + pages) * page_size;
  HIGHEST_GUARD = base + (2 + pages) * page_size;
  TAPE_PTR = base + page_size;
}

void free_tape_region(void) {
  NOB_ASSERT(munmap(LOW_GUARD, HIGHEST_GUARD - LOW_GUARD) == 0);
  LOW_GUARD = NULL;
  TAPE_PTR = NULL;
  HIGH_GUARD = NULL;
  HIGHEST_GUARD = NULL;
}

#else // defined(__APPLE__)
void *get_jit_region(size_t size) {
  void *ptr = mmap(NULL, size, PROT_READ | PROT_WRITE | PROT_EXEC,
                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  NOB_ASSERT(ptr != MAP_FAILED);
  return ptr;
}

void free_jit_region(void *ptr, size_t size) {
  NOB_ASSERT(munmap(ptr, size) == 0);
}

void on_fault(int sig, siginfo_t *info, void *uctx) {
  UNUSED(sig);
  UNUSED(uctx);
  if (TAPE_PTR == NULL) {
    RESULT = Bf_R_Unknown;
    siglongjmp(jump_env, 1);
  }
  uint8_t *addr = (uint8_t *)info->si_addr;
  if (LOW_GUARD <= addr && addr < TAPE_PTR)
    RESULT = Bf_R_Underflow;
  else if (HIGH_GUARD <= addr && addr < HIGHEST_GUARD)
    RESULT = Bf_R_Overflow;
  else
    RESULT = Bf_R_Unknown;
  siglongjmp(jump_env, 1);
}

void get_tape_region(size_t pages) {
  size_t page_size = getpagesize();
  uint8_t *base = mmap(NULL, (2 + pages) * page_size, PROT_NONE,
                       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  NOB_ASSERT(base != MAP_FAILED);
  mprotect(base + page_size, pages * page_size, PROT_READ | PROT_WRITE);

  struct sigaction sa = {0};
  sa.sa_sigaction = on_fault;
  sa.sa_flags = SA_SIGINFO;
  sigemptyset(&sa.sa_mask);
  sigaction(SIGSEGV, &sa, NULL);

  LOW_GUARD = base;
  HIGH_GUARD = base + (1 + pages) * page_size;
  HIGHEST_GUARD = base + (2 + pages) * page_size;
  TAPE_PTR = base + page_size;
}

void free_tape_region(void) {
  NOB_ASSERT(munmap(LOW_GUARD, HIGHEST_GUARD - LOW_GUARD) == 0);
  LOW_GUARD = NULL;
  TAPE_PTR = NULL;
  HIGH_GUARD = NULL;
  HIGHEST_GUARD = NULL;
}

#endif // defined(__APPLE__)

void *copy_to_jit_region(void *code, size_t size) {
  void *code_ptr = get_jit_region(size);
#if defined(__APPLE__)
  pthread_jit_write_protect_np(0);
#endif // defined(__APPLE__)

  memcpy(code_ptr, code, size);

#if defined(__APPLE__)
  pthread_jit_write_protect_np(1);
  sys_icache_invalidate(code_ptr, size);
#else  // defined(__APPLE__)
  __builtin___clear_cache(code_ptr, code_ptr + size);
#endif // defined(__APPLE__)
  return code_ptr;
}
