# Notes

## Possible Improvements
on empty command use stdin to get the program
on nonreadable run cli arg command

## ARM instructions
https://www.scs.stanford.edu/~zyedidia/arm64/index.html

## optimization thoughts
be carefull about wrapped w0 when optimizing away reads
syscalls for putc and getc

example optimizations
http://calmerthanyouare.org/2015/01/07/optimizing-brainfuck.html
(no https)

with explicit bound checks
Benchmark 1: ./bf-jit-old examples/mandelbrot.bf (10 runs) commit 759d9f1
                mean     ±       σ          min     …     max
  Wall Time    565.0 ms  ±     2.6 ms     561.6 ms  …   568.9 ms
  Memory         1.8 MiB ±     0.0 MiB      1.8 MiB …     1.8 MiB

with bound checks by operating system
Benchmark 2: ./bf-jit-new examples/mandelbrot.bf (10 runs) commit d32501d
                mean     ±       σ          min     …     max
  Wall Time    538.6 ms  ±     3.1 ms     535.1 ms  …   543.6 ms            -4.7%
  Memory         1.7 MiB ±     0.0 MiB      1.6 MiB …     1.7 MiB           -8.2%

## extensions
https://esolangs.org/wiki/Extended_Brainfuck
