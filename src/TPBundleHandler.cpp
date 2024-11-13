/**
 * @file TPBundleHandler.cpp TPBundleHandler Class Implementation
 *
 * The TPBundleHandler class takes care of assembling and repacking TriggerPrimitives
 * for storage on disk as part of a TP stream.
 *
 * This is part of the DUNE DAQ Software Suite, copyright 2020.
 * Licensing/copyright details are in the COPYING file that you should have
 * received with this code.
 */

#include "dfmodules/TPBundleHandler.hpp"

#include "detdataformats/DetID.hpp"
#include "logging/Logging.hpp"

#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace dunedaq {
namespace dfmodules {

void
TimeSliceAccumulator::add_tpset(trigger::TPSet&& tpset)
{
  // if this TPSet is near one of the edges of our window, handle it specially
  if (tpset.start_time <= m_begin_time || tpset.end_time >= m_end_time) {
    trigger::TPSet working_tpset;
    daqdataformats::timestamp_t first_time = daqdataformats::TypeDefaults::s_invalid_timestamp;
    daqdataformats::timestamp_t last_time = daqdataformats::TypeDefaults::s_invalid_timestamp;
    bool first = true;
    for (auto& trigprim : tpset.objects) {
      if (trigprim.time_start >= m_begin_time && trigprim.time_start < m_end_time) {
        working_tpset.objects.push_back(trigprim);
        if (first) {
          first_time = trigprim.time_start;
          first = false;
        }
        last_time = trigprim.time_start;
      }
    }
    if (working_tpset.objects.size() == 0) {
      if (tpset.end_time == m_begin_time) {
        // the end of the TPSet just missed the start of our window, so not a big deal
        TLOG_DEBUG(22) << "Note: no TPs were used from a TPSet with start_time=" << tpset.start_time
                       << ", end_time=" << tpset.end_time << ", TSAccumulator begin and end times:" << m_begin_time
                       << ", " << m_end_time;
      } else {
        // woah, something unexpected happened
        ers::warning(NoTPsInWindow(ERS_HERE, tpset.start_time, tpset.end_time, m_begin_time, m_end_time));
      }
      return;
    }
    working_tpset.type = tpset.type;
    working_tpset.seqno = tpset.seqno;
    working_tpset.origin = tpset.origin;
    working_tpset.start_time = first_time;
    working_tpset.end_time = last_time;
    tpset = std::move(working_tpset);
  }

  // create an entry in the top-level map for the sourceid in this TPSet, if needed
  auto lk = std::lock_guard<std::mutex>(m_bundle_map_mutex);
  if (m_tpbundles_by_sourceid_and_start_time.count(tpset.origin) == 0) {
    tpbundles_by_start_time_t empty_bundle_map;
    m_tpbundles_by_sourceid_and_start_time[tpset.origin] = empty_bundle_map;
  }

  // store the TPSet in the map
  if (m_tpbundles_by_sourceid_and_start_time[tpset.origin].count(tpset.start_time)) {
    ers::warning(DuplicateTPWindow(ERS_HERE, tpset.origin.id, tpset.start_time));
  }
  m_tpbundles_by_sourceid_and_start_time[tpset.origin].emplace(tpset.start_time, std::move(tpset));
  m_update_time = std::chrono::steady_clock::now();
}

std::unique_ptr<daqdataformats::TimeSlice>
TimeSliceAccumulator::get_timeslice()
{
  auto lk = std::lock_guard<std::mutex>(m_bundle_map_mutex);
  std::vector<std::unique_ptr<daqdataformats::Fragment>> list_of_fragments;

  // loop over all SourceID present in this accumulator
  for (auto& [sourceid, bundle_map] : m_tpbundles_by_sourceid_and_start_time) {

    // build up the list of pieces that we will use to contruct the Fragment
    std::vector<std::pair<void*, size_t>> list_of_pieces;
    for (auto& [start_time, tpset] : bundle_map) {
      list_of_pieces.push_back(std::make_pair<void*, size_t>(
        &tpset.objects[0], tpset.objects.size() * sizeof(trgdataformats::TriggerPrimitive)));
    }
    std::unique_ptr<daqdataformats::Fragment> frag(new daqdataformats::Fragment(list_of_pieces));

    frag->set_run_number(m_run_number);
    frag->set_trigger_number(m_slice_number);
    frag->set_window_begin(m_begin_time);
    frag->set_window_end(m_end_time);
    frag->set_element_id(sourceid);
    frag->set_detector_id(static_cast<uint16_t>(detdataformats::DetID::Subdetector::kDAQ));
    frag->set_type(daqdataformats::FragmentType::kTriggerPrimitive);

    size_t frag_payload_size = frag->get_size() - sizeof(dunedaq::daqdataformats::FragmentHeader);
    TLOG_DEBUG(21) << "In get_timeslice, Source ID is " << sourceid << ", number of pieces is " << list_of_pieces.size()
                   << ", size of Fragment payload is " << frag_payload_size << ", size of TP is "
                   << sizeof(trgdataformats::TriggerPrimitive);

    list_of_fragments.push_back(std::move(frag));
  }

  std::unique_ptr<daqdataformats::TimeSlice> time_slice(new daqdataformats::TimeSlice(m_slice_number, m_run_number));
  time_slice->set_fragments(std::move(list_of_fragments));
  return time_slice;
}

void
TPBundleHandler::add_tpset(trigger::TPSet&& tpset)
{
  // if the tpset seems to span multiple timeslices, then we add it to all
  // of the relevant accumulators. This runs the risk of adding a tpset
  // to an accumululator that won't find any TPs within the
  // accumulator window (because of edge effects), but we want to be
  // cautious here (and we'll protect against the absence of tpsets later).
  // Of course, adding the tpset to multiple accumulators requires copies...
  size_t tsidx_from_begin_time = tpset.start_time / m_slice_interval;
  size_t tsidx_from_end_time = tpset.end_time / m_slice_interval;
  if (m_slice_index_offset == 0) {
    m_slice_index_offset = tsidx_from_begin_time - 1;
  }

  // 24-Mar-2024, KAB: added check for TimeSlice indexes that are earlier
  // than the one that we started with. We try to gracefully handle them
  // by adjusting the slice_ids of existing slices, but if we can't do that,
  // we discard them so that we don't get
  // TimeSlices with large timeslice_ids (e.g. -1 converted to a uint64_t).
  if (tsidx_from_begin_time <= m_slice_index_offset) {
    auto lk = std::lock_guard<std::mutex>(m_accumulator_map_mutex);
    int64_t diff = static_cast<int64_t>(tsidx_from_begin_time) - static_cast<int64_t>(m_slice_index_offset);
    if (! m_one_or_more_time_slices_have_aged_out) {
      TLOG() << "Updating the slice numbers of existing accumulators by " << (1-diff);
      for (auto& [local_tsidx, local_accum] : m_timeslice_accumulators) {
        local_accum.update_slice_number(1 - diff);
      }
      m_slice_index_offset -= (1 - diff);
    }
    else {
      ers::warning(TardyTPSetReceived(ERS_HERE, tpset.origin.id, tpset.start_time, diff));
      return;
    }
  }

  // add the TPSet to any 'extra' accumulators
  for (size_t tsidx = (tsidx_from_begin_time + 1); tsidx <= tsidx_from_end_time; ++tsidx) {
    {
      auto lk = std::lock_guard<std::mutex>(m_accumulator_map_mutex);
      if (m_timeslice_accumulators.count(tsidx) == 0) {
        TimeSliceAccumulator accum(tsidx * m_slice_interval,
                                   (tsidx + 1) * m_slice_interval,
                                   tsidx - m_slice_index_offset,
                                   m_run_number);
        m_timeslice_accumulators[tsidx] = accum;
      }
    }
    trigger::TPSet tpset_copy = tpset;
    m_timeslice_accumulators[tsidx].add_tpset(std::move(tpset_copy));
  }

  // add the TPSet to the accumulator associated with the begin time
  {
    auto lk = std::lock_guard<std::mutex>(m_accumulator_map_mutex);
    if (m_timeslice_accumulators.count(tsidx_from_begin_time) == 0) {
      TimeSliceAccumulator accum(tsidx_from_begin_time * m_slice_interval,
                                 (tsidx_from_begin_time + 1) * m_slice_interval,
                                 tsidx_from_begin_time - m_slice_index_offset,
                                 m_run_number);
      m_timeslice_accumulators[tsidx_from_begin_time] = accum;
    }
  }
  m_timeslice_accumulators[tsidx_from_begin_time].add_tpset(std::move(tpset));
}

std::vector<std::unique_ptr<daqdataformats::TimeSlice>>
TPBundleHandler::get_properly_aged_timeslices()
{
  std::vector<std::unique_ptr<daqdataformats::TimeSlice>> list_of_timeslices;
  std::vector<daqdataformats::timestamp_t> elements_to_be_removed;

  auto now = std::chrono::steady_clock::now();
  for (auto& [tsidx, accum] : m_timeslice_accumulators) {
    if ((now - accum.get_update_time()) >= m_cooling_off_time) {
      list_of_timeslices.push_back(accum.get_timeslice());
      elements_to_be_removed.push_back(tsidx);
    }
  }

  auto lk = std::lock_guard<std::mutex>(m_accumulator_map_mutex);
  for (auto& tsidx : elements_to_be_removed) {
    m_timeslice_accumulators.erase(tsidx);
  }

  if (list_of_timeslices.size() > 0) {m_one_or_more_time_slices_have_aged_out = true;}
  return list_of_timeslices;
}

std::vector<std::unique_ptr<daqdataformats::TimeSlice>>
TPBundleHandler::get_all_remaining_timeslices()
{
  std::vector<std::unique_ptr<daqdataformats::TimeSlice>> list_of_timeslices;

  for (auto& [tsidx, accum] : m_timeslice_accumulators) {
    list_of_timeslices.push_back(accum.get_timeslice());
  }

  {
    auto lk = std::lock_guard<std::mutex>(m_accumulator_map_mutex);
    m_timeslice_accumulators.clear();
  }

  return list_of_timeslices;
}

} // namespace dfmodules
} // namespace dunedaq
