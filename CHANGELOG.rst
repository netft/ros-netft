^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^
Changelog for package netft_driver
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

0.2.2 (2026-07-22)
------------------
* Replace the project-specific sensor address in defaults and examples with
  ATI's factory-default ``192.168.1.1`` address.
* Keep tests focused on executable and machine-readable contracts instead of
  human-facing documentation, help, error, log, and diagnostic wording.
* Normalize generated release notes and remove the obsolete source-only
  availability notice.
* Contributors: Xudong Han

0.2.1 (2026-07-22)
------------------
* Fix ROS version detection when isolated build environments do not expose
  ``ROS_VERSION`` to CMake.
* Declare the ``ros_environment`` build dependency and validate ROS 1 and ROS 2
  configuration with ``ROS_VERSION`` unset.
* Increase the ros2_control smoke-test timeout margin so transient CI scheduler
  stalls do not fault the healthy sensor in the two-sensor isolation scenario.
* Contributors: Xudong Han

0.2.0 (2026-07-21)
------------------
* Replace the sensor runtime with one ROS-neutral C++17 transport, protocol,
  status, and recovery core.
* Add the ``netft_driver/NetFTHardwareInterface`` ros2_control sensor plugin,
  six standard force-torque state interfaces, reusable Xacro, and broadcaster
  configuration.
* Add fail-stop fault handling, lifecycle recovery, instance-local diagnostics
  and software bias, and multi-sensor isolation.
* Install native ``netft_node`` and ``netft_check`` executables for ROS 1 and
  ROS 2 while preserving the standalone ROS interfaces.
* Validate native builds and loopback integration for Noetic, Humble, Jazzy,
  Kilted, Lyrical, and Rolling.
* Contributors: Xudong Han

0.1.0 (2026-07-19)
------------------
* Initial public release.
* Add one ROS 1 and ROS 2 source package for ATI Ethernet Net F/T RDT sensors.
* Add reconnect, diagnostics, software bias, loopback integration tests, and a
  bounded non-biasing hardware check.
* Validate source builds for Noetic, Humble, Jazzy, Kilted, Lyrical, and
  Rolling in locked maintainer environments.
* Contributors: Xudong Han
