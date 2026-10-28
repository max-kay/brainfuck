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

its surprising that the change to using the tape ptr directly and having bound checks through os made the code slower

## extensions
https://esolangs.org/wiki/Extended_Brainfuck
