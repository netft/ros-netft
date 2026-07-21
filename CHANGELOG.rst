^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^
Changelog for package netft_driver
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

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
