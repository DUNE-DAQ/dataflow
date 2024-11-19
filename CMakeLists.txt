cmake_minimum_required(VERSION 3.12)
project(dfmodules VERSION 3.2.0)

find_package(daq-cmake REQUIRED)
daq_setup_environment()

find_package(appfwk REQUIRED)
find_package(logging REQUIRED)
find_package(ers REQUIRED)
find_package(hdf5libs REQUIRED)
find_package(dfmessages REQUIRED)
find_package(opmonlib REQUIRED)
find_package(iomanager REQUIRED)
find_package(detdataformats REQUIRED)
find_package(trgdataformats REQUIRED)

find_package(triggeralgs REQUIRED)
find_package(trigger REQUIRED)
find_package(Boost COMPONENTS iostreams unit_test_framework REQUIRED)


daq_protobuf_codegen( opmon/*.proto )

##############################################################################
daq_add_library( TriggerInhibitAgent.cpp TriggerRecordBuilderData.cpp TPBundleHandler.cpp
                 LINK_LIBRARIES 
                 opmonlib::opmonlib ers::ers HighFive appfwk::appfwk logging::logging stdc++fs dfmessages::dfmessages utilities::utilities trigger::trigger detdataformats::detdataformats trgdataformats::trgdataformats)

      
daq_add_plugin( HDF5DataStore     duneDataStore LINK_LIBRARIES dfmodules hdf5libs::hdf5libs stdc++fs)

daq_add_plugin( FragmentAggregatorModule    duneDAQModule LINK_LIBRARIES dfmodules iomanager::iomanager )
daq_add_plugin( DataWriterModule            duneDAQModule LINK_LIBRARIES dfmodules hdf5libs::hdf5libs iomanager::iomanager )
daq_add_plugin( DFOModule  duneDAQModule LINK_LIBRARIES dfmodules iomanager::iomanager )
daq_add_plugin( DFOBrokerModule duneDAQModule LINK_LIBRARIES dfmodules iomanager::iomanager )
daq_add_plugin( TRBModule  duneDAQModule LINK_LIBRARIES dfmodules iomanager::iomanager )
daq_add_plugin( FakeDataProdModule          duneDAQModule LINK_LIBRARIES dfmodules iomanager::iomanager)
daq_add_plugin( FakeDFOClientModule duneDAQModule LINK_LIBRARIES dfmodules iomanager::iomanager )
daq_add_plugin( TPStreamWriterModule        duneDAQModule LINK_LIBRARIES dfmodules hdf5libs::hdf5libs trigger::trigger Boost::iostreams )

##############################################################################
daq_add_unit_test( HDF5FileUtils_test       LINK_LIBRARIES dfmodules )

daq_add_unit_test( HDF5Write_test           LINK_LIBRARIES dfmodules hdf5libs::hdf5libs )
add_dependencies(  HDF5Write_test dfmodules_HDF5DataStore_duneDataStore )

daq_add_unit_test( DFOModule_test LINK_LIBRARIES dfmodules )
add_dependencies( DFOModule_test dfmodules_DFOModule_duneDAQModule)

daq_add_unit_test( TriggerRecordBuilderData_test LINK_LIBRARIES dfmodules )
daq_add_unit_test( DataStoreFactory_test    LINK_LIBRARIES dfmodules )

##############################################################################

daq_install()
