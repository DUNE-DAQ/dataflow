/**
 * @file FakeDFOClientModule.hpp
 *
 * This is part of the DUNE DAQ Software Suite, copyright 2024.
 * Licensing/copyright details are in the COPYING file that you should have
 * received with this code.
 */

#ifndef DFMODULES_PLUGINS_FakeDFOClientMODULE_HPP_
#define DFMODULES_PLUGINS_FakeDFOClientMODULE_HPP_

#include "appmodel/FakeDFOClientConf.hpp"
#include "dfmessages/TriggerDecision.hpp"
#include "dfmessages/TriggerDecisionToken.hpp"

#include "dfmodules/opmon/FakeDFOClientModule.pb.h"

#include "iomanager/Receiver.hpp"
#include "iomanager/Sender.hpp"

#include "appfwk/DAQModule.hpp"
#include "logging/Logging.hpp"
#include "utilities/WorkerThread.hpp"

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace dunedaq {

// Disable coverage checking LCOV_EXCL_START
// Re-enable coverage checking LCOV_EXCL_STOP

namespace dfmodules {

/**
 * @brief FakeDFOClientModule distributes triggers according to the
 * availability of the DF apps in the system
 */
class FakeDFOClientModule : public dunedaq::appfwk::DAQModule
{
public:
  /**
   * @brief FakeDFOClientModule Constructor
   * @param name Instance name for this FakeDFOClientModule instance
   */
  explicit FakeDFOClientModule(const std::string& name);

  FakeDFOClientModule(const FakeDFOClientModule&) = delete;            ///< FakeDFOClientModule is not copy-constructible
  FakeDFOClientModule& operator=(const FakeDFOClientModule&) = delete; ///< FakeDFOClientModule is not copy-assignable
  FakeDFOClientModule(FakeDFOClientModule&&) = delete;                 ///< FakeDFOClientModule is not move-constructible
  FakeDFOClientModule& operator=(FakeDFOClientModule&&) = delete;      ///< FakeDFOClientModule is not move-assignable

  void init(std::shared_ptr<appfwk::ModuleConfiguration> mcfg) override;

private:
  // Commands
  void do_conf(const data_t&);
  void do_scrap(const data_t&);
  void do_start(const data_t&);
  void do_stop(const data_t&);

  void generate_opmon_data() override;

  void wait_and_send_token(const dfmessages::TriggerDecision&, std::stop_token);
  void receive_trigger_decision(const dfmessages::TriggerDecision&);

  // Configuration
  const appmodel::FakeDFOClientConf* m_fakedfoclient_conf;
  std::chrono::microseconds m_token_wait_us;
  std::chrono::milliseconds m_send_token_timeout;

  // Threading
  std::stop_source m_stop_source;

  // Connections
  std::string m_token_connection;     // Output
  std::string m_td_connection;        // Input

  // Statistics
  std::atomic<uint64_t> m_received_decisions;
  std::atomic<uint64_t> m_sent_tokens;
  std::atomic<uint64_t> m_total_received_decisions;
  std::atomic<uint64_t> m_total_sent_tokens;
};
} // namespace dfmodules
} // namespace dunedaq

#endif // DFMODULES_PLUGINS_FakeDFOClientMODULE_HPP_
