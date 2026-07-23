add_executable(netft_types test_types.cpp)
target_link_libraries(netft_types PRIVATE netft_core GTest::gtest_main)

add_test(NAME netft_types COMMAND netft_types)

add_executable(netft_protocol test_protocol.cpp)
target_compile_definitions(netft_protocol PRIVATE
  NETFT_PROTOCOL_SOURCE_PATH="${CMAKE_CURRENT_SOURCE_DIR}/../../src/protocol.cpp")
target_link_libraries(netft_protocol PRIVATE netft_core GTest::gtest_main)
add_test(NAME netft_protocol COMMAND netft_protocol)

add_executable(netft_status test_status.cpp)
target_link_libraries(netft_status PRIVATE netft_core GTest::gtest_main)
add_test(NAME netft_status COMMAND netft_status)

add_executable(netft_diagnostics test_diagnostics.cpp)
target_include_directories(netft_diagnostics PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../../src)
target_link_libraries(netft_diagnostics PRIVATE
  netft_ros_support
  netft_sdk_core
  GTest::gtest_main)
add_test(NAME netft_diagnostics COMMAND netft_diagnostics)

add_executable(netft_client test_client.cpp support/fake_sensor.cpp)
target_link_libraries(netft_client PRIVATE netft_core GTest::gtest_main)
add_test(NAME netft_client COMMAND netft_client)

add_executable(netft_check_test
  test_check.cpp
  ../core/support/fake_http_server.cpp
  ../core/support/fake_sensor.cpp)
target_compile_definitions(netft_check_test PRIVATE
  NETFT_CHECK_PATH="$<TARGET_FILE:netft_check>")
target_include_directories(netft_check_test PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/..
  ${CMAKE_CURRENT_SOURCE_DIR}/../core
  ${NETFT_SNAPSHOT_SOURCE_DIR}/src)
target_link_libraries(netft_check_test PRIVATE
  netft_ros_support
  netft_sdk_core
  GTest::gtest_main)
add_dependencies(netft_check_test netft_check)
add_test(NAME netft_check COMMAND netft_check_test)

add_executable(netft_adapter_config test_adapter_config.cpp)
target_include_directories(netft_adapter_config PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../../src)
target_link_libraries(netft_adapter_config PRIVATE
  netft_ros_support
  netft_sdk_core
  GTest::gtest_main)
add_test(NAME netft_adapter_config COMMAND netft_adapter_config)

add_executable(netft_unit_conversion test_unit_conversion.cpp)
target_include_directories(netft_unit_conversion PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../../src)
target_link_libraries(netft_unit_conversion PRIVATE
  netft_ros_support
  netft_sdk_core
  GTest::gtest_main)
add_test(NAME netft_unit_conversion COMMAND netft_unit_conversion)
