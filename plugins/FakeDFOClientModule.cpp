/**
 * @file FakeDFOClientModule.cpp FakeDFOClientModule class implementation
 *
 * This is part of the DUNE DAQ Software Suite, copyright 2020.
 * Licensing/copyright details are in the COPYING file that you should have
 * received with this code.
 */

#include "FakeDFOClientModule.hpp"
#include "dfmodules/CommonIssues.hpp"

#include "appmodel/FakeDFOClientModule.hpp"
#include "confmodel/Connection.hpp"
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
#define TRACE_NAME "FakeDFOClientModule" // NOLINT
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

FakeDFOClientModule::FakeDFOClientModule(const std::string& name)
  : dunedaq::appfwk::DAQModule(name)
{
  register_command("conf", &FakeDFOClientModule::do_conf);
  register_command("scrap", &FakeDFOClientModule::do_scrap);
  register_command("start", &FakeDFOClientModule::do_start);
  register_command("stop", &FakeDFOClientModule::do_stop);
}

void
FakeDFOClientModule::init(std::shared_ptr<appfwk::ModuleConfiguration> mcfg)
{
  TLOG_DEBUG(TLVL_ENTER_EXIT_METHODS) << get_name() << ": Entering init() method";

  auto mdal = mcfg->module<appmodel::FakeDFOClientModule>(get_name());
  if (!mdal) {
    throw appfwk::CommandFailed(ERS_HERE, "init", get_name(), "Unable to retrieve configuration object");
  }
  auto iom = iomanager::IOManager::get();

  for (auto con : mdal->get_inputs()) {
    if (con->get_data_type() == datatype_to_string<dfmessages::TriggerDecision>()) {
      m_td_connection = con->UID();
    }
  }
  for (auto con : mdal->get_outputs()) {
    if (con->get_data_type() == datatype_to_string<dfmessages::TriggerDecisionToken>()) {
      m_token_connection = con->UID();
    }
  }

  if (m_token_connection == "") {
    throw appfwk::MissingConnection(
      ERS_HERE, get_name(), datatype_to_string<dfmessages::TriggerDecisionToken>(), "input");
  }
  if (m_td_connection == "") {
    throw appfwk::MissingConnection(ERS_HERE, get_name(), datatype_to_string<dfmessages::TriggerDecision>(), "output");
  }

  // these are just tests to check if the connections are ok
  iom->get_receiver<dfmessages::TriggerDecisionToken>(m_token_connection);

  m_fakedfoclient_conf = mdal->get_configuration();

  TLOG_DEBUG(TLVL_ENTER_EXIT_METHODS) << get_name() << ": Exiting init() method";
}

void
FakeDFOClientModule::do_conf(const data_t&)
{
  TLOG_DEBUG(TLVL_ENTER_EXIT_METHODS) << get_name() << ": Entering do_conf() method";
  
  m_token_wait_us = std::chrono::microseconds(m_fakedfoclient_conf->get_token_wait_microseconds());
  m_send_token_timeout = std::chrono::milliseconds(m_fakedfoclient_conf->get_send_token_timeout_milliseconds());

  TLOG_DEBUG(TLVL_ENTER_EXIT_METHODS) << get_name() << ": Exiting do_conf() method";
}

void
FakeDFOClientModule::do_scrap(const data_t&)
{
  TLOG_DEBUG(TLVL_ENTER_EXIT_METHODS) << get_name() << ": Entering do_scrap() method";

  TLOG_DEBUG(TLVL_ENTER_EXIT_METHODS) << get_name() << ": Exiting do_scrap() method";
}

void
FakeDFOClientModule::do_start(const data_t&)
{
  TLOG_DEBUG(TLVL_ENTER_EXIT_METHODS) << get_name() << ": Entering do_start() method";

  auto iom = iomanager::IOManager::get();
  iom->add_callback<dfmessages::TriggerDecision>(
    m_td_connection, std::bind(&FakeDFOClientModule::receive_trigger_decision, this, std::placeholders::_1));

  TLOG_DEBUG(TLVL_ENTER_EXIT_METHODS) << get_name() << ": Exiting do_start() method";
}

void
FakeDFOClientModule::do_stop(const data_t& /*args*/)
{
  TLOG_DEBUG(TLVL_ENTER_EXIT_METHODS) << get_name() << ": Entering do_stop() method";

  auto iom = iomanager::IOManager::get();
  iom->remove_callback<dfmessages::TriggerDecision>(m_td_connection);

  m_stop_source.request_stop();

  TLOG_DEBUG(TLVL_ENTER_EXIT_METHODS) << get_name() << ": Exiting do_stop() method";
}

void
FakeDFOClientModule::generate_opmon_data()
{
  opmon::FakeDFOClientInfo info;
  info.set_decisions_received(m_received_decisions.exchange(0));
  info.set_tokens_sent(m_sent_tokens.exchange(0));
  publish(std::move(info));
}

void
FakeDFOClientModule::wait_and_send_token(const dfmessages::TriggerDecision& decision, std::stop_token stoken)
{
  bool sent = false;

  while (!sent && !stoken.stop_requested()) {
    std::this_thread::sleep_for(m_token_wait_us);
    dfmessages::TriggerDecisionToken token;
    token.run_number = decision.run_number;
    token.decision_destination = "FakeDFOClient";
    token.trigger_number = decision.trigger_number;

    try {
      get_iom_sender<dfmessages::TriggerDecisionToken>(m_token_connection)
        ->send(std::move(token), m_send_token_timeout);
      sent = true;
      ++m_sent_tokens;
    } catch (iomanager::TimeoutExpired const& ex) {
      ers::warning(ex);
      TLOG(TLVL_DISPATCH_TO_TRB) << get_name() << ": Timeout from IOManager send call, will retry later";
    }
  }
}

void
FakeDFOClientModule::receive_trigger_decision(const dfmessages::TriggerDecision& decision)
{
  TLOG_DEBUG(TLVL_TRIGDEC_RECEIVED) << get_name() << " Received TriggerDecision for trigger_number "
                                    << decision.trigger_number << " and run " << decision.run_number;

  ++m_received_decisions;
  std::jthread thread(&FakeDFOClientModule::wait_and_send_token, this, decision, m_stop_source.get_token());
}

} // namespace dunedaq::dfmodules

DEFINE_DUNE_DAQ_MODULE(dunedaq::dfmodules::FakeDFOClientModule)
