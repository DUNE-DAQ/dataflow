/**
 * @file FakeDataProdModule.cpp FakeDataProdModule class implementation
 *
 * This is part of the DUNE DAQ Software Suite, copyright 2020.
 * Licensing/copyright details are in the COPYING file that you should have
 * received with this code.
 */

#include "FakeDataProdModule.hpp"
#include "dfmodules/CommonIssues.hpp"

#include "appmodel/FakeDataProdModule.hpp"
#include "confmodel/Connection.hpp"
#include "dfmessages/Fragment_serialization.hpp"
#include "dfmessages/TimeSync.hpp"
#include "iomanager/IOManager.hpp"
#include "logging/Logging.hpp"

#include <chrono>
#include <cstdlib>
#include <memory>
#include <string>
#include <thread>
#include <utility>
#include <vector>

/**
 * @brief Name used by TRACE TLOG calls from this source file
 */
#define TRACE_NAME "FakeDataProdModule" // NOLINT
enum
{
  TLVL_ENTER_EXIT_METHODS = 5,
  TLVL_CONFIG = 7,
  TLVL_WORK_STEPS = 10,
  TLVL_TIME_SYNCS = 12
};

namespace dunedaq {
namespace dfmodules {

FakeDataProdModule::FakeDataProdModule(const std::string& name)
  : dunedaq::appfwk::DAQModule(name)
  , m_timesync_thread(std::bind(&FakeDataProdModule::do_timesync, this, std::placeholders::_1))
  , m_queue_timeout(100)
  , m_run_number(0)
{
  register_command("conf", &FakeDataProdModule::do_conf);
  register_command("start", &FakeDataProdModule::do_start);
  register_command("stop", &FakeDataProdModule::do_stop);

  m_pid_of_current_process = getpid();
}

void
FakeDataProdModule::init(std::shared_ptr<appfwk::ConfigurationManager> mcfg)
{
  TLOG_DEBUG(TLVL_ENTER_EXIT_METHODS) << get_name() << ": Entering init() method";
  auto mdal = mcfg->get_dal<appmodel::FakeDataProdModule>(get_name());
  if (!mdal) {
    throw appfwk::CommandFailed(ERS_HERE, "init", get_name(), "Unable to retrieve configuration object");
  }

  auto inputs = mdal->get_inputs();
  auto outputs = mdal->get_outputs();

  if (inputs[0]->get_data_type() != datatype_to_string<dfmessages::DataRequest>()) {
    throw InvalidQueueFatalError(ERS_HERE, get_name(), "DataRequest Input queue");
  }
  m_data_request_id = inputs[0]->UID();

  for (auto con : outputs) {
    if (con->get_data_type() == datatype_to_string<dfmessages::TimeSync>()) {
      m_timesync_id = con->UID();
    }
  }
  m_fake_data_prod_conf = mdal->get_configuration();
  TLOG_DEBUG(TLVL_ENTER_EXIT_METHODS) << get_name() << ": Exiting init() method";
}

void
FakeDataProdModule::do_conf(const data_t& )
{
  TLOG_DEBUG(TLVL_ENTER_EXIT_METHODS) << get_name() << ": Entering do_conf() method";

  m_sourceid.subsystem = daqdataformats::SourceID::string_to_subsystem(m_fake_data_prod_conf->get_system_type());
  m_sourceid.id = m_fake_data_prod_conf->get_source_id();
  m_time_tick_diff = m_fake_data_prod_conf->get_time_tick_diff();
  m_frame_size = m_fake_data_prod_conf->get_frame_size();
  m_response_delay = m_fake_data_prod_conf->get_response_delay();
  m_fragment_type = daqdataformats::string_to_fragment_type(m_fake_data_prod_conf->get_fragment_type());

  TLOG_DEBUG(TLVL_CONFIG) << get_name() << ": configured for link number " << m_sourceid.id;

  TLOG_DEBUG(TLVL_ENTER_EXIT_METHODS) << get_name() << ": Exiting do_conf() method";
}

void
FakeDataProdModule::do_start(const data_t& payload)
{
  TLOG_DEBUG(TLVL_ENTER_EXIT_METHODS) << get_name() << ": Entering do_start() method";
  m_sent_fragments = 0;
  m_received_requests = 0;
  m_run_number = payload.value<dunedaq::daqdataformats::run_number_t>("run", 0);

  m_timesync_thread.start_working_thread();

  auto iom = iomanager::IOManager::get();
  iom->add_callback<dfmessages::DataRequest>(
    m_data_request_id, std::bind(&FakeDataProdModule::process_data_request, this, std::placeholders::_1));
  TLOG_DEBUG(TLVL_ENTER_EXIT_METHODS) << get_name() << ": Exiting do_start() method";
}

void
FakeDataProdModule::do_stop(const data_t& /*args*/)
{
  TLOG_DEBUG(TLVL_ENTER_EXIT_METHODS) << get_name() << ": Entering do_stop() method";
  m_timesync_thread.stop_working_thread();

  auto iom = iomanager::IOManager::get();
  iom->remove_callback<dfmessages::DataRequest>(m_data_request_id);
  TLOG_DEBUG(TLVL_ENTER_EXIT_METHODS) << get_name() << ": Exiting do_stop() method";
}

// void
// FakeDataProdModule::get_info(opmonlib::InfoCollector& ci, int /*level*/)
// {
//   fakedataprodinfo::Info info;
//   info.requests_received = m_received_requests;
//   info.fragments_sent = m_sent_fragments;
//   ci.add(info);
// }

void
FakeDataProdModule::do_timesync(std::atomic<bool>& running_flag)
{

  auto iom = iomanager::IOManager::get();
  auto sender_ptr = iom->get_sender<dfmessages::TimeSync>(m_timesync_id);

  int sent_count = 0;
  uint64_t msg_seqno = 0; // NOLINT (build/unsigned)
  while (running_flag.load()) {
    auto time_now = std::chrono::system_clock::now().time_since_epoch();
    uint64_t current_timestamp = // NOLINT (build/unsigned)
      std::chrono::duration_cast<std::chrono::nanoseconds>(time_now).count();
    auto timesyncmsg = dfmessages::TimeSync(current_timestamp);
    ++msg_seqno;
    timesyncmsg.run_number = m_run_number;
    timesyncmsg.sequence_number = msg_seqno;
    timesyncmsg.source_pid = m_pid_of_current_process;
    TLOG_DEBUG(TLVL_TIME_SYNCS) << "New timesync: daq=" << timesyncmsg.daq_time << " wall=" << timesyncmsg.system_time
                                << " run=" << timesyncmsg.run_number << " seqno=" << timesyncmsg.sequence_number
                                << " pid=" << timesyncmsg.source_pid;
    try {
      sender_ptr->send(std::move(timesyncmsg), std::chrono::milliseconds(500));
      ++sent_count;
    } catch (ers::Issue& excpt) {
      ers::warning(TimeSyncTransmissionFailed(ERS_HERE, get_name(), m_timesync_id, excpt));
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }
  TLOG() << get_name() << ": sent " << sent_count << " TimeSync messages.";
}

void
FakeDataProdModule::process_data_request(dfmessages::DataRequest& data_request)
{

  TLOG_DEBUG(TLVL_ENTER_EXIT_METHODS) << get_name() << ": processsing request " << data_request.request_number;

  m_received_requests++;

  // num_frames_to_send = ⌈window_size / tick_diff⌉
  size_t num_frames_to_send = (data_request.request_information.window_end -
                               data_request.request_information.window_begin + m_time_tick_diff - 1) /
                              m_time_tick_diff;
  size_t num_bytes_to_send = num_frames_to_send * m_frame_size;

  // We don't care about the content of the data, but the size should be correct

  std::vector<uint8_t> fake_data; // NOLINT (build/unsigned)
  try {
    fake_data.resize(num_bytes_to_send);
  } catch (const std::bad_alloc&) {
    throw dunedaq::dfmodules::MemoryAllocationFailed(ERS_HERE, get_name(), num_bytes_to_send);
  }

  auto data_fragment_ptr = std::make_unique<daqdataformats::Fragment>(fake_data.data(), num_bytes_to_send);

  data_fragment_ptr->set_trigger_number(data_request.trigger_number);
  data_fragment_ptr->set_run_number(m_run_number);
  data_fragment_ptr->set_element_id(m_sourceid);
  data_fragment_ptr->set_error_bits(0);
  data_fragment_ptr->set_type(m_fragment_type);
  data_fragment_ptr->set_trigger_timestamp(data_request.trigger_timestamp);
  data_fragment_ptr->set_window_begin(data_request.request_information.window_begin);
  data_fragment_ptr->set_window_end(data_request.request_information.window_end);
  data_fragment_ptr->set_sequence_number(data_request.sequence_number);

  if (m_response_delay > 0) {
    std::this_thread::sleep_for(std::chrono::nanoseconds(m_response_delay));
  }

  try {
    auto iom = iomanager::IOManager::get();
    iom->get_sender<std::unique_ptr<daqdataformats::Fragment>>(data_request.data_destination)
      ->send(std::move(data_fragment_ptr), std::chrono::milliseconds(1000));
  } catch (ers::Issue& e) {
    ers::warning(FragmentTransmissionFailed(ERS_HERE, get_name(), data_request.trigger_number, e));
  }

  TLOG_DEBUG(TLVL_ENTER_EXIT_METHODS) << get_name() << ": finishing processing request " << data_request.request_number;
}

} // namespace dfmodules
} // namespace dunedaq

DEFINE_DUNE_DAQ_MODULE(dunedaq::dfmodules::FakeDataProdModule)
