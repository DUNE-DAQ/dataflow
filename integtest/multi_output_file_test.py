import pytest
import os
import re
import copy
import urllib.request

import integrationtest.data_file_checks as data_file_checks
import integrationtest.log_file_checks as log_file_checks
import integrationtest.data_classes as data_classes

pytest_plugins = "integrationtest.integrationtest_drunc"

# Values that help determine the running conditions
number_of_data_producers = 2
number_of_readout_apps = 3
data_rate_slowdown_factor = 1

# Default values for validation parameters
expected_number_of_data_files = 4
check_for_logfile_errors = True

wibeth_frag_hsi_trig_params = {
    "fragment_type_description": "WIBEth",
    "fragment_type": "WIBEth",
    "hdf5_source_subsystem": "Detector_Readout",
    "expected_fragment_count": (number_of_data_producers * number_of_readout_apps),
    "min_size_bytes": 187272,
    "max_size_bytes": 194472,
}
wibeth_frag_multi_trig_params = {
    "fragment_type_description": "WIBEth",
    "fragment_type": "WIBEth",
    "hdf5_source_subsystem": "Detector_Readout",
    "expected_fragment_count": (number_of_data_producers * number_of_readout_apps),
    "min_size_bytes": 72,
    "max_size_bytes": 194472,
}
triggercandidate_frag_params = {
    "fragment_type_description": "Trigger Candidate",
    "fragment_type": "Trigger_Candidate",
    "hdf5_source_subsystem": "Trigger",
    "expected_fragment_count": 1,
    "min_size_bytes": 72,
    "max_size_bytes": 280,
}
triggeractivity_frag_params = {
    "fragment_type_description": "Trigger Activity",
    "fragment_type": "Trigger_Activity",
    "hdf5_source_subsystem": "Trigger",
    "expected_fragment_count": 1,
    "min_size_bytes": 72,
    "max_size_bytes": 520,
}
triggertp_frag_params = {
    "fragment_type_description": "Trigger with TPs",
    "fragment_type": "Trigger_Primitive",
    "hdf5_source_subsystem": "Trigger",
    "expected_fragment_count": (3 * number_of_readout_apps),
    "min_size_bytes": 72,
    "max_size_bytes": 16000,
}
hsi_frag_params = {
    "fragment_type_description": "HSI",
    "fragment_type": "Hardware_Signal",
    "hdf5_source_subsystem": "HW_Signals_Interface",
    "expected_fragment_count": 1,
    "min_size_bytes": 72,
    "max_size_bytes": 128,
}
ignored_logfile_problems = {
    "-controller": [
        "Worker with pid \\d+ was terminated due to signal 1",
    ],
    "local-connection-server": [
        "errorlog: -",
        "Worker with pid \\d+ was terminated due to signal 1",
    ],
    "log_.*_multioutput_": ["connect: Connection refused"],
}

# The next three variable declarations *must* be present as globals in the test
# file. They're read by the "fixtures" in conftest.py to determine how
# to run the config generation and nanorc

object_databases = ["config/daqsystemtest/integrationtest-objects.data.xml"]

conf_dict = data_classes.drunc_config()
conf_dict.dro_map_config.n_streams = number_of_data_producers
conf_dict.dro_map_config.n_apps = number_of_readout_apps
conf_dict.op_env = "integtest"
conf_dict.system = "multioutput"
conf_dict.tpg_enabled = False
conf_dict.fake_hsi_enabled = True  # FakeHSI must be enabled to set trigger window width!

conf_dict.config_substitutions.append(
    data_classes.config_substitution(
        obj_id=conf_dict.system,
        obj_class="System",
        updates={"data_rate_slowdown_factor": data_rate_slowdown_factor},
    )
)
conf_dict.config_substitutions.append(
    data_classes.config_substitution(
        obj_class="LatencyBuffer", updates={"size": 200000}
    )
)

conf_dict.config_substitutions.append(
    data_classes.config_substitution(
        obj_class="FakeHSIEventGeneratorConf",
        updates={"trigger_rate": 10.0},
    )
)

conf_dict.config_substitutions.append(
    data_classes.config_substitution(
        obj_class="HSISignalWindow",
        updates={
            "time_before": 52000,
            "time_after": 1000,
        },
    )
)
conf_dict.config_substitutions.append(
    data_classes.config_substitution(
        obj_class="DataStoreConf",
        obj_id="default",
        updates={"max_file_size": 1074000000},
    )
)
conf_dict.config_substitutions.append(
    data_classes.config_substitution(
        obj_class="TAMakerPrescaleAlgorithm",
        obj_id="dummy-ta-maker",
        updates={"prescale": 25},
    )
)

conf_dict.config_substitutions.append(
    data_classes.config_substitution(
        obj_class="DFOConf", updates={"busy_threshold": 5, "free_threshold": 2}
    )
)


swtpg_conf = copy.deepcopy(conf_dict)
swtpg_conf.tpg_enabled = True
swtpg_conf.frame_file = (
    "asset://?checksum=dd156b4895f1b06a06b6ff38e37bd798"  # WIBEth All Zeros
)

multiout_conf = copy.deepcopy(conf_dict)
multiout_conf.n_data_writers = 2
multiout_conf.config_substitutions.append(
    data_classes.config_substitution(
        obj_class="DataStoreConf",
        obj_id="default",
        updates={"max_file_size": 4 * 1024 * 1024 * 1024},
    )
)
multiout_conf.config_substitutions.append(
    data_classes.config_substitution(
        obj_class="QueueDescriptor",
        obj_id="trigger-records",
        updates={"queue_type": "kFollyMPMCQueue"},
    )
)


multiout_tpg_conf = copy.deepcopy(multiout_conf)
multiout_tpg_conf.tpg_enabled = True
multiout_tpg_conf.frame_file = (
    "asset://?checksum=dd156b4895f1b06a06b6ff38e37bd798"  # WIBEth All Zeros
)

confgen_arguments = {
    "WIBEth_System (Rollover files)": conf_dict,
    "Software_TPG_System (Rollover files)": swtpg_conf,
    "WIBEth_System (Multiple outputs)": multiout_conf,
    "Software_TPG_System (Multiple outputs)": multiout_tpg_conf,
}

# The commands to run in nanorc, as a list
run_duration = 120
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

# The tests themselves


def test_nanorc_success(run_nanorc):
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
    if check_for_logfile_errors:
        # Check that there are no warnings or errors in the log files
        assert log_file_checks.logs_are_error_free(
            run_nanorc.log_files, True, True, ignored_logfile_problems
        )


def test_data_files(run_nanorc):
    local_file_count = expected_number_of_data_files
    fragment_check_list = [triggercandidate_frag_params, hsi_frag_params]
    if run_nanorc.confgen_config.tpg_enabled:
        local_file_count = 4  # 5
        # fragment_check_list.append(wib1_frag_multi_trig_params) # ProtoWIB
        # fragment_check_list.append(wib2_frag_multi_trig_params) # DuneWIB
        fragment_check_list.append(wibeth_frag_multi_trig_params)  # WIBEth
        fragment_check_list.append(triggertp_frag_params)
        fragment_check_list.append(triggeractivity_frag_params)
    else:
        # fragment_check_list.append(wib1_frag_hsi_trig_params) # ProtoWIB
        # fragment_check_list.append(wib2_frag_hsi_trig_params) # DuneWIB
        fragment_check_list.append(wibeth_frag_hsi_trig_params)  # WIBEth

    # Run some tests on the output data file
    assert len(run_nanorc.data_files) == local_file_count

    for idx in range(len(run_nanorc.data_files)):
        data_file = data_file_checks.DataFile(run_nanorc.data_files[idx])
        assert data_file_checks.sanity_check(data_file)
        assert data_file_checks.check_file_attributes(data_file)
        for jdx in range(len(fragment_check_list)):
            assert data_file_checks.check_fragment_count(
                data_file, fragment_check_list[jdx]
            )
            assert data_file_checks.check_fragment_sizes(
                data_file, fragment_check_list[jdx]
            )


def test_cleanup(run_nanorc):
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
