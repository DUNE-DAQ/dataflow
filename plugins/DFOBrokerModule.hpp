/**
 * @file DFOBrokerModule.hpp
 *
 * This is part of the DUNE DAQ Software Suite, copyright 2024.
 * Licensing/copyright details are in the COPYING file that you should have
 * received with this code.
 */

#ifndef DFMODULES_PLUGINS_DFOBROKERMODULE_HPP_
#define DFMODULES_PLUGINS_DFOBROKERMODULE_HPP_

#include "dfmessages/DFODecision.hpp"
#include "dfmessages/DataflowHeartbeat.hpp"
#include "dfmessages/TriggerDecision.hpp"
#include "dfmessages/TriggerDecisionToken.hpp"
#include "appmodel/DFOBrokerConf.hpp"

#include "iomanager/Receiver.hpp"
#include "iomanager/Sender.hpp"

#include "appfwk/DAQModule.hpp"
#include "utilities/WorkerThread.hpp"
#include "logging/Logging.hpp"

#include <atomic>
#include <unordered_map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace dunedaq {

// Disable coverage checking LCOV_EXCL_START
ERS_DECLARE_ISSUE(dfmodules,
                  DFOBrokerRunNumberMismatch,
                  "DFOBroker encountered run number mismatch: recvd (" << received_run_number << ") != " << run_number
                                                                       << " from " << src_app << " for trigger_number "
                                                                       << trig_num,
                  ((uint32_t)received_run_number)((uint32_t)run_number)((std::string)src_app)(
                    (uint32_t)trig_num)) // NOLINT(build/unsigned)
ERS_DECLARE_ISSUE(dfmodules,
                  DFOBrokerDFONotFound,
                  "DFOBroker received message for unknown DFO: recvd (" << dfo_id << ")",
                  ((std::string)dfo_id)) // NOLINT(build/unsigned)
// Re-enable coverage checking LCOV_EXCL_STOP

namespace dfmodules {

/**
 * @brief DFOBrokerModule distributes triggers according to the
 * availability of the DF apps in the system
 */
class DFOBrokerModule : public dunedaq::appfwk::DAQModule
{
public:
  /**
   * @brief DFOBrokerModule Constructor
   * @param name Instance name for this DFOBrokerModule instance
   */
  explicit DFOBrokerModule(const std::string& name);

  DFOBrokerModule(const DFOBrokerModule&) = delete;            ///< DFOBrokerModule is not copy-constructible
  DFOBrokerModule& operator=(const DFOBrokerModule&) = delete; ///< DFOBrokerModule is not copy-assignable
  DFOBrokerModule(DFOBrokerModule&&) = delete;                 ///< DFOBrokerModule is not move-constructible
  DFOBrokerModule& operator=(DFOBrokerModule&&) = delete;      ///< DFOBrokerModule is not move-assignable

  void init(std::shared_ptr<appfwk::ModuleConfiguration> mcfg) override;

private:
  // Commands
  void do_conf(const data_t&);
  void do_scrap(const data_t&);
  void do_start(const data_t&);
  void do_stop(const data_t&);

  void do_enable_dfo(const data_t&);

  void generate_opmon_data() override;

  void receive_trigger_complete_token(const dfmessages::TriggerDecisionToken&);
  void receive_dfo_decision(const dfmessages::DFODecision&);
  void send_heartbeat(bool skip_time_check);
  void heartbeat_thread_proc(std::atomic<bool>& running);

  bool dfo_is_active(std::string const& dfo_id);
  std::vector<dfmessages::trigger_number_t> get_recent_completions();
  std::vector<dfmessages::trigger_number_t> get_outstanding_decisions();

  // Configuration
  const appmodel::DFOBrokerConf* m_dfobroker_conf;
  std::chrono::milliseconds m_send_heartbeat_interval;
  std::chrono::milliseconds m_send_heartbeat_timeout;
  std::chrono::milliseconds m_td_timeout;
  std::chrono::milliseconds m_stop_timeout;

  // Internal data
  struct DFOInfo
  {
    bool dfo_is_active{ false };
    std::set<dfmessages::trigger_number_t> recent_completions{};
  };

  dunedaq::utilities::WorkerThread m_thread;
  std::set<dfmessages::trigger_number_t> m_outstanding_decisions{};
  std::unordered_map<std::string, DFOInfo> m_dfo_information;
  daqdataformats::run_number_t m_run_number;

  // Connections
  std::string m_token_connection; // Input
  std::string m_dfod_connection; // Input
  std::string m_heartbeat_connection; // Output, pub/sub
  std::string m_td_connection; // Output

  // Coordination
  std::chrono::steady_clock::time_point m_last_heartbeat_sent;
  mutable std::mutex m_send_heartbeat_mutex;
  mutable std::mutex m_dfo_info_mutex;

  // Statistics
  std::atomic<uint64_t> m_sent_heartbeats;
  std::atomic<uint64_t> m_sent_decisions;
  std::atomic<uint64_t> m_received_decisions;
  std::atomic<uint64_t> m_received_tokens;
  std::atomic<uint64_t> m_pending_trs;

};
} // namespace dfmodules
} // namespace dunedaq

#endif // DFMODULES_PLUGINS_DFOBROKERMODULE_HPP_