/**
 * @file TRBModule.hpp
 *
 * This is part of the DUNE DAQ Software Suite, copyright 2020.
 * Licensing/copyright details are in the COPYING file that you should have
 * received with this code.
 */

#ifndef DFMODULES_PLUGINS_TRIGGERRECORDBUILDER_HPP_
#define DFMODULES_PLUGINS_TRIGGERRECORDBUILDER_HPP_

#include "appmodel/TRBConf.hpp"
#include "daqdataformats/Fragment.hpp"
#include "daqdataformats/SourceID.hpp"
#include "daqdataformats/TriggerRecord.hpp"
#include "daqdataformats/Types.hpp"
#include "appmodel/ReadoutApplication.hpp"
#include "appmodel/SmartDaqApplication.hpp"
#include "dfmessages/DataRequest.hpp"
#include "dfmessages/TRMonRequest.hpp"
#include "dfmessages/TriggerDecision.hpp"
#include "dfmessages/Types.hpp"

#include "appfwk/DAQModule.hpp"
#include "utilities/WorkerThread.hpp"
#include "iomanager/Sender.hpp"
#include "iomanager/Receiver.hpp"

#include "dfmodules/opmon/TRBModule.pb.h"

#include <chrono>
#include <list>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

namespace dunedaq {

namespace dfmodules {

/**
 * @brief TriggerId is a little class that defines a unique identifier for a
 * trigger decision/record It also provides an operator < to be used by map to
 * optimise bookkeeping
 */
struct TriggerId
{

  TriggerId() = default;

  explicit TriggerId(const dfmessages::TriggerDecision& td,
                     daqdataformats::sequence_number_t s = daqdataformats::TypeDefaults::s_invalid_sequence_number)
    : trigger_number(td.trigger_number)
    , sequence_number(s)
    , run_number(td.run_number)
  {
    ;
  }
  explicit TriggerId(daqdataformats::Fragment& f)
    : trigger_number(f.get_trigger_number())
    , sequence_number(f.get_sequence_number())
    , run_number(f.get_run_number())
  {
    ;
  }

  daqdataformats::trigger_number_t trigger_number;
  daqdataformats::sequence_number_t sequence_number;
  daqdataformats::run_number_t run_number;

  bool operator<(const TriggerId& other) const noexcept
  {
    return std::tuple(trigger_number, sequence_number, run_number) <
           std::tuple(other.trigger_number, other.sequence_number, other.run_number);
  }

  friend std::ostream& operator<<(std::ostream& out, const TriggerId& id) noexcept
  {
    out << id.trigger_number << '-' << id.sequence_number << '/' << id.run_number;
    return out;
  }

  friend TraceStreamer& operator<<(TraceStreamer& out, const TriggerId& id) noexcept
  {
    return out << id.trigger_number << '.' << id.sequence_number << "/" << id.run_number;
  }

  friend std::istream& operator>>(std::istream& in, TriggerId& id)
  {
    char t1, t2;
    in >> id.trigger_number >> t1 >> id.sequence_number >> t2 >> id.run_number;
    return in;
  }
};

} // namespace dfmodules

/**
 * @brief Unexpected trigger decision
 */
ERS_DECLARE_ISSUE(dfmodules,                 ///< Namespace
                  UnexpectedTriggerDecision, ///< Issue class name
                  "Unexpected Trigger Decisions: " << trigger << '/' << decision_run << " while in run " << current_run,
                  ((daqdataformats::trigger_number_t)trigger)  ///< Message parameters
                  ((daqdataformats::run_number_t)decision_run) ///< Message parameters
                  ((daqdataformats::run_number_t)current_run)  ///< Message parameters
)

/**
 * @brief Timed out Trigger Decision
 */
ERS_DECLARE_ISSUE(dfmodules,               ///< Namespace
                  TimedOutTriggerDecision, ///< Issue class name
                  "trigger id: " << trigger_id << " generate at: " << trigger_timestamp << " timed out", ///< Message
                  ((dfmodules::TriggerId)trigger_id)               ///< Message parameters
                  ((daqdataformats::timestamp_t)trigger_timestamp) ///< Message parameters
)

/**
 * @brief Unexpected fragment
 */
ERS_DECLARE_ISSUE(dfmodules,          ///< Namespace
                  UnexpectedFragment, ///< Issue class name
                  "Unexpected Fragment for triggerID " << trigger_id << ", type " << fragment_type << ", " << source_id,
                  ((dfmodules::TriggerId)trigger_id)               ///< Message parameters
                  ((daqdataformats::fragment_type_t)fragment_type) ///< Message parameters
                  ((daqdataformats::SourceID)source_id)                  ///< Message parameters
)

/**
 * @brief Duplicate trigger decision
 */
ERS_DECLARE_ISSUE(dfmodules,                 ///< Namespace
                  DuplicatedTriggerDecision, ///< Issue class name
                  "Duplicated trigger ID " << trigger_id,
                  ((dfmodules::TriggerId)trigger_id) ///< Message parameters
)

/**
 * @brief Abandoned TR
 */
ERS_DECLARE_ISSUE(dfmodules,                ///< Namespace
                  AbandonedTriggerDecision, ///< Issue class name
                  "trigger ID " << trigger_id << " could not be sent to writing and it's lost",
                  ((dfmodules::TriggerId)trigger_id) ///< Message parameters
)

/**
 * @brief Missing connection ID
 */
ERS_DECLARE_ISSUE(dfmodules,           ///< Namespace
                  MissingConnectionID, ///< Issue class name
                  "No connection ID was found for connection name \"" << conn_name
                  << "\" in the conn_ref list that was provided at 'init' time.",
                  ((std::string)conn_name)                   ///< Message parameters
)

namespace dfmodules {

/**
 * @brief TRBModule is the Module that collects Trigger
 TriggersDecisions, sends the corresponding data requests and collects Fragment
 to form a complete Trigger Record. The TR then sent out possibly to a write
 module
*/
class TRBModule : public dunedaq::appfwk::DAQModule
{
public:
  /**
   * @brief TRBModule Constructor
   * @param name Instance name for this TRBModule instance
   */
  explicit TRBModule(const std::string& name);

  TRBModule(const TRBModule&) = delete; ///< TRBModule is not copy-constructible
  TRBModule& operator=(const TRBModule&) =
    delete;                                                         ///< TRBModule is not copy-assignable
  TRBModule(TRBModule&&) = delete;            ///< TRBModule is not move-constructible
  TRBModule& operator=(TRBModule&&) = delete; ///< TRBModule is not move-assignable

  void init(std::shared_ptr<appfwk::ModuleConfiguration> mcfg) override;

  void generate_opmon_data() override;

protected:
  using trigger_decision_receiver_t = iomanager::ReceiverConcept<dfmessages::TriggerDecision>;
  using data_req_sender_t = iomanager::SenderConcept<dfmessages::DataRequest>;
  using fragment_receiver_t = iomanager::ReceiverConcept<std::unique_ptr<daqdataformats::Fragment>>;

  using trigger_record_ptr_t = std::unique_ptr<daqdataformats::TriggerRecord>;
  using trigger_record_sender_t = iomanager::SenderConcept<trigger_record_ptr_t>;

  bool read_fragments();

  bool read_and_process_trigger_decision(iomanager::Receiver::timeout_t, std::atomic<bool>& running);

  trigger_record_ptr_t extract_trigger_record(const TriggerId&);
  // build_trigger_record will allocate memory and then orphan it to the caller
  // via the returned pointer Plese note that the method will destroy the memory
  // saved in the bookkeeping map

  unsigned int create_trigger_records_and_dispatch(const dfmessages::TriggerDecision&, std::atomic<bool>& running);

  bool dispatch_data_requests(dfmessages::DataRequest,
                              const daqdataformats::SourceID&,
                              std::atomic<bool>& running);

  bool send_trigger_record(const TriggerId&, std::atomic<bool>& running);
  // this creates a trigger record and send it

  bool check_stale_requests(std::atomic<bool>& running);
  // it returns true when there are changes in the book = a TR timed out

private:
  // Commands
  void do_conf(const data_t&);
  void do_scrap(const data_t&);
  void do_start(const data_t&);
  void do_stop(const data_t&);

  // Monitoring callback
  void tr_requested(const dfmessages::TRMonRequest &);

  // Threading
  dunedaq::utilities::WorkerThread m_thread;
  void do_work(std::atomic<bool>&);

  // Configuration
  const appmodel::TRBConf* m_trb_conf;
  std::chrono::milliseconds m_queue_timeout;
  std::chrono::milliseconds m_loop_sleep;
  std::string m_reply_connection;
  daqdataformats::SourceID m_this_trb_source_id;

  // Input Connections
  std::shared_ptr<trigger_decision_receiver_t> m_trigger_decision_input;
  std::shared_ptr<fragment_receiver_t> m_fragment_input;

  // Output connections
  std::shared_ptr<trigger_record_sender_t> m_trigger_record_output;
  mutable std::mutex m_map_sourceid_connections_mutex;
  std::map<daqdataformats::SourceID, std::shared_ptr<data_req_sender_t>> m_map_sourceid_connections; ///< Mappinng between SourceID and connections

  // bookeeping
  using clock_type = std::chrono::high_resolution_clock;
  std::map<TriggerId, std::pair<clock_type::time_point, trigger_record_ptr_t>> m_trigger_records;

  // Data request properties
  daqdataformats::timestamp_diff_t m_max_time_window;

  // Run information
  std::unique_ptr<const daqdataformats::run_number_t> m_run_number = nullptr;

  // Monitoring related variables
  std::mutex m_mon_mutex;
  std::shared_ptr<iomanager::ReceiverConcept<dfmessages::TRMonRequest>> m_mon_receiver;
  std::list<dfmessages::TRMonRequest> m_mon_requests;

  // book related metrics
  using metric_counter_type = uint64_t; // decltype(triggerrecordbuilderinfo::Info::pending_trigger_decisions);
  mutable std::atomic<metric_counter_type> m_trigger_decisions_counter = { 0 }; // currently
  mutable std::atomic<metric_counter_type> m_fragment_counter = { 0 };          // currently
  mutable std::atomic<metric_counter_type> m_pending_fragment_counter = { 0 };  // currently

  mutable std::atomic<metric_counter_type> m_timed_out_trigger_records = { 0 };    // in the run
  mutable std::atomic<metric_counter_type> m_unexpected_fragments = { 0 };         // in the run
  mutable std::atomic<metric_counter_type> m_unexpected_trigger_decisions = { 0 }; // in the run
  mutable std::atomic<metric_counter_type> m_lost_fragments = { 0 };               // in the run
  mutable std::atomic<metric_counter_type> m_invalid_requests = { 0 };             // in the run
  mutable std::atomic<metric_counter_type> m_duplicated_trigger_ids = { 0 };       // in the run
  mutable std::atomic<metric_counter_type> m_abandoned_trigger_records = { 0 };    // in the run

  mutable std::atomic<metric_counter_type> m_received_trigger_decisions = { 0 }; // in between calls
  mutable std::atomic<metric_counter_type> m_generated_trigger_records = { 0 };  // in between calls
  mutable std::atomic<metric_counter_type> m_generated_data_requests = { 0 };    // in between calls
  mutable std::atomic<metric_counter_type> m_sleep_counter = { 0 };              // in between calls
  mutable std::atomic<metric_counter_type> m_loop_counter = { 0 };               // in between calls
  mutable std::atomic<metric_counter_type> m_data_waiting_time = { 0 };          // in between calls
  mutable std::atomic<metric_counter_type> m_trigger_decision_width = { 0 };     // in between calls
  mutable std::atomic<metric_counter_type> m_data_request_width = { 0 };         // in between calls

  mutable std::atomic<metric_counter_type> m_trmon_request_counter = { 0 };
  mutable std::atomic<metric_counter_type> m_trmon_sent_counter = { 0 };

  // time thresholds
  using duration_type = std::chrono::microseconds;
  duration_type m_old_trigger_threshold;
  duration_type m_trigger_timeout;
};
} // namespace dfmodules
} // namespace dunedaq

#endif // DFMODULES_PLUGINS_TRIGGERRECORDBUILDER_HPP_
