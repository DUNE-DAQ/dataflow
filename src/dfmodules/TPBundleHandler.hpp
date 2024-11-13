/**
 * @file TPBundleHandler.hpp TPBundleHandler Class
 *
 * The TPBundleHandler class takes care of assembling and repacking TriggerPrimitives
 * for storage on disk as part of a TP stream.
 *
 * This is part of the DUNE DAQ Software Suite, copyright 2020.
 * Licensing/copyright details are in the COPYING file that you should have
 * received with this code.
 */

#ifndef DFMODULES_SRC_DFMODULES_TPBUNDLEHANDLER_HPP_
#define DFMODULES_SRC_DFMODULES_TPBUNDLEHANDLER_HPP_

#include "daqdataformats/TimeSlice.hpp"
#include "daqdataformats/Types.hpp"
#include "trgdataformats/TriggerPrimitive.hpp"
#include "ers/Issue.hpp"
#include "trigger/TPSet.hpp"

#include <chrono>
#include <map>
#include <memory>
#include <mutex>
#include <vector>

namespace dunedaq {

// Disable coverage checking LCOV_EXCL_START
ERS_DECLARE_ISSUE(dfmodules,
                  NoTPsInWindow,
                  "No TriggerPrimitives were used from a TPSet with start_time="
                    << tpset_start_time << ", end_time=" << tpset_end_time
                    << ", TSAccumulator begin and end times:" << window_begin_time << ", " << window_end_time,
                  ((daqdataformats::timestamp_t)tpset_start_time)((daqdataformats::timestamp_t)tpset_end_time)(
                    (daqdataformats::timestamp_t)window_begin_time)((daqdataformats::timestamp_t)window_end_time))
ERS_DECLARE_ISSUE(dfmodules,
                  DuplicateTPWindow,
                  "Cannot add TPSet with sourceid="
                    << tpset_source_id << ", start_time=" << tpset_start_time
                    << " to bundle, because another TPSet with these values already exists",
                  ((size_t)tpset_source_id)((daqdataformats::timestamp_t)tpset_start_time))
ERS_DECLARE_ISSUE(dfmodules,
                  TardyTPSetReceived,
                  "Received a TPSet with a timestamp that is too early compared to ones that have already "
                  << "been processed, sourceid=" << tpset_source_id << ", start_time=" << tpset_start_time
                  << ", the calculated timeslice_id is " << tsid,
                  ((size_t)tpset_source_id)((daqdataformats::timestamp_t)tpset_start_time)((int64_t)tsid))
// Re-enable coverage checking LCOV_EXCL_STOP

namespace dfmodules {

class TimeSliceAccumulator
{
public:
  TimeSliceAccumulator() {}

  TimeSliceAccumulator(daqdataformats::timestamp_t begin_time,
                       daqdataformats::timestamp_t end_time,
                       daqdataformats::timeslice_number_t slice_number,
                       daqdataformats::run_number_t run_number)
    : m_begin_time(begin_time)
    , m_end_time(end_time)
    , m_slice_number(slice_number)
    , m_run_number(run_number)
    , m_update_time(std::chrono::steady_clock::now())
  {
  }

  TimeSliceAccumulator& operator=(const TimeSliceAccumulator& other)
  {
    if (this != &other) {
      std::lock(m_bundle_map_mutex, other.m_bundle_map_mutex);
      std::lock_guard<std::mutex> lhs_lk(m_bundle_map_mutex, std::adopt_lock);
      std::lock_guard<std::mutex> rhs_lk(other.m_bundle_map_mutex, std::adopt_lock);
      m_begin_time = other.m_begin_time;
      m_end_time = other.m_end_time;
      m_slice_number = other.m_slice_number;
      m_run_number = other.m_run_number;
      m_update_time = other.m_update_time;
      m_tpbundles_by_sourceid_and_start_time = other.m_tpbundles_by_sourceid_and_start_time;
    }
    return *this;
  }

  void add_tpset(trigger::TPSet&& tpset);

  std::unique_ptr<daqdataformats::TimeSlice> get_timeslice();

  std::chrono::steady_clock::time_point get_update_time() const
  {
    auto lk = std::lock_guard<std::mutex>(m_bundle_map_mutex);
    return m_update_time;
  }

  void update_slice_number(int delta)
  {
    m_slice_number += delta;
  }

private:
  daqdataformats::timestamp_t m_begin_time;
  daqdataformats::timestamp_t m_end_time;
  daqdataformats::timeslice_number_t m_slice_number;
  daqdataformats::run_number_t m_run_number;
  std::chrono::steady_clock::time_point m_update_time;
  typedef std::map<daqdataformats::timestamp_t, trigger::TPSet> tpbundles_by_start_time_t;
  typedef std::map<daqdataformats::SourceID, tpbundles_by_start_time_t> bundles_by_sourceid_t;
  bundles_by_sourceid_t m_tpbundles_by_sourceid_and_start_time;
  mutable std::mutex m_bundle_map_mutex;
};

class TPBundleHandler
{
public:
  TPBundleHandler(daqdataformats::timestamp_t slice_interval,
                  daqdataformats::run_number_t run_number,
                  std::chrono::steady_clock::duration cooling_off_time)
    : m_slice_interval(slice_interval)
    , m_run_number(run_number)
    , m_cooling_off_time(cooling_off_time)
    , m_slice_index_offset(0)
    , m_one_or_more_time_slices_have_aged_out(false)
  {
  }

  TPBundleHandler(TPBundleHandler const&) = delete;
  TPBundleHandler(TPBundleHandler&&) = delete;
  TPBundleHandler& operator=(TPBundleHandler const&) = delete;
  TPBundleHandler& operator=(TPBundleHandler&&) = delete;

  void add_tpset(trigger::TPSet&& tpset);

  std::vector<std::unique_ptr<daqdataformats::TimeSlice>> get_properly_aged_timeslices();

  std::vector<std::unique_ptr<daqdataformats::TimeSlice>> get_all_remaining_timeslices();

private:
  daqdataformats::timestamp_t m_slice_interval;
  daqdataformats::run_number_t m_run_number;
  std::chrono::steady_clock::duration m_cooling_off_time;
  size_t m_slice_index_offset;
  std::map<daqdataformats::timestamp_t, TimeSliceAccumulator> m_timeslice_accumulators;
  mutable std::mutex m_accumulator_map_mutex;
  bool m_one_or_more_time_slices_have_aged_out;
};
} // namespace dfmodules
} // namespace dunedaq

#endif // DFMODULES_SRC_DFMODULES_TPBUNDLEHANDLER_HPP_
