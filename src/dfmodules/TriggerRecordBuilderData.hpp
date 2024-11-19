/**
 * @file TriggerRecordBuilderData.hpp TriggerRecordBuilderData Class
 *
 * The TriggerRecordBuilderData class represents the current state of a TRBModule's Trigger Record buffers
 * for use by the DFO.
 *
 * This is part of the DUNE DAQ Application Framework, copyright 2020.
 * Licensing/copyright details are in the COPYING file that you should have
 * received with this code.
 */

#ifndef DFMODULES_SRC_DFMODULES_TRIGGERRECORDBUILDERDATA_HPP_
#define DFMODULES_SRC_DFMODULES_TRIGGERRECORDBUILDERDATA_HPP_

#include "daqdataformats/Types.hpp"
#include "dfmessages/TriggerDecision.hpp"
#include "dfmodules/opmon/TRBuilderData.pb.h"

#include "ers/Issue.hpp"
#include "nlohmann/json.hpp"
#include "opmonlib/MonitorableObject.hpp"

#include <atomic>
#include <chrono>
#include <functional>
#include <limits>
#include <list>
#include <memory>
#include <mutex>
#include <string>
#include <utility>

namespace dunedaq {
// Disable coverage checking LCOV_EXCL_START
ERS_DECLARE_ISSUE(dfmodules,
                  DFOThresholdsNotConsistent,
                  "Busy threshold (" << busy << ") is smaller than free threshold (" << free << ')',
                  ((size_t)busy)((size_t)free))
ERS_DECLARE_ISSUE(dfmodules,
                  AssignedTriggerDecisionNotFound,
                  "The Trigger Decision with trigger number "
                    << trigger_number << " was not found for dataflow application at " << connection_name,
                  ((daqdataformats::trigger_number_t)trigger_number)((std::string)connection_name))
ERS_DECLARE_ISSUE(dfmodules,
                  NoSlotsAvailable,
                  "The Trigger Decision with trigger number "
                    << trigger_number << " could not be assigned to the dataflow application at " << connection_name
                    << " because no slots were available.",
                  ((daqdataformats::trigger_number_t)trigger_number)((std::string)connection_name))
// Re-enable coverage checking LCOV_EXCL_STOP

namespace dfmodules {
struct AssignedTriggerDecision
{
  dfmessages::TriggerDecision decision;
  std::chrono::steady_clock::time_point assigned_time;
  std::string connection_name;

  AssignedTriggerDecision(dfmessages::TriggerDecision dec, std::string conn_name)
    : decision(dec)
    , assigned_time(std::chrono::steady_clock::now())
    , connection_name(conn_name)
  {}
};

class TriggerRecordBuilderData : public opmonlib::MonitorableObject
{
public:
  TriggerRecordBuilderData() = default;
  TriggerRecordBuilderData(std::string connection_name, size_t busy_threshold);
  TriggerRecordBuilderData(std::string connection_name, size_t busy_threshold, size_t free_threshold);

  TriggerRecordBuilderData(TriggerRecordBuilderData const&) = delete;
  TriggerRecordBuilderData(TriggerRecordBuilderData&&) = delete;
  TriggerRecordBuilderData& operator=(TriggerRecordBuilderData const&) = delete;
  TriggerRecordBuilderData& operator=(TriggerRecordBuilderData&&) = delete;

  ~TriggerRecordBuilderData() = default;

  bool is_busy() const { return m_in_error || m_is_busy; }
  size_t used_slots() const { return m_assigned_trigger_decisions.size(); }

  size_t busy_threshold() const { return m_busy_threshold.load(); }
  size_t free_threshold() const { return m_free_threshold.load(); }

  std::shared_ptr<AssignedTriggerDecision> get_assignment(daqdataformats::trigger_number_t trigger_number) const;
  std::shared_ptr<AssignedTriggerDecision> extract_assignment(daqdataformats::trigger_number_t trigger_number);
  std::shared_ptr<AssignedTriggerDecision> make_assignment(dfmessages::TriggerDecision decision);
  void add_assignment(std::shared_ptr<AssignedTriggerDecision> assignment);
  std::shared_ptr<AssignedTriggerDecision> complete_assignment(
    daqdataformats::trigger_number_t trigger_number,
    std::function<void(nlohmann::json&)> metadata_fun = nullptr);
  std::list<std::shared_ptr<AssignedTriggerDecision>> flush();
  bool has_assignment(daqdataformats::trigger_number_t trigger_number) const;

  void generate_opmon_data() override;

  std::chrono::microseconds average_latency(std::chrono::steady_clock::time_point since) const;

  bool is_in_error() const { return m_in_error.load(); }
  void set_in_error(bool err) { m_in_error = err; }

  std::vector<dfmessages::trigger_number_t> extract_completions_to_acknowledge();
  void update_completions_to_acknowledge_list(std::vector<dfmessages::trigger_number_t> completions);

private:
  std::atomic<size_t> m_busy_threshold{ 0 };
  std::atomic<size_t> m_free_threshold{ std::numeric_limits<size_t>::max() };
  std::atomic<bool> m_is_busy{ false };
  std::list<std::shared_ptr<AssignedTriggerDecision>> m_assigned_trigger_decisions;
  std::set<dfmessages::trigger_number_t> completions_to_acknowledge;
  mutable std::mutex m_completions_to_acknowledge_mutex;
  mutable std::mutex m_assigned_trigger_decisions_mutex;

  // TODO: Eric Flumerfelt <eflumerf@github.com> Dec-03-2021: Replace with circular buffer
  std::list<std::pair<std::chrono::steady_clock::time_point, std::chrono::microseconds>> m_latency_info;
  mutable std::mutex m_latency_info_mutex;

  std::atomic<bool> m_in_error{ true };

  nlohmann::json m_metadata;
  std::string m_connection_name{ "" };

  // monitoring
  using metric_t = dunedaq::dfmodules::opmon::DFApplicationInfo;
  using const_time_counter_t = std::invoke_result<decltype(&metric_t::min_time_since_assignment),
						  metric_t>::type;
  using time_counter_t = std::remove_const<const_time_counter_t>::type;
  std::atomic<uint32_t> m_complete_counter{ 0 };
  std::atomic<time_counter_t> m_min_complete_time{ std::numeric_limits<time_counter_t>::max() }, m_max_complete_time{ 0 };  // in us
  double m_last_average_time{0.};
};
} // namespace dfmodules
} // namespace dunedaq

#endif // DFMODULES_SRC_DFMODULES_TRIGGERRECORDBUILDERDATA_HPP_
