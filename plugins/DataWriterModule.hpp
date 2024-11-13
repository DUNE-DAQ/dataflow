/**
 * @file DataWriterModule.hpp
 *
 * This is part of the DUNE DAQ Software Suite, copyright 2020.
 * Licensing/copyright details are in the COPYING file that you should have
 * received with this code.
 */

#ifndef DFMODULES_PLUGINS_DATAWRITER_HPP_
#define DFMODULES_PLUGINS_DATAWRITER_HPP_

#include "dfmodules/DataStore.hpp"

#include "appfwk/DAQModule.hpp"
#include "appmodel/DataWriterConf.hpp"
#include "daqdataformats/TriggerRecord.hpp"
#include "dfmessages/TriggerDecisionToken.hpp"
#include "iomanager/Receiver.hpp"
#include "iomanager/Sender.hpp"
#include "utilities/WorkerThread.hpp"

#include <chrono>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace dunedaq {
namespace dfmodules {

/**
 * @brief DataWriterModule is a shell for what we might write for the MiniDAQApp.
 */
class DataWriterModule : public dunedaq::appfwk::DAQModule
{
public:
  /**
   * @brief DataWriterModule Constructor
   * @param name Instance name for this DataWriterModule instance
   */
  explicit DataWriterModule(const std::string& name);

  DataWriterModule(const DataWriterModule&) = delete;            ///< DataWriterModule is not copy-constructible
  DataWriterModule& operator=(const DataWriterModule&) = delete; ///< DataWriterModule is not copy-assignable
  DataWriterModule(DataWriterModule&&) = delete;                 ///< DataWriterModule is not move-constructible
  DataWriterModule& operator=(DataWriterModule&&) = delete;      ///< DataWriterModule is not move-assignable

  void init(std::shared_ptr<appfwk::ModuleConfiguration> mcfg) override;
  void generate_opmon_data() override;

private:
  // Commands
  void do_conf(const data_t&);
  void do_start(const data_t&);
  void do_stop(const data_t&);
  void do_scrap(const data_t&);

  // Callback
  void receive_trigger_record(std::unique_ptr<daqdataformats::TriggerRecord>&);
  std::atomic<bool> m_running = false;

  // Configuration
  std::shared_ptr<appfwk::ModuleConfiguration> m_module_configuration;
  const appmodel::DataWriterConf* m_data_writer_conf;
  std::string m_writer_identifier;

  // size_t m_sleep_msec_while_running;
  std::chrono::milliseconds m_queue_timeout;
  bool m_data_storage_is_enabled;
  int m_data_storage_prescale;
  daqdataformats::run_number_t m_run_number;
  size_t m_min_write_retry_time_usec;
  size_t m_max_write_retry_time_usec;
  int m_write_retry_time_increase_factor;

  // Connections
  std::string m_trigger_record_connection;
  using tr_receiver_ct = iomanager::ReceiverConcept<std::unique_ptr<daqdataformats::TriggerRecord>>;
  std::shared_ptr<tr_receiver_ct> m_tr_receiver;

  using token_sender_t = iomanager::SenderConcept<dfmessages::TriggerDecisionToken>;
  std::shared_ptr<token_sender_t> m_token_output;
  std::string m_trigger_decision_connection;

  // Worker(s)
  dunedaq::utilities::WorkerThread m_thread;
  void do_work(std::atomic<bool>&);

  std::shared_ptr<DataStore> m_data_writer;

  // Metrics
  std::atomic<uint64_t> m_records_received = { 0 };     // NOLINT(build/unsigned)
  std::atomic<uint64_t> m_records_received_tot = { 0 }; // NOLINT(build/unsigned)
  std::atomic<uint64_t> m_records_written = { 0 };      // NOLINT(build/unsigned)
  std::atomic<uint64_t> m_records_written_tot = { 0 };  // NOLINT(build/unsigned)
  std::atomic<uint64_t> m_bytes_output = { 0 };         // NOLINT(build/unsigned)
  std::atomic<uint64_t> m_bytes_output_tot = { 0 };     // NOLINT(build/unsigned)
  std::atomic<uint64_t> m_writing_us = { 0 };           // NOLINT(build/unsigned)

  
  // Other
  std::map<daqdataformats::trigger_number_t, size_t> m_seqno_counts;

  inline double elapsed_seconds(std::chrono::steady_clock::time_point then,
                                std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now()) const
  {
    return std::chrono::duration_cast<std::chrono::seconds>(now - then).count();
  }
};
} // namespace dfmodules

ERS_DECLARE_ISSUE_BASE(dfmodules,
                       InvalidDataWriterModule,
                       appfwk::GeneralDAQModuleIssue,
                       "A valid dataWriter instance is not available so it will not be possible to write data. A "
                       "likely cause for this is a skipped or missed Configure transition.",
                       ((std::string)name),
                       ERS_EMPTY)

ERS_DECLARE_ISSUE_BASE(dfmodules,
                       DataWritingProblem,
                       appfwk::GeneralDAQModuleIssue,
                       "A problem was encountered when writing TriggerRecord number " << trnum << "." << seqnum
                                                                                      << " in run " << runnum,
                       ((std::string)name),
                       ((size_t)trnum)((size_t)seqnum)((size_t)runnum))

ERS_DECLARE_ISSUE_BASE(dfmodules,
                       InvalidRunNumber,
                       appfwk::GeneralDAQModuleIssue,
                       "An invalid run number was received in a "
                         << msg_type << "message, "
                         << "received=" << received << ", expected=" << expected << ", trig/seq_number=" << trnum << "."
                         << seqnum,
                       ((std::string)name),
                       ((std::string)msg_type)((size_t)received)((size_t)expected)((size_t)trnum)((size_t)seqnum))

} // namespace dunedaq

#endif // DFMODULES_PLUGINS_DATAWRITER_HPP_
