import pytest
import os
import re

import dfmodules.data_file_checks as data_file_checks
import integrationtest.log_file_checks as log_file_checks

# Values that help determine the running conditions
number_of_data_producers=2
number_of_readout_apps=2
number_of_dataflow_apps=2
pulser_trigger_rate=1.0 # Hz
run_duration=30  # seconds

# Default values for validation parameters
expected_number_of_data_files=2*number_of_dataflow_apps
check_for_logfile_errors=True
expected_event_count=run_duration*pulser_trigger_rate/number_of_dataflow_apps
expected_event_count_tolerance=expected_event_count/10
wib1_frag_hsi_trig_params={"fragment_type_description": "WIB",
                           "hdf5_detector_group": "TPC", "hdf5_region_prefix": "APA",
                           "expected_fragment_count": (number_of_data_producers*number_of_readout_apps),
                           "min_size_bytes": 37200, "max_size_bytes": 37200}
wib1_frag_multi_trig_params={"fragment_type_description": "WIB",
                             "hdf5_detector_group": "TPC", "hdf5_region_prefix": "APA",
                             "expected_fragment_count": (number_of_data_producers*number_of_readout_apps),
                             "min_size_bytes": 80, "max_size_bytes": 37200}
wib1_tpset_params={"fragment_type_description": "TP Stream",
                             "hdf5_detector_group": "TPC", "hdf5_region_prefix": "APA",
                             "expected_fragment_count": (number_of_data_producers*number_of_readout_apps),
                             "min_size_bytes": 80, "max_size_bytes": 2800000}
triggercandidate_frag_params={"fragment_type_description": "Trigger Candidate",
                              "hdf5_detector_group": "Trigger", "hdf5_region_prefix": "Region",
                              "expected_fragment_count": 1,
                              "min_size_bytes": 130, "max_size_bytes": 150}
triggertp_frag_params={"fragment_type_description": "Trigger with TPs",
                       "hdf5_detector_group": "Trigger", "hdf5_region_prefix": "Region",
                       "expected_fragment_count": 3,
                       "min_size_bytes": 80, "max_size_bytes": 16000}
ignored_logfile_problems={"dqm": ["client will not be able to connect to Kafka cluster"]}

# The next three variable declarations *must* be present as globals in the test
# file. They're read by the "fixtures" in conftest.py to determine how
# to run the config generation and nanorc

# The name of the python module for the config generation
confgen_name="daqconf_multiru_gen"
# The arguments to pass to the config generator, excluding the json
# output directory (the test framework handles that)
output_dir="."
confgen_arguments_base=[ "-d", "./frames.bin", "-o", output_dir, "-s", "10", "-n", str(number_of_data_producers), "-b", "1000", "-a", "1000", "-t", str(pulser_trigger_rate), "--latency-buffer-size", "200000"] + [ "--host-ru", "localhost" ] * number_of_readout_apps + [ "--host-df", "localhost" ] * number_of_dataflow_apps
for idx in range(number_of_readout_apps):
    confgen_arguments_base+=["--region-id", str(idx)] 
confgen_arguments={"Software_TPG_System": confgen_arguments_base+["--enable-software-tpg", "--enable-tpset-writing", "-c", str(3*number_of_data_producers*number_of_readout_apps)],
                  }
# The commands to run in nanorc, as a list
nanorc_command_list="integtest-partition boot conf".split()
nanorc_command_list+="start_run --wait 2 101 wait ".split() + [str(run_duration)] + "stop_run          wait 2".split()
nanorc_command_list+="start_run          102 wait ".split() + [str(run_duration)] + "stop_run --wait 2 wait 2".split()
nanorc_command_list+="scrap terminate".split()

# The tests themselves

def test_nanorc_success(run_nanorc):
    current_test=os.environ.get('PYTEST_CURRENT_TEST')
    match_obj = re.search(r".*\[(.+)\].*", current_test)
    if match_obj:
        current_test = match_obj.group(1)
    banner_line = re.sub(".", "=", current_test)
    print(banner_line)
    print(current_test)
    print(banner_line)
    # Check that nanorc completed correctly
    assert run_nanorc.completed_process.returncode==0

def test_log_files(run_nanorc):
    if check_for_logfile_errors:
        # Check that there are no warnings or errors in the log files
        assert log_file_checks.logs_are_error_free(run_nanorc.log_files, True, True, ignored_logfile_problems)

def test_data_files(run_nanorc):
    local_expected_event_count=expected_event_count
    local_event_count_tolerance=expected_event_count_tolerance
    low_number_of_files=expected_number_of_data_files
    high_number_of_files=expected_number_of_data_files
    fragment_check_list=[]
    if "--enable-software-tpg" in run_nanorc.confgen_arguments:
        local_expected_event_count+=(270*number_of_data_producers*number_of_readout_apps*run_duration/(100*number_of_dataflow_apps))
        local_event_count_tolerance+=(10*number_of_data_producers*number_of_readout_apps*run_duration/(100*number_of_dataflow_apps))
        fragment_check_list.append(wib1_frag_multi_trig_params)
        fragment_check_list.append(triggertp_frag_params)
    else:
        low_number_of_files-=number_of_dataflow_apps
        if low_number_of_files < 1:
            low_number_of_files=1
        fragment_check_list.append(wib1_frag_hsi_trig_params)
        fragment_check_list.append(triggercandidate_frag_params)

    # Run some tests on the output data file
    assert len(run_nanorc.data_files)==high_number_of_files or len(run_nanorc.data_files)==low_number_of_files

    for idx in range(len(run_nanorc.data_files)):
        data_file=data_file_checks.DataFile(run_nanorc.data_files[idx])
        assert data_file_checks.sanity_check(data_file)
        assert data_file_checks.check_file_attributes(data_file)
        assert data_file_checks.check_event_count(data_file, local_expected_event_count, local_event_count_tolerance)
        for jdx in range(len(fragment_check_list)):
            assert data_file_checks.check_fragment_count(data_file, fragment_check_list[jdx])
            assert data_file_checks.check_fragment_sizes(data_file, fragment_check_list[jdx])

def test_tpstream_files(run_nanorc):
    data_dir=output_dir if output_dir != "." and output_dir != "" else run_nanorc.run_dir
    tpstream_files = list(data_dir.glob("tpstream_*.hdf5"))
    local_expected_event_count=run_duration # TPStreamWriter is currently configured to write at 1 Hz
    local_event_count_tolerance = local_expected_event_count / 10
    fragment_check_list=[wib1_tpset_params]

    assert len(tpstream_files) == 2 # one for each run

    for idx in range(len(tpstream_files)):
        data_file=data_file_checks.DataFile(tpstream_files[idx])
        #assert data_file_checks.sanity_check(data_file) # Sanity check doesn't work for stream files
        assert data_file_checks.check_file_attributes(data_file)
        assert data_file_checks.check_event_count(data_file, local_expected_event_count, local_event_count_tolerance)
        for jdx in range(len(fragment_check_list)):
            assert data_file_checks.check_fragment_count(data_file, fragment_check_list[jdx])
            assert data_file_checks.check_fragment_sizes(data_file, fragment_check_list[jdx])

