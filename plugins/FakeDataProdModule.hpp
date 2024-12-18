/**
 * @file FakeDataProdModule.hpp
 *
 * This is part of the DUNE DAQ Software Suite, copyright 2020.
 * Licensing/copyright details are in the COPYING file that you should have
 * received with this code.
 */

#ifndef DFMODULES_PLUGINS_FAKEDATAPROD_HPP_
#define DFMODULES_PLUGINS_FAKEDATAPROD_HPP_

#include "daqdataformats/Fragment.hpp"
#include "dfmessages/DataRequest.hpp"

#include "appmodel/FakeDataProdConf.hpp"
#include "appfwk/DAQModule.hpp"
#include "utilities/WorkerThread.hpp"

#include <memory>
#include <string>
#include <vector>

namespace dunedaq {

// Disable coverage checking LCOV_EXCL_START
ERS_DECLARE_ISSUE(dfmodules,
                  FragmentTransmissionFailed,
                  mod_name << " failed to send data for trigger number " << tr_num << ".",
                  ((std::string)mod_name)((int64_t)tr_num))

ERS_DECLARE_ISSUE(dfmodules,
                  TimeSyncTransmissionFailed,
                  mod_name << " failed to send send TimeSync message to " << dest << ".",
                  ((std::string)mod_name)((std::string)dest))
ERS_DECLARE_ISSUE_BASE(dfmodules,
                       MemoryAllocationFailed,
                       appfwk::GeneralDAQModuleIssue,
                       "Malloc of " << bytes << " bytes failed",
                       ((std::string)name),
                       ((size_t)bytes))

// Disable coverage checking LCOV_EXCL_STOP

namespace dfmodules {

/**
 * @brief FakeDataProdModule is simply an example
 */
class FakeDataProdModule : public dunedaq::appfwk::DAQModule
{
public:
  /**
   * @brief FakeDataProdModule Constructor
   * @param name Instance name for this FakeDataProdModule instance
   */
  explicit FakeDataProdModule(const std::string& name);

  FakeDataProdModule(const FakeDataProdModule&) = delete;            ///< FakeDataProdModule is not copy-constructible
  FakeDataProdModule& operator=(const FakeDataProdModule&) = delete; ///< FakeDataProdModule is not copy-assignable
  FakeDataProdModule(FakeDataProdModule&&) = delete;                 ///< FakeDataProdModule is not move-constructible
  FakeDataProdModule& operator=(FakeDataProdModule&&) = delete;      ///< FakeDataProdModule is not move-assignable

  void init(std::shared_ptr<appfwk::ConfigurationManager> mcfg) override;

private:
  // Commands
  void do_conf(const data_t&);
  void do_start(const data_t&);
  void do_stop(const data_t&);

  //  void get_info(opmonlib::InfoCollector& ci, int level) override;

  // Threading
  dunedaq::utilities::WorkerThread m_timesync_thread;
  void process_data_request(dfmessages::DataRequest&);
  void do_timesync(std::atomic<bool>&);

  // Configuration
  const appmodel::FakeDataProdConf* m_fake_data_prod_conf;
  // size_t m_sleep_msec_while_running;
  std::chrono::milliseconds m_queue_timeout;
  dunedaq::daqdataformats::run_number_t m_run_number;
  daqdataformats::SourceID m_sourceid;
  uint64_t m_time_tick_diff; // NOLINT (build/unsigned)
  uint64_t m_frame_size;     // NOLINT (build/unsigned)
  uint64_t m_response_delay; // NOLINT (build/unsigned)
  daqdataformats::FragmentType m_fragment_type;
  std::string m_timesync_topic_name;
  uint32_t m_pid_of_current_process; // NOLINT (build/unsigned)

  std::string m_data_request_id;
  std::string m_timesync_id;

  std::atomic<uint64_t> m_received_requests{ 0 }; // NOLINT (build/unsigned)
  std::atomic<uint64_t> m_sent_fragments{ 0 };    // NOLINT (build/unsigned)
};
} // namespace dfmodules
} // namespace dunedaq

#endif // DFMODULES_PLUGINS_FAKEDATAPROD_HPP_
