# Brainfuck interpreter & compiler

This repo contains the code for a brainfuck interpreter.

## How to build

Only a C compiler is necessary to run this code.

To bootstrap `nob` run `cc -o nob nob.c`.

Build the project using `./nob`, this also rebuilds `nob` if necessary.

Run tests with `./nob test`

## Usage

```
Usage bf-jit FLAGS <filename.bf>
   --interprete-ir  -i     interpret ir directly
   --stats          -s     print statistics
   --tape-len       -t     set lenght of tape (multiples of 2^12 = 4'096)
   --emit-ir        -e     emit intermediate representation
   --test-output           output for tests
```


## Intermediate representation

The brainfuck code is first translated into an intermediate representation with the follwing op codes:

- (`A`, n) *a*dd n to the value at tape index
- (`S`, n) *s*ubtract n from the value at tape index
- (`R`, n) move *r*ight by n and halt execution on overflow
- (`L`, n) move *l*eft by n and halt execution on underflow
- (`F`, a) jump *f*oreward to instruction a+1 if the value at the tape index is zero
- (`B`, a) jump *b*ackeard to instruction a+1 if the value at the tape index is nonzero
- (`I`, n) get a single char as *i*nput from stdin and write it to tape index n times
- (`O`, n) *o*utput the value at tape index to stdout n times

## Translation to assembly

The assembly for the one brainfuck file is translated to a single function:
`bool bf_assembled(void *tape_base);`
The return value specifies if the function exited normaly (`true`) or if an out of bounds error occurred (`false`).

One register holds the tape base pointer (`xbase`) and one holds the tape index (`xindx`) throughout the whole program.

Since x19 to x28 are callee preserved registers, `xbase` and `xindx` will use one of those registers.
Like this they never need to be preserved.

### Prelude
```asm
.globl _bf_assembled
_bf_assembled:
  sub sp, sp, 32
  stp x29, x30, [sp, 16]
  stp xbase, xindx, [sp]
  mov x29, sp
  mov xbase, x0
  mov xindx, xzr
```

### Postlude
```asm
  mov x0, 1
bf_postlude:
  ldp x29, x30, [sp, 16]
  ldp xbase, xindx, [sp]
  add sp, sp, 32
  ret

bf_overflow:
  adr x1, overflow_msg
  b bf_exit_err

bf_underflow:
  adr x1, underflow_msg

bf_exit_err:
  mov x0, 2
  bl _fprintf
  mov x0, 0
  b bf_postlude

overflow_msg:
  .asciz "OVERFLOW\n"
underflow_msg:
  .asciz "UNDERFLOW\n"
```

### (`R`, n)
```asm
  add xindx, xindx, n
  cmp xindx, TAPE_LEN
  b.hs bf_overflow
```

### (`L`, n)
```asm
  subs xindx, xindx, n
  b.lo bf_underflow
```

### (`A`, n)
```asm
  ldrb w0, [xbase, xindx]
  add w1, w0, (n % 256)
  strb w1, [xbase, xindx]
```

### (`S`, n)
```asm
  ldrb w0, [xbase, xindx]
  add w1, w0, (256 - (n % 256))
  strb w1, [xbase, xindx]
```

### (`F`, a)
```asm
  ldrb w0, [xbase, xindx]
  cbz w0, offset(a)
```

### (`B`, a)
```asm
  ldrb w0, [xbase, xindx]
  cbnz w0, offset(a)
```

### (`O`, n)
```asm
  ldrb w0, [xbase, xindx]
  bl _putchar
```
repeated n times

### (`I`, n)
```asm
  bl _getchar
  strb w0, [xbase, xindx]
```
repeated n times

