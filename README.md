# Brainfuck Interpreter & JIT Compiler

This repo contains the code for a brainfuck interpreter.
It works on MacOs and Linux.

## How to build

Only a C compiler is necessary to run this code.

To bootstrap `nob` run `cc -o nob nob.c`.

Build the project using `./nob`, this also rebuilds `nob` if necessary.

Run tests with `./nob test`.

To compile with debug symbols run `./nob debug`.

## Usage

```
Usage bf-jit FLAGS <filename.bf>
   --interpret-ir   -i     interpret ir directly
   --optimize       -o     optimize code
   --stats          -s     print statistics
   --tape-len              number of pages used for tape (xxx on this machine)
   --emit-ir        -e     emit intermediate representation
   --test-output           output for tests
   --debug          -d     print debug output
```


