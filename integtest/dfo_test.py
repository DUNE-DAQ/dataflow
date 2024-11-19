import pytest
import os
import copy

import integrationtest.log_file_checks as log_file_checks
import integrationtest.data_classes as data_classes

pytest_plugins = "integrationtest.integrationtest_drunc"

# Values that help determine the running conditions
run_duration = 20  # seconds

# Default values for validation parameters
check_for_logfile_errors = True

ignored_logfile_problems = {
    "-controller": [
        "Worker with pid \\d+ was terminated due to signal",
        "Connection '.*' not found on the application registry",
    ],
    "local-connection-server": [
        "errorlog: -",
        "Worker with pid \\d+ was terminated due to signal",
    ],
    "log_.*": ["connect: Connection refused"],
}

# The arguments to pass to the config generator, excluding the json
# output directory (the test framework handles that)

config_obj = data_classes.drunc_config()
config_obj.attempt_cleanup = True
config_obj.op_env = "dfotest"
config_obj.config_db = os.path.dirname(__file__) + "/../test/config/dfo-test.data.xml"
config_obj.session = "dfo-test"

confgen_arguments = {
    "DFO Test": config_obj,
}


# The commands to run in nanorc, as a list
nanorc_command_list = (
    "boot wait 5 conf".split()
    + "start --run-number 101 wait 1 enable-triggers wait ".split()
    + [str(run_duration)]
    + "disable-triggers wait 2 drain-dataflow wait 2 stop-trigger-sources stop wait 2".split()
    + "start --run-number 102 wait 1 enable-triggers wait ".split()
    + [str(run_duration)]
    + "disable-triggers wait 2 drain-dataflow wait 2 stop-trigger-sources stop wait 2".split()
    + "start --run-number 103 wait 1 enable-triggers wait ".split()
    + [str(run_duration)]
    + "disable-triggers wait 2 drain-dataflow wait 2 stop-trigger-sources stop wait 2".split()
    + "scrap wait 5 terminate".split()
)

# The tests themselves


def test_nanorc_success(run_nanorc):

    # Check that nanorc completed correctly
    assert run_nanorc.completed_process.returncode == 0


def test_log_files(run_nanorc):
    current_test = os.environ.get("PYTEST_CURRENT_TEST")

    # Check that at least some of the expected log files are present
    assert any(
        f"{run_nanorc.session}_df-01" in str(logname)
        for logname in run_nanorc.log_files
    )
    assert any(
        f"{run_nanorc.session}_dfo" in str(logname) for logname in run_nanorc.log_files
    )

    if check_for_logfile_errors:
        # Check that there are no warnings or errors in the log files
        assert log_file_checks.logs_are_error_free(
            run_nanorc.log_files, True, True, ignored_logfile_problems
        )
