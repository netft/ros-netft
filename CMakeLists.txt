cmake_minimum_required(VERSION 3.10)
project(netft_driver)

if("$ENV{ROS_VERSION}" STREQUAL "1")
  find_package(catkin REQUIRED)
  catkin_python_setup()
  catkin_package()
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
  catkin_install_python(
    PROGRAMS scripts/netft_node scripts/netft_check
    DESTINATION ${CATKIN_PACKAGE_BIN_DESTINATION}
  )
elseif("$ENV{ROS_VERSION}" STREQUAL "2")
  find_package(ament_cmake REQUIRED)
  find_package(ament_cmake_python REQUIRED)
  ament_python_install_package(${PROJECT_NAME})
  install(
    PROGRAMS scripts/netft_node scripts/netft_check
    DESTINATION lib/${PROJECT_NAME}
  )
  install(
    FILES launch/netft.launch.py
    DESTINATION share/${PROJECT_NAME}/launch
  )
  install(
    FILES config/netft_ros2.yaml
    DESTINATION share/${PROJECT_NAME}/config
  )
  if(BUILD_TESTING)
    find_package(ament_cmake_pytest REQUIRED)
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
  endif()
  ament_package()
else()
  message(FATAL_ERROR "ROS_VERSION must be 1 or 2")
endif()
