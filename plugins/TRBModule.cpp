/**
 * @file TRBModule.cpp TRBModule class implementation
 *
 * This is part of the DUNE DAQ Software Suite, copyright 2020.
 * Licensing/copyright details are in the COPYING file that you should have
 * received with this code.
 */

#include "TRBModule.hpp"
#include "dfmodules/CommonIssues.hpp"

#include "appmodel/NetworkConnectionDescriptor.hpp"
#include "appmodel/NetworkConnectionRule.hpp"
#include "appmodel/ReadoutApplication.hpp"
#include "appmodel/SourceIDConf.hpp"
#include "appmodel/SourceIDToNetworkConnection.hpp"
#include "appmodel/TRBModule.hpp"
#include "appmodel/TriggerApplication.hpp"
#include "confmodel/Application.hpp"
#include "confmodel/Connection.hpp"
#include "confmodel/DetectorStream.hpp"
#include "confmodel/DetectorToDaqConnection.hpp"
#include "confmodel/NetworkConnection.hpp"
#include "confmodel/Session.hpp"
#include "dfmessages/TriggerRecord_serialization.hpp"
#include "logging/Logging.hpp"

#include "iomanager/IOManager.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <memory>
#include <numeric>
#include <string>
#include <thread>
#include <utility>
#include <vector>

/**
 * @brief TRACE debug levels used in this source file
 */
enum
{
  TLVL_ENTER_EXIT_METHODS = 5,
  TLVL_INIT = 8,
  TLVL_WORK_STEPS = 10,
  TLVL_BOOKKEEPING = 15,
  TLVL_DISPATCH_DATAREQ = 21,
  TLVL_FRAGMENT_RECEIVE = 22
};

namespace dunedaq {
namespace dfmodules {

using daqdataformats::TriggerRecordErrorBits;

TRBModule::TRBModule(const std::string& name)
  : dunedaq::appfwk::DAQModule(name)
  , m_thread(std::bind(&TRBModule::do_work, this, std::placeholders::_1))
  , m_queue_timeout(100)
{

  register_command("conf", &TRBModule::do_conf);
  register_command("scrap", &TRBModule::do_scrap);
  register_command("start", &TRBModule::do_start);
  register_command("stop", &TRBModule::do_stop);
}

void
TRBModule::init(std::shared_ptr<appfwk::ConfigurationManager> mcfg)
{

  TLOG_DEBUG(TLVL_ENTER_EXIT_METHODS) << get_name() << ": Entering init() method";

  //--------------------------------
  // Get single queues
  //---------------------------------

  auto mdal = mcfg->get_dal<appmodel::TRBModule>(get_name());
  if (!mdal) {
    throw appfwk::CommandFailed(ERS_HERE, "init", get_name(), "Unable to retrieve configuration object");
  }

  auto iom = iomanager::IOManager::get();
  for (auto con : mdal->get_inputs()) {
    if (con->get_data_type() == datatype_to_string<dfmessages::TriggerDecision>()) {
      m_trigger_decision_input = iom->get_receiver<dfmessages::TriggerDecision>(con->UID());
    }
    if (con->get_data_type() == datatype_to_string<std::unique_ptr<daqdataformats::Fragment>>()) {
      m_fragment_input = iom->get_receiver<std::unique_ptr<daqdataformats::Fragment>>(con->UID());

      // save the data fragment receiver global connection name for later, when it gets
      // copied into the DataRequests so that data producers know where to send their fragments
      m_reply_connection = con->UID();
    }
    if (con->get_data_type() == datatype_to_string<dfmessages::TRMonRequest>()) {
      m_mon_receiver = iom->get_receiver<dfmessages::TRMonRequest>(con->UID());
    }
  }

  if (m_trigger_decision_input == nullptr) {
    throw InvalidQueueFatalError(ERS_HERE, get_name(), "TriggerDecision Input queue");
  }
  if (m_fragment_input == nullptr) {
    throw InvalidQueueFatalError(ERS_HERE, get_name(), "Fragment Input queue");
  }

  for (auto con : mdal->get_outputs()) {
    if (con->get_data_type() == datatype_to_string<std::unique_ptr<daqdataformats::TriggerRecord>>()) {
      m_trigger_record_output = iom->get_sender<std::unique_ptr<daqdataformats::TriggerRecord>>(con->UID());
    }
  }

  for (auto con : mdal->get_request_connections()) {
    for (auto source_id : con->get_source_ids()) {

      // find the queue for sourceid_req in the map
      std::unique_lock<std::mutex> lk(m_map_sourceid_connections_mutex);
      daqdataformats::SourceID sid;
      sid.subsystem = daqdataformats::SourceID::string_to_subsystem(source_id->get_subsystem());
      sid.id = source_id->get_sid();
      auto it_req = m_map_sourceid_connections.find(sid);
      if (it_req == m_map_sourceid_connections.end() || it_req->second == nullptr) {
        m_map_sourceid_connections[sid] = get_iom_sender<dfmessages::DataRequest>(con->get_netconn()->UID());
      }
      lk.unlock();
    }
  }

  m_trb_conf = mdal->get_configuration();

  TLOG_DEBUG(TLVL_ENTER_EXIT_METHODS) << get_name() << ": Exiting init() method";
}

void
TRBModule::generate_opmon_data()
{

  opmon::TRBInfo i;

  // status metrics
  i.set_pending_trigger_decisions(m_trigger_decisions_counter.load());
  i.set_fragments_in_the_book(m_fragment_counter.load());
  i.set_pending_fragments(m_pending_fragment_counter.load());

  // operation metrics
  i.set_received_trigger_decisions(m_received_trigger_decisions.exchange(0));
  i.set_generated_trigger_records(m_generated_trigger_records.exchange(0));
  i.set_generated_data_requests(m_generated_data_requests.exchange(0));
  i.set_sleep_counter(m_sleep_counter.exchange(0));
  i.set_loop_counter(m_loop_counter.exchange(0));
  i.set_data_waiting_time(m_data_waiting_time.exchange(0));
  i.set_data_request_width(m_data_request_width.exchange(0));
  i.set_trigger_decision_width(m_trigger_decision_width.exchange(0));
  i.set_received_trmon_requests(m_trmon_request_counter.exchange(0));
  i.set_sent_trmon(m_trmon_sent_counter.exchange(0));

  publish(std::move(i));

  opmon::TRBErrors err;
  // error counters
  err.set_timed_out_trigger_records(m_timed_out_trigger_records.load());
  err.set_abandoned_trigger_records(m_abandoned_trigger_records.load());
  err.set_unexpected_fragments(m_unexpected_fragments.load());
  err.set_unexpected_trigger_decisions(m_unexpected_trigger_decisions.load());
  err.set_lost_fragments(m_lost_fragments.load());
  err.set_invalid_requests(m_invalid_requests.load());
  err.set_duplicated_trigger_ids(m_duplicated_trigger_ids.load());

  publish(std::move(err));
}

void
TRBModule::do_conf(const data_t&)
{
  TLOG_DEBUG(TLVL_ENTER_EXIT_METHODS) << get_name() << ": Entering do_conf() method";

  m_trigger_timeout = std::chrono::milliseconds(m_trb_conf->get_trigger_record_timeout_ms());

  m_loop_sleep = m_queue_timeout = std::chrono::milliseconds(m_trb_conf->get_queues_timeout());

  TLOG() << get_name() << ": timeouts (ms): queue = " << m_queue_timeout.count() << ", loop = " << m_loop_sleep.count();
  m_max_time_window = m_trb_conf->get_max_time_window();
  TLOG() << get_name() << ": Max time window is " << m_max_time_window;

  m_this_trb_source_id.subsystem = daqdataformats::SourceID::Subsystem::kTRBuilder;
  m_this_trb_source_id.id = m_trb_conf->get_source_id();

  TLOG_DEBUG(TLVL_ENTER_EXIT_METHODS) << get_name() << ": Exiting do_conf() method";
}

void
TRBModule::do_scrap(const data_t& /*args*/)
{
  TLOG_DEBUG(TLVL_ENTER_EXIT_METHODS) << get_name() << ": Entering do_scrap() method";

  TLOG() << get_name() << " successfully scrapped";
  TLOG_DEBUG(TLVL_ENTER_EXIT_METHODS) << get_name() << ": Exiting do_scrap() method";
}

void
TRBModule::do_start(const data_t& args)
{
  TLOG_DEBUG(TLVL_ENTER_EXIT_METHODS) << get_name() << ": Entering do_start() method";

  {
    std::unique_lock<std::mutex> lk(m_map_sourceid_connections_mutex);
    for (const auto& sid_sender : m_map_sourceid_connections) {
      std::shared_ptr<data_req_sender_t> sender = sid_sender.second;
      if (sender != nullptr) {
        bool is_ready = sender->is_ready_for_sending(std::chrono::milliseconds(100));
        TLOG_DEBUG(0) << "The sender for " << sid_sender.first << " " << (is_ready ? "is" : "is not") << " ready.";
      }
    }
  }

  m_run_number.reset(new const daqdataformats::run_number_t(args.at("run").get<daqdataformats::run_number_t>()));

  // Register the callback to receive monitoring requests
  if (m_mon_receiver) {
    m_mon_requests.clear();
    m_mon_receiver->add_callback(std::bind(&TRBModule::tr_requested, this, std::placeholders::_1));
  }

  m_thread.start_working_thread(get_name());
  TLOG() << get_name() << " successfully started";
  TLOG_DEBUG(TLVL_ENTER_EXIT_METHODS) << get_name() << ": Exiting do_start() method";
}

void
TRBModule::do_stop(const data_t& /*args*/)
{
  TLOG_DEBUG(TLVL_ENTER_EXIT_METHODS) << get_name() << ": Entering do_stop() method";
  // Unregister the monitoring requests callback

  if (m_mon_receiver) {
    m_mon_receiver->remove_callback();
  }

  m_thread.stop_working_thread();
  TLOG() << get_name() << " successfully stopped";
  TLOG_DEBUG(TLVL_ENTER_EXIT_METHODS) << get_name() << ": Exiting do_stop() method";
}

void
TRBModule::tr_requested(const dfmessages::TRMonRequest& req)
{
  ++m_trmon_request_counter;

  // Ignore requests that don't belong to the ongoing run
  if (req.run_number != *m_run_number)
    return;

  // Add requests to pending requests
  // To be done: choose a concurrent container implementation.
  const std::lock_guard<std::mutex> lock(m_mon_mutex);
  m_mon_requests.push_back(req);
}

void
TRBModule::do_work(std::atomic<bool>& running_flag)
{
  TLOG_DEBUG(TLVL_ENTER_EXIT_METHODS) << get_name() << ": Entering do_work() method";

  // clean books from possible previous memory
  m_trigger_records.clear();
  m_trigger_decisions_counter.store(0);
  m_unexpected_trigger_decisions.store(0);
  m_pending_fragment_counter.store(0);
  m_generated_trigger_records.store(0);
  m_fragment_counter.store(0);
  m_timed_out_trigger_records.store(0);
  m_abandoned_trigger_records.store(0);
  m_unexpected_fragments.store(0);
  m_lost_fragments.store(0);
  m_invalid_requests.store(0);
  m_duplicated_trigger_ids.store(0);

  bool run_again = false;

  while (running_flag.load() || run_again) {

    bool book_updates = false;

    // read decision requests
    book_updates = read_and_process_trigger_decision(iomanager::Receiver::s_no_block, running_flag);

    // read the fragments queues
    bool new_fragments = read_fragments();

    //-------------------------------------------------
    // Check if trigger records are complete or timedout
    // and create dedicated record
    //--------------------------------------------------

    if (new_fragments) {

      TLOG_DEBUG(TLVL_BOOKKEEPING) << "Bookeeping status: " << m_trigger_records.size()
                                   << " trigger records in progress ";

      std::vector<TriggerId> complete;
      for (const auto& tr : m_trigger_records) {

        auto comp_size = tr.second.second->get_fragments_ref().size();
        auto requ_size = tr.second.second->get_header_ref().get_num_requested_components();
        std::ostringstream message;
        message << tr.first << " with " << comp_size << '/' << requ_size << " components";

        if (comp_size == requ_size) {

          message << ": complete";
          complete.push_back(tr.first);
        }

        TLOG_DEBUG(TLVL_BOOKKEEPING) << message.str();

      } // loop over TRs to check if they are complete

      //------------------------------------------------
      // Create TriggerRecords and send them
      //-----------------------------------------------

      for (const auto& id : complete) {

        send_trigger_record(id, running_flag);

      } // loop over compled trigger id

    } // if books were updated

    //-------------------------------------------------
    // Check if some fragments are obsolete
    //--------------------------------------------------
    book_updates |= check_stale_requests(running_flag);

    run_again = book_updates || new_fragments;

    if (!run_again) {
      if (running_flag.load()) {
        ++m_sleep_counter;
        run_again = read_and_process_trigger_decision(m_loop_sleep, running_flag);
      }
    } else {
      ++m_loop_counter;
    }

  } // working loop

  TLOG_DEBUG(TLVL_ENTER_EXIT_METHODS) << get_name() << ": Starting draining phase ";
  std::chrono::steady_clock::time_point t1 = std::chrono::steady_clock::now();

  // //-------------------------------------------------
  // // Here we drain what has been left from the running condition
  // //--------------------------------------------------

  // create all possible trigger record
  std::vector<TriggerId> triggers;
  for (const auto& entry : m_trigger_records) {
    triggers.push_back(entry.first);
  }

  // create the trigger record and send it
  for (const auto& t : triggers) {
    send_trigger_record(t, running_flag);
  }

  std::chrono::steady_clock::time_point t2 = std::chrono::steady_clock::now();

  std::chrono::duration<double> time_span = std::chrono::duration_cast<std::chrono::duration<double>>(t2 - t1);

  std::ostringstream oss_summ;
  oss_summ << ": Exiting the do_work() method, " << m_trigger_records.size() << " remaining Trigger Records"
           << std::endl
           << "Draining took : " << time_span.count() << " s";
  TLOG() << ProgressUpdate(ERS_HERE, get_name(), oss_summ.str());

  TLOG_DEBUG(TLVL_ENTER_EXIT_METHODS) << get_name() << ": Exiting do_work() method";
} // NOLINT(readability/fn_size)

bool
TRBModule::read_fragments()
{
  std::optional<std::unique_ptr<daqdataformats::Fragment>> temp_fragment;

  try {
    temp_fragment = m_fragment_input->try_receive(iomanager::Receiver::s_no_block);
  } catch (const ers::Issue& e) {
    ers::error(e);
    return false;
  }

  if (!temp_fragment)
    return false;

  TLOG_DEBUG(TLVL_FRAGMENT_RECEIVE) << get_name() << " Received fragment for trigger/sequence_number "
                                    << temp_fragment.value()->get_trigger_number() << "."
                                    << temp_fragment.value()->get_sequence_number() << " from "
                                    << temp_fragment.value()->get_element_id();

  TriggerId temp_id(*temp_fragment.value());
  bool requested = false;

  auto it = m_trigger_records.find(temp_id);

  if (it != m_trigger_records.end()) {

    // check if the fragment has a Source Id that was desired
    daqdataformats::TriggerRecordHeader& header = it->second.second->get_header_ref();

    for (size_t i = 0; i < header.get_num_requested_components(); ++i) {

      const daqdataformats::ComponentRequest& request = header[i];
      if (request.component == temp_fragment.value()->get_element_id()) {
        requested = true;
        break;
      }

    } // request loop

  } // if there is a corresponding trigger ID entry in the boook

  if (requested) {
    it->second.second->add_fragment(std::move(*temp_fragment));
    ++m_fragment_counter;
    --m_pending_fragment_counter;
  } else {
    ers::error(UnexpectedFragment(
      ERS_HERE, temp_id, temp_fragment.value()->get_fragment_type_code(), temp_fragment.value()->get_element_id()));
    ++m_unexpected_fragments;
  }

  return true;
}

bool
TRBModule::read_and_process_trigger_decision(iomanager::Receiver::timeout_t timeout, std::atomic<bool>& running)
{

  std::optional<dfmessages::TriggerDecision> temp_dec;

  try {
    // get the trigger decision
    temp_dec = m_trigger_decision_input->try_receive(timeout);

  } catch (const ers::Issue& ex) {
    ers::error(ex);
  }

  if (!temp_dec)
    return false;

  if (temp_dec->run_number != *m_run_number) {
    ers::error(UnexpectedTriggerDecision(ERS_HERE, temp_dec->trigger_number, temp_dec->run_number, *m_run_number));
    ++m_unexpected_trigger_decisions;
    return false;
  }

  ++m_received_trigger_decisions;

  bool book_updates = create_trigger_records_and_dispatch(*temp_dec, running) > 0;

  return book_updates;
}

TRBModule::trigger_record_ptr_t
TRBModule::extract_trigger_record(const TriggerId& id)
{

  auto it = m_trigger_records.find(id);

  trigger_record_ptr_t temp = std::move(it->second.second);

  auto time = clock_type::now();
  auto duration = time - it->second.first;

  m_data_waiting_time += std::chrono::duration_cast<duration_type>(duration).count();

  m_trigger_records.erase(it);

  --m_trigger_decisions_counter;
  m_fragment_counter -= temp->get_fragments_ref().size();

  auto missing_fragments = temp->get_header_ref().get_num_requested_components() - temp->get_fragments_ref().size();

  if (missing_fragments > 0) {

    m_lost_fragments += missing_fragments;
    m_pending_fragment_counter -= missing_fragments;
    temp->get_header_ref().set_error_bit(TriggerRecordErrorBits::kIncomplete, true);

    TLOG() << get_name() << " sending incomplete TriggerRecord downstream at Stop time "
           << "(trigger/run_number=" << id << ", " << temp->get_fragments_ref().size() << " of "
           << temp->get_header_ref().get_num_requested_components() << " fragments included)";
  }

  return temp;
}

unsigned int
TRBModule::create_trigger_records_and_dispatch(const dfmessages::TriggerDecision& td, std::atomic<bool>& running)
{

  unsigned int new_tr_counter = 0;

  // check the whole time window
  daqdataformats::timestamp_t begin = std::numeric_limits<daqdataformats::timestamp_t>::max();
  daqdataformats::timestamp_t end = 0;

  for (const auto& component : td.components) {
    if (component.window_begin < begin)
      begin = component.window_begin;
    if (component.window_end > end)
      end = component.window_end;
  }

  daqdataformats::timestamp_diff_t tot_width = end - begin;
  daqdataformats::sequence_number_t max_sequence_number =
    (m_max_time_window > 0 && tot_width > 0) ? ((tot_width - 1) / m_max_time_window) : 0;

  TLOG_DEBUG(TLVL_WORK_STEPS) << get_name() << ": trig_number " << td.trigger_number << ": run_number " << td.run_number
                              << ": trig_timestamp " << td.trigger_timestamp << " will have " << max_sequence_number + 1
                              << " sequences";

  m_trigger_decision_width += tot_width;

  // create the trigger records
  for (daqdataformats::sequence_number_t sequence = 0; sequence <= max_sequence_number; ++sequence) {

    daqdataformats::timestamp_t slice_begin = begin + sequence * m_max_time_window;
    daqdataformats::timestamp_t slice_end =
      m_max_time_window > 0 ? std::min(slice_begin + m_max_time_window, end) : end;

    TLOG_DEBUG(TLVL_WORK_STEPS) << get_name() << ": trig_number " << td.trigger_number << ", sequence " << sequence
                                << " ts=" << slice_begin << ":" << slice_end << " (TR " << begin << ":" << end << ")";

    // create the components cropped in time
    decltype(td.components) slice_components;
    for (const auto& component : td.components) {

      if (component.window_begin > slice_end)
        continue;
      if (component.window_end < slice_begin)
        continue;

      daqdataformats::timestamp_t new_begin = std::max(slice_begin, component.window_begin);
      daqdataformats::timestamp_t new_end = std::min(slice_end, component.window_end);

      daqdataformats::ComponentRequest temp(component.component, new_begin, new_end);
      slice_components.push_back(temp);

      m_data_request_width += new_end - new_begin;

    } // loop over component in trigger decision

    // Pleae note that the system could generate empty sequences
    // The code keeps them.

    // create the book entry
    TriggerId slice_id(td, sequence);

    auto it = m_trigger_records.find(slice_id);
    if (it != m_trigger_records.end()) {
      ers::error(DuplicatedTriggerDecision(ERS_HERE, slice_id));
      ++m_duplicated_trigger_ids;
      continue;
    }

    // create trigger record for the slice
    auto& entry = m_trigger_records[slice_id] = std::make_pair(clock_type::now(), trigger_record_ptr_t());
    ;
    trigger_record_ptr_t& trp = entry.second;
    trp.reset(new daqdataformats::TriggerRecord(slice_components));
    daqdataformats::TriggerRecord& tr = *trp;

    tr.get_header_ref().set_trigger_number(td.trigger_number);
    tr.get_header_ref().set_sequence_number(sequence);
    tr.get_header_ref().set_max_sequence_number(max_sequence_number);
    tr.get_header_ref().set_run_number(td.run_number);
    tr.get_header_ref().set_trigger_timestamp(td.trigger_timestamp);
    tr.get_header_ref().set_trigger_type(td.trigger_type);
    tr.get_header_ref().set_element_id(m_this_trb_source_id);

    m_trigger_decisions_counter++;
    m_pending_fragment_counter += slice_components.size();
    ++new_tr_counter;

    // create and send the requests
    TLOG_DEBUG(TLVL_WORK_STEPS) << get_name() << ": Trigger Decision components: " << td.components.size()
                                << ", slice components: " << slice_components.size();

    for (const auto& component : slice_components) {

      dfmessages::DataRequest dataReq;
      dataReq.trigger_number = td.trigger_number;
      dataReq.sequence_number = sequence;
      dataReq.run_number = td.run_number;
      dataReq.trigger_timestamp = td.trigger_timestamp;
      dataReq.readout_type = td.readout_type;
      dataReq.request_information = component;
      dataReq.data_destination = m_reply_connection;
      TLOG_DEBUG(TLVL_WORK_STEPS) << get_name() << ": TR " << slice_id << ": trig_timestamp "
                                  << dataReq.trigger_timestamp << ": SourceID " << component.component << ": window ["
                                  << dataReq.request_information.window_begin << ", "
                                  << dataReq.request_information.window_end << ']';

      dispatch_data_requests(std::move(dataReq), component.component, running);

    } // loop loop over component in the slice

  } // sequence loop

  return new_tr_counter;
}

bool
TRBModule::dispatch_data_requests(dfmessages::DataRequest dr,
                                  const daqdataformats::SourceID& sid,
                                  std::atomic<bool>& running)

{

  // find the queue for sourceid_req in the map
  std::unique_lock<std::mutex> lk(m_map_sourceid_connections_mutex);
  std::shared_ptr<data_req_sender_t> sender = nullptr;
  auto it_req = m_map_sourceid_connections.find(sid);
  if (it_req == m_map_sourceid_connections.end() || it_req->second == nullptr) {

    // if sourceid request is not valid. then print error and continue
    ers::error(
      dunedaq::dfmodules::DRSenderLookupFailed(ERS_HERE, sid, dr.run_number, dr.trigger_number, dr.sequence_number));
    ++m_invalid_requests;
    return false; // lk goes out of scope, is destroyed
  } else {
    // get the queue from map element
    sender = it_req->second;
  }
  lk.unlock();

  if (sender == nullptr) {
    // if sender lookup failed, report error and continue
    ers::error(
      dunedaq::dfmodules::DRSenderLookupFailed(ERS_HERE, sid, dr.run_number, dr.trigger_number, dr.sequence_number));
    ++m_invalid_requests;
    return false;
  }

  bool wasSentSuccessfully = false;
  do {
    TLOG_DEBUG(TLVL_DISPATCH_DATAREQ) << get_name() << ": Pushing the DataRequest from trigger/sequence number "
                                      << dr.trigger_number << "." << dr.sequence_number
                                      << " onto connection :" << sender->get_name();

    // send data request into the corresponding connection
    try {
      sender->send(std::move(dr), m_queue_timeout);
      wasSentSuccessfully = true;
      ++m_generated_data_requests;
    } catch (const ers::Issue& excpt) {
      std::ostringstream oss_warn;
      oss_warn << "Send to connection \"" << sender->get_name() << "\" failed";
      ers::warning(iomanager::OperationFailed(ERS_HERE, oss_warn.str(), excpt));
    }
  } while (!wasSentSuccessfully && running.load());

  return wasSentSuccessfully;
}

bool
TRBModule::send_trigger_record(const TriggerId& id, std::atomic<bool>& running)
{

  trigger_record_ptr_t temp_record(extract_trigger_record(id));

  // Send to monitoring, if needed

  if (m_mon_receiver) {
    const std::lock_guard<std::mutex> lock(m_mon_mutex);
    auto it = m_mon_requests.begin();
    while (it != m_mon_requests.end()) {
      // send TR to mon if correct trigger type
      if (it->trigger_type == temp_record->get_header_data().trigger_type) {
        auto iom = iomanager::IOManager::get();
        bool wasSentSuccessfully = false;
        do {
          try {
            // HACK to copy the trigger record so we can send it off to monitoring
            auto trigger_record_bytes =
              serialization::serialize(temp_record, serialization::SerializationType::kMsgPack);
            trigger_record_ptr_t record_copy = serialization::deserialize<trigger_record_ptr_t>(trigger_record_bytes);
            iom->get_sender<trigger_record_ptr_t>(it->data_destination)->send(std::move(record_copy), m_queue_timeout);
            ++m_trmon_sent_counter;
            wasSentSuccessfully = true;
          } catch (const ers::Issue& excpt) {
            std::ostringstream oss_warn;
            oss_warn << "Sending TR to connection \"" << it->data_destination << "\" failed";
            ers::warning(iomanager::OperationFailed(ERS_HERE, oss_warn.str(), excpt));
          }
        } while (running.load() && !wasSentSuccessfully);
        it = m_mon_requests.erase(it);
      } else {
        ++it;
      }
    }
  } // if m_mon_receiver

  bool wasSentSuccessfully = false;
  do {
    try {
      m_trigger_record_output->send(std::move(temp_record), m_queue_timeout);
      wasSentSuccessfully = true;
      ++m_generated_trigger_records;
    } catch (const ers::Issue& excpt) {
      ers::warning(excpt);
    }
  } while (running.load() && !wasSentSuccessfully); // push while loop

  if (!wasSentSuccessfully) {
    ++m_abandoned_trigger_records;
    m_lost_fragments += temp_record->get_fragments_ref().size();
    ers::error(dunedaq::dfmodules::AbandonedTriggerDecision(ERS_HERE, id));
  }

  return wasSentSuccessfully;
}

bool
TRBModule::check_stale_requests(std::atomic<bool>& running)
{

  bool book_updates = false;

  // -----------------------------------------------
  // optionally send over stale trigger records
  // -----------------------------------------------

  if (m_trigger_timeout.count() > 0) {

    std::vector<TriggerId> stale_triggers;

    for (auto it = m_trigger_records.begin(); it != m_trigger_records.end(); ++it) {

      daqdataformats::TriggerRecord& tr = *it->second.second;

      auto tr_time = clock_type::now() - it->second.first;

      if (tr_time > m_trigger_timeout) {

        ers::error(TimedOutTriggerDecision(ERS_HERE, it->first, tr.get_header_ref().get_trigger_timestamp()));

        // mark trigger record for seding
        stale_triggers.push_back(it->first);
        ++m_timed_out_trigger_records;

        book_updates = true;
      }

    } // trigger record loop

    // create the trigger record and send it
    for (const auto& t : stale_triggers) {
      send_trigger_record(t, running);
    }

  } //  m_trigger_timeout > 0

  return book_updates;
}

} // namespace dfmodules
} // namespace dunedaq

DEFINE_DUNE_DAQ_MODULE(dunedaq::dfmodules::TRBModule)
