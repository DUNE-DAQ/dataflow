/**
 * @file DataStoreFactory_test.cxx Test application that tests and demonstrates
 * the functionality of the mechanisms to allocate DataStores via the factory.
 *
 * This is part of the DUNE DAQ Application Framework, copyright 2020.
 * Licensing/copyright details are in the COPYING file that you should have
 * received with this code.
 */

#include "dfmodules/DataStore.hpp"

#define BOOST_TEST_MODULE DataStoreFactory_test // NOLINT

#include "boost/test/unit_test.hpp"

#include <memory>

using namespace dunedaq::dfmodules;

BOOST_AUTO_TEST_SUITE(DataStoreFactory_test)

BOOST_AUTO_TEST_CASE(invalid_request)
{

  // we want to pass an invalid DataStore type and see if we get an exception
  BOOST_CHECK_THROW(make_data_store("dummy", "dummy", nullptr, "dummy_writer"), DataStoreCreationFailed);

}

#if 0
BOOST_AUTO_TEST_CASE(valid_request)
{

  nlohmann::json conf ;
  conf["name"] = "test" ;

  // we want to ask for a valid Data Store type
  auto ds = make_data_store( "TrashCanDataStore", conf, "test" ) ;

  // and we want to check if we created something valid
  BOOST_TEST( ds.get() ) ;

}
#endif

BOOST_AUTO_TEST_SUITE_END()
