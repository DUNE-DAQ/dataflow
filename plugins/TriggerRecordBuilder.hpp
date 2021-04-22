/**
 * @file TriggerRecordBuilder.hpp
 *
 * This is part of the DUNE DAQ Software Suite, copyright 2020.
 * Licensing/copyright details are in the COPYING file that you should have
 * received with this code.
 */

#ifndef DFMODULES_PLUGINS_TRIGGERRECORDBUILDER_HPP_
#define DFMODULES_PLUGINS_TRIGGERRECORDBUILDER_HPP_


#include "dfmodules/TriggerDecisionForwarder.hpp"

#include "dfmodules/fragmentreceiverinfo/Nljs.hpp"

#include "dataformats/Fragment.hpp"
#include "dataformats/TriggerRecord.hpp"
#include "dataformats/Types.hpp"
#include "dfmessages/TriggerDecision.hpp"
#include "dfmessages/Types.hpp"

#include "appfwk/DAQModule.hpp"
#include "appfwk/DAQSink.hpp"
#include "appfwk/DAQSource.hpp"
#include "appfwk/ThreadHelper.hpp"

#include <map>
#include <memory>
#include <string>
#include <vector>

namespace dunedaq {

  namespace dfmodules {

    /**
     * @brief TriggerId is a little class that defines a unique identifier for a trigger decision/record
     * It also provides an operator < to be used by map to optimise bookkeeping
     */
    struct TriggerId
    {

      explicit TriggerId(const dfmessages::TriggerDecision& td)
	: trigger_number(td.trigger_number)
	, run_number(td.run_number)
      {
	;
      }
      explicit TriggerId(dataformats::Fragment& f)
	: trigger_number(f.get_trigger_number())
	, run_number(f.get_run_number())
      {
	;
      }

      dataformats::trigger_number_t trigger_number;
      dataformats::run_number_t run_number;

      bool operator<(const TriggerId& other) const noexcept
      {
	return run_number == other.run_number ? trigger_number < other.trigger_number : run_number < other.run_number;
      }

      friend std::ostream& operator<<(std::ostream& out, const TriggerId& id)
      {
	out << id.trigger_number << '/' << id.run_number;
	return out;
      }

      friend TraceStreamer& operator<<(TraceStreamer& out, const TriggerId& id)
      {
	return out << id.trigger_number << "/" << id.run_number;
      }
    };


    /**
     * @brief Timed out Trigger Decision
     */
    ERS_DECLARE_ISSUE(ERS_EMPTY,               ///< Namespace
		      TimedOutTriggerDecision, ///< Issue class name
		      "trigger id " << trigger_id 
		      << " generate at: " << trigger_timestamp 
		      << " too late for: " << present_time,           ///< Message
		      ((TriggerID)trigger_id)                         ///< Message parameters
		      ((dataformats::timestamp_t)trigger_timestamp)   ///< Message parameters
		      ((dataformats::timestamp_t)present_time)        ///< Message parameters
		      )
    
    /**
     * @brief Unexpected fragment
     */
    ERS_DECLARE_ISSUE(ERS_EMPTY,                  ///< Namespace
		      UnexpectedFragment,         ///< Issue class name
		      "Unexpected fragment - triggerID: " << trigger_id 
		      << " type: " << fragment_type 
		      << " GeoID: " << geo_id,
		      ((TriggerID)trigger_id) ///< Message parameters
		      ((dataformats::fragment_type_t)fragment_type)   ///< Message parameters
		      ((dataformats::GeoID)geo_id)
		      )
    
    using apatype = decltype(dataformats::GeoID::apa_number);
    using linktype = decltype(dataformats::GeoID::link_number);

    /**
     * @brief Unknown GeoID
     */
    ERS_DECLARE_ISSUE(ERS_EMPTY,    ///< Namespace
		      UnknownGeoID, ///< Issue class name
		      "trigger id " << trigger_id << " of run: " << run_number << " of APA: " << apa
		      << " of Link: " << link,
		      ((TriggerID)trigger_id)    ///< Message parameters
		      ((apatype)apa)                 ///< Message parameters
		      ((linktype)link)               ///< Message parameters
		      )

    /**
     * @brief Duplicate trigger decision
     */
    ERS_DECLARE_ISSUE(ERS_EMPTY,    ///< Namespace
		      DuplicatedTriggerDecision, ///< Issue class name
		      "trigger id " << trigger_id << " already in the book" 
		      ((TriggerID)trigger_id)    ///< Message parameters
		      )




    /**
     * @brief TriggerRecordBuilder is the Module that collects Trigger TriggersDecisions, 
     sends the corresponding data requests and collects Fragment to form a complete Trigger Record.
     The TR then sent out possibly to a write module
    */
    class TriggerRecordBuilder : public dunedaq::appfwk::DAQModule
    {
    public:
      /**
       * @brief TriggerRecordBuilder Constructor
       * @param name Instance name for this TriggerRecordBuilder instance
       */
      explicit TriggerRecordBuilder(const std::string& name);

      TriggerRecordBuilder(const TriggerRecordBuilder&) = delete;            ///< TriggerRecordBuilder is not copy-constructible
      TriggerRecordBuilder& operator=(const TriggerRecordBuilder&) = delete; ///< TriggerRecordBuilder is not copy-assignable
      TriggerRecordBuilder(TriggerRecordBuilder&&) = delete;                 ///< TriggerRecordBuilder is not move-constructible
      TriggerRecordBuilder& operator=(TriggerRecordBuilder&&) = delete;      ///< TriggerRecordBuilder is not move-assignable

      void init(const data_t&) override;
      void get_info(opmonlib::InfoCollector& ci, int level) override;

    protected:
      using trigger_decision_source_t = dunedaq::appfwk::DAQSource<dfmessages::TriggerDecision>;
      using datareqsink_t = dunedaq::appfwk::DAQSink<dfmessages::DataRequest>;
      using datareqsinkmap_t = std::map<dataformats::GeoID, std::unique_ptr<datareqsink_t> > ;

      using fragment_source_t = dunedaq::appfwk::DAQSource<std::unique_ptr<dataformats::Fragment>>;
      using fragment_sources_t = std::vector<std::unique_ptr<fragment_source_t>>;

      using trigger_record_ptr_t = std::unique_ptr<dataformats::TriggerRecord> ;
      using trigger_record_sink_t = appfwk::DAQSink<trigger_record_ptr_t>;

      
      bool read_fragments( fragment_sources_t&, bool drain = false);
      
      trigger_record_ptr_t extract_trigger_record(const TriggerId&);
      // build_trigger_record will allocate memory and then orphan it to the caller via the returned pointer
      // Plese note that the method will destroy the memory saved in the bookkeeping map

      bool dispatch_data_requests( const dfmessages::TriggerDecision &, datareqsinkmap_t & ) const ;

      bool send_trigger_record(const TriggerId&, trigger_record_sink_t&, std::atomic<bool>& running);
      // this creates a trigger record and send it
      
      bool check_stale_requests() const;
      
      void fill_counters() const;

    private:
      // Commands
      void do_conf(const data_t&);
      void do_start(const data_t&);
      void do_stop(const data_t&);

      // Threading
      dunedaq::appfwk::ThreadHelper m_thread;
      void do_work(std::atomic<bool>&);

      // Configuration
      // size_t m_sleep_msec_while_running;
      std::chrono::milliseconds m_queue_timeout;

      // Input Queues
      std::string m_trigger_decision_source_name;
      std::vector<std::string> m_fragment_source_names;

      // Output queues
      std::string m_trigger_record_sink_name;
      std::map<dataformats::GeoID, std::string> m_map_geoid_queues; ///< Mappinng between GeoID and queues

      // bookeeping
      std::map<TriggerId, trigger_record_ptr_t> m_trigger_records;

      // book related metrics
      using metric_counter_type = decltype(fragmentreceiverinfo::Info::trigger_decisions);
      mutable std::atomic<metric_counter_type> m_trigger_decisions_counter = { 0 };
      mutable std::atomic<metric_counter_type> m_fragment_counter = { 0 };
      mutable std::atomic<metric_counter_type> m_old_fragment_index_counter = { 0 };
      mutable std::atomic<metric_counter_type> m_old_fragment_counter = { 0 };

      dataformats::timestamp_diff_t m_max_time_difference;
      dataformats::timestamp_t m_current_time = 0;
    };
  } // namespace dfmodules
} // namespace dunedaq

#endif // DFMODULES_PLUGINS_TRIGGERRECORDBUILDER_HPP_