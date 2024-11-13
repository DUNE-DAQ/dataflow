/**
 * @file DFOBrokerModule.cpp DFOBrokerModule class implementation
 *
 * This is part of the DUNE DAQ Software Suite, copyright 2020.
 * Licensing/copyright details are in the COPYING file that you should have
 * received with this code.
 */

#include "DFOBrokerModule.hpp"
#include "dfmodules/CommonIssues.hpp"

#include "dfmodules/opmon/DFOBrokerModule.pb.h"

#include "appmodel/DFOApplication.hpp"
#include "appmodel/DFOBrokerModule.hpp"
#include "confmodel/Application.hpp"
#include "confmodel/Connection.hpp"
#include "confmodel/Session.hpp"
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
#define TRACE_NAME "DFOBrokerModule" // NOLINT
enum
{
  TLVL_ENTER_EXIT_METHODS = 5,
  TLVL_CONFIG = 7,
  TLVL_WORK_STEPS = 10,
  TLVL_TRIGDEC_RECEIVED = 21,
  TLVL_NOTIFY_TRIGGER = 22,
  TLVL_DISPATCH_TO_TRB = 23,
  TLVL_TDTOKEN_RECEIVED = 24
};

namespace dunedaq::dfmodules {

DFOBrokerModule::DFOBrokerModule(const std::string& name)
  : dunedaq::appfwk::DAQModule(name)
  , m_thread(std::bind(&DFOBrokerModule::heartbeat_thread_proc, this, std::placeholders::_1))
{
  register_command("conf", &DFOBrokerModule::do_conf);
  register_command("scrap", &DFOBrokerModule::do_scrap);
  register_command("start", &DFOBrokerModule::do_start);
  register_command("stop", &DFOBrokerModule::do_stop);
  register_command("enable_dfo", &DFOBrokerModule::do_enable_dfo);
}

void
DFOBrokerModule::init(std::shared_ptr<appfwk::ModuleConfiguration> mcfg)
{
  TLOG_DEBUG(TLVL_ENTER_EXIT_METHODS) << get_name() << ": Entering init() method";

  auto mdal = mcfg->module<appmodel::DFOBrokerModule>(get_name());
  if (!mdal) {
    throw appfwk::CommandFailed(ERS_HERE, "init", get_name(), "Unable to retrieve configuration object");
  }
  auto iom = iomanager::IOManager::get();

  for (auto con : mdal->get_inputs()) {
    if (con->get_data_type() == datatype_to_string<dfmessages::TriggerDecisionToken>()) {
      m_token_connection = con->UID();
    }
    if (con->get_data_type() == datatype_to_string<dfmessages::DFODecision>()) {
      m_dfod_connection = con->UID();
    }
  }
  for (auto con : mdal->get_outputs()) {
    if (con->get_data_type() == datatype_to_string<dfmessages::TriggerDecision>()) {
      m_td_connection = con->UID();
    }
    if (con->get_data_type() == datatype_to_string<dfmessages::DataflowHeartbeat>()) {
      m_heartbeat_connection = con->UID();
    }
  }

  if (m_token_connection == "") {
    throw appfwk::MissingConnection(
      ERS_HERE, get_name(), datatype_to_string<dfmessages::TriggerDecisionToken>(), "input");
  }
  if (m_dfod_connection == "") {
    throw appfwk::MissingConnection(ERS_HERE, get_name(), datatype_to_string<dfmessages::DFODecision>(), "input");
  }
  if (m_heartbeat_connection == "") {
    throw appfwk::MissingConnection(
      ERS_HERE, get_name(), datatype_to_string<dfmessages::DataflowHeartbeat>(), "output");
  }
  if (m_td_connection == "") {
    throw appfwk::MissingConnection(ERS_HERE, get_name(), datatype_to_string<dfmessages::TriggerDecision>(), "output");
  }

  // these are just tests to check if the connections are ok
  iom->get_receiver<dfmessages::TriggerDecisionToken>(m_token_connection);
  iom->get_receiver<dfmessages::DFODecision>(m_dfod_connection);
  iom->get_sender<dfmessages::DataflowHeartbeat>(m_heartbeat_connection); // Should be pub/sub, so this is a bind

  const confmodel::Session* session = mcfg->configuration_manager()->session();
  for (auto& app : session->get_all_applications()) {
    auto dfoapp = app->cast<appmodel::DFOApplication>();
    if (dfoapp != nullptr) {
      // if (!dfoapp->disabled(*session)) {
      m_dfo_information[dfoapp->UID()] = {};
      //}
    }
  }

  m_dfobroker_conf = mdal->get_configuration();

  TLOG_DEBUG(TLVL_ENTER_EXIT_METHODS) << get_name() << ": Exiting init() method";
}

void
DFOBrokerModule::do_conf(const data_t&)
{
  TLOG_DEBUG(TLVL_ENTER_EXIT_METHODS) << get_name() << ": Entering do_conf() method";
  m_send_heartbeat_interval = std::chrono::milliseconds(m_dfobroker_conf->get_send_heartbeat_interval_ms());
  m_send_heartbeat_timeout = std::chrono::milliseconds(m_dfobroker_conf->get_send_heartbeat_timeout_ms());
  m_td_timeout = std::chrono::milliseconds(m_dfobroker_conf->get_td_timeout_ms());
  m_stop_timeout = std::chrono::milliseconds(m_dfobroker_conf->get_stop_timeout_ms());

  auto initial_active_dfo = m_dfobroker_conf->get_initial_active_dfo();
  if (initial_active_dfo != nullptr) {
    auto enabled_dfo_id = initial_active_dfo->UID();

    std::lock_guard<std::mutex> lk(m_dfo_info_mutex);
    for (auto& dfo_pair : m_dfo_information) {
      if (dfo_pair.first == enabled_dfo_id) {
        dfo_pair.second.dfo_is_active = true;
      } else {
        dfo_pair.second.dfo_is_active = false;
      }
    }
  }
  TLOG_DEBUG(TLVL_ENTER_EXIT_METHODS) << get_name() << ": Exiting do_conf() method, there are "
                                      << m_dfo_information.size() << " DFO apps defined";
}

void
DFOBrokerModule::do_scrap(const data_t&)
{
  TLOG_DEBUG(TLVL_ENTER_EXIT_METHODS) << get_name() << ": Entering do_scrap() method";

  m_dfo_information.clear();

  TLOG() << get_name() << " successfully scrapped";
  TLOG_DEBUG(TLVL_ENTER_EXIT_METHODS) << get_name() << ": Exiting do_scrap() method";
}

void
DFOBrokerModule::do_start(const data_t& payload)
{
  TLOG_DEBUG(TLVL_ENTER_EXIT_METHODS) << get_name() << ": Entering do_start() method";

  m_run_number = payload.value<dunedaq::daqdataformats::run_number_t>("run", 0);

  m_last_heartbeat_sent = std::chrono::steady_clock::now();

  auto iom = iomanager::IOManager::get();
  iom->add_callback<dfmessages::TriggerDecisionToken>(
    m_token_connection, std::bind(&DFOBrokerModule::receive_trigger_complete_token, this, std::placeholders::_1));

  iom->add_callback<dfmessages::DFODecision>(
    m_dfod_connection, std::bind(&DFOBrokerModule::receive_dfo_decision, this, std::placeholders::_1));

  m_thread.start_working_thread(get_name());
  TLOG_DEBUG(TLVL_ENTER_EXIT_METHODS) << get_name() << ": Exiting do_start() method";
}

void
DFOBrokerModule::do_stop(const data_t& /*args*/)
{
  TLOG_DEBUG(TLVL_ENTER_EXIT_METHODS) << get_name() << ": Entering do_stop() method";

  auto iom = iomanager::IOManager::get();
  iom->remove_callback<dfmessages::DFODecision>(m_dfod_connection);

  const int wait_steps = 20;
  auto step_timeout = m_stop_timeout / wait_steps;
  int step_counter = 0;
  while (get_outstanding_decisions().size() != 0 && step_counter < wait_steps) {
    TLOG() << get_name() << ": stop delayed while waiting for " << get_outstanding_decisions().size()
           << " TDs to completed";
    std::this_thread::sleep_for(step_timeout);
    ++step_counter;
  }

  iom->remove_callback<dfmessages::TriggerDecisionToken>(m_token_connection);

  m_thread.stop_working_thread();
  for (auto& dfo_info : m_dfo_information) {
    dfo_info.second = DFOInfo();
  }

  TLOG() << get_name() << " successfully stopped";
  TLOG_DEBUG(TLVL_ENTER_EXIT_METHODS) << get_name() << ": Exiting do_stop() method";
}

void
DFOBrokerModule::do_enable_dfo(const data_t& args)
{
  TLOG_DEBUG(TLVL_ENTER_EXIT_METHODS) << get_name() << ": Entering do_enable_dfo() method";
  auto enabled_dfo_id = args.value<std::string>("dfo", "");

  std::lock_guard<std::mutex> lk(m_dfo_info_mutex);
  for (auto& dfo_pair : m_dfo_information) {
    if (dfo_pair.first == enabled_dfo_id) {
      dfo_pair.second.dfo_is_active = true;
    } else {
      dfo_pair.second.dfo_is_active = false;
    }
  }
  TLOG_DEBUG(TLVL_ENTER_EXIT_METHODS) << get_name() << ": Exiting do_enable_dfo() method";
}

void
DFOBrokerModule::generate_opmon_data()
{
  opmon::DFOBrokerInfo info;
  info.set_heartbeats_sent(m_sent_heartbeats.exchange(0));
  info.set_decisions_sent(m_sent_decisions.exchange(0));
  info.set_decisions_received(m_received_decisions.exchange(0));
  info.set_tokens_received(m_received_tokens.exchange(0));
  info.set_pending_trs(m_pending_trs.load());
  publish(std::move(info));

}

void
DFOBrokerModule::receive_trigger_complete_token(const dfmessages::TriggerDecisionToken& token)
{
  if (token.run_number == 0 && token.trigger_number == 0) {
    return;
  }

  TLOG_DEBUG(TLVL_TDTOKEN_RECEIVED) << get_name() << " Received TriggerDecisionToken for trigger_number "
                                    << token.trigger_number << " and run " << token.run_number << " (current run is "
                                    << m_run_number << ")";
  // add a check to see if the application data found
  if (token.run_number != m_run_number) {
    std::ostringstream oss_source;
    oss_source << "TRB at connection " << token.decision_destination;
    ers::error(
      DFOBrokerRunNumberMismatch(ERS_HERE, token.run_number, m_run_number, oss_source.str(), token.trigger_number));
    return;
  }


  ++m_received_tokens;
  --m_pending_trs;
  {
    std::lock_guard<std::mutex> lk(m_dfo_info_mutex);
    for (auto& dfo_pair : m_dfo_information) {
      m_outstanding_decisions.erase(token.trigger_number);
      dfo_pair.second.recent_completions.insert(token.trigger_number);
    }
  }
  send_heartbeat(true);
}

void
DFOBrokerModule::receive_dfo_decision(const dfmessages::DFODecision& decision)
{
  TLOG_DEBUG(TLVL_TRIGDEC_RECEIVED) << get_name() << " Received DFODecision for trigger_number "
                                    << decision.trigger_decision.trigger_number << " and run "
                                    << decision.trigger_decision.run_number << " (current run is " << m_run_number
                                    << ") from DFO " << decision.dfo_id << "( active DFO? " << std::boolalpha
                                    << dfo_is_active(decision.dfo_id) << ")";
  if (decision.trigger_decision.run_number != m_run_number) {
    ers::error(DFOBrokerRunNumberMismatch(ERS_HERE,
                                          decision.trigger_decision.run_number,
                                          m_run_number,
                                          decision.dfo_id,
                                          decision.trigger_decision.trigger_number));
    return;
  }
  if (!m_dfo_information.count(decision.dfo_id)) {
    ers::error(DFOBrokerDFONotFound(ERS_HERE, decision.dfo_id));
    return;
  }

    ++m_pending_trs;
  ++m_received_decisions;
  {
    std::lock_guard<std::mutex> lk(m_dfo_info_mutex);
    for (auto& ack : decision.acknowledged_completions) {
      m_dfo_information[decision.dfo_id].recent_completions.erase(ack);
    }

    if (m_dfo_information[decision.dfo_id].dfo_is_active) {
      m_outstanding_decisions.insert(decision.trigger_decision.trigger_number);
      dfmessages::TriggerDecision decision_copy = dfmessages::TriggerDecision(decision.trigger_decision);
      auto sender = get_iom_sender<dfmessages::TriggerDecision>(m_td_connection);
      sender->send(std::move(decision_copy), m_td_timeout);
      ++m_sent_decisions;
    }
  }
  send_heartbeat(true);
}

void
DFOBrokerModule::send_heartbeat(bool skip_time_check)
{
  std::lock_guard<std::mutex> lk(m_send_heartbeat_mutex);
  auto begin_time = std::chrono::steady_clock::now();

  if (!skip_time_check) {
    if (std::chrono::duration_cast<std::chrono::milliseconds>(begin_time - m_last_heartbeat_sent) <
        m_send_heartbeat_interval) {
      return;
    }
  }
  TLOG(TLVL_DISPATCH_TO_TRB) << get_name() << ": Sending Heartbeat to DFO";

  dfmessages::DataflowHeartbeat theHeartbeat;
  theHeartbeat.run_number = m_run_number;
  theHeartbeat.recent_completed_triggers = get_recent_completions();
  theHeartbeat.outstanding_decisions = get_outstanding_decisions();
  theHeartbeat.decision_destination = m_dfod_connection;

  try {
    get_iom_sender<dfmessages::DataflowHeartbeat>(m_heartbeat_connection)
      ->send(std::move(theHeartbeat), m_send_heartbeat_timeout);
    m_last_heartbeat_sent = begin_time;
    ++m_sent_heartbeats;
  } catch (iomanager::TimeoutExpired const& ex) {
    ers::warning(ex);
    TLOG(TLVL_DISPATCH_TO_TRB) << get_name() << ": Timeout from IOManager send call, will retry later";
  }
}

void
DFOBrokerModule::heartbeat_thread_proc(std::atomic<bool>& running)
{
  while (running.load()) {
    send_heartbeat(false);
    std::this_thread::sleep_for(m_send_heartbeat_interval / 25);
  }
  send_heartbeat(true); // Always send heartbeat at end
}

bool
DFOBrokerModule::dfo_is_active(const std::string& dfo_id)
{
  std::lock_guard<std::mutex> lk(m_dfo_info_mutex);
  if (!m_dfo_information.count(dfo_id)) {
    ers::error(DFOBrokerDFONotFound(ERS_HERE, dfo_id));
    return false;
  }

  return m_dfo_information[dfo_id].dfo_is_active;
}

std::vector<dfmessages::trigger_number_t>
DFOBrokerModule::get_recent_completions()
{
  std::set<dfmessages::trigger_number_t> output_set;
  {
    std::lock_guard<std::mutex> lk(m_dfo_info_mutex);
    for (auto& dfo_pair : m_dfo_information) {
      for (auto& tn : dfo_pair.second.recent_completions) {
        output_set.insert(tn);
      }
    }
  }

  std::vector<dfmessages::trigger_number_t> output_vector;
  for (auto& tn : output_set) {
    output_vector.push_back(tn);
  }
  return output_vector;
}

std::vector<dfmessages::trigger_number_t>
DFOBrokerModule::get_outstanding_decisions()
{
  std::vector<dfmessages::trigger_number_t> output;
  {
    std::lock_guard<std::mutex> lk(m_dfo_info_mutex);
    for (auto& tn : m_outstanding_decisions) {
      output.push_back(tn);
    }
  }
  return output;
}

} // namespace dunedaq::dfmodules

DEFINE_DUNE_DAQ_MODULE(dunedaq::dfmodules::DFOBrokerModule)
