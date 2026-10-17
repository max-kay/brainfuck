# Notes

## ARM instructions

https://www.scs.stanford.edu/~zyedidia/arm64/index.html

## additional for JIT macOS / Apple Silicon specifics

Allocate with mmap(... MAP_JIT), call pthread_jit_write_protect_np(0) before writing and (1) after.

Call sys_icache_invalidate(buf, len) (or __builtin___clear_cache) before executing. Without it you’ll get random crashes.
Branch offsets are only known after you emit the loop body,
so record the jmp instruction index and patch it when you hit the matching jmpback. Your update_branch design supports that.

## optimization thought
be carefull about wrapped w0 when optimizing away reads


