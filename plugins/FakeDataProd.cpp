/**
 * @file FakeDataProd.cpp FakeDataProd class implementation
 *
 * This is part of the DUNE DAQ Software Suite, copyright 2020.
 * Licensing/copyright details are in the COPYING file that you should have
 * received with this code.
 */

#include "FakeDataProd.hpp"
#include "dfmodules/CommonIssues.hpp"
#include "dfmodules/fakedataprod/Nljs.hpp"
#include "dfmodules/fakedataprodinfo/InfoNljs.hpp"

#include "appfwk/DAQModuleHelper.hpp"
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
#define TRACE_NAME "FakeDataProd" // NOLINT
enum
{
  TLVL_ENTER_EXIT_METHODS = 5,
  TLVL_CONFIG = 7,
  TLVL_WORK_STEPS = 10
};

namespace dunedaq {
namespace dfmodules {

FakeDataProd::FakeDataProd(const std::string& name)
  : dunedaq::appfwk::DAQModule(name)
  , m_thread(std::bind(&FakeDataProd::do_work, this, std::placeholders::_1))
  , m_timesync_thread(std::bind(&FakeDataProd::do_timesync, this, std::placeholders::_1))
  , m_queue_timeout(100)
  , m_run_number(0)
  , m_data_request_input_queue(nullptr)
  , m_data_fragment_output_queue(nullptr)
{
  register_command("conf", &FakeDataProd::do_conf);
  register_command("start", &FakeDataProd::do_start);
  register_command("stop", &FakeDataProd::do_stop);
}

void
FakeDataProd::init(const data_t& init_data)
{
  TLOG_DEBUG(TLVL_ENTER_EXIT_METHODS) << get_name() << ": Entering init() method";
  auto qi = appfwk::queue_index(init_data,
                                { "data_request_input_queue", "data_fragment_output_queue", "timesync_output_queue" });
  try {
    m_data_request_input_queue.reset(new datareqsource_t(qi["data_request_input_queue"].inst));
  } catch (const ers::Issue& excpt) {
    throw InvalidQueueFatalError(ERS_HERE, get_name(), "data_request_input_queue", excpt);
  }
  try {
    m_data_fragment_output_queue.reset(new datafragsink_t(qi["data_fragment_output_queue"].inst));
  } catch (const ers::Issue& excpt) {
    throw InvalidQueueFatalError(ERS_HERE, get_name(), "data_fragment_output_queue", excpt);
  }
  try {
    m_timesync_output_queue.reset(new timesyncsink_t(qi["timesync_output_queue"].inst));
  } catch (const ers::Issue& excpt) {
    throw InvalidQueueFatalError(ERS_HERE, get_name(), "timesync_output_queue", excpt);
  }
  TLOG_DEBUG(TLVL_ENTER_EXIT_METHODS) << get_name() << ": Exiting init() method";
}

void
FakeDataProd::do_conf(const data_t& payload)
{
  TLOG_DEBUG(TLVL_ENTER_EXIT_METHODS) << get_name() << ": Entering do_conf() method";

  fakedataprod::Conf tmpConfig = payload.get<fakedataprod::Conf>();
  m_geoid.system_type = dataformats::GeoID::string_to_system_type(tmpConfig.system_type);
  m_geoid.region_id = tmpConfig.apa_number;
  m_geoid.element_id = tmpConfig.link_number;
  m_time_tick_diff = tmpConfig.time_tick_diff;
  m_frame_size = tmpConfig.frame_size;
  m_response_delay = tmpConfig.response_delay;
  m_fragment_type = dataformats::string_to_fragment_type(tmpConfig.fragment_type);

  TLOG_DEBUG(TLVL_CONFIG) << get_name() << ": configured for link number " << m_geoid.element_id;

  TLOG_DEBUG(TLVL_ENTER_EXIT_METHODS) << get_name() << ": Exiting do_conf() method";
}

void
FakeDataProd::do_start(const data_t& payload)
{
  TLOG_DEBUG(TLVL_ENTER_EXIT_METHODS) << get_name() << ": Entering do_start() method";
  m_sent_fragments = 0;
  m_received_requests = 0;
  m_run_number = payload.value<dunedaq::dataformats::run_number_t>("run", 0);
  m_thread.start_working_thread();
  m_timesync_thread.start_working_thread();
  TLOG() << get_name() << " successfully started for run number " << m_run_number;
  TLOG_DEBUG(TLVL_ENTER_EXIT_METHODS) << get_name() << ": Exiting do_start() method";
}

void
FakeDataProd::do_stop(const data_t& /*args*/)
{
  TLOG_DEBUG(TLVL_ENTER_EXIT_METHODS) << get_name() << ": Entering do_stop() method";
  m_thread.stop_working_thread();
  m_timesync_thread.stop_working_thread();
  TLOG() << get_name() << " successfully stopped";
  TLOG_DEBUG(TLVL_ENTER_EXIT_METHODS) << get_name() << ": Exiting do_stop() method";
}

void
FakeDataProd::get_info(opmonlib::InfoCollector& ci, int /*level*/)
{
  fakedataprodinfo::Info info;
  info.requests_received = m_received_requests;
  info.fragments_sent = m_sent_fragments;
  ci.add(info);
}

void
FakeDataProd::do_timesync(std::atomic<bool>& running_flag)
{
  while (running_flag.load()) {
    auto time_now = std::chrono::system_clock::now().time_since_epoch();
    uint64_t current_timestamp =  // NOLINT (build/unsigned)
      std::chrono::duration_cast<std::chrono::nanoseconds>(time_now).count();
    auto timesyncmsg = dfmessages::TimeSync(current_timestamp);
    try {
      m_timesync_output_queue->push(std::move(timesyncmsg));
    } catch (const dunedaq::appfwk::QueueTimeoutExpired& excpt) {
      ers::warning(dunedaq::appfwk::QueueTimeoutExpired(ERS_HERE, get_name(), "Could not send timesync", 0));
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }
}

void
FakeDataProd::do_work(std::atomic<bool>& running_flag)
{
  TLOG_DEBUG(TLVL_ENTER_EXIT_METHODS) << get_name() << ": Entering do_work() method";
  int32_t receivedCount = 0;

  while (running_flag.load()) {
    dfmessages::DataRequest data_request;
    try {
      m_data_request_input_queue->pop(data_request, m_queue_timeout);
      m_received_requests++;
      ++receivedCount;
    } catch (const dunedaq::appfwk::QueueTimeoutExpired& excpt) {
      // it is perfectly reasonable that there might be no data in the queue
      // some fraction of the times that we check, so we just continue on and try again
      continue;
    }

    // num_frames_to_send = ⌈window_size / tick_diff⌉
    size_t num_frames_to_send =
      (data_request.window_end - data_request.window_begin + m_time_tick_diff - 1) / m_time_tick_diff;
    size_t num_bytes_to_send = num_frames_to_send * m_frame_size;

    // We don't care about the content of the data, but the size should be correct
    void* fake_data = malloc(num_bytes_to_send);

    // This should really not happen
    if (fake_data == nullptr) {
      throw dunedaq::dataformats::MemoryAllocationFailed(ERS_HERE, num_bytes_to_send);
    }
    std::unique_ptr<dataformats::Fragment> data_fragment_ptr(new dataformats::Fragment(fake_data, num_bytes_to_send));
    data_fragment_ptr->set_trigger_number(data_request.trigger_number);
    data_fragment_ptr->set_run_number(m_run_number);
    data_fragment_ptr->set_element_id(m_geoid);
    data_fragment_ptr->set_error_bits(0);
    data_fragment_ptr->set_type(m_fragment_type);
    data_fragment_ptr->set_trigger_timestamp(data_request.trigger_timestamp);
    data_fragment_ptr->set_window_begin(data_request.window_begin);
    data_fragment_ptr->set_window_end(data_request.window_end);
    data_fragment_ptr->set_sequence_number(data_request.sequence_number);

    if (m_response_delay > 0) {
      std::this_thread::sleep_for(std::chrono::nanoseconds(m_response_delay));
    }

    bool wasSentSuccessfully = false;
    while (!wasSentSuccessfully && running_flag.load()) {
      TLOG_DEBUG(TLVL_WORK_STEPS) << get_name() << ": Pushing the Data Fragment for trigger number "
                                  << data_fragment_ptr->get_trigger_number() << " onto the output queue";
      try {
        m_data_fragment_output_queue->push(std::move(data_fragment_ptr), m_queue_timeout);
        wasSentSuccessfully = true;
        m_sent_fragments++;
      } catch (const dunedaq::appfwk::QueueTimeoutExpired& excpt) {
        std::ostringstream oss_warn;
        oss_warn << "push to output queue \"" << m_data_fragment_output_queue->get_name() << "\"";
        ers::warning(dunedaq::appfwk::QueueTimeoutExpired(
          ERS_HERE,
          get_name(),
          oss_warn.str(),
          std::chrono::duration_cast<std::chrono::milliseconds>(m_queue_timeout).count()));
      }
    }
    free(fake_data);

    // TLOG_DEBUG(TLVL_WORK_STEPS) << get_name() << ": Start of sleep while waiting for run Stop";
    // std::this_thread::sleep_for(std::chrono::milliseconds(m_sleep_msec_while_running));
    // TLOG_DEBUG(TLVL_WORK_STEPS) << get_name() << ": End of sleep while waiting for run Stop";
  }

  std::ostringstream oss_summ;
  oss_summ << ": Exiting the do_work() method, received Fake trigger decision messages for " << receivedCount
           << " triggers.";
  TLOG() << ProgressUpdate(ERS_HERE, get_name(), oss_summ.str());
  TLOG_DEBUG(TLVL_ENTER_EXIT_METHODS) << get_name() << ": Exiting do_work() method";
}

} // namespace dfmodules
} // namespace dunedaq

DEFINE_DUNE_DAQ_MODULE(dunedaq::dfmodules::FakeDataProd)
