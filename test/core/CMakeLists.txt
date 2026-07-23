if(TARGET GTest::gtest_main)
  set(NETFT_GTEST_MAIN_TARGET GTest::gtest_main)
elseif(TARGET GTest::Main)
  set(NETFT_GTEST_MAIN_TARGET GTest::Main)
else()
  message(FATAL_ERROR "GoogleTest does not provide a main target")
endif()

function(add_netft_snapshot_test target source)
  add_executable(${target} ${source} ${ARGN})
  target_include_directories(${target} PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${NETFT_SNAPSHOT_SOURCE_DIR}/src
  )
  target_link_libraries(${target} PRIVATE
    netft_core ${NETFT_GTEST_MAIN_TARGET})
  add_test(NAME ${target} COMMAND ${target})
endfunction()

add_netft_snapshot_test(netft_snapshot_types test_types.cpp)
add_netft_snapshot_test(netft_snapshot_status test_status.cpp)

add_netft_snapshot_test(netft_snapshot_protocol
  test_protocol.cpp
  ${NETFT_SNAPSHOT_SOURCE_DIR}/src/detail/protocol.cpp)

add_netft_snapshot_test(netft_snapshot_sequence
  test_sequence.cpp
  ${NETFT_SNAPSHOT_SOURCE_DIR}/src/detail/sequence.cpp)

add_netft_snapshot_test(netft_snapshot_fault_latch
  test_fault_latch.cpp
  ${NETFT_SNAPSHOT_SOURCE_DIR}/src/detail/fault_latch.cpp)

add_executable(netft_snapshot_posix_transport
  test_posix_transport.cpp
  ${NETFT_SNAPSHOT_SOURCE_DIR}/src/detail/posix_transport.cpp)
target_compile_features(netft_snapshot_posix_transport PRIVATE cxx_std_17)
target_include_directories(netft_snapshot_posix_transport PRIVATE
  ${NETFT_SNAPSHOT_SOURCE_DIR}/src)
target_link_options(netft_snapshot_posix_transport PRIVATE
  "-Wl,--wrap=poll"
  "-Wl,--wrap=recv"
  "-Wl,--wrap=send")
target_link_libraries(netft_snapshot_posix_transport PRIVATE
  ${NETFT_GTEST_MAIN_TARGET})
add_test(NAME netft_snapshot_posix_transport
  COMMAND netft_snapshot_posix_transport)

add_netft_snapshot_test(netft_snapshot_discovery
  test_discovery.cpp
  support/fake_http_server.cpp
  ${NETFT_XML_CONFIG_SOURCE})

add_netft_snapshot_test(netft_compat_discovery
  test_discovery.cpp
  support/fake_http_server.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/../../src/compat/xml_config.cpp)

add_netft_snapshot_test(netft_snapshot_client_stream
  test_client_stream.cpp
  support/fake_http_server.cpp
  support/fake_sensor.cpp)

add_netft_snapshot_test(netft_snapshot_client_recovery
  test_client_recovery.cpp
  support/fake_http_server.cpp
  support/fake_sensor.cpp)

add_library(netft_snapshot_client_impl_test STATIC
  ${NETFT_SNAPSHOT_SOURCE_DIR}/src/detail/client_impl.cpp
  ${NETFT_SNAPSHOT_SOURCE_DIR}/src/detail/fault_latch.cpp
  ${NETFT_SNAPSHOT_SOURCE_DIR}/src/detail/posix_transport.cpp
  ${NETFT_SNAPSHOT_SOURCE_DIR}/src/detail/protocol.cpp
  ${NETFT_SNAPSHOT_SOURCE_DIR}/src/detail/sequence.cpp
  ${NETFT_XML_CONFIG_SOURCE})
target_include_directories(netft_snapshot_client_impl_test PRIVATE
  ${NETFT_SNAPSHOT_SOURCE_DIR}/src)
target_link_libraries(netft_snapshot_client_impl_test PUBLIC netft_core)

add_executable(netft_snapshot_client_lifecycle
  test_client_lifecycle.cpp
  support/fake_http_server.cpp
  support/fake_sensor.cpp)
target_include_directories(netft_snapshot_client_lifecycle PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${NETFT_SNAPSHOT_SOURCE_DIR}/src)
target_link_libraries(netft_snapshot_client_lifecycle PRIVATE
  netft_snapshot_client_impl_test
  ${NETFT_GTEST_MAIN_TARGET})
add_test(NAME netft_snapshot_client_lifecycle
  COMMAND netft_snapshot_client_lifecycle)
