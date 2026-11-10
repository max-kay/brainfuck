#import "bf-lib.typ": tape, transform, transform_vertical
#set heading(numbering: "1.")
#set page(numbering: "1");

#set raw(syntaxes: ("tape-ir.sublime-syntax", "aarch64.sublime-syntax", "brainfuck.sublime-syntax"))

// Display block code in a larger block
// with more padding.
#show raw.where(block: true): block.with(
  fill: luma(240),
  // width: 100%,
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

#outline()

#pagebreak()

= Brainfuck

Brainfuck is an minimalist language operating on a tape of zero initialised unsigned bytes through a pointer to that tape.
The tape is bounded on the left and unbounded to the right,
although implementations usually use an upperbound for practical purposes.

The operations of brainfuck are shown in @bf-instructions.

#figure(
  table(
    columns: 3,
    align: (center, left, left),
    [*Instruction*], [*Effect*], [*C equivalent*],
    [`+`], [increase the value pointed at by one], [```c (*t_ptr)++;```],
    [`-`], [decrease the value pointed at by one], [```c (*t_ptr)--;```],
    [`>`], [increase the tape pointer by one], [```c t_ptr++;```],
    [`<`], [decrease the tape pointer by one], [```c t_ptr--;```],

    [`[`],
    [if the value pointed at is zero goto the instruction after the matching `]`],
    [```c while (*t_ptr != 0) {```],

    [`]`], [if the value pointed at is non zero goto the instruction after the matching `[`], [```c }```],

    [`.`], [output the byte at the tape pointer], [```c putchar(*t_ptr);```],
    [`,`], [get one byte of input and write it to the tape], [```c *t_ptr = getchar();```],
  ),
  caption: [Brainfuck Instructions],
)<bf-instructions>

== Comments

Any character which does not match any of the described operations is considered a comment.
Additionally, if a program starts with `[` anything until the matching `]` can be considered a comment.
This second type of comment can use any character, including operations,
with the only limmitation that `[` and `]` need to be matched.
Since the tape is zero initialized any code written here is skipped.

== Examples

#figure(
  transform(
    tape(ptr: 0, $x$),
    ```bf
    [-]
    ```,
    tape(ptr: 0, $0$),
  ),
  caption: [Clear loop],
)<ex_clr_loop>

@ex_clr_loop shows the transformation to clear the value at the current tape pointer.
This loop decrements the value at the tape pointer by one until zero is reached. Note that this loop runs exactly $x$ times.

#figure(
  transform(
    tape(ptr: 0, $x$, $y$),
    ```bf
    [>+<-]
    ```,
    tape(ptr: 0, $0$, $y+x$),
  ),
  caption: [Addition],
)<ex_add>

The loop shown in @ex_add adds the value at the tape pointer to the next value on the tape pointer.
This works because the loop above runs exactly $x$ times and increases the value by one each time.

#figure(
  transform(
    tape(ptr: 0, $x$, $y$),
    ```bf
    [>+++<-]
    ```,
    tape(ptr: 0, $0$, $y + 3x$),
  ),
  caption: [Multiplication by Constant],
)<ex_mul_const>

The loop shown in @ex_mul_const works in a simmilar fashion to the example in @ex_add,
with the difference that the increment operation is run three times which multiplies the value at offset 0
by three.
This can be generalized to a multiplication by an arbitrary constant.

#figure(
  transform_vertical(
    tape(ptr: 0, $x$, $y$, $0$),
    ```bf
    [>+>+<<-]
    >>
    ```,
    tape(ptr: 2, $0$, $y+x$, $x$),
    ```
    [<<+>>-]
    <<
    ```,
    tape(ptr: 0, $x$, $y+x$, $0$),
  ),
  caption: [Addition Preserving Original value],
)<ex_add_preserve>
The loop shown in @ex_add adds the value at the tape pointer to the next value on the tape pointer.

#figure(
  transform_vertical(
    tape(ptr: 0, $x$, $y$, $0$, $0$),
    ```bf
    [
      >
      [>+>+<<-]
      >
      [<+>-]
      <<-
    ]
    ```,
    tape(ptr: 0, $0$, $y$, $0$, $x y$),
  ),
  caption: [Dynamic Multiplication],
)


= Intermediate representation

The brainfuck code is first translated into an intermediate representation (IR).
The instructions can operate on an immediate offset to the tape pointer.
These offsets are indicated with `^`.

The operations of the IR are shown in @ir-instructions.

#figure(
  table(
    columns: 3,
    align: left,
    [*Operation*], [*Description*], [*C equivalent*],
    [```tir ADD ^off imm```], [add `imm` to the value at tape pointer + `off`], [```c *(t_ptr + off) += imm;```],
    [```tir MOV imm```], [move the tape pointer by `imm`], [```c t_ptr += imm;```],

    [```tir LSTART```],
    [jump forward to the instruction after the matching `LEND` if the value at the tape pointer is zero],
    [```c while (*t_ptr != 0) {```],

    [```tir LEND```],
    [jump backward to the instruction after the matching `LSTART` if the value at the tape pointer is non-zero],
    [```c }```],

    [```tir PCH ^off```], [output the value at tape pointer + `off`], [```c putchar(*(t_ptr + off));```],
    [```tir GCH ^off```],
    [get a byte of input and write it to tape pointer + `off`],
    [```c *(t_ptr + off) = getchar();```],

    [```tir MAD ^dst ^src imm```],
    [multiply the value at tape pointer + `src` by `imm` and add the result to the value at tape pointer + `dst`
      // TODO: case dst == src
    ],
    [```c *(t_ptr + dst) +=```\ #h(3em) ```c *(t_ptr + src) * imm;```],

    [```tir MUL ^dst ^src1 ^src2 imm```],
    [multiply the value at tape pointer + `src1` by the value at tape pointer +`src2` and by `imm`
      and add the result to the value at tape pointer + `dst`
    ],
    [```c *(t_ptr + dst) +=```\ #h(3em) ```c *(t_ptr + src1)```\ #h(3em) ```c * *(t_ptr + src2)```\ #h(3em) ```c * imm;```],

    [```tir NEG ^off```],
    [negate the value at tape pointer + `off`],
    [```c *(t_ptr + off) = ```\ #h(3em) ```c -*(t_ptr + off);```],

    [```tir CLR ^off```], [set the value at tape pointer + `off` to zero], [```c *(t_ptr + off) = 0;```],
    [```tir NOP ```], [do nothing], align(center)[-],
  ),
  caption: [IR-Instructions],
)<ir-instructions>

#figure(
  table(
    columns: 2,
    align: (center, left),
    [*BF-Instruction*], [*IR-Instruction*],
    [`+`], [```tir ADD     ^0   1```],
    [`-`], [```tir ADD     ^0 255```],
    [`>`], [```tir MOV      1```],
    [`<`], [```tir MOV     -1```],

    [`[`], [```tir LSTART ```],

    [`]`], [```tir LEND``` ],

    [`.`], [```tir PCH     ^0```],
    [`,`], [```tir GCH     ^0```],
  ),
  caption: [Translation to IR],
)



= Canonicalization of Basic Blocks

A basic block is a segment of IR which does not contain any of `LSTART`, `LEND`.
Canonicalization of such blocks happends in three stages.
- collecting the `MOV`-operations into one.
- sorting the operations
- eliminating and combining operations.

== Collecting `MOV`
The `MOV` operations are collected and put at the end of a basic block
and the offsets to the arithmetic and io operations are updated accordingly.

#figure(
  grid(
    gutter: 1em,
    columns: 2,
    ```tir
    ADD ^0 1
    MOV  1
    ADD ^0 1
    CLR ^1
    ```,
    ```tir
    ADD ^0 1
    ADD ^1 1
    CLR ^2
    MOV  1
    ```,
  ),
  caption: [],
)<canon_mov_collect>


== Sorting Operations
To ensure correct sorting of the instructions bubble sort with an additional commutativity test is used.

The special case of the commutativity of `MOV` can be ignored,
since the `MOV` instructions were already moved to the end of the basic block in the last step of the canonicalization.
The IO-operations `PCH` and `GCH` never commute.

The commutativity of the other instructions can be determined from their foot print.

== Combining Operations



= Loop elimination


```tir
LSTART
ADD ^0 -1
ADD ^o  n
LEND
```

```tir
MAD ^o ^0 n
CLR ^0
```
for `o`$!=0$

```tir
LSTART
ADD ^0  1
ADD ^o  n
LEND
```
```tir
NEG ^o
MAD ^o ^0 n
CLR ^0
```

```tir
LSTART
ADD ^0 -1
MAD ^d ^s n
LEND
```
if $0!= d != s != 0$
$ x'_d = x_d + n x_s + ... + n x_s = x_d + x_0 + x_0 n x_s $



```tir
MUL ^d ^s ^0 n
CLR ^0
```

```tir
LSTART
ADD ^0 -1
CLR ^o
LEND
```


= Underflow and Overflow Handling
Underflows and overflows are handled by the operating system.
The tape region is a multiple of the page size of the system,
which allows me to allocate a page with no permission below and above the valid region.
Out of bounds access is then handeled by catching the segfault and is determined to be an underflow or overflow
by which of the guard regions is accessed.

== Limmitations

If the memory access by brainfuck is outside the bounds by more than a region an unknown error is returned,
as I cannot be sure if the access was meant to access the tape.

The validity of a tape address is only checked on access to a value.
The tape pointer is allowed to be invalid if it is never accessed.
The programm `<> +` does not report an underflow even though after `<` it points outside the tape region.


= Translation to Aarch64 Assembly

The assembly for the one brainfuck file is translated to a single function:
```c void bf_assembled(void *tape_base, int *putchar(int), int *getchar(void));```

One register holds the tape pointer (`xtape`) throughout the whole program.

`xtape` is chosen to be x19, since it is a callee saved register.

To call `getchar` and `putchar` the function pointers are passed to the function.
Simmilar to `xtape` callee saved registers (`xgetc` and `xputc`) are used for them.


== Prologue
The following prologue is the header to the function body.
It sets up the registers and the stack for the execution of the program.

```aarch64
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

== Epilogue
The following epilogue is the footer to the function body.
It restores the registers and deallocates the stack.
```aarch64
  ; restore callee saved registers
  ldp xtape, xunused, [sp, 16]
  ldp xputc, xgetc, [sp, 32]

  ; restore {fp, lr} and deallocate
  ldp fp, lr, [sp], 48
  ret
```

*the following sections are stale*

== (`MOV`, n)
If n > 0
```aarch64
  add xtape, xtape, n
```
If n < 0
```aarch64
  sub xtape, xtape, -n
```

== (`ADD`, n, o)
If n != 0
```aarch64
  ldurb w0, [xtape, o]
  add w1, w0, (n % 256)
  sturb w1, [xtape, o]
```

== (`LSTART`, a)
```aarch64
  ldrb w0, [xtape]
  cbz w0, offset(a)
```

== (`LEND`, a)
```aarch64
  ldrb w0, [xtape]
  cbnz w0, offset(a)
```

== (`PCH`, n)
```aarch64
  ldrb w0, [xtape]
  blr xputc
```
repeated n times

== (`GCH`, n)
```aarch64
  blr xgetc
  strb w0, [xtape]
```
repeated n times

