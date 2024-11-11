# two of the goals of this test are to verify that...
# * if the TR size is slightly over half of the maximum HDF5 file size, then
#   we see one TR per file
# * if the TR size is larger than the maximum HDF5 file size, we also see one
#   TR per file.

import pytest
import os
import re
import urllib.request
import copy
import shutil

import integrationtest.data_file_checks as data_file_checks
import integrationtest.log_file_checks as log_file_checks
import integrationtest.data_classes as data_classes

pytest_plugins = "integrationtest.integrationtest_drunc"

# Values that help determine the running conditions
output_path_parameter = "."
number_of_data_producers = 10
run_duration = 32  # seconds
number_of_readout_apps = 3
number_of_dataflow_apps = 1
trigger_rate = 0.06  # Hz
token_count = 1
readout_window_time_before = 10000000
readout_window_time_after = 1000000
data_rate_slowdown_factor = 1
minimum_total_disk_space_gb = 33  # 50% more than what we need
minimum_free_disk_space_gb = 28  # 25% more than what we need

# Default values for validation parameters
expected_number_of_data_files = 2
check_for_logfile_errors = True
expected_event_count = 1
expected_event_count_tolerance = 1
wibeth_frag_55pct_params = {
    "fragment_type_description": "WIBEth",
    "fragment_type": "WIBEth",
    "hdf5_source_subsystem": "Detector_Readout",
    "expected_fragment_count": (number_of_data_producers * number_of_readout_apps),
    "min_size_bytes": 38678472,
    "max_size_bytes": 38678472,
}
wibeth_frag_125pct_params = {
    "fragment_type_description": "WIBEth",
    "fragment_type": "WIBEth",
    "hdf5_source_subsystem": "Detector_Readout",
    "expected_fragment_count": (number_of_data_producers * number_of_readout_apps),
    "min_size_bytes": 91411272,
    "max_size_bytes": 91411272,
}
triggercandidate_frag_params = {
    "fragment_type_description": "Trigger Candidate",
    "fragment_type": "Trigger_Candidate",
    "hdf5_source_subsystem": "Trigger",
    "expected_fragment_count": 1,
    "min_size_bytes": 72,
    "max_size_bytes": 280,
}
ignored_logfile_problems = {
    "-controller": [
        "Worker with pid \\d+ was terminated due to signal 1",
    ],
    "local-connection-server": [
        "errorlog: -",
        "Worker with pid \\d+ was terminated due to signal 1",
    ],
    "log_.*_largerecord_": ["connect: Connection refused"],
}

# Determine if the conditions are right for these tests
sufficient_disk_space = True
actual_output_path = output_path_parameter
if output_path_parameter == ".":
    actual_output_path = "/tmp"
disk_space = shutil.disk_usage(actual_output_path)
total_disk_space_gb = disk_space.total / (1024 * 1024 * 1024)
free_disk_space_gb = disk_space.free / (1024 * 1024 * 1024)
print(
    f"DEBUG: Space on disk for output path {actual_output_path}: total = {total_disk_space_gb} GB and free = {free_disk_space_gb} GB."
)
if (
    total_disk_space_gb < minimum_total_disk_space_gb
    or free_disk_space_gb < minimum_free_disk_space_gb
):
    sufficient_disk_space = False

# The next three variable declarations *must* be present as globals in the test
# file. They're read by the "fixtures" in conftest.py to determine how
# to run the config generation and nanorc

object_databases = ["config/daqsystemtest/integrationtest-objects.data.xml"]

conf_dict = data_classes.drunc_config()
conf_dict.dro_map_config.n_streams = number_of_data_producers
conf_dict.dro_map_config.n_apps = number_of_readout_apps
conf_dict.op_env = "integtest"
conf_dict.system = "largerecord"
conf_dict.tpg_enabled = False
conf_dict.n_df_apps = number_of_dataflow_apps

conf_dict.config_substitutions.append(
    data_classes.config_substitution(
        obj_id=conf_dict.system,
        obj_class="System",
        updates={"data_rate_slowdown_factor": data_rate_slowdown_factor},
    )
)

conf_dict.config_substitutions.append(
    data_classes.config_substitution(
        obj_class="RandomTCMakerConf",
        updates={"trigger_rate_hz": trigger_rate},
    )
)
conf_dict.config_substitutions.append(
    data_classes.config_substitution(
        obj_class="DataStoreConf",
        obj_id="default",
        updates={"max_file_size": 2 * 1024 * 1024 * 1024},
    )
)
oversize_conf = copy.deepcopy(conf_dict)  # Copy before setting the readout window

conf_dict.config_substitutions.append(
    data_classes.config_substitution(
        obj_class="TCReadoutMap",
        updates={
            "time_before": readout_window_time_before,
            "time_after": readout_window_time_after,
        },
    )
)

# Now set the readout window for the over-size case
oversize_conf.config_substitutions.append(
    data_classes.config_substitution(
        obj_class="TCReadoutMap",
        updates={
            "time_before": 2.5 * readout_window_time_before,
            "time_after": readout_window_time_after,
        },
    )
)

confgen_arguments = {
    "TRSize_55PercentOfMaxFileSize": conf_dict,
    "TRSize_125PercentOfMaxFileSize": oversize_conf,
}
# The commands to run in nanorc, as a list
if sufficient_disk_space:
    nanorc_command_list = (
        "boot conf wait 5".split()
        + "start 101 wait 1 enable-triggers wait ".split()
        + [str(run_duration)]
        + "disable-triggers wait 2 drain-dataflow wait 2 stop-trigger-sources stop ".split()
        + "start 102 wait 1 enable-triggers wait ".split()
        + [str(run_duration)]
        + "disable-triggers wait 2 drain-dataflow wait 2 stop-trigger-sources stop ".split()
        + " scrap terminate".split()
    )
else:
    nanorc_command_list = ["boot", "terminate"]

# The tests themselves


def test_nanorc_success(run_nanorc):
    if not sufficient_disk_space:
        pytest.skip(
            f"The raw data output path ({actual_output_path}) does not have enough space to run this test."
        )

    current_test = os.environ.get("PYTEST_CURRENT_TEST")
    match_obj = re.search(r".*\[(.+)\].*", current_test)
    if match_obj:
        current_test = match_obj.group(1)
    banner_line = re.sub(".", "=", current_test)
    print(banner_line)
    print(current_test)
    print(banner_line)
    # Check that nanorc completed correctly
    assert run_nanorc.completed_process.returncode == 0


def test_log_files(run_nanorc):
    if not sufficient_disk_space:
        pytest.skip(
            f"The raw data output path ({actual_output_path}) does not have enough space to run this test."
        )

    if check_for_logfile_errors:
        # Check that there are no warnings or errors in the log files
        assert log_file_checks.logs_are_error_free(
            run_nanorc.log_files, True, True, ignored_logfile_problems
        )


def test_data_files(run_nanorc):
    if not sufficient_disk_space:
        print(
            f"The raw data output path ({actual_output_path}) does not have enough space to run this test."
        )
        print(
            f"    (Free and total space are {free_disk_space_gb} GB and {total_disk_space_gb} GB.)"
        )
        print(
            f"    (Minimums are {minimum_free_disk_space_gb} GB and {minimum_total_disk_space_gb} GB.)"
        )
        pytest.skip(
            f"The raw data output path ({actual_output_path}) does not have enough space to run this test."
        )

    local_expected_event_count = expected_event_count
    local_event_count_tolerance = expected_event_count_tolerance
    fragment_check_list = [triggercandidate_frag_params]
    current_test = os.environ.get("PYTEST_CURRENT_TEST")
    match_obj = re.search(r".*\[(.+)\].*", current_test)
    if match_obj:
        current_test = match_obj.group(1)
    if current_test == "TRSize_125PercentOfMaxFileSize":
        fragment_check_list.append(wibeth_frag_125pct_params)
    else:
        fragment_check_list.append(wibeth_frag_55pct_params)

    # Run some tests on the output data file
    assert len(run_nanorc.data_files) == expected_number_of_data_files

    all_ok = True

    for idx in range(len(run_nanorc.data_files)):
        data_file = data_file_checks.DataFile(run_nanorc.data_files[idx])
        all_ok &= data_file_checks.sanity_check(data_file)
        all_ok &= data_file_checks.check_file_attributes(data_file)
        all_ok &= data_file_checks.check_event_count(
            data_file, local_expected_event_count, local_event_count_tolerance
        )
        for jdx in range(len(fragment_check_list)):
            all_ok &= data_file_checks.check_fragment_count(
                data_file, fragment_check_list[jdx]
            )
            all_ok &= data_file_checks.check_fragment_sizes(
                data_file, fragment_check_list[jdx]
            )

    assert all_ok


def test_cleanup(run_nanorc):
    if not sufficient_disk_space:
        pytest.skip(
            f"The raw data output path ({actual_output_path}) does not have enough space to run this test."
        )

    pathlist_string = ""
    filelist_string = ""
    for data_file in run_nanorc.data_files:
        filelist_string += " " + str(data_file)
        if str(data_file.parent) not in pathlist_string:
            pathlist_string += " " + str(data_file.parent)

    if pathlist_string and filelist_string:
        print("============================================")
        print("Listing the hdf5 files before deleting them:")
        print("============================================")

        os.system(f"df -h {pathlist_string}")
        print("--------------------")
        os.system(f"ls -alF {filelist_string}")

        for data_file in run_nanorc.data_files:
            data_file.unlink()

        print("--------------------")
        os.system(f"df -h {pathlist_string}")
        print("============================================")
