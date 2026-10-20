# slowmail-cli: headless driver for the whole stack (simulator, scripted tests, manual Graph contract runs).
file(GLOB SLOWMAIL_CLI_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/*.cpp)
if(SLOWMAIL_CLI_SOURCES)
  add_executable(slowmail-cli ${SLOWMAIL_CLI_SOURCES})
  target_link_libraries(slowmail-cli PRIVATE slowmail_core slowmail_storage slowmail_graph slowmail_platform)
  slowmail_apply_warnings(slowmail-cli)
endif()
