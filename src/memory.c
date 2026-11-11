#include <setjmp.h>
#include <sys/mman.h>
#include <unistd.h>

#ifdef __APPLE__
#include <libkern/OSCacheControl.h>
#include <pthread.h>
#else  // __APPLE__
#endif // __APPLE__

#include "common.h"
#include "memory.h"

uint8_t *TAPE_PTR = NULL;
static uint8_t *LOW_GUARD = NULL;
static uint8_t *HIGH_GUARD = NULL;
static uint8_t *HIGHEST_GUARD = NULL;

#ifndef NO_BOUNDS_CHECK

#include <signal.h>

static struct sigaction OLD_SEGV;
#ifdef __APPLE__
static struct sigaction OLD_BUS;
#endif

sigjmp_buf jump_env;

static void on_fault(int sig, siginfo_t *info, void *uctx) {
  UNUSED(sig);
  UNUSED(uctx);
  if (TAPE_PTR == NULL) {
    RESULT = Bf_R_Unknown;
    siglongjmp(jump_env, 1);
  }
  uint8_t *addr = (uint8_t *)info->si_addr;
  if (LOW_GUARD <= addr && addr < TAPE_PTR) {
    RESULT = Bf_R_Underflow;
  } else if (HIGH_GUARD <= addr && addr < HIGHEST_GUARD) {
    RESULT = Bf_R_Overflow;
  } else {
    RESULT = Bf_R_Unknown;
  }
  siglongjmp(jump_env, 1);
}

#endif // NO_BOUNDS_CHECK

void get_tape_region(size_t pages) {
  size_t page_size = getpagesize();
  uint8_t *base = mmap(NULL, (2 + pages) * page_size, PROT_NONE,
                       MAP_PRIVATE | MAP_ANON, -1, 0);
  NOB_ASSERT(base != MAP_FAILED);
  NOB_ASSERT(mprotect(base + page_size, pages * page_size,
                      PROT_READ | PROT_WRITE) == 0);

#ifndef NO_BOUNDS_CHECK
  struct sigaction sa = {0};
  sa.sa_sigaction = on_fault;
  sa.sa_flags = SA_SIGINFO;
  sigemptyset(&sa.sa_mask);
  sigaction(SIGSEGV, &sa, &OLD_SEGV);
#ifdef __APPLE__
  sigaction(SIGBUS, &sa, &OLD_BUS);
#endif
#endif // NO_BOUNDS_CHECK

  LOW_GUARD = base;
  HIGH_GUARD = base + (1 + pages) * page_size;
  HIGHEST_GUARD = base + (2 + pages) * page_size;
  TAPE_PTR = base + page_size;
}

void free_tape_region(void) {
#ifndef NO_BOUNDS_CHECK
  sigaction(SIGSEGV, &OLD_SEGV, NULL);
#ifdef __APPLE__
  sigaction(SIGBUS, &OLD_BUS, NULL);
#endif
#endif // NO_BOUNDS_CHECK

  NOB_ASSERT(munmap(LOW_GUARD, HIGHEST_GUARD - LOW_GUARD) == 0);
  LOW_GUARD = NULL;
  TAPE_PTR = NULL;
  HIGH_GUARD = NULL;
  HIGHEST_GUARD = NULL;
}

void *copy_to_jit_region(void *code, size_t size) {
#ifdef __APPLE__
  void *code_ptr = mmap(NULL, size, PROT_READ | PROT_WRITE | PROT_EXEC,
                        MAP_PRIVATE | MAP_ANON | MAP_JIT, -1, 0);
#else  // __APPLE__
  void *code_ptr = mmap(NULL, size, PROT_READ | PROT_WRITE | PROT_EXEC,
                        MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
#endif // __APPLE__
  NOB_ASSERT(code_ptr != MAP_FAILED);

#ifdef __APPLE__
  pthread_jit_write_protect_np(0);
#endif // __APPLE__

  memcpy(code_ptr, code, size);

#ifdef __APPLE__
  pthread_jit_write_protect_np(1);
  sys_icache_invalidate(code_ptr, size);
#else  // __APPLE__
  __builtin___clear_cache(code_ptr, code_ptr + size);
#endif // __APPLE__
  return code_ptr;
}

void free_jit_region(void *ptr, size_t size) {
  NOB_ASSERT(munmap(ptr, size) == 0);
}
