#set heading(numbering: "1.")

#set raw(syntaxes: "tape-ir.yaml")

// Display block code in a larger block
// with more padding.
#show raw.where(block: true): block.with(
  fill: luma(240),
  width: 100%,
  inset: 10pt,
  radius: 4pt,
)

#{
  set align(center)
  [
    #text(size: 22pt)[*Brainfuck Interpretation and Optimization*]

    Max Krummenacher
  ]
}
#pagebreak()

= Brainfuck

Brainfuck is an extremely simple language.
It operates on a tape of zero initialised unsigned bytes through a pointer to that tape.
The tape is bounded on the left and unbounded to the right,
although implementations usually use an upperbound for practical purposes.

The operations of brainfuck are shown in @bf-instructions.

#figure(
  table(
    columns: 3,
    align: (center, left, left),
    [*Instruction*], [*Effect*], [*C equivalent*],
    [`+`], [increase the value pointed at by one], [```c *t_ptr++;```],
    [`-`], [decrease the value pointed at by one], [```c *t_ptr--;```],
    [`>`], [increase the tape pointer by one], [```c t_ptr++;```],
    [`<`], [decrease the tape pointer by one], [```c t_ptr--;```],

    [`[`],
    [if the value pointed at is zero goto the instruction after matching the matching `]`],
    [```c while (*t_ptr != 0) {```],

    [`]`], [if the value pointed at is non zero goto the instruction after matching the matching `[`], [```c }```],

    [`.`], [output the byte at the tape pointer], [```c putchar(*t_ptr);```],
    [`,`], [get one byte of input and write it to the tape], [```c *t_ptr = getchar();```],
  ),
  caption: [Brainfuck Instructions],
)<bf-instructions>

== Comments

Any character which does not match any of the described operations is considered a comment.
Additionally, if a program starts with `[` anything until the matching `]` can be considered a comment.

= Intermediate representation

The brainfuck code is first translated into an intermediate representation (IR).
The instructions can operate on an immediate ofset to the tape pointer.
These offsets are indicated with `^`.

The operations of the IR are shown in @ir-instructions.

#figure(
  table(
    columns: 3,
    align: left,
    [*Operation*], [*Desrciption*], [*C equivalent*],
    [```tir ADD ^off imm```], [add `imm` to the value at tape pointer + `off`], [```c *(t_ptr + off) += imm;```],
    [```tir MOV imm```], [add `imm` to the tape pointer], [```c t_ptr += imm;```],

    [```tir LSTART```],
    [jump forward to the instruction after the matching `LEND` if the value at the tape pointer is zero],
    [```c while (*t_ptr != 0) {```],

    [```tir LEND```],
    [jump backward to the instruction after the matching `LSTART` if the value at the tape pointer is nonzero],
    [```c }```],

    [```tir PCH ^off```], [output the value at tape pointer + `off`], [```c putchar(*(t_ptr + off));```],
    [```tir GCH ^off```],
    [get a byte of input from and write it to tape pointer + `off`],
    [```c *(t_ptr + off) = getchar();```],

    [```tir MAD ^dst ^src imm```],
    [add the value calculate the value of],
    [```c *(t_ptr + dst) = *(t_ptr + src) * imm;```],

    [```tir CLR ^off```], [], [```c *(t_ptr + off) = 0;```],
  ),
  caption: [IR-Instructions],
)<ir-instructions>


= Canonicalization of Basic Blocks

A basic block is a segment of IR which does not contain any of `LSTART`, `LEND`.
The `MOV` operations are collected and put at the end of a basic block
and the offsets to the arithmetic and io operations are updated accordingly.

Since the order of some instructions does not matter we can sort them.

Sorting happens by insertion sort and runs until we en

```tir
ADD ^0  1
MAD ^0 ^1 4
```
is the same as

```tir
MAD ^0 ^1 4
ADD ^0  1
```

since it is just the transformation form
`[x, y]` to `[x + 1 + 4*y, y] = [x + 4*y + 1, y]`
But
```tir
ADD ^0  1
MAD ^1 ^0 4
```
`[x, y] -> [x+1, y+4*(x+1)]`
and
```tir
MAD ^1 ^0 4
ADD ^0  1
```
`[x, y] -> [x+1, y+4*x)]`
lead to different outcomes

Additionally the arithmetic operations are sorted by the offset to which they are applied,
such that arithmetic operations on the same value can be combined.


= Underflow and Overflow Handling
Underflows and overflows are handled by the operating system.
The tape region is a multiple of the page size of the system,
which allows me to allocate a page with no permission below and above the valid region.
Out of bounds access is then handeled by catching the segfault and is determined to be an underflow or overflow
by which of the guard regions is accessed.

== Limmitations

1. If the memory access by brainfuck is outside the bounds by more than a region an unknown error is returned,
  as I cannot be sure if the access was meant to access the tape.
2. The validity of a tape address is only checked on access to a value.
  The tape pointer is allowed to be invalid if it is never accessed.
  The programm `<> +` does not report an underflow even though after `<` it points outside the tape region.


= Translation to Aarch64 Assembly

The assembly for the one brainfuck file is translated to a single function:
```c void bf_assembled(void *tape_base, int *putchar(int), int *getchar(void));```

One register holds the tape pointer (`xtape`) throughout the whole program.

`xtape` is chosen to be x19, since it is a callee preserved register.

To call `getchar` and `putchar` the function pointers are passed to the function.
Simmilar to `xtape` callee preserved registers (`xgetc` and `xputc`) are used for them.


== Prelude
```asm
  ; allocate frame, save {fp, lr} at [sp, 0]
  stp fp, lr, [sp, -48]!

  ; point fp to {fp, lr} record
  mov fp, sp

  ; preserve callee preserved registers
  stp xtape, xunused, [sp, 16]
  stp xputc, xgetc, [sp, 32]

  ; move arguments into correct registers
  mov xtape, x0
  mov xputc, x1
  mov xgetc, x2
```

== Postlude
```asm
  ; restore callee preserved registers
  ldp xtape, xunused, [sp, 16]
  ldp xputc, xgetc, [sp, 32]

  ; restore {fp, lr} and deallocate
  ldp fp, lr, [sp], 48
  ret
```

*the following sections are stale*

== (`MOV`, n)
If n > 0
```asm
  add xtape, xtape, n
```
If n < 0
```asm
  sub xtape, xtape, -n
```

== (`ADD`, n, o)
If n != 0
```asm
  ldurb w0, [xtape, o]
  add w1, w0, (n % 256)
  sturb w1, [xtape, o]
```

== (`LSTART`, a)
```asm
  ldrb w0, [xtape]
  cbz w0, offset(a)
```

== (`LEND`, a)
```asm
  ldrb w0, [xtape]
  cbnz w0, offset(a)
```

== (`PCH`, n)
```asm
  ldrb w0, [xtape]
  blr xputc
```
repeated n times

== (`GCH`, n)
```asm
  blr xgetc
  strb w0, [xtape]
```
repeated n times

