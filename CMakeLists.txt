cmake_minimum_required(VERSION 3.16)
project(netft_driver)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(NETFT_CORE_ONLY "Build only the ROS-neutral C++ core" OFF)
option(NETFT_INSTALL_TEST_TOOLS "Install private integration-test tools" OFF)

find_package(Threads REQUIRED)
find_package(CURL 7.62 REQUIRED)

set(NETFT_SNAPSHOT_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/src/core")
set(NETFT_SNAPSHOT_SOURCES
  src/core/src/types.cpp
  src/core/src/status.cpp
  src/core/src/discovery.cpp
  src/core/src/client.cpp
  src/core/src/detail/client_impl.cpp
  src/core/src/detail/fault_latch.cpp
  src/core/src/detail/posix_transport.cpp
  src/core/src/detail/protocol.cpp
  src/core/src/detail/sequence.cpp
  src/core/src/detail/xml_config.cpp
)

add_library(netft_core STATIC ${NETFT_SNAPSHOT_SOURCES})
set_target_properties(netft_core PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_compile_features(netft_core PUBLIC cxx_std_17)
target_compile_definitions(netft_core PRIVATE NETFT_BUILDING_LIBRARY)
target_include_directories(netft_core
  PUBLIC ${NETFT_SNAPSHOT_SOURCE_DIR}/include
  PRIVATE ${NETFT_SNAPSHOT_SOURCE_DIR}/src
)
target_link_libraries(netft_core PUBLIC Threads::Threads CURL::libcurl)

add_library(netft_ros_support STATIC
  src/ros/unit_conversion.cpp
  src/ros/diagnostics.cpp)
set_target_properties(netft_ros_support PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_compile_features(netft_ros_support PUBLIC cxx_std_17)
target_include_directories(netft_ros_support
  PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${NETFT_SNAPSHOT_SOURCE_DIR}/include)
target_link_libraries(netft_ros_support PUBLIC netft_core)

add_executable(netft_check src/check_main.cpp)
target_link_libraries(netft_check PRIVATE netft_ros_support netft_core)

if(NETFT_CORE_ONLY)
  enable_testing()
  find_package(GTest REQUIRED)
  add_subdirectory(test/core)
  add_subdirectory(test/cpp)
  return()
endif()

if(DEFINED ROS_VERSION AND NOT "${ROS_VERSION}" STREQUAL "")
  set(NETFT_ROS_VERSION "${ROS_VERSION}")
elseif(DEFINED ENV{ROS_VERSION} AND NOT "$ENV{ROS_VERSION}" STREQUAL "")
  set(NETFT_ROS_VERSION "$ENV{ROS_VERSION}")
else()
  find_package(ament_cmake QUIET)
  find_package(catkin QUIET)
  if(ament_cmake_FOUND AND catkin_FOUND)
    message(FATAL_ERROR
      "Both ament_cmake and catkin are available; set ROS_VERSION to 1 or 2")
  elseif(ament_cmake_FOUND)
    set(NETFT_ROS_VERSION "2")
  elseif(catkin_FOUND)
    set(NETFT_ROS_VERSION "1")
  endif()
endif()

if(NETFT_ROS_VERSION STREQUAL "1")
  find_package(catkin REQUIRED COMPONENTS roscpp geometry_msgs std_srvs diagnostic_msgs)
  catkin_package(
    CATKIN_DEPENDS roscpp geometry_msgs std_srvs diagnostic_msgs)
  add_executable(netft_node_cpp src/ros1_node.cpp)
  set_target_properties(netft_node_cpp PROPERTIES OUTPUT_NAME netft_node)
  target_link_libraries(netft_node_cpp PRIVATE
    netft_ros_support
    netft_core
    ${catkin_LIBRARIES})
  target_include_directories(netft_node_cpp PRIVATE ${catkin_INCLUDE_DIRS})
  if(CATKIN_ENABLE_TESTING)
    catkin_run_tests_target(
      pytest netft_unit "pytest-netft_unit.xml"
      WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
      COMMAND "${CMAKE_COMMAND} -E env PYTHONPATH=${CMAKE_CURRENT_SOURCE_DIR} ${PYTHON_EXECUTABLE} -m pytest ${CMAKE_CURRENT_SOURCE_DIR}/test --junitxml=${CATKIN_TEST_RESULTS_DIR}/${PROJECT_NAME}/pytest-netft_unit.xml -p no:cacheprovider"
    )
    catkin_run_tests_target(
      pytest netft_ros1_smoke_harness "pytest-netft_ros1_smoke_harness.xml"
      WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
      COMMAND "${CMAKE_COMMAND} -E env NETFT_HARNESS=ros1 PYTHONPATH=${CMAKE_CURRENT_SOURCE_DIR} ${PYTHON_EXECUTABLE} -m pytest ${CMAKE_CURRENT_SOURCE_DIR}/test/test_shell_harness.py --junitxml=${CATKIN_TEST_RESULTS_DIR}/${PROJECT_NAME}/pytest-netft_ros1_smoke_harness.xml -p no:cacheprovider"
    )
  endif()
  install(
    FILES launch/netft.launch
    DESTINATION ${CATKIN_PACKAGE_SHARE_DESTINATION}/launch
  )
  install(
    FILES config/netft_ros1.yaml
    DESTINATION ${CATKIN_PACKAGE_SHARE_DESTINATION}/config
  )
  install(TARGETS netft_node_cpp netft_check
    RUNTIME DESTINATION ${CATKIN_PACKAGE_BIN_DESTINATION})
  if(CATKIN_ENABLE_TESTING)
    catkin_add_gtest(netft_ros1_node test/cpp/test_ros1_node.cpp)
    if(TARGET netft_ros1_node)
      target_link_libraries(netft_ros1_node
        netft_ros_support
        netft_core
        ${catkin_LIBRARIES})
      target_include_directories(netft_ros1_node PRIVATE ${catkin_INCLUDE_DIRS})
    endif()
  endif()
elseif(NETFT_ROS_VERSION STREQUAL "2")
  find_package(ament_cmake REQUIRED)
  find_package(rclcpp REQUIRED)
  find_package(geometry_msgs REQUIRED)
  find_package(std_srvs REQUIRED)
  find_package(diagnostic_msgs REQUIRED)
  find_package(hardware_interface REQUIRED)
  find_package(pluginlib REQUIRED)
  find_package(realtime_tools REQUIRED)
  if(hardware_interface_VERSION VERSION_LESS "4.0.0")
    set(NETFT_ROS2_CONTROL_API_DEFINITION NETFT_ROS2_CONTROL_LEGACY_API)
  else()
    set(NETFT_ROS2_CONTROL_API_DEFINITION NETFT_ROS2_CONTROL_MODERN_API)
  endif()

  function(netft_configure_ros2_control_target target)
    target_compile_definitions(${target} PRIVATE
      ${NETFT_ROS2_CONTROL_API_DEFINITION})
    target_include_directories(${target} PRIVATE
      ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(${target} PRIVATE
      netft_ros_support
      netft_core
      ${hardware_interface_TARGETS}
      ${pluginlib_TARGETS}
      ${rclcpp_TARGETS}
      ${realtime_tools_TARGETS}
      ${std_srvs_TARGETS}
      ${diagnostic_msgs_TARGETS}
    )
  endfunction()

  add_library(netft_ros2_control SHARED src/netft_hardware_interface.cpp)
  netft_configure_ros2_control_target(netft_ros2_control)
  pluginlib_export_plugin_description_file(
    hardware_interface netft_hardware_plugins.xml)
  add_executable(netft_node_cpp src/ros2_node.cpp)
  set_target_properties(netft_node_cpp PROPERTIES OUTPUT_NAME netft_node)
  target_link_libraries(netft_node_cpp PRIVATE
    netft_ros_support
    netft_core
    ${rclcpp_TARGETS}
    ${geometry_msgs_TARGETS}
    ${std_srvs_TARGETS}
    ${diagnostic_msgs_TARGETS}
  )
  install(
    FILES launch/netft.launch.py launch/netft_ros2_control.launch.py
    DESTINATION share/${PROJECT_NAME}/launch
  )
  install(
    FILES config/netft_ros2.yaml config/netft_ros2_control.yaml
    DESTINATION share/${PROJECT_NAME}/config
  )
  install(
    FILES urdf/netft.ros2_control.xacro
    DESTINATION share/${PROJECT_NAME}/urdf
  )
  if(NETFT_INSTALL_TEST_TOOLS)
    install(
      PROGRAMS test/integration/ros2_control_smoke.sh
      DESTINATION share/${PROJECT_NAME}/test/integration
    )
    install(
      FILES test/integration/ros2_domain_lease.sh
      DESTINATION share/${PROJECT_NAME}/test/integration
    )
    install(
      PROGRAMS test/integration/ros2_graph_probe.py
      DESTINATION share/${PROJECT_NAME}/test/integration
    )
    install(
      FILES test/integration/fake_sensor_process.py
      DESTINATION share/${PROJECT_NAME}/test/integration
    )
    install(
      FILES test/__init__.py
      DESTINATION share/${PROJECT_NAME}/test
    )
    install(
      FILES test/support/__init__.py test/support/fake_sensor.py
      DESTINATION share/${PROJECT_NAME}/test/support
    )
  endif()
  if(BUILD_TESTING)
    find_package(ament_cmake_pytest REQUIRED)
    find_package(ament_cmake_gtest REQUIRED)

    set(
      NETFT_ROS2_CONTROL_TEST_PREFIX
      "${CMAKE_CURRENT_BINARY_DIR}/netft_ros2_control_test_prefix")
    set(
      NETFT_ROS2_CONTROL_TEST_SHARE
      "${NETFT_ROS2_CONTROL_TEST_PREFIX}/share/${PROJECT_NAME}")
    file(MAKE_DIRECTORY
      "${NETFT_ROS2_CONTROL_TEST_PREFIX}/lib"
      "${NETFT_ROS2_CONTROL_TEST_PREFIX}/share/ament_index/resource_index/packages"
      "${NETFT_ROS2_CONTROL_TEST_PREFIX}/share/ament_index/resource_index/hardware_interface__pluginlib__plugin"
      "${NETFT_ROS2_CONTROL_TEST_SHARE}")
    configure_file(
      test/cpp/netft_hardware_test_plugins.xml
      "${NETFT_ROS2_CONTROL_TEST_SHARE}/netft_hardware_test_plugins.xml"
      COPYONLY)
    configure_file(
      package.xml
      "${NETFT_ROS2_CONTROL_TEST_SHARE}/package.xml"
      COPYONLY)
    file(WRITE
      "${NETFT_ROS2_CONTROL_TEST_PREFIX}/share/ament_index/resource_index/packages/${PROJECT_NAME}"
      "")
    file(WRITE
      "${NETFT_ROS2_CONTROL_TEST_PREFIX}/share/ament_index/resource_index/hardware_interface__pluginlib__plugin/${PROJECT_NAME}"
      "share/${PROJECT_NAME}/netft_hardware_test_plugins.xml")

    add_library(
      netft_ros2_control_testing SHARED
      src/netft_hardware_interface.cpp)
    netft_configure_ros2_control_target(netft_ros2_control_testing)
    target_compile_definitions(netft_ros2_control_testing PRIVATE
      NETFT_ROS2_CONTROL_TESTING)
    set_target_properties(netft_ros2_control_testing PROPERTIES
      LIBRARY_OUTPUT_DIRECTORY "${NETFT_ROS2_CONTROL_TEST_PREFIX}/lib"
      RUNTIME_OUTPUT_DIRECTORY "${NETFT_ROS2_CONTROL_TEST_PREFIX}/lib")

    ament_add_gtest(netft_ros2_node test/cpp/test_ros2_node.cpp)
    if(TARGET netft_ros2_node)
      target_link_libraries(netft_ros2_node
        netft_ros_support
        netft_core
        ${rclcpp_TARGETS}
        ${geometry_msgs_TARGETS}
        ${std_srvs_TARGETS}
        ${diagnostic_msgs_TARGETS}
      )
    endif()
    ament_add_gtest(netft_hardware_interface
      test/cpp/test_hardware_interface.cpp
      test/cpp/support/ros2_control_test_access.cpp
      test/core/support/fake_http_server.cpp
      test/core/support/fake_sensor.cpp
      ENV
        "AMENT_PREFIX_PATH=${NETFT_ROS2_CONTROL_TEST_PREFIX}:$ENV{AMENT_PREFIX_PATH}"
    )
    if(TARGET netft_hardware_interface)
      target_compile_definitions(netft_hardware_interface PRIVATE
        ${NETFT_ROS2_CONTROL_API_DEFINITION})
      target_include_directories(netft_hardware_interface PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${CMAKE_CURRENT_SOURCE_DIR}/test/core
        ${NETFT_SNAPSHOT_SOURCE_DIR}/src)
      target_link_libraries(netft_hardware_interface
        netft_ros_support
        netft_core
        ${CMAKE_DL_LIBS}
        ${hardware_interface_TARGETS}
        ${rclcpp_TARGETS}
        ${std_srvs_TARGETS}
        ${diagnostic_msgs_TARGETS}
      )
      add_dependencies(
        netft_hardware_interface
        netft_ros2_control_testing)
    endif()
    ament_add_pytest_test(netft_unit test
      WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
      APPEND_ENV PYTHONPATH=${CMAKE_CURRENT_SOURCE_DIR}
      TIMEOUT 90
    )
    ament_add_pytest_test(netft_ros2_smoke_harness test/test_shell_harness.py
      WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
      APPEND_ENV PYTHONPATH=${CMAKE_CURRENT_SOURCE_DIR}
      ENV NETFT_HARNESS=ros2
      TIMEOUT 90
    )
    ament_add_pytest_test(netft_ros2_domain_lease_harness test/test_shell_harness.py
      WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
      APPEND_ENV PYTHONPATH=${CMAKE_CURRENT_SOURCE_DIR}
      ENV NETFT_HARNESS=ros2-domain-lease
      TIMEOUT 30
    )
    ament_add_pytest_test(netft_ros2_control_cleanup_harness test/test_shell_harness.py
      WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
      APPEND_ENV PYTHONPATH=${CMAKE_CURRENT_SOURCE_DIR}
      ENV NETFT_HARNESS=ros2-control-cleanup
      TIMEOUT 30
    )
  endif()
  install(TARGETS netft_node_cpp netft_check DESTINATION lib/${PROJECT_NAME})
  install(TARGETS netft_ros2_control
    ARCHIVE DESTINATION lib
    LIBRARY DESTINATION lib
    RUNTIME DESTINATION bin)
  ament_package()
else()
  message(FATAL_ERROR
    "Could not determine the ROS version; set ROS_VERSION to 1 or 2")
endif()
