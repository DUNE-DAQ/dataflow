/**
 * @file TriggerRecordBuilderData.cpp TriggerRecordBuilderData Class Implementation
 *
 * The TriggerRecordBuilderData class represents the current state of a dataflow application's Trigger Record buffers
 * for use by the DFO.
 *
 * This is part of the DUNE DAQ Application Framework, copyright 2020.
 * Licensing/copyright details are in the COPYING file that you should have
 * received with this code.
 */

#include "dfmodules/TriggerRecordBuilderData.hpp"
#include "dfmodules/opmon/TRBuilderData.pb.h"

#include "logging/Logging.hpp"

#include <limits>
#include <memory>
#include <string>
#include <utility>

/**
 * @brief Name used by TRACE TLOG calls from this source file
 */
#define TRACE_NAME "TRBData" // NOLINT

namespace dunedaq {
namespace dfmodules {

TriggerRecordBuilderData::TriggerRecordBuilderData(std::string connection_name, size_t busy_threshold)
  : m_busy_threshold(busy_threshold)
  , m_free_threshold(busy_threshold)
  , m_is_busy(false)
  , m_in_error(false)
  , m_connection_name(connection_name)
{
}

TriggerRecordBuilderData::TriggerRecordBuilderData(std::string connection_name,
                                                   size_t busy_threshold,
                                                   size_t free_threshold)
  : m_busy_threshold(busy_threshold)
  , m_free_threshold(busy_threshold)
  , m_is_busy(false)
  , m_in_error(false)
  , m_connection_name(connection_name)
{
  if (busy_threshold < free_threshold)
    throw dfmodules::DFOThresholdsNotConsistent(ERS_HERE, busy_threshold, free_threshold);
}

std::shared_ptr<AssignedTriggerDecision>
TriggerRecordBuilderData::get_assignment(daqdataformats::trigger_number_t trigger_number) const
{
  auto lk = std::lock_guard<std::mutex>(m_assigned_trigger_decisions_mutex);
  for (auto ptr : m_assigned_trigger_decisions) {
    if (ptr->decision.trigger_number == trigger_number) {
      return ptr;
    }
  }

  return nullptr;
}

bool
TriggerRecordBuilderData::has_assignment(daqdataformats::trigger_number_t trigger_number) const
{
  auto lk = std::lock_guard<std::mutex>(m_assigned_trigger_decisions_mutex);
  for (auto ptr : m_assigned_trigger_decisions) {
    if (ptr->decision.trigger_number == trigger_number) {
      return true;
    }
  }

  return false;
}

std::shared_ptr<AssignedTriggerDecision>
TriggerRecordBuilderData::extract_assignment(daqdataformats::trigger_number_t trigger_number)
{
  std::shared_ptr<AssignedTriggerDecision> dec_ptr;
  auto lk = std::lock_guard<std::mutex>(m_assigned_trigger_decisions_mutex);
  for (auto it = m_assigned_trigger_decisions.begin(); it != m_assigned_trigger_decisions.end(); ++it) {
    if ((*it)->decision.trigger_number == trigger_number) {
      dec_ptr = *it;
      m_assigned_trigger_decisions.erase(it);
      break;
    }
  }

  if (m_assigned_trigger_decisions.size() < m_free_threshold.load())
    m_is_busy.store(false);

  return dec_ptr;
}

std::shared_ptr<AssignedTriggerDecision>
TriggerRecordBuilderData::make_assignment(dfmessages::TriggerDecision decision)
{
  return std::make_shared<AssignedTriggerDecision>(decision, m_connection_name);
}

void
TriggerRecordBuilderData::add_assignment(std::shared_ptr<AssignedTriggerDecision> assignment)
{
  auto lk = std::lock_guard<std::mutex>(m_assigned_trigger_decisions_mutex);

  if (is_in_error())
    throw NoSlotsAvailable(ERS_HERE, assignment->decision.trigger_number, m_connection_name);

  m_assigned_trigger_decisions.push_back(assignment);
  TLOG_DEBUG(13) << "Size of assigned_trigger_decision list is " << m_assigned_trigger_decisions.size();

  if (m_assigned_trigger_decisions.size() >= m_busy_threshold.load()) {
    m_is_busy.store(true);
  }
}

std::shared_ptr<AssignedTriggerDecision>
TriggerRecordBuilderData::complete_assignment(daqdataformats::trigger_number_t trigger_number,
                                              std::function<void(nlohmann::json&)> metadata_fun)
{

  auto dec_ptr = extract_assignment(trigger_number);

  if (dec_ptr == nullptr)
    throw AssignedTriggerDecisionNotFound(ERS_HERE, trigger_number, m_connection_name);

  auto now = std::chrono::steady_clock::now();
  auto time = std::chrono::duration_cast<std::chrono::microseconds>(now - dec_ptr->assigned_time);
  {
    auto lk = std::lock_guard<std::mutex>(m_latency_info_mutex);
    m_latency_info.emplace_back(now, time);

    if (m_latency_info.size() > 1000)
      m_latency_info.pop_front();
  }

  if (metadata_fun)
    metadata_fun(m_metadata);

  ++m_complete_counter;
  auto completion_time =
    std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - dec_ptr->assigned_time);
  if (completion_time.count() < m_min_complete_time.load())
    m_min_complete_time.store(completion_time.count());
  if (completion_time.count() > m_max_complete_time.load())
    m_max_complete_time.store(completion_time.count());

  opmon::TRCompleteInfo i;
  i.set_completion_time(completion_time.count());
  i.set_tr_number(dec_ptr->decision.trigger_number);
  i.set_run_number(dec_ptr->decision.run_number);
  i.set_trigger_type(dec_ptr->decision.trigger_type);
  publish(std::move(i), {}, opmonlib::to_level(opmonlib::EntryOpMonLevel::kEventDriven));

  return dec_ptr;
}

std::list<std::shared_ptr<AssignedTriggerDecision>>
TriggerRecordBuilderData::flush()
{

  auto lk = std::lock_guard<std::mutex>(m_assigned_trigger_decisions_mutex);
  std::list<std::shared_ptr<AssignedTriggerDecision>> ret;

  for (const auto& td : m_assigned_trigger_decisions) {
    ret.push_back(td);
  }
  m_assigned_trigger_decisions.clear();

  auto stat_lock = std::lock_guard<std::mutex>(m_latency_info_mutex);
  m_latency_info.clear();
  m_is_busy = false;

  m_in_error = false;
  m_metadata = nlohmann::json();

  return ret;
}

void
TriggerRecordBuilderData::generate_opmon_data()
{
  metric_t info;
  info.set_min_time_since_assignment(std::numeric_limits<time_counter_t>::max());
  info.set_max_time_since_assignment(0);

  time_counter_t time = 0;

  auto lk = std::unique_lock<std::mutex>(m_assigned_trigger_decisions_mutex);
  info.set_outstanding_decisions(m_assigned_trigger_decisions.size());
  auto current_time = std::chrono::steady_clock::now();
  for (const auto& dec_ptr : m_assigned_trigger_decisions) {
    auto us_since_assignment =
      std::chrono::duration_cast<std::chrono::microseconds>(current_time - dec_ptr->assigned_time);
    time += us_since_assignment.count();
    if (us_since_assignment.count() < info.min_time_since_assignment())
      info.set_min_time_since_assignment(us_since_assignment.count());
    if (us_since_assignment.count() > info.max_time_since_assignment())
      info.set_max_time_since_assignment(us_since_assignment.count());
  }
  lk.unlock();

  info.set_total_time_since_assignment(time);

  // estimate of the capcity
  auto completed_trigger_records = m_complete_counter.exchange(0);
  if (completed_trigger_records > 0) {
    m_last_average_time =
      1e-6 * 0.5 * (m_min_complete_time.exchange(0) + m_max_complete_time.exchange(0)); // in seconds
  }

  if ( m_last_average_time > 0. ) {
    // prediction rate metrics
    info.set_capacity_rate( 0.5*(m_busy_threshold.load()+m_free_threshold.load())/m_last_average_time );
  }
  
  publish(std::move(info));
}

std::chrono::microseconds
TriggerRecordBuilderData::average_latency(std::chrono::steady_clock::time_point since) const
{
  auto lk = std::lock_guard<std::mutex>(m_latency_info_mutex);
  std::chrono::microseconds sum = std::chrono::microseconds(0);
  size_t count = 0;
  for (auto it = m_latency_info.rbegin(); it != m_latency_info.rend(); ++it) {
    if (it->first < since)
      break;

    count++;
    sum += it->second;
  }

  return sum / count;
}

std::vector<dfmessages::trigger_number_t>
TriggerRecordBuilderData::extract_completions_to_acknowledge()
{
  std::lock_guard<std::mutex> lk(m_completions_to_acknowledge_mutex);
  std::vector<dfmessages::trigger_number_t> output(completions_to_acknowledge.begin(),
                                                   completions_to_acknowledge.end());
  completions_to_acknowledge.clear();
  return output;
}

void
TriggerRecordBuilderData::update_completions_to_acknowledge_list(std::vector<dfmessages::trigger_number_t> completions)
{
  std::lock_guard<std::mutex> lk(m_completions_to_acknowledge_mutex);
  for (auto& tn : completions) {
    completions_to_acknowledge.insert(tn);
  }
}

} // namespace dfmodules
} // namespace dunedaq
