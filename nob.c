#include <math.h>
#define NOB_STRIP_PREFIX
#define NOB_NO_ECHO
#define NOB_IMPLEMENTATION
#include "nob.h"

#define BIN_PATH "./bf-jit"

#define TEST_PATH "tests"

#define EOL_MARK "\033[7m%\033[27m\n"

static Cmd CMD = {0};

char *TEST_FLAGS[] = {
    NULL,
    "-o",
    "-i",
    "-io",
};

#define T_FLAG_COUNT (sizeof(TEST_FLAGS) / sizeof(char *))

bool run_test(String_View test_name, String_Builder *test_log) {
  size_t alloc_mark = temp_save();
  String_Builder sb_expected = {0};
  String_Builder output = {0};
  bool test_passed = true;

  printf("running tests for `" SV_Fmt "`\n", SV_Arg(test_name));

  char *path_expected =
      temp_sprintf("%s/" SV_Fmt ".expected", TEST_PATH, SV_Arg(test_name));
  if (!file_exists(path_expected)) {
    nob_log(ERROR, "could not find expected output for test " SV_Fmt,
            SV_Arg(test_name));
    exit(-1);
  }

  if (!read_entire_file(path_expected, &sb_expected)) {
    nob_log(ERROR, "could not read expected output for test " SV_Fmt,
            SV_Arg(test_name));
    exit(-1);
  };

  String_View expected_output = sb_to_sv(sb_expected);
  bool should_fail = sv_chop_prefix(&expected_output, SVLIT("SHOULD FAIL\n"));

  char *stdout_path = temp_sprintf("%s/bf_test_" SV_Fmt ".stdout", TEST_PATH,
                                   SV_Arg(test_name));
  char *stderr_path = NULL;
  if (should_fail) {
    stderr_path = temp_sprintf("%s/bf_test_" SV_Fmt ".stderr", TEST_PATH,
                               SV_Arg(test_name));
  }

  char *input_path =
      temp_sprintf("%s/" SV_Fmt ".in", TEST_PATH, SV_Arg(test_name));

  if (!file_exists(input_path)) {
    input_path = NULL;
  }

  test_log->count = 0;
  sb_appendf(test_log, "\033[1;31mTest `" SV_Fmt "` failed!\033[0m\n",
             SV_Arg(test_name));

  for (int flag_idx = 0; flag_idx < T_FLAG_COUNT; flag_idx++) {
    char *flags = TEST_FLAGS[flag_idx];
    // TODO: add timeout
    //"timeout", "--foreground", "1",
    cmd_append(&CMD, BIN_PATH, "--test-output");
    if (flags) {
      cmd_append(&CMD, flags);
    }
    cmd_append(&CMD,
               temp_sprintf("%s/" SV_Fmt ".bf", TEST_PATH, SV_Arg(test_name)));

    bool cmd_failed =
        !cmd_run(&CMD, .stdin_path = input_path, .stdout_path = stdout_path,
                 .stderr_path = stderr_path);

    if (!read_entire_file(stdout_path, &output)) {
      nob_log(WARNING, "could not read output of test " SV_Fmt,
              SV_Arg(test_name));
      exit(-1);
    }

    char *reason = NULL;
    if (should_fail) {
      if (!cmd_failed) {
        reason = "should fail but did not\n";
      } else {
        output.count = 0;
        if (!read_entire_file(stderr_path, &output)) {
          nob_log(WARNING, "could not read err output of test " SV_Fmt,
                  SV_Arg(test_name));
          exit(-1);
        }
      }
    } else {
      if (cmd_failed) {
        reason = "should not fail but did\n";
      }
    }
    if (!reason &&
        (expected_output.count != output.count ||
         memcmp(output.items, expected_output.items, output.count) != 0)) {
      String_Builder msg = {0};

      sb_appendf(&msg, "expected output:\n" SV_Fmt, SV_Arg(expected_output));
      if (expected_output.count == 0 || da_last(&msg) != '\n')
        sb_append_cstr(&msg, EOL_MARK);

      sb_appendf(&msg, "output:\n" SV_Fmt, SV_Arg(sb_to_sv(output)));
      if (output.count == 0 || da_last(&msg) != '\n')
        sb_append_cstr(&msg, EOL_MARK);

      char *output = temp_sprintf("output:\n" SV_Fmt, SV_Arg(sb_to_sv(msg)));

      reason = temp_sprintf("wrong output\n"
                            "output:\n" SV_Fmt,
                            SV_Arg(sb_to_sv(msg)));
      sb_free(msg);
    }

    if (reason) {
      if (flags) {
        sb_appendf(test_log, "flags: %s\n", flags);
      } else {
        sb_appendf(test_log, "no flags\n");
      }
      sb_appendf(test_log, "%s\n", reason);
      test_passed = false;
    }

    if (!delete_file(stdout_path)) {
      nob_log(ERROR, "could not delete file: %s\n", stdout_path);
      exit(-1);
    }
    output.count = 0;
    if (stderr_path) {
      if (!delete_file(stderr_path)) {
        nob_log(ERROR, "could not delete file: %s\n", stderr_path);
        exit(-1);
      }
    }
  }

  temp_rewind(alloc_mark);
  da_free(sb_expected);
  da_free(output);
  return test_passed;
}

bool run_all_tests() {
  File_Paths paths = {0};
  read_entire_dir(TEST_PATH, &paths);

  bool had_no_failure = true;

  String_Builder test_log = {0};

  for (size_t i = 0; i < paths.count; i++) {
    String_View name = sv_from_cstr(paths.items[i]);
    if (!sv_chop_suffix(&name, SVLIT(".bf")))
      continue;

    if (!run_test(name, &test_log)) {
      printf(SV_Fmt, SV_Arg(sb_to_sv(test_log)));
      had_no_failure = false;
    }
  }

  da_free(paths);
  sb_free(test_log);
  return had_no_failure;
}

int main(int argc, char **argv) {
  GO_REBUILD_URSELF(argc, argv);

  bool run_tests = false;
  bool debug = false;
  shift(argv, argc);
  while (argc > 0) {
    if (strcmp(*argv, "test") == 0) {
      run_tests = true;
    } else if (strcmp(*argv, "debug") == 0) {
      debug = true;
    } else {
      nob_log(ERROR, "unrecognized option %s", *argv);
      return -1;
    }

    shift(argv, argc);
  }

  cmd_append(&CMD, "cc", "-Wall", "-Wextra", "-Wstrict-prototypes", "-Werror",
             "-o", BIN_PATH);

  if (debug) {
    // FIXME: make this work with different compilers
    cmd_append(&CMD, "-DNO_BOUNDS_CHECK", "-O0", "-g",
               "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
               "-fno-sanitize-recover=all");
  } else {
    cmd_append(&CMD, "-O3", "-flto");
  }
  cmd_append(&CMD, "src/main.c", "src/aarch64.c", "src/frontend.c",
             "src/interp.c", "src/ir.c", "src/ir_canon.c", "src/ir_opt.c",
             "src/memory.c");

  if (!cmd_run(&CMD))
    return 1;

  if (run_tests) {
    printf("running tests\n");
    if (!run_all_tests()) {
      return -1;
    } else {
      printf("\033[32mall tests passed\033[0m\n");
    }
  }

  return 0;
}
