#include <string.h>
#define NOB_STRIP_PREFIX
#define NOB_IMPLEMENTATION
#include "nob.h"

#define TEST_PATH "tests"

static Cmd CMD = {0};

bool run_test(String_View test_name) {
  size_t alloc_mark = temp_save();
  String_Builder expected = {0};
  String_Builder output = {0};

  char *path_expected =
      temp_sprintf("%s/" SV_Fmt ".expected", TEST_PATH, SV_Arg(test_name));
  if (!file_exists(path_expected)) {
    nob_log(ERROR, "could not find expected output for test " SV_Fmt,
            SV_Arg(test_name));
    goto error;
  }

  if (!read_entire_file(path_expected, &expected)) {
    goto error;
  };

  bool should_fail = false;
  if (sv_starts_with(sb_to_sv(expected), SVLIT("SHOULD FAIL")))
    should_fail = true;

  char *input_path =
      temp_sprintf("%s/" SV_Fmt ".in", TEST_PATH, SV_Arg(test_name));
  bool has_input = file_exists(input_path);

  cmd_append(&CMD, "./main",
             temp_sprintf("%s/" SV_Fmt ".bf", TEST_PATH, SV_Arg(test_name)));

  const char *stdout_path =
      temp_sprintf("/tmp/bf_test_" SV_Fmt ".stdout", SV_Arg(test_name));
  bool cmd_failed;
  if (has_input) {
    cmd_failed =
        !cmd_run(&CMD, .stdin_path = input_path, .stdout_path = stdout_path,
                 .stderr_path = "/dev/null");
  } else {
    cmd_failed =
        !cmd_run(&CMD, .stdout_path = stdout_path, .stderr_path = "/dev/null");
  }

  if (!read_entire_file(stdout_path, &output)) {
    nob_log(WARNING, "could not read output of test " SV_Fmt,
            SV_Arg(test_name));
  }

  if (cmd_failed) {
    if (should_fail)
      goto ok;
    goto error;
  }

  if (should_fail) {
    goto error;
  }

  if (expected.count != output.count) {
    goto error;
  }
  if (strncmp(output.items, expected.items, output.count) != 0)
    goto error;

ok:
  temp_rewind(alloc_mark);
  da_free(expected);
  da_free(output);
  return true;
error:
  temp_rewind(alloc_mark);
  da_free(expected);
  da_free(output);
  return false;
}

bool run_all_tests() {
  nob_log(INFO, "running tests");
  File_Paths paths = {0};
  read_entire_dir(TEST_PATH, &paths);

  bool had_no_failure = true;

  for (size_t i = 0; i < paths.count; i++) {
    String_View name = sv_from_cstr(paths.items[i]);
    if (!sv_chop_suffix(&name, SVLIT(".bf")))
      continue;
    if (!run_test(name)) {
      nob_log(ERROR, "test " SV_Fmt " failed!", SV_Arg(name));
      had_no_failure = false;
    }
  }

  da_free(paths);
  return had_no_failure;
}

int main(int argc, char **argv) {
  GO_REBUILD_URSELF(argc, argv);

  bool run_tests = false;
  shift(argv, argc);
  while (argc > 0) {
    if (strcmp(*argv, "test") == 0)
      run_tests = true;
    shift(argv, argc);
  }

  cmd_append(&CMD, "cc", "-Wall", "-Wextra", "-Werror", "-o", "main", "-O3",
             "main.c");
  if (!cmd_run(&CMD))
    return 1;

  if (run_tests) {
    if (!run_all_tests()) {
      return -1;
    } else {
      nob_log(INFO, "all tests passed");
    }
  }

  return 0;
}
