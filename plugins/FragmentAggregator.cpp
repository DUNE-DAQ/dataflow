/**
 * @file FragmentAggregator.cpp FragmentAggregator implementation
 *
 * This is part of the DUNE DAQ , copyright 2020.
 * Licensing/copyright details are in the COPYING file that you should have
 * received with this code.
 */


#include "FragmentAggregator.hpp"
#include "dfmodules/CommonIssues.hpp"

#include "appfwk/DAQModuleHelper.hpp"
#include "appfwk/cmd/Nljs.hpp"
#include "appfwk/cmd/Nljs.hpp"
//#include "dfmessages/Fragment_serialization.hpp"
#include "logging/Logging.hpp"

#include "iomanager/IOManager.hpp"

#include <string>
#include <iostream>

namespace dunedaq {
namespace dfmodules {

FragmentAggregator::FragmentAggregator(const std::string& name)
  : DAQModule(name)
{
  register_command("start", &FragmentAggregator::do_start);
  register_command("stop_trigger_sources", &FragmentAggregator::do_stop);
}

void
FragmentAggregator::init(const data_t& init_data)
{
  auto ci = appfwk::connection_index(init_data, { "data_req_input", "fragment_input"});

  m_data_req_input = ci["data_req_input"];
  m_fragment_input = ci["fragment_input"];
}

void
FragmentAggregator::get_info(opmonlib::InfoCollector& /*ci*/, int /* level */)
{
  //dummyconsumerinfo::Info info;
  //info.packets_processed = m_packets_processed;

  //ci.add(info);
}

void
FragmentAggregator::do_start(const data_t& /* args */)
{
  m_packets_processed = 0;
  auto iom = iomanager::IOManager::get();
  iom->add_callback<dfmessages::DataRequest>(
    m_data_req_input, std::bind(&FragmentAggregator::process_data_request, this, std::placeholders::_1));
  iom->add_callback<std::unique_ptr<daqdataformats::Fragment>>(
    m_fragment_input, std::bind(&FragmentAggregator::process_fragment, this, std::placeholders::_1));
}

void
FragmentAggregator::do_stop(const data_t& /* args */)
{
  auto iom = iomanager::IOManager::get();
  iom->remove_callback<dfmessages::DataRequest>(m_data_req_input);
  iom->remove_callback<std::unique_ptr<daqdataformats::Fragment>>(m_fragment_input);
  m_data_req_map.clear();
}

void
FragmentAggregator::process_data_request(dfmessages::DataRequest& data_request) {

	{
           std::scoped_lock lock(m_mutex);
	   std::tuple<dfmessages::trigger_number_t, dfmessages::sequence_number_t, daqdataformats::SourceID> triplet = {data_request.trigger_number, data_request.sequence_number, data_request.request_information.component};
	   m_data_req_map[triplet] = data_request.data_destination;
	}
	// Forward Data Request to the right DLH
	try {
		auto sender = get_iom_sender<dfmessages::DataRequest>
			("data_requests_for_" + data_request.request_information.component.to_string());
		data_request.data_destination = "fragment_output";
		sender->send(std::move(data_request), iomanager::Sender::s_no_block);
	}
	catch(const ers::Issue& excpt) {
		ers::warning(excpt);
	}
}


void
FragmentAggregator::process_fragment(std::unique_ptr<daqdataformats::Fragment>& fragment) {
       // Forward Fragment to the right TRB
	std::string trb_identifier;
        {
           std::scoped_lock lock(m_mutex);
           auto dr_iter = m_data_req_map.find(std::make_tuple<dfmessages::trigger_number_t, dfmessages::sequence_number_t, daqdataformats::SourceID>
                   (fragment->get_trigger_number(), fragment->get_sequence_number(), fragment->get_element_id()));
	   if (dr_iter != m_data_req_map.end())
	   	trb_identifier = dr_iter->second;
	   else {
		   ers::error(UnknownFragmentDestination(ERS_HERE, fragment->get_trigger_number(), 
				fragment->get_sequence_number(), fragment->get_element_id()));
	   }
        }
        // Forward Data Request to the right DLH
        try {
                auto sender = get_iom_sender<std::unique_ptr<daqdataformats::Fragment>> (trb_identifier);
                sender->send(std::move(fragment), iomanager::Sender::s_no_block);
        }
        catch(const ers::Issue& excpt) {
                ers::warning(excpt);
        }
}

} // namespace dfmodules
} // namespace dunedaq

DEFINE_DUNE_DAQ_MODULE(dunedaq::dfmodules::FragmentAggregator)


