/**
 * @file TPSetWriter.cpp TPSetWriter class implementation
 *
 * This is part of the DUNE DAQ Software Suite, copyright 2020.
 * Licensing/copyright details are in the COPYING file that you should have
 * received with this code.
 */

#include "TPSetWriter.hpp"
#include "dfmodules/tpsetwriter/Nljs.hpp"

#include "iomanager/IOManager.hpp" 
#include "appfwk/DAQModuleHelper.hpp"
#include "appfwk/app/Nljs.hpp"
#include "daqdataformats/Fragment.hpp"
#include "logging/Logging.hpp"
#include "rcif/cmd/Nljs.hpp"
#include "readoutlibs/utils/BufferedFileWriter.hpp"
#include "serialization/Serialization.hpp"
#include "triggeralgs/Types.hpp"

#include "boost/date_time/posix_time/posix_time.hpp"

#include <chrono>
#include <sstream>
#include <string>
#include <vector>

enum
{
  TLVL_ENTER_EXIT_METHODS = 5,
  TLVL_CONFIG = 7,
};

namespace dunedaq {
namespace dfmodules {

TPSetWriter::TPSetWriter(const std::string& name)
  : dunedaq::appfwk::DAQModule(name)
  , m_thread(std::bind(&TPSetWriter::do_work, this, std::placeholders::_1))
  , m_queue_timeout(100)
{
  register_command("conf", &TPSetWriter::do_conf);
  register_command("start", &TPSetWriter::do_start);
  register_command("stop", &TPSetWriter::do_stop);
  register_command("scrap", &TPSetWriter::do_scrap);
}

void
TPSetWriter::init(const nlohmann::json& payload)
{
  TLOG_DEBUG(TLVL_ENTER_EXIT_METHODS) << get_name() << ": Entering init() method";
  auto iom = iomanager::IOManager::get();
  m_tpset_source = iom->get_receiver<incoming_t>(appfwk::connection_inst(payload, "tpset_source"));
  TLOG_DEBUG(TLVL_ENTER_EXIT_METHODS) << get_name() << ": Exiting init() method";
}

void
TPSetWriter::do_conf(const data_t& payload)
{
  TLOG_DEBUG(TLVL_ENTER_EXIT_METHODS) << get_name() << ": Entering do_conf() method";
  tpsetwriter::ConfParams conf_params = payload.get<tpsetwriter::ConfParams>();
  m_max_file_size = conf_params.max_file_size_bytes;
  TLOG_DEBUG(TLVL_CONFIG) << get_name() << ": max file size is " << m_max_file_size << " bytes.";
  TLOG_DEBUG(TLVL_ENTER_EXIT_METHODS) << get_name() << ": Exiting do_conf() method";
}

void
TPSetWriter::do_start(const nlohmann::json& payload)
{
  TLOG_DEBUG(TLVL_ENTER_EXIT_METHODS) << get_name() << ": Entering do_start() method";
  rcif::cmd::StartParams start_params = payload.get<rcif::cmd::StartParams>();
  m_run_number = start_params.run;
  m_thread.start_working_thread(get_name());
  TLOG_DEBUG(TLVL_ENTER_EXIT_METHODS) << get_name() << ": Exiting do_start() method";
}

void
TPSetWriter::do_stop(const nlohmann::json& /*payload*/)
{
  TLOG_DEBUG(TLVL_ENTER_EXIT_METHODS) << get_name() << ": Entering do_stop() method";
  m_thread.stop_working_thread();
  TLOG() << get_name() << " successfully stopped for run number " << m_run_number;
  TLOG_DEBUG(TLVL_ENTER_EXIT_METHODS) << get_name() << ": Exiting do_stop() method";
}

void
TPSetWriter::do_scrap(const data_t& /*payload*/)
{
  TLOG_DEBUG(TLVL_ENTER_EXIT_METHODS) << get_name() << ": Entering do_scrap() method";
  TLOG_DEBUG(TLVL_ENTER_EXIT_METHODS) << get_name() << ": Exiting do_scrap() method";
}

void
TPSetWriter::do_work(std::atomic<bool>& running_flag)
{
  TLOG_DEBUG(TLVL_ENTER_EXIT_METHODS) << get_name() << ": Entering do_work() method";

  using namespace std::chrono;

  size_t n_tpset_received = 0;

  auto start_time = steady_clock::now();

  triggeralgs::timestamp_t first_timestamp = 0;
  triggeralgs::timestamp_t last_timestamp = 0;

  // uint32_t last_seqno = 0;
  dunedaq::readoutlibs::BufferedFileWriter tpset_writer;
  size_t bytes_written = 0;
  int file_index = 0;

  // 11-Feb-2022, KAB: modified this loop to stop immediately when the
  // running flag is set to false (instead of also waiting for the incoming
  // TPSets to stop). It seems that with the newconf configuration model,
  // the order of stop commands is slightly different (the flow of TPSets
  // never stops, so something that previous was stopped earlier than this
  // code must now be stopped later).
  while (running_flag.load()) {
    trigger::TPSet tpset;
    try {
      tpset = m_tpset_source->receive(std::chrono::milliseconds(100));
      ++n_tpset_received;
    } catch (iomanager::TimeoutExpired&) {
      continue;
    }

    // Do some checks on the received TPSet
    // if (last_seqno != 0 && tpset.seqno != last_seqno + 1) {
    //  TLOG() << "Missed TPSets: last_seqno=" << last_seqno << ", current seqno=" << tpset.seqno;
    //}
    // last_seqno = tpset.seqno;

    // if (tpset.start_time < last_timestamp) {
    //  TLOG() << "TPSets out of order: last start time " << last_timestamp << ", current start time "
    //         << tpset.start_time;
    //}
    if (tpset.type == trigger::TPSet::Type::kHeartbeat) {
      TLOG() << "Heartbeat TPSet with start time " << tpset.start_time;
    } else if (tpset.objects.empty()) {
      TLOG() << "Empty TPSet with start time " << tpset.start_time;
    }
    for (auto const& tp : tpset.objects) {
      if (tp.time_start < tpset.start_time || tp.time_start > tpset.end_time) {
        TLOG() << "TPSet with start time " << tpset.start_time << ", end time " << tpset.end_time
               << " contains out-of-bounds TP with start time " << tp.time_start;
      }
    }

    // NOLINTNEXTLINE(build/unsigned)
    // std::vector<uint8_t> tpset_bytes =
    //  dunedaq::serialization::serialize(tpset, dunedaq::serialization::SerializationType::kMsgPack);
    // TLOG_DEBUG(9) << "Size of serialized TPSet is " << tpset_bytes.size() << ", TPSet size is " <<
    // tpset.objects.size();

    TLOG_DEBUG(9) << "Number of TPs in TPSet is " << tpset.objects.size() << ", Source ID is " << tpset.origin
                  << ", seqno is " << tpset.seqno << ", start timestamp is " << tpset.start_time << ", run number is "
                  << m_run_number;

    if (!tpset_writer.is_open()) {
      std::ostringstream work_oss;
      work_oss << "tpsets_run" << std::setfill('0') << std::setw(6) << m_run_number << "_" << std::setw(4)
               << file_index;
      time_t now = time(0);
      work_oss << "_" << boost::posix_time::to_iso_string(boost::posix_time::from_time_t(now)) << ".bin";
      tpset_writer.open(work_oss.str(), 1024, "None", false);
    }

    // daqdataformats::Fragment frag(&tpset_bytes[0], tpset_bytes.size());

    int dummy_val = 0xdeadbeef;
    daqdataformats::Fragment frag(&dummy_val, sizeof(dummy_val));
    frag.set_run_number(m_run_number);
    frag.set_window_begin(tpset.start_time);
    frag.set_window_end(tpset.end_time);
    daqdataformats::SourceID sourceid;
    sourceid.subsystem = tpset.origin.subsystem;
    sourceid.id = tpset.origin.id;
    frag.set_element_id(sourceid);
    frag.set_type(daqdataformats::FragmentType::kSW_TriggerPrimitive);

    tpset_writer.write(static_cast<const char*>(frag.get_storage_location()), frag.get_size());

    size_t num_longwords = 1 + ((frag.get_size() - 1) / sizeof(int64_t));
    size_t padding = (num_longwords * sizeof(int64_t)) - frag.get_size();
    char zero = 0;
    for (uint32_t idx = 0; idx < padding; ++idx) { // NOLINT(build/unsigned)
      tpset_writer.write(&zero, 1);
    }
    bytes_written += frag.get_size() + padding;
    if (m_max_file_size > 0 && bytes_written > m_max_file_size) {
      if (tpset_writer.is_open()) {
        tpset_writer.close();
      }
      ++file_index;
      bytes_written = 0;
    }

    if (first_timestamp == 0) {
      first_timestamp = tpset.start_time;
    }
    last_timestamp = tpset.start_time;
  } // while(true)

  if (tpset_writer.is_open()) {
    tpset_writer.close();
  }

  auto end_time = steady_clock::now();
  auto time_ms = duration_cast<milliseconds>(end_time - start_time).count();
  float rate_hz = 1e3 * static_cast<float>(n_tpset_received) / time_ms;
  float inferred_clock_frequency = 1e3 * (last_timestamp - first_timestamp) / time_ms;

  TLOG() << "Received " << n_tpset_received << " TPSets in " << time_ms << "ms. " << rate_hz
         << " TPSet/s. Inferred clock frequency " << inferred_clock_frequency << "Hz";
  TLOG_DEBUG(TLVL_ENTER_EXIT_METHODS) << get_name() << ": Exiting do_work() method";
} // NOLINT Function length

} // namespace dfmodules
} // namespace dunedaq

DEFINE_DUNE_DAQ_MODULE(dunedaq::dfmodules::TPSetWriter)
