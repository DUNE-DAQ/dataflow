/**
 * @file HDF5DataStore.hpp
 *
 * An implementation of the DataStore interface that uses the
 * highFive library to create objects in HDF5 Groups and datasets
 *
 * This is part of the DUNE DAQ Application Framework, copyright 2020.
 * Licensing/copyright details are in the COPYING file that you should have
 * received with this code.
 */

#ifndef DFMODULES_PLUGINS_HDF5DATASTORE_HPP_
#define DFMODULES_PLUGINS_HDF5DATASTORE_HPP_

#include "HDF5FileUtils.hpp"
#include "dfmodules/DataStore.hpp"
#include "dfmodules/opmon/DataStore.pb.h"

#include "hdf5libs/HDF5RawDataFile.hpp"

#include "appmodel/DataStoreConf.hpp"
#include "appmodel/FilenameParams.hpp"
#include "confmodel/DetectorConfig.hpp"
#include "confmodel/Session.hpp"

#include "appfwk/DAQModule.hpp"
#include "logging/Logging.hpp"

#include "boost/date_time/posix_time/posix_time.hpp"
#include "boost/lexical_cast.hpp"

#include <cstdlib>
#include <functional>
#include <memory>
#include <string>
#include <sys/statvfs.h>
#include <utility>
#include <vector>

namespace dunedaq {

// Disable coverage checking LCOV_EXCL_START
/**
 * @brief A ERS Issue to report an HDF5 exception
 */
ERS_DECLARE_ISSUE_BASE(dfmodules,
                       InvalidOperationMode,
                       appfwk::GeneralDAQModuleIssue,
                       "Selected operation mode \"" << selected_operation
                                                    << "\" is NOT supported. Please update the configuration file.",
                       ((std::string)name),
                       ((std::string)selected_operation))

ERS_DECLARE_ISSUE_BASE(dfmodules,
                       FileOperationProblem,
                       appfwk::GeneralDAQModuleIssue,
                       "A problem was encountered when opening or closing file \"" << filename << "\"",
                       ((std::string)name),
                       ((std::string)filename))

ERS_DECLARE_ISSUE_BASE(dfmodules,
                       InvalidHDF5Dataset,
                       appfwk::GeneralDAQModuleIssue,
                       "The HDF5 Dataset associated with name \"" << data_set << "\" is invalid. (file = " << filename
                                                                  << ")",
                       ((std::string)name),
                       ((std::string)data_set)((std::string)filename))

ERS_DECLARE_ISSUE_BASE(dfmodules,
                       InvalidOutputPath,
                       appfwk::GeneralDAQModuleIssue,
                       "The specified output destination, \"" << output_path
                                                              << "\", is not a valid file system path on this server.",
                       ((std::string)name),
                       ((std::string)output_path))

ERS_DECLARE_ISSUE_BASE(dfmodules,
                       InsufficientDiskSpace,
                       appfwk::GeneralDAQModuleIssue,
                       "There is insufficient free space on the disk associated with output file path \""
                         << path << "\". There are " << free_bytes << " bytes free, and the "
                         << "required minimum is " << needed_bytes << " bytes based on " << criteria << ".",
                       ((std::string)name),
                       ((std::string)path)((size_t)free_bytes)((size_t)needed_bytes)((std::string)criteria))

ERS_DECLARE_ISSUE_BASE(dfmodules,
                       EmptyDataBlockList,
                       appfwk::GeneralDAQModuleIssue,
                       "There was a request to write out a list of data blocks, but the list was empty. "
                         << "Ignoring this request",
                       ((std::string)name),
                       ERS_EMPTY)

// Re-enable coverage checking LCOV_EXCL_STOP
namespace dfmodules {

/**
 * @brief HDF5DataStore creates an HDF5 instance
 * of the DataStore class
 */
class HDF5DataStore : public DataStore
{

public:
  enum
  {
    TLVL_BASIC = 2,
    TLVL_FILE_SIZE = 5
  };

  /**
   * @brief HDF5DataStore Constructor
   * @param name, path, filename, operationMode
   *
   */
  explicit HDF5DataStore(std::string const& name,
                         std::shared_ptr<appfwk::ModuleConfiguration> mcfg,
                         std::string const& writer_name)
    : DataStore(name)
    , m_basic_name_of_open_file("")
    , m_open_flags_of_open_file(0)
    , m_run_number(0)
    , m_writer_identifier(writer_name)
  {
    TLOG_DEBUG(TLVL_BASIC) << get_name();

    m_config_params = mcfg->module<appmodel::DataStoreConf>(name);
    m_file_layout_params = m_config_params->get_file_layout_params();
    m_session = mcfg->configuration_manager()->session();
    m_operational_environment = mcfg->configuration_manager()->session()->get_detector_configuration()->get_op_env();
    m_offline_data_stream = mcfg->configuration_manager()->session()->get_detector_configuration()->get_offline_data_stream();

    m_operation_mode = m_config_params->get_mode();
    m_path = m_config_params->get_directory_path();
    m_max_file_size = m_config_params->get_max_file_size();
    m_disable_unique_suffix = m_config_params->get_disable_unique_filename_suffix();
    m_free_space_safety_factor_for_write = m_config_params->get_free_space_safety_factor();
    if (m_free_space_safety_factor_for_write < 1.1) {
      m_free_space_safety_factor_for_write = 1.1;
    }
    m_compression_level = m_config_params->get_compression_level();

    m_file_index = 0;
    m_recorded_size = 0;
    m_current_record_number = std::numeric_limits<size_t>::max();

    if (m_operation_mode != "one-event-per-file"
        //&& m_operation_mode != "one-fragment-per-file"
        && m_operation_mode != "all-per-file") {

      throw InvalidOperationMode(ERS_HERE, get_name(), m_operation_mode);
    }

    // 05-Apr-2022, KAB: added warning message when the output destination
    // is not a valid directory.
    struct statvfs vfs_results;
    int retval = statvfs(m_path.c_str(), &vfs_results);
    if (retval != 0) {
      ers::warning(InvalidOutputPath(ERS_HERE, get_name(), m_path));
    }
  }

  /**
   * @brief HDF5DataStore write()
   * Method used to write constant data
   * into HDF5 format. Operational mode
   * defined in the configuration file.
   *
   */
  virtual void write(const daqdataformats::TriggerRecord& tr)
  {

    // check if there is sufficient space for this record
    size_t current_free_space = get_free_space(m_path);
    size_t tr_size = tr.get_total_size_bytes();
    if (current_free_space < (m_free_space_safety_factor_for_write * tr_size)) {
      std::ostringstream msg_oss;
      msg_oss << "a safety factor of " << m_free_space_safety_factor_for_write << " times the trigger record size";
      InsufficientDiskSpace issue(ERS_HERE,
                                  get_name(),
                                  m_path,
                                  current_free_space,
                                  (m_free_space_safety_factor_for_write * tr_size),
                                  msg_oss.str());
      std::string msg =
        "writing a trigger record to file" + (m_file_handle ? " " + m_file_handle->get_file_name() : "");
      throw RetryableDataStoreProblem(ERS_HERE, get_name(), msg, issue);
    }

    // check if a new file should be opened for this record
    if (! increment_file_index_if_needed(tr_size)) {
      if (m_operation_mode == "one-event-per-file") {
        if (m_current_record_number != std::numeric_limits<size_t>::max() &&
            tr.get_header_ref().get_trigger_number() != m_current_record_number) {
          ++m_file_index;
        }
      }
    }
    m_current_record_number = tr.get_header_ref().get_trigger_number();

    // determine the filename from Storage Key + configuration parameters
    std::string full_filename = get_file_name(tr.get_header_ref().get_run_number());

    try {
      open_file_if_needed(full_filename, HighFive::File::OpenOrCreate);
    } catch (std::exception const& excpt) {
      throw FileOperationProblem(ERS_HERE, get_name(), full_filename, excpt);
    } catch (...) { // NOLINT(runtime/exceptions)
      // NOLINT here because we *ARE* re-throwing the exception!
      throw FileOperationProblem(ERS_HERE, get_name(), full_filename);
    }

    // write the record
    m_file_handle->write(tr);
    m_recorded_size = m_file_handle->get_recorded_size();

    m_new_bytes += tr_size;
    ++m_new_objects;
  }

  /**
   * @brief HDF5DataStore write()
   * Method used to write constant data
   * into HDF5 format. Operational mode
   * defined in the configuration file.
   *
   */
  virtual void write(const daqdataformats::TimeSlice& ts)
  {

    // check if there is sufficient space for this record
    size_t current_free_space = get_free_space(m_path);
    size_t ts_size = ts.get_total_size_bytes();
    if (current_free_space < (m_free_space_safety_factor_for_write * ts_size)) {
      std::ostringstream msg_oss;
      msg_oss << "a safety factor of " << m_free_space_safety_factor_for_write << " times the time slice size";
      InsufficientDiskSpace issue(ERS_HERE,
                                  get_name(),
                                  m_path,
                                  current_free_space,
                                  (m_free_space_safety_factor_for_write * ts_size),
                                  msg_oss.str());
      std::string msg = "writing a time slice to file " + m_file_handle->get_file_name();
      throw RetryableDataStoreProblem(ERS_HERE, get_name(), msg, issue);
    }

    // check if a new file should be opened for this record
    if (! increment_file_index_if_needed(ts_size)) {
      if (m_operation_mode == "one-event-per-file") {
        if (m_current_record_number != std::numeric_limits<size_t>::max() &&
            ts.get_header().timeslice_number != m_current_record_number) {
          ++m_file_index;
        }
      }
    }
    m_current_record_number = ts.get_header().timeslice_number;

    // determine the filename from Storage Key + configuration parameters
    std::string full_filename = get_file_name(ts.get_header().run_number);

    try {
      open_file_if_needed(full_filename, HighFive::File::OpenOrCreate);
    } catch (std::exception const& excpt) {
      throw FileOperationProblem(ERS_HERE, get_name(), full_filename, excpt);
    } catch (...) { // NOLINT(runtime/exceptions)
      // NOLINT here because we *ARE* re-throwing the exception!
      throw FileOperationProblem(ERS_HERE, get_name(), full_filename);
    }

    // write the record
    try {
      m_file_handle->write(ts);
      m_recorded_size = m_file_handle->get_recorded_size();
    } catch (hdf5libs::TimeSliceAlreadyExists const& excpt) {
      std::string msg = "writing a time slice to file " + m_file_handle->get_file_name();
      throw IgnorableDataStoreProblem(ERS_HERE, get_name(), msg, excpt);
    }

    m_new_bytes += ts_size;
    ++m_new_objects;
  }

  /**
   * @brief Informs the HDF5DataStore that writes or reads of records
   * associated with the specified run number will soon be requested.
   * This allows the DataStore to test that the output file path is valid
   * and any other checks that are useful in advance of the first data
   * blocks being written or read.
   *
   * This method may throw an exception if it finds a problem.
   */
  void prepare_for_run(daqdataformats::run_number_t run_number,
                       bool run_is_for_test_purposes)
  {
    m_run_number = run_number;
    m_run_is_for_test_purposes = run_is_for_test_purposes;

    struct statvfs vfs_results;
    TLOG_DEBUG(TLVL_BASIC) << get_name() << ": Preparing to get the statvfs results for path: \"" << m_path << "\"";

    int retval = statvfs(m_path.c_str(), &vfs_results);
    TLOG_DEBUG(TLVL_BASIC) << get_name() << ": statvfs return code is " << retval;
    if (retval != 0) {
      throw InvalidOutputPath(ERS_HERE, get_name(), m_path);
    }

    size_t free_space = vfs_results.f_bsize * vfs_results.f_bavail;
    TLOG_DEBUG(TLVL_BASIC) << get_name() << ": Free space on disk with path \"" << m_path << "\" is " << free_space
                           << " bytes. This will be compared with the maximum size of a single file ("
                           << m_max_file_size << ") as a simple test to see if there is enough free space.";
    if (free_space < m_max_file_size) {
      throw InsufficientDiskSpace(
        ERS_HERE, get_name(), m_path, free_space, m_max_file_size, "the configured maximum size of a single file");
    }

    m_file_index = 0;
    m_recorded_size = 0;
    m_current_record_number = std::numeric_limits<size_t>::max();
  }

  /**
   * @brief Informs the HD5DataStore that writes or reads of records
   * associated with the specified run number have finished, for now.
   * This allows the DataStore to close open files and do any other
   * cleanup or shutdown operations that are useful once the writes or
   * reads for a given run number have finished.
   */
  void finish_with_run(daqdataformats::run_number_t /*run_number*/)
  {
    if (m_file_handle.get() != nullptr) {
      std::string open_filename = m_file_handle->get_file_name();
      try {
        m_file_handle.reset();
        m_run_number = 0;
      } catch (std::exception const& excpt) {
        m_run_number = 0;
        throw FileOperationProblem(ERS_HERE, get_name(), open_filename, excpt);
      } catch (...) { // NOLINT(runtime/exceptions)
        m_run_number = 0;
        // NOLINT here because we *ARE* re-throwing the exception!
        throw FileOperationProblem(ERS_HERE, get_name(), open_filename);
      }
    }
  }

protected:
  void generate_opmon_data() override
  {

    opmon::HDF5DataStoreInfo info;

    info.set_new_bytes_output(m_new_bytes.exchange(0));
    info.set_new_written_object(m_new_objects.exchange(0));
    info.set_bytes_in_file(m_recorded_size.load());
    info.set_written_files(m_file_index.load());
    publish(std::move(info), { { "path", m_path } });
  }

private:
  HDF5DataStore(const HDF5DataStore&) = delete;
  HDF5DataStore& operator=(const HDF5DataStore&) = delete;
  HDF5DataStore(HDF5DataStore&&) = delete;
  HDF5DataStore& operator=(HDF5DataStore&&) = delete;

  std::unique_ptr<hdf5libs::HDF5RawDataFile> m_file_handle;
  const appmodel::HDF5FileLayoutParams* m_file_layout_params;
  std::string m_basic_name_of_open_file;
  unsigned m_open_flags_of_open_file;
  daqdataformats::run_number_t m_run_number;
  bool m_run_is_for_test_purposes;
  const confmodel::Session* m_session;
  std::string m_operational_environment;
  std::string m_offline_data_stream;
  std::string m_writer_identifier;

  // Total number of generated files
  std::atomic<size_t> m_file_index;

  // Total size of data being written
  std::atomic<size_t> m_recorded_size;

  // Record number for the record that is currently being written out
  // This is only useful for long-readout windows, in which there may
  // be multiple calls to write()
  size_t m_current_record_number;

  // incremental written data
  std::atomic<uint64_t> m_new_bytes;
  std::atomic<uint64_t> m_new_objects;

  // Configuration
  const appmodel::DataStoreConf* m_config_params;
  std::string m_operation_mode;
  std::string m_path;
  size_t m_max_file_size;
  bool m_disable_unique_suffix;
  float m_free_space_safety_factor_for_write;
  uint8_t m_compression_level;

  // std::unique_ptr<HDF5KeyTranslator> m_key_translator_ptr;

  /**
   * @brief Translates the specified input parameters into the appropriate filename.
   */
  std::string get_file_name(daqdataformats::run_number_t run_number)
  {
    std::ostringstream work_oss;
    work_oss << m_config_params->get_directory_path();
    if (work_oss.str().length() > 0) {
      work_oss << "/";
    }
    work_oss << m_operational_environment + "_" + m_config_params->get_filename_params()->get_file_type_prefix();
    if (work_oss.str().length() > 0) {
      work_oss << "_";
    }

    work_oss << m_config_params->get_filename_params()->get_run_number_prefix();
    work_oss << std::setw(m_config_params->get_filename_params()->get_digits_for_run_number()) << std::setfill('0')
             << run_number;
    work_oss << "_";

    work_oss << m_config_params->get_filename_params()->get_file_index_prefix();
    work_oss << std::setw(m_config_params->get_filename_params()->get_digits_for_file_index()) << std::setfill('0')
             << m_file_index;

    work_oss << "_" << m_writer_identifier;
    work_oss << ".hdf5";
    return work_oss.str();
  }

  bool increment_file_index_if_needed(size_t size_of_next_write)
  {
    if ((m_recorded_size + size_of_next_write) > m_max_file_size && m_recorded_size > 0) {
      ++m_file_index;
      m_recorded_size = 0;
      return true;
    }
    return false;
  }

  void open_file_if_needed(const std::string& file_name, unsigned open_flags = HighFive::File::ReadOnly)
  {

    if (m_file_handle.get() == nullptr || m_basic_name_of_open_file.compare(file_name) ||
        m_open_flags_of_open_file != open_flags) {

      // 04-Feb-2021, KAB: adding unique substrings to the filename
      std::string unique_filename = file_name;
      time_t now = time(0);
      std::string file_creation_timestamp = boost::posix_time::to_iso_string(boost::posix_time::from_time_t(now));
      if (!m_disable_unique_suffix) {
        // timestamp substring
        size_t ufn_len = unique_filename.length();
        if (ufn_len > 6) { // len GT 6 gives us some confidence that we have at least x.hdf5
          std::string timestamp_substring = "_" + file_creation_timestamp;
          TLOG_DEBUG(TLVL_BASIC) << get_name() << ": timestamp substring for filename: " << timestamp_substring;
          unique_filename.insert(ufn_len - 5, timestamp_substring);
        }
      }

      // close an existing open file
      if (m_file_handle.get() != nullptr) {
        std::string open_filename = m_file_handle->get_file_name();
        try {
          m_file_handle.reset();
        } catch (std::exception const& excpt) {
          throw FileOperationProblem(ERS_HERE, get_name(), open_filename, excpt);
        } catch (...) { // NOLINT(runtime/exceptions)
          // NOLINT here because we *ARE* re-throwing the exception!
          throw FileOperationProblem(ERS_HERE, get_name(), open_filename);
        }
      }

      // opening file for the first time OR something changed in the name or the way of opening the file
      TLOG_DEBUG(TLVL_BASIC) << get_name() << ": going to open file " << unique_filename << " with open_flags "
                             << std::to_string(open_flags);
      m_basic_name_of_open_file = file_name;
      m_open_flags_of_open_file = open_flags;
      try {
        m_file_handle.reset(
          new hdf5libs::HDF5RawDataFile(unique_filename,
                                        m_run_number,
                                        m_file_index,
                                        m_writer_identifier,
                                        m_file_layout_params,
                                        hdf5libs::HDF5SourceIDHandler::make_source_id_geo_id_map(m_session),
                                        m_compression_level,
                                        ".writing",
                                        open_flags));
      } catch (std::exception const& excpt) {
        throw FileOperationProblem(ERS_HERE, get_name(), unique_filename, excpt);
      } catch (...) { // NOLINT(runtime/exceptions)
        // NOLINT here because we *ARE* re-throwing the exception!
        throw FileOperationProblem(ERS_HERE, get_name(), unique_filename);
      }

      if (open_flags == HighFive::File::ReadOnly) {
        TLOG_DEBUG(TLVL_BASIC) << get_name() << "Opened HDF5 file read-only.";
      } else {
        TLOG_DEBUG(TLVL_BASIC) << get_name() << "Created HDF5 file (" << unique_filename << ").";

        // write attributes that aren't being handled by the HDF5RawDataFile right now
        // m_file_handle->write_attribute("data_format_version",(int)m_key_translator_ptr->get_current_version());
        m_file_handle->write_attribute("operational_environment", (std::string)m_operational_environment);
        m_file_handle->write_attribute("offline_data_stream", (std::string)m_offline_data_stream);
        m_file_handle->write_attribute("run_was_for_test_purposes", (std::string)(m_run_is_for_test_purposes ? "true" : "false"));
      }
    } else {
      TLOG_DEBUG(TLVL_BASIC) << get_name() << ": Pointer file to  " << m_basic_name_of_open_file
                             << " was already opened with open_flags " << std::to_string(m_open_flags_of_open_file);
    }
  }

  size_t get_free_space(const std::string& the_path)
  {
    struct statvfs vfs_results;
    int retval = statvfs(the_path.c_str(), &vfs_results);
    if (retval != 0) {
      return 0;
    }
    return vfs_results.f_bsize * vfs_results.f_bavail;
  }
};

} // namespace dfmodules
} // namespace dunedaq

#endif // DFMODULES_PLUGINS_HDF5DATASTORE_HPP_

// Local Variables:
// c-basic-offset: 2
// End:
