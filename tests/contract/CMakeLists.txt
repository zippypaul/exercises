# Contract tests: run the real graph adapter + storage + core against tools/fake_graph/server.py.
# The test binary spawns the Python server itself (see contract_env.hpp) so `ctest` is self-contained.
file(GLOB SLOWMAIL_CONTRACT_TESTS CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/*.cpp)
if(SLOWMAIL_CONTRACT_TESTS)
  add_executable(contract_tests ${SLOWMAIL_CONTRACT_TESTS})
  target_link_libraries(contract_tests PRIVATE slowmail_core slowmail_storage slowmail_graph GTest::gtest GTest::gtest_main)
  target_compile_definitions(contract_tests PRIVATE
    SLOWMAIL_FAKE_GRAPH_SERVER="${CMAKE_SOURCE_DIR}/tools/fake_graph/server.py"
    SLOWMAIL_FIXTURE_DIR="${CMAKE_SOURCE_DIR}/tests/contract/fixtures")
  add_test(NAME contract_tests COMMAND contract_tests)
  set_tests_properties(contract_tests PROPERTIES TIMEOUT 300)
endif()
