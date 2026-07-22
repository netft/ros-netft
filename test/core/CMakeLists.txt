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
    netft_sdk_core ${NETFT_GTEST_MAIN_TARGET})
  add_test(NAME ${target} COMMAND ${target})
endfunction()
