# Brainfuck Interpreter & JIT Compiler

This repo contains the code for a brainfuck interpreter.
It works on MacOs and Linux.

## How to build

Only a C compiler is necessary to run this code.

To bootstrap `nob` run `cc -o nob nob.c`.

Build the project using `./nob`, this also rebuilds `nob` if necessary.

Run tests with `./nob test`

## Usage

```
Usage bf-jit FLAGS <filename.bf>
   --interpret-ir   -i     interpret ir directly
   --stats          -s     print statistics
   --tape-len       -t     number of pages used for tape (xxxx on this machine)
   --emit-ir        -e     emit intermediate representation
   --test-output           output for tests
```


## Intermediate representation

The brainfuck code is first translated into an intermediate representation with the follwing op codes:

- (`ADD`, n) add n to the value at tape index
- (`MOV`, n) move tape poiter by n an halt execution on underflow or overflow
- (`GCH`, n) get a single char as input from stdin and write it to tape index n times
- (`PCH`, n) output the value at tape index to stdout n times
- (`LSTART`, a) jump forward to instruction a if the value at the tape index is zero
- (`LEND`, a) jump backward to instruction a if the value at the tape index is nonzero

## Translation to assembly

The assembly for the one brainfuck file is translated to a single function:
`Bf_Result bf_assembled(void *tape_base, int *putchar(int), int *getchar(void));`
The return value specifies if the function exited normaly (`true`) or if an out of bounds error occurred (`false`).

One register holds the tape pointer (`xtape`) throughout the whole program.

`xtape` is chosen to be x19, since it is a callee preserved register.

To call `getchar` and `putchar` the function pointers are passed to the function.
Simmilar to `xtape` callee preserved registers (`xgetc` and `xputc`) are used for them.


### Prelude
```asm
  sub sp, sp, 48
  stp x29, x30, [sp, 32]
  mov x29, sp
  stp xtape, xunused , [sp, 16]
  stp xgetc, xputc, [sp]
  mov xindx, xzr
  mov xbase, x0
  mov xputc, x1
  mov xgetc, x2
```

### Postlude
```asm
  ldp x29, x30, [sp, 32]
  ldp xtape, xunused, [sp, 16]
  ldp xputc, xgetc, [sp]
  add sp, sp, 48
  ret
```

### (`MOV`, n)
If n > 0
```asm
  add xtape, xtape, n
```
If n < 0
```asm
  sub xtape, xtape, -n
```

### (`ADD`, n)
If n != 0
```asm
  ldrb w0, [xtape]
  add w1, w0, (n % 256)
  strb w1, [xtape]
```

### (`LSTART`, a)
```asm
  ldrb w0, [xtape]
  cbz w0, offset(a)
```

### (`LEND`, a)
```asm
  ldrb w0, [xtape]
  cbnz w0, offset(a)
```

### (`PCH`, n)
```asm
  ldrb w0, [xtape]
  blr xputc
```
repeated n times

### (`GCH`, n)
```asm
  blr xgetc
  strb w0, [xtape]
```
repeated n times

## Underflow and Overflow handling
Underflows and overflows are handled by the operating system.
The tape region is a multiple of the page size of the system,
which allows me to allocate a page with no permission below and above the valid region.
Out of bounds access is then handeled by catching the segfault and is determined to be an underflow or overflow
by which of the guard regions is accessed.

### Limmitations

1. If the memory access by brainfuck is outside the bounds by more than a region an unknown error is returned,
  as I cannot be sure if the access was meant to access the tape.
2. The validity of a tape address is only checked on access to a value.
  The tape pointer is allowed to be invalid if it is never accessed.
  The programm `<> +` does not report an underflow even though after `<` it points outside the tape region.



