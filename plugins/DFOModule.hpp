/**
 * @file DFOModule.hpp
 *
 * This is part of the DUNE DAQ Software Suite, copyright 2020.
 * Licensing/copyright details are in the COPYING file that you should have
 * received with this code.
 */

#ifndef DFMODULES_PLUGINS_DATAFLOWORCHESTRATOR_HPP_
#define DFMODULES_PLUGINS_DATAFLOWORCHESTRATOR_HPP_

#include "dfmodules/TriggerRecordBuilderData.hpp"

#include "appmodel/DFOConf.hpp"

#include "daqdataformats/TriggerRecord.hpp"
#include "dfmessages/DataRequest.hpp"
#include "dfmessages/TriggerDecision.hpp"
#include "dfmessages/TriggerDecisionToken.hpp"
#include "dfmessages/TriggerInhibit.hpp"
#include "trgdataformats/TriggerCandidateData.hpp"

#include "iomanager/Sender.hpp"

#include "appfwk/DAQModule.hpp"
#include "logging/Logging.hpp"

#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>
#include <mutex>

namespace dunedaq {

// Disable coverage checking LCOV_EXCL_START
ERS_DECLARE_ISSUE(dfmodules,
                  TRBModuleAppUpdate,
                  "TRBModule app " << connection_name << ": " << message,
                  ((std::string)connection_name)((std::string)message))
ERS_DECLARE_ISSUE(dfmodules,
                  UnknownTokenSource,
                  "Token from unknown source: " << connection_name,
                  ((std::string)connection_name))
ERS_DECLARE_ISSUE(dfmodules,
                  DFOModuleRunNumberMismatch,
                  "DFOModule encountered run number mismatch: recvd ("
                    << received_run_number << ") != " << run_number << " from " << src_app << " for trigger_number "
                    << trig_num,
                  ((uint32_t)received_run_number)((uint32_t)run_number)((std::string)src_app)(
                    (uint32_t)trig_num)) // NOLINT(build/unsigned)
ERS_DECLARE_ISSUE(dfmodules,
                  IncompleteTriggerDecision,
                  "TriggerDecision " << trigger_number << " didn't complete within timeout in run " << run_number,
                  ((uint32_t)trigger_number)((uint32_t)run_number)) // NOLINT(build/unsigned)
ERS_DECLARE_ISSUE(dfmodules,
                  UnableToAssign,
                  "TriggerDecision " << trigger_number << " could not be assigned",
                  ((uint32_t)trigger_number)) // NOLINT(build/unsigned)
ERS_DECLARE_ISSUE(dfmodules,
                  AssignedToBusyApp,
                  "TriggerDecision " << trigger_number << " was assigned to DF app " << app << " that was busy with "
                                     << used_slots << " TDs",
                  ((uint32_t)trigger_number)((std::string)app)((size_t)used_slots)) // NOLINT(build/unsigned)
// Re-enable coverage checking LCOV_EXCL_STOP

namespace dfmodules {

/**
 * @brief DFOModule distributes triggers according to the
 * availability of the DF apps in the system
 */
class DFOModule : public dunedaq::appfwk::DAQModule
{
public:
  /**
   * @brief DFOModule Constructor
   * @param name Instance name for this DFOModule instance
   */
  explicit DFOModule(const std::string& name);

  DFOModule(const DFOModule&) = delete; ///< DFOModule is not copy-constructible
  DFOModule& operator=(const DFOModule&) =
    delete;                                                         ///< DFOModule is not copy-assignable
  DFOModule(DFOModule&&) = delete;            ///< DFOModule is not move-constructible
  DFOModule& operator=(DFOModule&&) = delete; ///< DFOModule is not move-assignable

  void init(std::shared_ptr<appfwk::ModuleConfiguration> mcfg) override;

protected:
  virtual std::shared_ptr<AssignedTriggerDecision> find_slot(const dfmessages::TriggerDecision& decision);
  // find_slot operates on a round-robin logic

  using trbd_ptr_t = std::shared_ptr<TriggerRecordBuilderData>;
  using data_structure_t = std::map<std::string, trbd_ptr_t>;
  data_structure_t m_dataflow_availability;
  data_structure_t::iterator m_last_assignement_it;
  std::function<void(nlohmann::json&)> m_metadata_function;

private:
  // Commands
  void do_conf(const data_t&);
  void do_start(const data_t&);
  void do_stop(const data_t&);
  void do_scrap(const data_t&);

  void generate_opmon_data() override;

  virtual void receive_trigger_complete_token(const dfmessages::TriggerDecisionToken&);
  void receive_trigger_decision(const dfmessages::TriggerDecision&);
  virtual bool is_busy() const;
  bool is_empty() const;
  size_t used_slots() const;
  void notify_trigger(bool busy) const;
  bool dispatch(const std::shared_ptr<AssignedTriggerDecision>& assignment);
  virtual void assign_trigger_decision(const std::shared_ptr<AssignedTriggerDecision>& assignment);

  // Configuration
  const appmodel::DFOConf* m_dfo_conf;
  std::chrono::milliseconds m_queue_timeout;
  std::chrono::microseconds m_stop_timeout;
  dunedaq::daqdataformats::run_number_t m_run_number;

  // Connections
  std::shared_ptr<iomanager::SenderConcept<dfmessages::TriggerInhibit>> m_busy_sender;
  std::string m_token_connection;
  std::string m_td_connection;
  size_t m_td_send_retries;
  size_t m_busy_threshold;
  size_t m_free_threshold;
  std::vector<std::string> m_trb_conn_ids;

  // Coordination
  std::atomic<bool> m_running_status{ false };
  mutable std::atomic<bool> m_last_notified_busy{ false };
  std::chrono::steady_clock::time_point m_last_token_received;
  std::chrono::steady_clock::time_point m_last_td_received;

  // Struct for statistic
  struct TriggerData {
    std::atomic<uint64_t> received{0};
    std::atomic<uint64_t> completed{0};
  };
  static std::set<trgdataformats::TriggerCandidateData::Type>
  unpack_types( decltype(dfmessages::TriggerDecision::trigger_type) t) {
    std::set<trgdataformats::TriggerCandidateData::Type> results;
    if (t == dfmessages::TypeDefaults::s_invalid_trigger_type)
      return results;
    const std::bitset<64> bits(t);
    for( size_t i = 0; i < bits.size(); ++i ) {
      if ( bits[i] ) results.insert((trgdataformats::TriggerCandidateData::Type)i);
    }
    return results;
  }
  
  // Statistics
  std::atomic<uint64_t> m_received_tokens{ 0 };      // NOLINT (build/unsigned)
  std::atomic<uint64_t> m_sent_decisions{ 0 };       // NOLINT (build/unsigned)
  std::atomic<uint64_t> m_received_decisions{ 0 };   // NOLINT (build/unsigned)
  std::atomic<uint64_t> m_waiting_for_decision{ 0 }; // NOLINT (build/unsigned)
  std::atomic<uint64_t> m_deciding_destination{ 0 }; // NOLINT (build/unsigned)
  std::atomic<uint64_t> m_forwarding_decision{ 0 };  // NOLINT (build/unsigned)
  std::atomic<uint64_t> m_waiting_for_token{ 0 };    // NOLINT (build/unsigned)
  std::atomic<uint64_t> m_processing_token{ 0 };     // NOLINT (build/unsigned)
  std::map<dunedaq::trgdataformats::TriggerCandidateData::Type, TriggerData> m_trigger_counters;
  std::mutex m_trigger_mutex;  // used to safely handle the map above
  TriggerData & get_trigger_counter(trgdataformats::TriggerCandidateData::Type type) {
    auto it = m_trigger_counters.find(type);
    if (it != m_trigger_counters.end()) return it->second;
    
    std::lock_guard<std::mutex> guard(m_trigger_mutex);
    return m_trigger_counters[type];
  }
  
};
} // namespace dfmodules
} // namespace dunedaq

#endif // DFMODULES_PLUGINS_DATAFLOWORCHESTRATOR_HPP_
