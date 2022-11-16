/**
 * @file DataFlowOrchestrator.cpp DataFlowOrchestrator class implementation
 *
 * This is part of the DUNE DAQ Software Suite, copyright 2020.
 * Licensing/copyright details are in the COPYING file that you should have
 * received with this code.
 */

#include "DataFlowOrchestrator.hpp"
#include "dfmodules/CommonIssues.hpp"

#include "dfmodules/datafloworchestrator/Nljs.hpp"
#include "dfmodules/datafloworchestratorinfo/InfoNljs.hpp"

#include "appfwk/DAQModuleHelper.hpp"
#include "appfwk/app/Nljs.hpp"
#include "iomanager/IOManager.hpp"
#include "logging/Logging.hpp"

#include <chrono>
#include <cstdlib>
#include <future>
#include <limits>
#include <list>
#include <map>
#include <memory>
#include <string>
#include <thread>
#include <utility>
#include <vector>

/**
 * @brief Name used by TRACE TLOG calls from this source file
 */
#define TRACE_NAME "DataFlowOrchestrator" // NOLINT
enum
{
  TLVL_ENTER_EXIT_METHODS = 5,
  TLVL_CONFIG = 7,
  TLVL_WORK_STEPS = 10,
  TLVL_TRIGDEC_RECEIVED = 21,
  TLVL_NOTIFY_TRIGGER = 22,
  TLVL_DISPATCH_TO_TRB = 23
};

namespace dunedaq::dfmodules {

DataFlowOrchestrator::DataFlowOrchestrator(const std::string& name)
  : dunedaq::appfwk::DAQModule(name)
  , m_queue_timeout(100)
  , m_run_number(0)
{
  register_command("conf", &DataFlowOrchestrator::do_conf);
  register_command("start", &DataFlowOrchestrator::do_start);
  register_command("drain_dataflow", &DataFlowOrchestrator::do_stop);
  register_command("scrap", &DataFlowOrchestrator::do_scrap);
}

void
DataFlowOrchestrator::init(const data_t& init_data)
{
  TLOG_DEBUG(TLVL_ENTER_EXIT_METHODS) << get_name() << ": Entering init() method";

  auto iom = iomanager::IOManager::get();
  auto mandatory_connections =
    appfwk::connection_index(init_data, { "token_connection", "td_connection", "busy_connection" });

  m_token_connection = mandatory_connections["token_connection"];
  m_td_connection = mandatory_connections["td_connection"];
  auto busy_connection = mandatory_connections["busy_connection"];

  // these are just tests to check if the connections are ok
  iom->get_receiver<dfmessages::TriggerDecisionToken>(m_token_connection);
  iom->get_receiver<dfmessages::TriggerDecision>(m_td_connection);
  m_busy_sender = iom->get_sender<dfmessages::TriggerInhibit>(busy_connection);

  TLOG_DEBUG(TLVL_ENTER_EXIT_METHODS) << get_name() << ": Exiting init() method";
}

void
DataFlowOrchestrator::do_conf(const data_t& payload)
{
  TLOG_DEBUG(TLVL_ENTER_EXIT_METHODS) << get_name() << ": Entering do_conf() method";

  auto parsed_conf = payload.get<datafloworchestrator::ConfParams>();

  m_queue_timeout = std::chrono::milliseconds(parsed_conf.general_queue_timeout);
  m_stop_timeout = std::chrono::microseconds(parsed_conf.stop_timeout * 1000);
  m_busy_threshold = parsed_conf.thresholds.busy;
  m_free_threshold = parsed_conf.thresholds.free;

  m_td_send_retries = parsed_conf.td_send_retries;

  TLOG_DEBUG(TLVL_ENTER_EXIT_METHODS) << get_name() << ": Exiting do_conf() method, there are "
                                      << m_dataflow_availability.size() << " TRB apps defined";
}

void
DataFlowOrchestrator::do_start(const data_t& payload)
{
  TLOG_DEBUG(TLVL_ENTER_EXIT_METHODS) << get_name() << ": Entering do_start() method";

  m_received_tokens = 0;
  m_run_number = payload.value<dunedaq::daqdataformats::run_number_t>("run", 0);

  m_running_status.store(true);
  m_last_notified_busy.store(false);
  m_last_assignement_it = m_dataflow_availability.end();

  m_last_token_received = m_last_td_received = std::chrono::steady_clock::now();

  auto iom = iomanager::IOManager::get();
  iom->add_callback<dfmessages::TriggerDecisionToken>(
    m_token_connection, std::bind(&DataFlowOrchestrator::receive_trigger_complete_token, this, std::placeholders::_1));

  iom->add_callback<dfmessages::TriggerDecision>(
    m_td_connection, std::bind(&DataFlowOrchestrator::receive_trigger_decision, this, std::placeholders::_1));

  TLOG_DEBUG(TLVL_ENTER_EXIT_METHODS) << get_name() << ": Exiting do_start() method";
}

void
DataFlowOrchestrator::do_stop(const data_t& /*args*/)
{
  TLOG_DEBUG(TLVL_ENTER_EXIT_METHODS) << get_name() << ": Entering do_stop() method";

  m_running_status.store(false);

  auto iom = iomanager::IOManager::get();
  iom->remove_callback<dfmessages::TriggerDecision>(m_td_connection);

  const int wait_steps = 20;
  auto step_timeout = m_stop_timeout / wait_steps;
  int step_counter = 0;
  while (!is_empty() && step_counter < wait_steps) {
    TLOG() << get_name() << ": stop delayed while waiting for " << used_slots() << " TDs to completed";
    std::this_thread::sleep_for(step_timeout);
    ++step_counter;
  }

  iom->remove_callback<dfmessages::TriggerDecisionToken>(m_token_connection);

  std::list<std::shared_ptr<AssignedTriggerDecision>> remnants;
  for (auto& app : m_dataflow_availability) {
    auto temp = app.second.flush();
    for (auto& td : temp) {
      remnants.push_back(td);
    }
  }

  for (auto& r : remnants) {
    ers::error(IncompleteTriggerDecision(ERS_HERE, r->decision.trigger_number, m_run_number));
  }

  TLOG() << get_name() << " successfully stopped";
  TLOG_DEBUG(TLVL_ENTER_EXIT_METHODS) << get_name() << ": Exiting do_stop() method";
}

void
DataFlowOrchestrator::do_scrap(const data_t& /*args*/)
{
  TLOG_DEBUG(TLVL_ENTER_EXIT_METHODS) << get_name() << ": Entering do_scrap() method";

  m_dataflow_availability.clear();

  TLOG() << get_name() << " successfully scrapped";
  TLOG_DEBUG(TLVL_ENTER_EXIT_METHODS) << get_name() << ": Exiting do_scrap() method";
}

void
DataFlowOrchestrator::receive_trigger_decision(const dfmessages::TriggerDecision& decision)
{
  TLOG_DEBUG(TLVL_TRIGDEC_RECEIVED) << get_name() << " Received TriggerDecision for trigger_number "
                                    << decision.trigger_number << " and run " << decision.run_number
                                    << " (current run is " << m_run_number << ")";
  if (decision.run_number != m_run_number) {
    ers::error(DataFlowOrchestratorRunNumberMismatch(
      ERS_HERE, decision.run_number, m_run_number, "MLT", decision.trigger_number));
    return;
  }

  ++m_received_decisions;
  auto decision_received = std::chrono::steady_clock::now();

  std::chrono::steady_clock::time_point decision_assigned;
  do {

    auto assignment = find_slot(decision);

    if (assignment == nullptr) { // this can happen if all application are in error state
      ers::error(UnableToAssign(ERS_HERE, decision.trigger_number));
      usleep(500);
      continue;
    }

    TLOG_DEBUG(TLVL_TRIGDEC_RECEIVED) << get_name() << " Slot found for trigger_number " << decision.trigger_number
                                      << " on connection " << assignment->connection_name
                                      << ", number of used slots is " << used_slots();
    decision_assigned = std::chrono::steady_clock::now();
    auto dispatch_successful = dispatch(assignment);

    if (dispatch_successful) {
      assign_trigger_decision(assignment);
      TLOG_DEBUG(TLVL_TRIGDEC_RECEIVED) << get_name() << " Assigned trigger_number " << decision.trigger_number
                                        << " to connection " << assignment->connection_name;
      break;
    } else {
      ers::error(
        TriggerRecordBuilderAppUpdate(ERS_HERE, assignment->connection_name, "Could not send Trigger Decision"));
      m_dataflow_availability[assignment->connection_name].set_in_error(true);
    }

  } while (m_running_status.load());

  notify_trigger(is_busy());

  m_waiting_for_decision +=
    std::chrono::duration_cast<std::chrono::microseconds>(decision_received - m_last_td_received).count();
  m_last_td_received = std::chrono::steady_clock::now();
  m_deciding_destination +=
    std::chrono::duration_cast<std::chrono::microseconds>(decision_assigned - decision_received).count();
  m_forwarding_decision +=
    std::chrono::duration_cast<std::chrono::microseconds>(m_last_td_received - decision_assigned).count();
}

std::shared_ptr<AssignedTriggerDecision>
DataFlowOrchestrator::find_slot(const dfmessages::TriggerDecision& decision)
{

  // this find_slot assings the decision with a round-robin logic
  // across all the available applications.
  // Applications in error are skipped.
  // we only probe the applications once.
  // if they are all unavailable the assignment is set to
  // the application with the lowest used slots
  // returning a nullptr will be considered as an error
  // from the upper level code

  std::shared_ptr<AssignedTriggerDecision> output = nullptr;
  auto minimum_occupied = m_dataflow_availability.end();
  size_t minimum = std::numeric_limits<size_t>::max();
  unsigned int counter = 0;

  auto candidate_it = m_last_assignement_it;
  if (candidate_it == m_dataflow_availability.end())
    candidate_it = m_dataflow_availability.begin();

  while (output == nullptr && counter < m_dataflow_availability.size()) {

    ++counter;
    ++candidate_it;
    if (candidate_it == m_dataflow_availability.end())
      candidate_it = m_dataflow_availability.begin();

    // get rid of the applications in error state
    if (candidate_it->second.is_in_error()) {
      continue;
    }

    // monitor
    auto slots = candidate_it->second.used_slots();
    if (slots < minimum) {
      minimum = slots;
      minimum_occupied = candidate_it;
    }

    if (candidate_it->second.is_busy())
      continue;

    output = candidate_it->second.make_assignment(decision);
    m_last_assignement_it = candidate_it;
  }

  if (!output) {
    // in this case all applications were busy
    // so we assign the decision to that with the lowest
    // number of assignments
    if (minimum_occupied != m_dataflow_availability.end()) {
      output = minimum_occupied->second.make_assignment(decision);
      m_last_assignement_it = minimum_occupied;
      ers::warning(AssignedToBusyApp(ERS_HERE, decision.trigger_number, minimum_occupied->first, minimum));
    }
  }

  if (output != nullptr) {
    TLOG_DEBUG(TLVL_WORK_STEPS) << "Assigned TriggerDecision with trigger number " << decision.trigger_number
                                << " to TRB at connection " << output->connection_name;
  }
  return output;
}

void
DataFlowOrchestrator::get_info(opmonlib::InfoCollector& ci, int level)
{

  for (auto& [name, app] : m_dataflow_availability) {
    opmonlib::InfoCollector tmp_ic;
    app.get_info(tmp_ic, level);
    ci.add(name, tmp_ic);
  }

  datafloworchestratorinfo::Info info;
  info.tokens_received = m_received_tokens.exchange(0);
  info.decisions_sent = m_sent_decisions.exchange(0);
  info.decisions_received = m_received_decisions.exchange(0);
  info.waiting_for_decision = m_waiting_for_decision.exchange(0);
  info.deciding_destination = m_deciding_destination.exchange(0);
  info.forwarding_decision = m_forwarding_decision.exchange(0);
  info.waiting_for_token = m_waiting_for_token.exchange(0);
  info.processing_token = m_processing_token.exchange(0);
  ci.add(info);
}

void
DataFlowOrchestrator::receive_trigger_complete_token(const dfmessages::TriggerDecisionToken& token)
{
  if (token.run_number == 0 && token.trigger_number == 0) {
    if (m_dataflow_availability.count(token.decision_destination) == 0) {
      TLOG_DEBUG(TLVL_CONFIG) << "Creating dataflow availability struct for uid " << token.decision_destination;
      m_dataflow_availability[token.decision_destination] =
        TriggerRecordBuilderData(token.decision_destination, m_busy_threshold, m_free_threshold);
    } else {
      TLOG() << "Ignoring initialization token from " << token.decision_destination << ", already configured";
    }
    return;
  }

  TLOG() << get_name() << " Received TriggerDecisionToken for trigger_number " << token.trigger_number << " and run "
         << token.run_number << " (current run is " << m_run_number << ")";
  // add a check to see if the application data found
  if (token.run_number != m_run_number) {
    std::ostringstream oss_source;
    oss_source << "TRB at connection " << token.decision_destination;
    ers::error(DataFlowOrchestratorRunNumberMismatch(
      ERS_HERE, token.run_number, m_run_number, oss_source.str(), token.trigger_number));
    return;
  }

  auto app_it = m_dataflow_availability.find(token.decision_destination);
  // check if application data exists;
  if (app_it == m_dataflow_availability.end()) {
    ers::error(UnknownTokenSource(ERS_HERE, token.decision_destination));
    return;
  }

  ++m_received_tokens;
  auto callback_start = std::chrono::steady_clock::now();

  try {
    auto dec_ptr = app_it->second.complete_assignment(token.trigger_number, m_metadata_function);
  } catch (AssignedTriggerDecisionNotFound const& err) {
    ers::error(err);
  }

  if (app_it->second.is_in_error()) {
    TLOG() << TriggerRecordBuilderAppUpdate(ERS_HERE, token.decision_destination, "Has reconnected");
    app_it->second.set_in_error(false);
  }

  if (!app_it->second.is_busy()) {
    notify_trigger(false);
  }

  m_waiting_for_token +=
    std::chrono::duration_cast<std::chrono::microseconds>(callback_start - m_last_token_received).count();
  m_last_token_received = std::chrono::steady_clock::now();
  m_processing_token +=
    std::chrono::duration_cast<std::chrono::microseconds>(m_last_token_received - callback_start).count();
}

bool
DataFlowOrchestrator::is_busy() const
{
  for (auto& dfapp : m_dataflow_availability) {
    if (!dfapp.second.is_busy())
      return false;
  }
  return true;
}

bool
DataFlowOrchestrator::is_empty() const
{
  for (auto& dfapp : m_dataflow_availability) {
    if (dfapp.second.used_slots() != 0)
      return false;
  }
  return true;
}

size_t
DataFlowOrchestrator::used_slots() const
{
  size_t total = 0;
  for (auto& dfapp : m_dataflow_availability) {
    total += dfapp.second.used_slots();
  }
  return total;
}

void
DataFlowOrchestrator::notify_trigger(bool busy) const
{

  if (busy == m_last_notified_busy.load())
    return;

  bool wasSentSuccessfully = false;

  do {
    try {
      dfmessages::TriggerInhibit message{ busy, m_run_number };
      m_busy_sender->send(std::move(message), m_queue_timeout);
      wasSentSuccessfully = true;
      TLOG_DEBUG(TLVL_NOTIFY_TRIGGER) << get_name() << " Sent BUSY status " << busy << " to trigger in run "
                                      << m_run_number;
    } catch (const ers::Issue& excpt) {
      std::ostringstream oss_warn;
      oss_warn << "Send with sender \"" << m_busy_sender->get_name() << "\" failed";
      ers::warning(iomanager::OperationFailed(ERS_HERE, oss_warn.str(), excpt));
    }

  } while (!wasSentSuccessfully && m_running_status.load());

  m_last_notified_busy.store(busy);
}

bool
DataFlowOrchestrator::dispatch(const std::shared_ptr<AssignedTriggerDecision>& assignment)
{

  TLOG_DEBUG(TLVL_ENTER_EXIT_METHODS) << get_name() << ": Entering dispatch() method. assignment->connection_name: "
                                      << assignment->connection_name;

  bool wasSentSuccessfully = false;
  int retries = m_td_send_retries;
  auto iom = iomanager::IOManager::get();
  do {

    try {
      auto decision_copy = dfmessages::TriggerDecision(assignment->decision);
      iom->get_sender<dfmessages::TriggerDecision>(assignment->connection_name)
        ->send(std::move(decision_copy), m_queue_timeout);
      wasSentSuccessfully = true;
      ++m_sent_decisions;
      TLOG_DEBUG(TLVL_DISPATCH_TO_TRB) << get_name() << " Sent TriggerDecision for trigger_number "
                                       << decision_copy.trigger_number << " to TRB at connection "
                                       << assignment->connection_name << " for run number " << decision_copy.run_number;
    } catch (const ers::Issue& excpt) {
      std::ostringstream oss_warn;
      oss_warn << "Send to connection \"" << assignment->connection_name << "\" failed";
      ers::warning(iomanager::OperationFailed(ERS_HERE, oss_warn.str(), excpt));
    }

    retries--;

  } while (!wasSentSuccessfully && m_running_status.load() && retries > 0);

  TLOG_DEBUG(TLVL_ENTER_EXIT_METHODS) << get_name() << ": Exiting dispatch() method";
  return wasSentSuccessfully;
}

void
DataFlowOrchestrator::assign_trigger_decision(const std::shared_ptr<AssignedTriggerDecision>& assignment)
{
  m_dataflow_availability[assignment->connection_name].add_assignment(assignment);
}

} // namespace dunedaq::dfmodules

DEFINE_DUNE_DAQ_MODULE(dunedaq::dfmodules::DataFlowOrchestrator)
