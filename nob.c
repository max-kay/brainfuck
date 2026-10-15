#define NOB_STRIP_PREFIX
#define NOB_IMPLEMENTATION
#include "nob.h"

int main(int argc, char **argv) {
  GO_REBUILD_URSELF(argc, argv);
  Cmd cmd = {0};
  cmd_append(&cmd, "cc", "-Wall", "-Wextra", "-Werror", "-o", "main", "main.c");
  if (!cmd_run(&cmd))
    return 1;
  return 0;
}
