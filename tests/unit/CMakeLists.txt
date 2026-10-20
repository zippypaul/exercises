# Unit tests: one executable per library, all GLOB'd. Add a *_test.cpp and reconfigure.
file(GLOB SLOWMAIL_CORE_TESTS CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/core/*.cpp)
if(SLOWMAIL_CORE_TESTS)
  add_executable(core_tests ${SLOWMAIL_CORE_TESTS})
  target_link_libraries(core_tests PRIVATE slowmail_core GTest::gtest GTest::gtest_main)
  add_test(NAME core_tests COMMAND core_tests)
endif()

file(GLOB SLOWMAIL_STORAGE_TESTS CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/storage/*.cpp)
if(SLOWMAIL_STORAGE_TESTS)
  add_executable(storage_tests ${SLOWMAIL_STORAGE_TESTS})
  target_link_libraries(storage_tests PRIVATE slowmail_storage GTest::gtest GTest::gtest_main)
  add_test(NAME storage_tests COMMAND storage_tests)
endif()

file(GLOB SLOWMAIL_GRAPH_TESTS CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/graph/*.cpp)
if(SLOWMAIL_GRAPH_TESTS)
  add_executable(graph_tests ${SLOWMAIL_GRAPH_TESTS})
  target_link_libraries(graph_tests PRIVATE slowmail_graph GTest::gtest GTest::gtest_main)
  add_test(NAME graph_tests COMMAND graph_tests)
endif()

# Engine scenarios run against MemoryStore and SqliteStore (store-contract divergence).
file(GLOB SLOWMAIL_INTEGRATION_TESTS CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/integration/*.cpp)
if(SLOWMAIL_INTEGRATION_TESTS)
  add_executable(integration_tests ${SLOWMAIL_INTEGRATION_TESTS})
  target_link_libraries(integration_tests PRIVATE slowmail_core slowmail_storage GTest::gtest GTest::gtest_main)
  add_test(NAME integration_tests COMMAND integration_tests)
endif()
