#set heading(numbering: "1.")

#set raw(syntaxes: ("tape-ir.sublime-syntax", "aarch64.sublime-syntax"))

#let tape(
  cells,
  ptr: 0,
  size: 4em,
  edge: 3.4em,
  accent: rgb("#e8590c"),
  caret: true,
) = {
  let n = cells.len()
  let gray = luma(210)

  // FIXME: corner radius changes with edge
  let left-edge = rect(
    width: 90%,
    height: 90%,
    radius: (top-right: 10%, bottom-right: 10%, rest: 0pt),
    fill: gradient.linear(white, gray, angle: 0deg),
  )
  let right-edge = rect(
    width: 90%,
    height: 90%,
    radius: (top-left: 10%, bottom-left: 10%, rest: 0pt),
    fill: gradient.linear(gray, white, angle: 0deg),
  )

  let cell(c, i) = {
    let active = i == ptr
    rect(
      width: 90%,
      height: 90%,
      fill: gray,
      radius: 10%,
      stroke: if active { accent + 1.4pt } else { none },
      align(center + horizon, c),
    )
  }

  // pointer row: a caret under the active cell
  let carets = range(n + 2).map(j => {
    if caret and j == ptr + 1 {
      align(center + top, text(fill: accent, size: 0.9em)[▲])
    } else { [] }
  })

  align(center, grid(
    columns: (edge, ..((size,) * n), edge),
    rows: (size, auto),
    row-gutter: 0.15em,
    left-edge,
    ..cells.enumerate().map(((i, c)) => cell(c, i)),
    right-edge,
    ..carets,
  ))
}

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

=== Setting a value equal to zero
#tape(($x$,), ptr: 0)
```
[-]
```
#tape(($0$,), ptr: 0)

=== Addition
#tape(($x$, $y$), ptr: 0)
```
[>+<-]
```
#tape(($0$, $y+x$), ptr: 0)

=== Addition Preserving original value
#tape(($x$, $y$, $0$), ptr: 0)

```bf
[>+>+<<-]
>>
```

#tape(($0$, $y+x$, $x$), ptr: 2)

```bf
[<<+>>-]
<<
```

#tape(($x$, $y+x$, $0$), ptr: 0)
=== Multiplication by Constant
#tape(($x$, $y$), ptr: 0)
```
[>+++<-]
```
#tape(($0$, $y + 3x$), ptr: 0)


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
      `dst` is not allowed to eqaul `src`.
    ],
    [```c
    *(t_ptr + dst) =
        *(t_ptr + src) * imm;
    ```],

    [```tir CLR ^off```], [set the value at tape pointer + `off` to zero], [```c *(t_ptr + off) = 0;```],
    [```tir NOP ```], [do nothing], align(center)[-],
  ),
  caption: [IR-Instructions],
)<ir-instructions>


= Canonicalization of Basic Blocks

A basic block is a segment of IR which does not contain any of `LSTART`, `LEND`.
Canonicalization of such blocks happends in three stages.
- collecting the `MOV`-operations into one.
- sorting the operations
- eliminating and combining instructions on the same offset.
== Collecting `MOV`
The `MOV` operations are collected and put at the end of a basic block
and the offsets to the arithmetic and io operations are updated accordingly.

== Sorting Operations
Since the order of some instructions does not matter we can sort them.

```tir
ADD ^0  1
MAD ^0 ^1  4
```
is the same as

```tir
MAD ^0 ^1  4
ADD ^0  1
```

since it is just the transformation from `[x, y]` to `[x + 1 + 4*y, y] = [x + 4*y + 1, y]`

But
```tir
ADD ^0  1
MAD ^1 ^0  4
```
`{..., x, y] -> [x+1, y + 4 * (x+1)]`
and
```tir
MAD ^1 ^0  4
ADD ^0  1
```
`[x, y] -> [x+1, y + 4*x]`
lead to different outcomes

== Combining Operations

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

`xtape` is chosen to be x19, since it is a callee preserved register.

To call `getchar` and `putchar` the function pointers are passed to the function.
Simmilar to `xtape` callee preserved registers (`xgetc` and `xputc`) are used for them.


== Prelude
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

== Postlude
```aarch64
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

