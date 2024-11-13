/**
 * @file FragmentAggregatorModule.cpp FragmentAggregatorModule implementation
 *
 * This is part of the DUNE DAQ , copyright 2020.
 * Licensing/copyright details are in the COPYING file that you should have
 * received with this code.
 */

#include "FragmentAggregatorModule.hpp"
#include "dfmodules/CommonIssues.hpp"

#include "appmodel/FragmentAggregatorModule.hpp"
#include "confmodel/Connection.hpp"
#include "confmodel/QueueWithSourceId.hpp"
#include "dfmessages/Fragment_serialization.hpp"
#include "logging/Logging.hpp"

#include "iomanager/IOManager.hpp"

#include <iostream>
#include <string>

namespace dunedaq {
namespace dfmodules {

FragmentAggregatorModule::FragmentAggregatorModule(const std::string& name)
  : DAQModule(name)
{
  register_command("start", &FragmentAggregatorModule::do_start);
  register_command("stop_trigger_sources", &FragmentAggregatorModule::do_stop);
}

void
FragmentAggregatorModule::init(std::shared_ptr<appfwk::ModuleConfiguration> mcfg)
{
  auto mdal = mcfg->module<appmodel::FragmentAggregatorModule>(get_name());
  if (!mdal) {
    throw appfwk::CommandFailed(ERS_HERE, "init", get_name(), "Unable to retrieve configuration object");
  }

  auto inputs = mdal->get_inputs();
  for (auto con : mdal->get_inputs()) {
    if (con->get_data_type() == datatype_to_string < dfmessages::DataRequest>()) {
      m_data_req_input = con->UID();
    }
    if (con->get_data_type() == datatype_to_string<daqdataformats::Fragment>()) {
      m_fragment_input = con->UID();
    }
  }

  m_producer_conn_ids.clear();
  for (const auto cr : mdal->get_outputs()) {
    if (cr->get_data_type() == datatype_to_string<dfmessages::DataRequest>()) {
	auto qid = cr->cast<confmodel::QueueWithSourceId>();
      	    m_producer_conn_ids[qid->get_source_id()] = cr->UID();
    }
  }

  // this is just to get the data request receiver registered early (before Start)
  auto iom = iomanager::IOManager::get();
  iom->get_receiver<dfmessages::DataRequest>(m_data_req_input);
}

// void
// FragmentAggregatorModule::get_info(opmonlib::InfoCollector& /*ci*/, int /* level */)
// {
//   // dummyconsumerinfo::Info info;
//   // info.packets_processed = m_packets_processed;

//   // ci.add(info);
// }

void
FragmentAggregatorModule::do_start(const data_t& /* args */)
{
  m_packets_processed = 0;
  auto iom = iomanager::IOManager::get();
  iom->add_callback<dfmessages::DataRequest>(
    m_data_req_input, std::bind(&FragmentAggregatorModule::process_data_request, this, std::placeholders::_1));
  iom->add_callback<std::unique_ptr<daqdataformats::Fragment>>(
    m_fragment_input, std::bind(&FragmentAggregatorModule::process_fragment, this, std::placeholders::_1));
}

void
FragmentAggregatorModule::do_stop(const data_t& /* args */)
{
  auto iom = iomanager::IOManager::get();
  iom->remove_callback<dfmessages::DataRequest>(m_data_req_input);
  iom->remove_callback<std::unique_ptr<daqdataformats::Fragment>>(m_fragment_input);
  m_data_req_map.clear();
}

void
FragmentAggregatorModule::process_data_request(dfmessages::DataRequest& data_request)
{

  {
    std::scoped_lock lock(m_mutex);
    std::tuple<dfmessages::trigger_number_t, dfmessages::sequence_number_t, daqdataformats::SourceID> triplet = {
      data_request.trigger_number, data_request.sequence_number, data_request.request_information.component
    };
    m_data_req_map[triplet] = data_request.data_destination;
  }
  // Forward Data Request to the right DLH
  try {
    //std::string component_name = "inputReqToDLH-" + data_request.request_information.component.to_string();
    auto uid_elem = m_producer_conn_ids.find(data_request.request_information.component.id);
    if (uid_elem == m_producer_conn_ids.end()) {
      ers::error(dunedaq::dfmodules::DRSenderLookupFailed(ERS_HERE,
                                                          data_request.request_information.component,
                                                          data_request.run_number,
                                                          data_request.trigger_number,
                                                          data_request.sequence_number));
    } else {
      TLOG_DEBUG(30) << "Send data request to " << uid_elem->second;
      auto sender = get_iom_sender<dfmessages::DataRequest>(uid_elem->second);
      data_request.data_destination = m_fragment_input;
      sender->send(std::move(data_request), iomanager::Sender::s_no_block);
    }
  } catch (const ers::Issue& excpt) {
    ers::warning(excpt);
  }
}

void
FragmentAggregatorModule::process_fragment(std::unique_ptr<daqdataformats::Fragment>& fragment)
{
  // Forward Fragment to the right TRB
  std::string trb_identifier;
  {
    std::scoped_lock lock(m_mutex);
    auto dr_iter = m_data_req_map.find(
      std::make_tuple<dfmessages::trigger_number_t, dfmessages::sequence_number_t, daqdataformats::SourceID>(
        fragment->get_trigger_number(), fragment->get_sequence_number(), fragment->get_element_id()));
    if (dr_iter != m_data_req_map.end()) {
      trb_identifier = dr_iter->second;
      m_data_req_map.erase(dr_iter);
    } else {
      ers::error(UnknownFragmentDestination(
        ERS_HERE, fragment->get_trigger_number(), fragment->get_sequence_number(), fragment->get_element_id()));
      return;
    }
  }
  try {
    TLOG_DEBUG(27) << get_name() << " Sending fragment for trigger/sequence_number "
                   << fragment->get_trigger_number() << "."
                   << fragment->get_sequence_number() << " and SourceID "
                   << fragment->get_element_id() << " to "
                   << trb_identifier;
    auto sender = get_iom_sender<std::unique_ptr<daqdataformats::Fragment>>(trb_identifier);
    sender->send(std::move(fragment), iomanager::Sender::s_no_block);
  } catch (const ers::Issue& excpt) {
    ers::warning(excpt);
  }
}

} // namespace dfmodules
} // namespace dunedaq

DEFINE_DUNE_DAQ_MODULE(dunedaq::dfmodules::FragmentAggregatorModule)
