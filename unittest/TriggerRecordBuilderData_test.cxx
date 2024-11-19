/**
 * @file TriggerRecordBuilderData_test.cxx Test application that tests and demonstrates
 * the functionality of the TriggerRecordBuilderData class.
 *
 * This is part of the DUNE DAQ Application Framework, copyright 2020.
 * Licensing/copyright details are in the COPYING file that you should have
 * received with this code.
 */

#include "dfmodules/TriggerRecordBuilderData.hpp"
#include "opmonlib/TestOpMonManager.hpp"

#define BOOST_TEST_MODULE TriggerRecordBuilderData_test // NOLINT

#include "boost/test/unit_test.hpp"

#include <chrono>
#include <thread>
#include <utility>

using namespace dunedaq::dfmodules;

BOOST_AUTO_TEST_SUITE(TriggerRecordBuilderData_Test)

BOOST_AUTO_TEST_CASE(CopyAndMoveSemantics)
{
  BOOST_REQUIRE(!std::is_copy_constructible_v<TriggerRecordBuilderData>);
  BOOST_REQUIRE(!std::is_copy_assignable_v<TriggerRecordBuilderData>);
  BOOST_REQUIRE(!std::is_move_constructible_v<TriggerRecordBuilderData>);
  BOOST_REQUIRE(!std::is_move_assignable_v<TriggerRecordBuilderData>);
}

BOOST_AUTO_TEST_CASE(Constructors)
{
  dunedaq::dfmessages::TriggerDecision td;
  td.trigger_number = 1;
  td.run_number = 2;
  td.trigger_timestamp = 3;
  td.trigger_type = 4;
  td.readout_type = dunedaq::dfmessages::ReadoutType::kLocalized;

  AssignedTriggerDecision atd(td, "test");

  BOOST_REQUIRE_EQUAL(atd.decision.trigger_number, td.trigger_number);
  BOOST_REQUIRE_EQUAL(atd.connection_name, "test");

  // TRBD must have a default constructor so that it can be used in a std::map, but a default-constructed TRBD is
  // invalid.
  TriggerRecordBuilderData trbd;
  BOOST_REQUIRE(trbd.is_in_error());

  TriggerRecordBuilderData trbd2("test", 10);

  BOOST_REQUIRE_EQUAL(trbd2.used_slots(), 0);
  BOOST_REQUIRE_EQUAL(trbd2.is_busy(), false);
  BOOST_REQUIRE(!trbd2.is_in_error());

  trbd2.set_in_error(true);
  BOOST_REQUIRE(trbd2.is_in_error());
  BOOST_REQUIRE_EQUAL(trbd2.is_busy(), true);

  trbd2.set_in_error(false);
  BOOST_REQUIRE_EQUAL(trbd2.used_slots(), 0);
  BOOST_REQUIRE_EQUAL(trbd2.is_busy(), false);
  BOOST_REQUIRE(!trbd2.is_in_error());

  BOOST_REQUIRE_EXCEPTION(TriggerRecordBuilderData("test", 10, 15),
                          DFOThresholdsNotConsistent,
                          [](DFOThresholdsNotConsistent const&) { return true; });
}

BOOST_AUTO_TEST_CASE(Assignments)
{
  auto start_time = std::chrono::steady_clock::now();
  dunedaq::dfmessages::TriggerDecision td;
  td.trigger_number = 1;
  td.run_number = 2;
  td.trigger_timestamp = 3;
  td.trigger_type = 4;
  td.readout_type = dunedaq::dfmessages::ReadoutType::kLocalized;

  dunedaq::opmonlib::TestOpMonManager opmgr;
  auto trbd_p = std::make_shared<TriggerRecordBuilderData>("test", 2);
  opmgr.register_node("trbd", trbd_p);
  BOOST_REQUIRE_EQUAL(trbd_p->used_slots(), 0);
  BOOST_REQUIRE(!trbd_p->is_busy());

  auto assignment = trbd_p->make_assignment(td);
  BOOST_REQUIRE_EQUAL(assignment->connection_name, "test");
  trbd_p->add_assignment(assignment);

  BOOST_REQUIRE_EQUAL(trbd_p->used_slots(), 1);
  auto got_assignment = trbd_p->get_assignment(1);
  BOOST_REQUIRE_EQUAL(got_assignment->decision.trigger_number, assignment->decision.trigger_number);
  BOOST_REQUIRE_EQUAL(got_assignment->decision.trigger_timestamp, assignment->decision.trigger_timestamp);
  BOOST_REQUIRE_EQUAL(got_assignment.get(), assignment.get());
  BOOST_REQUIRE_EQUAL(trbd_p->used_slots(), 1);

  auto extracted_assignment = trbd_p->extract_assignment(1);
  BOOST_REQUIRE_EQUAL(extracted_assignment.get(), assignment.get());
  BOOST_REQUIRE_EQUAL(trbd_p->used_slots(), 0);
  trbd_p->add_assignment(extracted_assignment);
  BOOST_REQUIRE_EQUAL(trbd_p->used_slots(), 1);

  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  trbd_p->complete_assignment(1, [](nlohmann::json&) {});
  BOOST_REQUIRE_EQUAL(trbd_p->used_slots(), 0);

  auto latency =
    std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - assignment->assigned_time)
      .count();

  BOOST_REQUIRE_CLOSE(
    static_cast<double>(trbd_p->average_latency(start_time).count()), static_cast<double>(latency), 5);

  auto null_got_assignment = trbd_p->get_assignment(2);
  BOOST_REQUIRE_EQUAL(null_got_assignment, nullptr);
  auto null_extracted_assignment = trbd_p->extract_assignment(3);
  BOOST_REQUIRE_EQUAL(null_extracted_assignment, nullptr);

  trbd_p->add_assignment(assignment);
  BOOST_REQUIRE_EQUAL(trbd_p->used_slots(), 1);
  auto remnants = trbd_p->flush();
  BOOST_REQUIRE_EQUAL(trbd_p->used_slots(), 0);
  BOOST_REQUIRE_EQUAL(remnants.size(), 1);
}

BOOST_AUTO_TEST_CASE(Exceptions)
{
  dunedaq::dfmessages::TriggerDecision td;
  td.trigger_number = 1;
  td.run_number = 2;
  td.trigger_timestamp = 3;
  td.trigger_type = 4;
  td.readout_type = dunedaq::dfmessages::ReadoutType::kLocalized;
  dunedaq::dfmessages::TriggerDecision another_td;
  another_td.trigger_number = 2;
  another_td.run_number = 2;
  another_td.trigger_timestamp = 5;
  another_td.trigger_type = 4;
  another_td.readout_type = dunedaq::dfmessages::ReadoutType::kLocalized;
  dunedaq::dfmessages::TriggerDecision yet_another_td;
  yet_another_td.trigger_number = 3;
  yet_another_td.run_number = 2;
  yet_another_td.trigger_timestamp = 7;
  yet_another_td.trigger_type = 4;
  yet_another_td.readout_type = dunedaq::dfmessages::ReadoutType::kLocalized;

  TriggerRecordBuilderData trbd("test", 2);
  BOOST_REQUIRE_EQUAL(trbd.used_slots(), 0);
  BOOST_REQUIRE(!trbd.is_busy());

  auto assignment = trbd.make_assignment(td);
  BOOST_REQUIRE_EQUAL(assignment->connection_name, "test");
  trbd.add_assignment(assignment);

  auto another_assignment = trbd.make_assignment(another_td);
  trbd.add_assignment(another_assignment);

  BOOST_REQUIRE_EQUAL(trbd.used_slots(), 2);
  BOOST_REQUIRE(trbd.is_busy());

  BOOST_REQUIRE_EXCEPTION(trbd.complete_assignment(3),
                          AssignedTriggerDecisionNotFound,
                          [](AssignedTriggerDecisionNotFound const&) { return true; });

  auto yet_another_assignment = trbd.make_assignment(yet_another_td);
  BOOST_CHECK_NO_THROW(trbd.add_assignment(yet_another_assignment));
  // we are now above threshold but we can accept new assigments anyway because we are not in error
  BOOST_REQUIRE(trbd.is_busy());

  trbd.set_in_error(true);
  dunedaq::dfmessages::TriggerDecision err_td;
  err_td.trigger_number = 4;
  err_td.run_number = 2;
  err_td.trigger_timestamp = 10;
  err_td.trigger_type = 4;
  err_td.readout_type = dunedaq::dfmessages::ReadoutType::kLocalized;
  auto err_assignment = trbd.make_assignment(err_td);

  BOOST_REQUIRE_EXCEPTION(
    trbd.add_assignment(err_assignment), NoSlotsAvailable, [](NoSlotsAvailable const&) { return true; });
}

template<typename T>
bool
contains(std::vector<T> vec, T val)
{
  for (auto& vv : vec) {
    if (vv == val)
      return true;
  }
  return false;
}

BOOST_AUTO_TEST_CASE(Acknowledgements)
{
  std::vector<dunedaq::dfmessages::trigger_number_t> list1{ 1, 3, 5, 7, 9 };
  std::vector<dunedaq::dfmessages::trigger_number_t> list2{ 2, 4, 6 };
  std::vector<dunedaq::dfmessages::trigger_number_t> list3{ 5, 7, 9, 11, 13 };

  TriggerRecordBuilderData trbd("test", 2);

  trbd.update_completions_to_acknowledge_list(list1);
  auto check1 = trbd.extract_completions_to_acknowledge();
  BOOST_REQUIRE_EQUAL(list1.size(), check1.size());
  for (size_t ii = 0; ii < list1.size(); ++ii) {
    BOOST_REQUIRE(contains(check1, list1[ii]));
  }
  auto empty1 = trbd.extract_completions_to_acknowledge();
  BOOST_REQUIRE_EQUAL(empty1.size(), 0);

  trbd.update_completions_to_acknowledge_list(list1);
  trbd.update_completions_to_acknowledge_list(list2);
  auto check2 = trbd.extract_completions_to_acknowledge();
  BOOST_REQUIRE_EQUAL(list1.size() + list2.size(), check2.size());
  for (size_t ii = 0; ii < list1.size(); ++ii) {
    BOOST_REQUIRE(contains(check2, list1[ii]));
  }
  for (size_t ii = 0; ii < list2.size(); ++ii) {
    BOOST_REQUIRE(contains(check2, list2[ii]));
  }
  auto empty2 = trbd.extract_completions_to_acknowledge();
  BOOST_REQUIRE_EQUAL(empty2.size(), 0);

  trbd.update_completions_to_acknowledge_list(list1);
  trbd.update_completions_to_acknowledge_list(list2);
  trbd.update_completions_to_acknowledge_list(list3);
  auto check3 = trbd.extract_completions_to_acknowledge();
  BOOST_REQUIRE_EQUAL(list1.size() + list2.size() + 2, check3.size());
  for (size_t ii = 0; ii < list1.size(); ++ii) {
    BOOST_REQUIRE(contains(check3, list1[ii]));
  }
  for (size_t ii = 0; ii < list2.size(); ++ii) {
    BOOST_REQUIRE(contains(check3, list2[ii]));
  }
  for (size_t ii = 0; ii < list3.size(); ++ii) {
    BOOST_REQUIRE(contains(check3, list3[ii]));
  }
  auto empty3 = trbd.extract_completions_to_acknowledge();
  BOOST_REQUIRE_EQUAL(empty3.size(), 0);
}

BOOST_AUTO_TEST_SUITE_END()
