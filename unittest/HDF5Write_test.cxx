/**
 * @file HDF5Write_test.cxx Application that tests and demonstrates
 * the write functionality of the HDF5DataStore class.
 *
 * This is part of the DUNE DAQ Application Framework, copyright 2020.
 * Licensing/copyright details are in the COPYING file that you should have
 * received with this code.
 */

#include "dfmodules/DataStore.hpp"
#include "appfwk/ModuleConfiguration.hpp"

#include "appmodel/DataWriterModule.hpp"
#include "appmodel/DataWriterConf.hpp"
#include "appmodel/FilenameParams.hpp"
#include "confmodel/DetectorConfig.hpp"
#include "confmodel/System.hpp"
#include "appmodel/DataStoreConf.hpp"
#include "detdataformats/DetID.hpp"

#define BOOST_TEST_MODULE HDF5Write_test // NOLINT

#include "boost/test/unit_test.hpp"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <regex>
#include <string>
#include <utility>
#include <vector>

using namespace dunedaq::dfmodules;

std::vector<std::string>
get_files_matching_pattern(const std::string& path, const std::string& pattern)
{
  std::regex regex_search_pattern(pattern);
  std::vector<std::string> file_list;
  for (const auto& entry : std::filesystem::directory_iterator(path)) {
    if (std::regex_match(entry.path().filename().string(), regex_search_pattern)) {
      file_list.push_back(entry.path());
    }
  }
  return file_list;
}

std::vector<std::string>
delete_files_matching_pattern(const std::string& path, const std::string& pattern)
{
  std::regex regex_search_pattern(pattern);
  std::vector<std::string> file_list;
  for (const auto& entry : std::filesystem::directory_iterator(path)) {
    if (std::regex_match(entry.path().filename().string(), regex_search_pattern)) {
      if (std::filesystem::remove(entry.path())) {
        file_list.push_back(entry.path());
      }
    }
  }
  return file_list;
}

dunedaq::daqdataformats::TriggerRecord
create_trigger_record(int trig_num, int fragment_size, int element_count)
{
  const int run_number = 53;
  const dunedaq::daqdataformats::SourceID::Subsystem stype_to_use =
    dunedaq::daqdataformats::SourceID::Subsystem::kDetectorReadout;

  // setup our dummy_data
  std::vector<char> dummy_vector(fragment_size);
  char* dummy_data = dummy_vector.data();

  // get a timestamp for this trigger
  uint64_t ts = std::chrono::duration_cast<std::chrono::milliseconds>( // NOLINT(build/unsigned)
                  system_clock::now().time_since_epoch())
                  .count();

  // create TriggerRecordHeader
  dunedaq::daqdataformats::TriggerRecordHeaderData trh_data;
  trh_data.trigger_number = trig_num;
  trh_data.trigger_timestamp = ts;
  trh_data.num_requested_components = element_count;
  trh_data.run_number = run_number;
  trh_data.sequence_number = 0;
  trh_data.max_sequence_number = 1;
  trh_data.element_id = dunedaq::daqdataformats::SourceID(dunedaq::daqdataformats::SourceID::Subsystem::kTRBuilder, 0);

  dunedaq::daqdataformats::TriggerRecordHeader trh(&trh_data);

  // create out TriggerRecord
  dunedaq::daqdataformats::TriggerRecord tr(trh);

  // loop over elements
  for (int ele_num = 0; ele_num < element_count; ++ele_num) {

    // create our fragment
    dunedaq::daqdataformats::FragmentHeader fh;
    fh.trigger_number = trig_num;
    fh.trigger_timestamp = ts;
    fh.window_begin = ts - 10;
    fh.window_end = ts;
    fh.run_number = run_number;
    fh.sequence_number = 0;
    fh.fragment_type =
      static_cast<dunedaq::daqdataformats::fragment_type_t>(dunedaq::daqdataformats::FragmentType::kWIB);
    fh.detector_id = static_cast<uint16_t>(dunedaq::detdataformats::DetID::Subdetector::kHD_TPC);
    fh.element_id = dunedaq::daqdataformats::SourceID(stype_to_use, ele_num);
    std::unique_ptr<dunedaq::daqdataformats::Fragment> frag_ptr(
      new dunedaq::daqdataformats::Fragment(dummy_data, fragment_size));
    frag_ptr->set_header_fields(fh);

    // add fragment to TriggerRecord
    tr.add_fragment(std::move(frag_ptr));

  } // end loop over elements

  return tr;
}

struct CfgFixture
{
  CfgFixture(std::string systemName)
  {
    TLOG_DEBUG(4) << "Creating CfgFixture";
    setenv("DUNEDAQ_SYSTEM", systemName.c_str(), 1);
    std::string oksConfig = "oksconflibs:test/config/hdf5write_test.data.xml:"+systemName;
    std::string appName = "TestApp";
    cfgMgr = std::make_shared<dunedaq::appfwk::ConfigurationManager>(oksConfig, appName);
    modCfg = std::make_shared<dunedaq::appfwk::ModuleConfiguration>(cfgMgr);
    TLOG_DEBUG(4) << "Done with CfgFixture";
  }

  const dunedaq::confmodel::DetectorConfig* get_detector_config()
  {
    return cfgMgr->system()->get_detector_configuration();
  }

  std::shared_ptr<dunedaq::appfwk::ConfigurationManager> cfgMgr;
  std::shared_ptr<dunedaq::appfwk::ModuleConfiguration> modCfg;
};

BOOST_AUTO_TEST_SUITE(HDF5Write_test)

BOOST_AUTO_TEST_CASE(WriteEventFiles)
{
  std::string file_path(std::filesystem::temp_directory_path());

  const int trigger_count = 5;
  const int apa_count = 3;
  const int link_count = 1;
  const int fragment_size = 10 + sizeof(dunedaq::daqdataformats::FragmentHeader);

  // delete any pre-existing files so that we start with a clean slate
  std::string delete_pattern = "hdf5writetest.*\\.hdf5";
  delete_files_matching_pattern(file_path, delete_pattern);

  // create the DataStore
  CfgFixture cfg("test-system-3-1");
  auto data_writer_conf = cfg.modCfg->module<dunedaq::appmodel::DataWriterModule>("dwm-01")->get_configuration();
  auto data_store_conf = data_writer_conf->get_data_store_params();

  auto data_store_conf_obj = data_store_conf->config_object();
  data_store_conf_obj.set_by_val<std::string>("directory_path", file_path);
  data_store_conf_obj.set_by_val<std::string>("mode", "one-event-per-file");

  auto data_store_ptr = make_data_store(data_store_conf->get_type(), data_store_conf->UID(), cfg.modCfg, "dwm-01");

  // write several events, each with several fragments
  for (int trigger_number = 1; trigger_number <= trigger_count; ++trigger_number)
    data_store_ptr->write(create_trigger_record(trigger_number, fragment_size, link_count * apa_count));

  data_store_ptr.reset(); // explicit destruction

  // check that the expected number of files was created
  std::string search_pattern = "hdf5writetest.*\\.hdf5";
  std::vector<std::string> file_list = get_files_matching_pattern(file_path, search_pattern);
  BOOST_REQUIRE_EQUAL(file_list.size(), trigger_count);

  // clean up the files that were created
  file_list = delete_files_matching_pattern(file_path, delete_pattern);
  delete_files_matching_pattern(file_path, "HardwareMap.*\\.txt");
  BOOST_REQUIRE_EQUAL(file_list.size(), trigger_count);
}

BOOST_AUTO_TEST_CASE(WriteOneFile)
{
  std::string file_path(std::filesystem::temp_directory_path());

  const int trigger_count = 5;
  const int apa_count = 3;
  const int link_count = 1;
  const int fragment_size = 10 + sizeof(dunedaq::daqdataformats::FragmentHeader);

  // delete any pre-existing files so that we start with a clean slate
  std::string delete_pattern = "hdf5writetest.*\\.hdf5";
  delete_files_matching_pattern(file_path, delete_pattern);

  // create the DataStore
  CfgFixture cfg("test-system-3-1");
  auto data_writer_conf = cfg.modCfg->module<dunedaq::appmodel::DataWriterModule>("dwm-01")->get_configuration();
  auto data_store_conf = data_writer_conf->get_data_store_params();

  auto data_store_conf_obj = data_store_conf->config_object();
  data_store_conf_obj.set_by_val<std::string>("directory_path", file_path);

  auto data_store_ptr = make_data_store(data_store_conf->get_type(), data_store_conf->UID(), cfg.modCfg, "dwm-01");

  // write several events, each with several fragments
  for (int trigger_number = 1; trigger_number <= trigger_count; ++trigger_number)
    data_store_ptr->write(create_trigger_record(trigger_number, fragment_size, apa_count * link_count));

  data_store_ptr.reset(); // explicit destruction

  // check that the expected number of files was created
  std::string search_pattern = "hdf5writetest.*\\.hdf5";
  std::vector<std::string> file_list = get_files_matching_pattern(file_path, search_pattern);
  BOOST_REQUIRE_EQUAL(file_list.size(), 1);

  // clean up the files that were created
  file_list = delete_files_matching_pattern(file_path, delete_pattern);
  delete_files_matching_pattern(file_path, "HardwareMap.*\\.txt");
  BOOST_REQUIRE_EQUAL(file_list.size(), 1);
}

BOOST_AUTO_TEST_CASE(CheckWritingSuffix)
{
  std::string file_path(std::filesystem::temp_directory_path());

  const int trigger_count = 5;
  const int apa_count = 3;
  const int link_count = 1;
  const int fragment_size = 10 + sizeof(dunedaq::daqdataformats::FragmentHeader);

  // delete any pre-existing files so that we start with a clean slate
  std::string delete_pattern = "hdf5writetest.*\\.hdf5";
  delete_files_matching_pattern(file_path, delete_pattern);

  // create the DataStore
  CfgFixture cfg("test-system-3-1");
  auto data_writer_conf = cfg.modCfg->module<dunedaq::appmodel::DataWriterModule>("dwm-01")->get_configuration();
  auto data_store_conf = data_writer_conf->get_data_store_params();

  auto data_store_conf_obj = data_store_conf->config_object();
  data_store_conf_obj.set_by_val<std::string>("directory_path", file_path);

  auto data_store_ptr = make_data_store(data_store_conf->get_type(), data_store_conf->UID(), cfg.modCfg, "dwm-01");

  // write several events, each with several fragments
  for (int trigger_number = 1; trigger_number <= trigger_count; ++trigger_number) {
    data_store_ptr->write(create_trigger_record(trigger_number, fragment_size, apa_count * link_count));

    // check that the .writing file is there
    std::string search_pattern = "hdf5writetest.*\\.writing";
    std::vector<std::string> file_list = get_files_matching_pattern(file_path, search_pattern);
    BOOST_REQUIRE_EQUAL(file_list.size(), 1);
  }

  data_store_ptr.reset(); // explicit destruction

  // check that the expected number of files was created
  std::string search_pattern = "hdf5writetest.*\\.writing";
  std::vector<std::string> file_list = get_files_matching_pattern(file_path, search_pattern);
  BOOST_REQUIRE_EQUAL(file_list.size(), 0);

  // clean up the files that were created
  file_list = delete_files_matching_pattern(file_path, delete_pattern);
  delete_files_matching_pattern(file_path, "HardwareMap.*\\.txt");
  BOOST_REQUIRE_EQUAL(file_list.size(), 1);
}

BOOST_AUTO_TEST_CASE(FileSizeLimitResultsInMultipleFiles)
{
  std::string file_path(std::filesystem::temp_directory_path());

  const int trigger_count = 15;
  const int apa_count = 5;
  const int link_count = 10;
  const int fragment_size = 10000;

  // 5 APAs times 10 links times 10000 bytes per fragment gives 500,000 bytes per TR
  // So, 15 TRs would give 7,500,000 bytes total.

  // delete any pre-existing files so that we start with a clean slate
  std::string delete_pattern = "hdf5writetest.*\\.hdf5";
  delete_files_matching_pattern(file_path, delete_pattern);

  // create the DataStore
  CfgFixture cfg("test-system-5-10");
  auto data_writer_conf = cfg.modCfg->module<dunedaq::appmodel::DataWriterModule>("dwm-01")->get_configuration();
  auto data_store_conf = data_writer_conf->get_data_store_params();

  auto data_store_conf_obj = data_store_conf->config_object();
  data_store_conf_obj.set_by_val<std::string>("directory_path", file_path);
  data_store_conf_obj.set_by_val<int>("max_file_size", 3000000); // goal is 6 events per file

  auto data_store_ptr = make_data_store(data_store_conf->get_type(), data_store_conf->UID(), cfg.modCfg, "dwm-01");

  // write several events, each with several fragments
  for (int trigger_number = 1; trigger_number <= trigger_count; ++trigger_number)
    data_store_ptr->write(create_trigger_record(trigger_number, fragment_size, apa_count * link_count));

  data_store_ptr.reset(); // explicit destruction

  // check that the expected number of files was created
  std::string search_pattern = "hdf5writetest.*\\.hdf5";
  std::vector<std::string> file_list = get_files_matching_pattern(file_path, search_pattern);
  // 7,500,000 bytes stored in files of size 3,000,000 should result in three files.
  BOOST_REQUIRE_EQUAL(file_list.size(), 3);

  // clean up the files that were created
  file_list = delete_files_matching_pattern(file_path, delete_pattern);
  delete_files_matching_pattern(file_path, "HardwareMap.*\\.txt");
  BOOST_REQUIRE_EQUAL(file_list.size(), 3);
}

BOOST_AUTO_TEST_CASE(SmallFileSizeLimitDataBlockListWrite)
{
  std::string file_path(std::filesystem::temp_directory_path());

  const int trigger_count = 5;
  const int apa_count = 5;
  const int link_count = 1;
  const int fragment_size = 100000;

  // 5 APAs times 100000 bytes per fragment gives 500,000 bytes per TR

  // delete any pre-existing files so that we start with a clean slate
  std::string delete_pattern = "hdf5writetest.*\\.hdf5";
  delete_files_matching_pattern(file_path, delete_pattern);

  // create the DataStore
  CfgFixture cfg("test-system-5-1");
  auto data_writer_conf = cfg.modCfg->module<dunedaq::appmodel::DataWriterModule>("dwm-01")->get_configuration();
  auto data_store_conf = data_writer_conf->get_data_store_params();

  auto data_store_conf_obj = data_store_conf->config_object();
  data_store_conf_obj.set_by_val<std::string>("directory_path", file_path);
  data_store_conf_obj.set_by_val<int>("max_file_size", 150000); // ~1.5 Fragment, ~0.3 TR

  auto data_store_ptr = make_data_store(data_store_conf->get_type(), data_store_conf->UID(), cfg.modCfg,"dwm-01");

  // write several events, each with several fragments
  for (int trigger_number = 1; trigger_number <= trigger_count; ++trigger_number)
    data_store_ptr->write(create_trigger_record(trigger_number, fragment_size, apa_count * link_count));

  data_store_ptr.reset(); // explicit destruction

  // check that the expected number of files was created
  std::string search_pattern = "hdf5writetest.*\\.hdf5";
  std::vector<std::string> file_list = get_files_matching_pattern(file_path, search_pattern);
  // each TriggerRecord should be stored in its own file
  BOOST_REQUIRE_EQUAL(file_list.size(), 5);

  // clean up the files that were created
  file_list = delete_files_matching_pattern(file_path, delete_pattern);
  delete_files_matching_pattern(file_path, "HardwareMap.*\\.txt");
  BOOST_REQUIRE_EQUAL(file_list.size(), 5);
}

BOOST_AUTO_TEST_SUITE_END()
