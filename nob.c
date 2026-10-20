#include <string.h>
#define NOB_STRIP_PREFIX
#define NOB_IMPLEMENTATION
#include "nob.h"

#define BIN_PATH "./bf-jit"

#define TEST_PATH "tests"

static Cmd CMD = {0};

bool run_test(String_View test_name) {
  size_t alloc_mark = temp_save();
  String_Builder sb_expected = {0};
  String_Builder output = {0};
  bool test_passed = false;

  char *path_expected =
      temp_sprintf("%s/" SV_Fmt ".expected", TEST_PATH, SV_Arg(test_name));
  if (!file_exists(path_expected)) {
    nob_log(ERROR, "could not find expected output for test " SV_Fmt,
            SV_Arg(test_name));
    goto tail;
  }

  if (!read_entire_file(path_expected, &sb_expected)) {
    goto tail;
  };

  String_View expected_output = sb_to_sv(sb_expected);
  bool should_fail = sv_chop_prefix(&expected_output, SVLIT("SHOULD FAIL\n"));

  char *stdout_path =
      temp_sprintf("/tmp/bf_test_" SV_Fmt ".stdout", SV_Arg(test_name));
  char *stderr_path = NULL;
  if (should_fail) {
    stderr_path =
        temp_sprintf("/tmp/bf_test_" SV_Fmt ".stderr", SV_Arg(test_name));
  }

  char *input_path =
      temp_sprintf("%s/" SV_Fmt ".in", TEST_PATH, SV_Arg(test_name));

  if (!file_exists(input_path)) {
    input_path = NULL;
  }

  cmd_append(&CMD, BIN_PATH, "--test-output",
             temp_sprintf("%s/" SV_Fmt ".bf", TEST_PATH, SV_Arg(test_name)));

  bool cmd_failed =
      !cmd_run(&CMD, .stdin_path = input_path, .stdout_path = stdout_path,
               .stderr_path = stderr_path);

  if (!read_entire_file(stdout_path, &output)) {
    nob_log(WARNING, "could not read output of test " SV_Fmt,
            SV_Arg(test_name));
    goto tail;
  }

  if (should_fail) {
    if (!cmd_failed) {
      goto tail;
    } else {
      output.count = 0;
      if (!read_entire_file(stderr_path, &output)) {
        nob_log(WARNING, "could not read err output of test " SV_Fmt,
                SV_Arg(test_name));
        goto tail;
      }
    }
  } else {
    if (cmd_failed) {
      goto tail;
    }
  }
  if (expected_output.count != output.count ||
      strncmp(output.items, expected_output.items, output.count) != 0) {
    nob_log(ERROR,
            "test " SV_Fmt " failed\n"
            "  expected `" SV_Fmt "`\n"
            "  got `" SV_Fmt "`\n",
            SV_Arg(test_name), SV_Arg(expected_output),
            SV_Arg(sb_to_sv(output)));
    goto tail;
  }
  test_passed = true;

tail:
  temp_rewind(alloc_mark);
  da_free(sb_expected);
  da_free(output);
  return test_passed;
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

  cmd_append(&CMD, "cc", "-Wall", "-Wextra", "-Werror", "-o", BIN_PATH, "-O3",
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
