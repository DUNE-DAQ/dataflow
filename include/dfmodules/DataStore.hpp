/**
 * @file DataStore.hpp
 *
 * This is the interface for storing and retrieving data from
 * various storage systems.
 *
 * This is part of the DUNE DAQ Application Framework, copyright 2020.
 * Licensing/copyright details are in the COPYING file that you should have
 * received with this code.
 */

// 09-Sep-2020, KAB: the initial version of this class was based on the
// Queue interface from the appfwk repo.

#ifndef DFMODULES_INCLUDE_DFMODULES_DATASTORE_HPP_
#define DFMODULES_INCLUDE_DFMODULES_DATASTORE_HPP_

#include "appfwk/ModuleConfiguration.hpp"
#include "opmonlib/MonitorableObject.hpp"
#include "cetlib/BasicPluginFactory.h"
#include "cetlib/compiler_macros.h"
#include "daqdataformats/TimeSlice.hpp"
#include "daqdataformats/TriggerRecord.hpp"
#include "daqdataformats/Types.hpp"
#include "logging/Logging.hpp"
#include "utilities/NamedObject.hpp"

#include "nlohmann/json.hpp"

#include <chrono>
#include <cstddef>
#include <memory>
#include <string>
#include <vector>

#ifndef EXTERN_C_FUNC_DECLARE_START
// NOLINTNEXTLINE(build/define_used)
#define EXTERN_C_FUNC_DECLARE_START                                                                                    \
  extern "C"                                                                                                           \
  {
#endif
/**
 * @brief Declare the function that will be called by the plugin loader
 * @param klass Class to be defined as a DUNE IPM Receiver
 */
// NOLINTNEXTLINE(build/define_used)
#define DEFINE_DUNE_DATA_STORE(klass)                                                                                  \
  EXTERN_C_FUNC_DECLARE_START                                                                                          \
  std::shared_ptr<dunedaq::dfmodules::DataStore> make(const std::string& name,                                         \
                                                      std::shared_ptr<dunedaq::appfwk::ModuleConfiguration> mcfg,      \
		  				      const std::string& writer_name	)                              \
  {                                                                                                                    \
    return std::shared_ptr<dunedaq::dfmodules::DataStore>(new klass(name, mcfg, writer_name));                         \
  }                                                                                                                    \
  }

namespace dunedaq {

/**
 * @brief An ERS Issue for DataStore creation failure
 * @cond Doxygen doesn't like ERS macros LCOV_EXCL_START
 */
ERS_DECLARE_ISSUE(dfmodules,                                                             ///< Namespace
                  DataStoreCreationFailed,                                               ///< Type of the Issue
                  "Failed to create DataStore " << plugin_name << " with name " << name, ///< Log Message from the issue
                  ((std::string)plugin_name)((std::string)name)                          ///< Message parameters
)
/// @endcond LCOV_EXCL_STOP

/**
 * @brief An ERS Issue for DataStore problems in which it is
 * reasonable to retry the operation.
 * @cond Doxygen doesn't like ERS macros LCOV_EXCL_START
 */
ERS_DECLARE_ISSUE(dfmodules,
                  RetryableDataStoreProblem,
                  "Module " << mod_name << ": A problem was encountered when " << description,
                  ((std::string)mod_name)((std::string)description))
/// @endcond LCOV_EXCL_STOP

/**
 * @brief An ERS Issue for DataStore problems in which it is
 * reasonable to skip any warning or error message.
 * @cond Doxygen doesn't like ERS macros LCOV_EXCL_START
 */
ERS_DECLARE_ISSUE(dfmodules,
                  IgnorableDataStoreProblem,
                  "Module " << mod_name << ": A problem was encountered when " << description,
                  ((std::string)mod_name)((std::string)description))
/// @endcond LCOV_EXCL_STOP

/**
 * @brief An ERS Issue for DataStore problems in which it is
 * not clear whether retrying the operation might succeed or not.
 * @cond Doxygen doesn't like ERS macros LCOV_EXCL_START
 */
ERS_DECLARE_ISSUE(dfmodules,
                  GeneralDataStoreProblem,
                  "Module " << mod_name << ": A problem was encountered when " << description,
                  ((std::string)mod_name)((std::string)description))
/// @endcond LCOV_EXCL_STOP

namespace dfmodules {

/**
 * @brief comment
 */
class DataStore : public utilities::NamedObject, public opmonlib::MonitorableObject
{
public:
  /**
   * @brief DataStore Constructor
   * @param name Name of the DataStore instance
   */
  explicit DataStore(const std::string& name)
    : utilities::NamedObject(name), MonitorableObject()
  {
  }

  /**
   * @brief Writes the TriggerRecord into the DataStore.
   * @param tr TriggerRecord to write.
   */
  virtual void write(const daqdataformats::TriggerRecord& tr) = 0;

  /**
   * @brief Writes the TimeSlice into the DataStore.
   * @param ts TimeSlice to write.
   */
  virtual void write(const daqdataformats::TimeSlice& ts) = 0;

  /**
   * @brief Informs the DataStore that writes or reads of data blocks associated
   * with the specified run number will soon be requested.
   * This allows DataStore instances to make any preparations that will be
   * beneficial in advance of the first data blocks being written or read.
   */
  virtual void prepare_for_run(daqdataformats::run_number_t run_number,
                               bool run_is_for_test_purposes) = 0;

  /**
   * @brief Informs the DataStore that writes or reads of data blocks associated
   * with the specified run number have finished, for now.
   * This allows DataStore instances to do any cleanup or shutdown operations
   * that are useful once the writes or reads for a given run number have finished.
   */
  virtual void finish_with_run(daqdataformats::run_number_t run_number) = 0;

private:
  DataStore(const DataStore&) = delete;
  DataStore& operator=(const DataStore&) = delete;
  DataStore(DataStore&&) = delete;
  DataStore& operator=(DataStore&&) = delete;
};

/**
 * @brief Load a DataSrore plugin and return a unique_ptr to the contained
 * DAQModule class
 * @param plugin_name Name of the plugin, e.g. HDF5DataStore
 * @param json configuration for the DataStore
 * @return unique_ptr to created DataStore instance
 */
inline std::shared_ptr<DataStore>
make_data_store(const std::string& type,
                const std::string& name,
                std::shared_ptr<dunedaq::appfwk::ModuleConfiguration> mcfg,
		const std::string& writer_identifier)
{
  static cet::BasicPluginFactory bpf("duneDataStore", "make"); // NOLINT

  std::shared_ptr<DataStore> ds;
  try {
    ds = bpf.makePlugin<std::shared_ptr<DataStore>>(type, name, mcfg, writer_identifier);
  } catch (const cet::exception& cexpt) {
    throw DataStoreCreationFailed(ERS_HERE, type, name, cexpt);
  }

  return ds;
}

} // namespace dfmodules
} // namespace dunedaq

#endif // DFMODULES_INCLUDE_DFMODULES_DATASTORE_HPP_
