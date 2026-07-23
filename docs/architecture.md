# Architecture

This document describes the native implementation for maintainers and
contributors. The README is the source for installation, configuration,
operator procedures, and safety guidance.

## Package boundaries

The package contains one ROS-neutral C++17 core and three adapters:

```text
netft_core
├── src/protocol.cpp          RDT wire encoding and decoding
├── src/status.cpp            status, sequence, and diagnostic policy
└── src/client.cpp            UDP transport, acquisition, and recovery

src/check_main.cpp            bounded command-line endpoint check
src/ros1_node.cpp             roscpp standalone adapter
src/ros2_node.cpp             rclcpp standalone adapter
src/netft_hardware_interface.cpp
                              ros2_control SensorInterface plugin
```

`netft_core` contains no ROS headers and owns the single transport and fault
implementation used by every adapter. The build selects either the ROS 1 or
ROS 2 standalone adapter and installs it as `netft_node`. `netft_check` is a
native executable that performs a bounded acquisition without ROS graph
traffic or a software-bias command.

The ROS 2 build additionally exports
`netft_driver/NetFTHardwareInterface` through `netft_hardware_plugins.xml`.
The plugin is one consumer of a robot's existing controller manager; it does
not implement a controller or require a second controller manager.

## Protocol and transport core

RDT requests contain a 16-bit header, 16-bit command, and 32-bit sample count.
Records contain the RDT sequence, FT sequence, device status, three signed
force counts, and three signed torque counts. All fields use network byte
order, and force and torque use independent counts-per-unit scales.

`src/protocol.cpp` owns exact request construction and 36-byte record parsing.
`src/status.cpp` owns device-status classification, unsigned 32-bit RDT
sequence comparison, FT progress and restart confirmation, diagnostic report
construction, and bounded fault-log throttling. `src/client.cpp` owns endpoint
resolution, the UDP socket, streaming commands, the receiver thread, unit
conversion, health counters, and the latest complete sample.

ATI RDT permits one active UDP client. The driver sends only Start Streaming,
Stop Streaming, and an explicitly requested Software Bias command; it does
not modify persistent calibration, filter, transform, rate, unit, or network
settings.

## Acquisition and threading

`NetFTClient::start()` creates one receiver thread. That thread owns blocking
UDP receive activity, decodes complete datagrams, classifies sequence and
device state, updates the health snapshot, and invokes the adapter callback
for accepted samples. Shutdown closes the socket to wake blocked receive,
joins the worker, and sends Stop Streaming when possible.

Standalone adapters publish from the receiver callback and keep ROS timers,
services, logging, and message conversion outside the core. The plugin's
receiver callback immediately converts each accepted native-unit sample to SI
and writes the result into `realtime_tools::RealtimeBuffer<SiSample>`. Its
controller-loop `read()` uses `RealtimeBuffer::readFromRT()`, copies SI values
only, and performs no unit conversion, network I/O, reconnect, service work,
or unbounded mutex wait. A controller loop may consume the same current sample
more than once when it runs faster than the sensor stream.

This separation is control-loop-friendly, but it is not an end-to-end
hard-real-time guarantee: UDP transport, Linux scheduling, controller code,
and sensor firmware remain outside the driver.

## Recovery policies

The core provides two explicit `RecoveryPolicy` values:

- `Reconnect` is used by the standalone nodes. A timeout or socket failure
  closes the current session and enters bounded exponential backoff before a
  new streaming session. Serious samples are filtered unless
  `publish_on_error` is enabled.
- `FailStop` is used by the `ros2_control` plugin. The first fatal transport,
  timeout, FT-progress, serious-status, or malformed-storm fault is latched;
  acquisition stops and lifecycle recovery is required.

Both policies share parsing, sequence semantics, scaling, status
classification, counters, and health snapshots. A new RDT session resets only
the RDT baseline. FT tracking persists across standalone reconnects and uses
the confirmed low-window restart algorithm to distinguish a sensor restart
from a backward FT sequence fault.

## Standalone adapters

`src/ros1_node.cpp` and `src/ros2_node.cpp` expose the same public behavior:

- `/netft/wrench` as `geometry_msgs/WrenchStamped`;
- `/netft/bias` as `std_srvs/Trigger`;
- `/diagnostics` as `diagnostic_msgs/DiagnosticArray`;
- the same endpoint, scaling, publish, recovery, and diagnostics parameters.

ROS 1 uses roscpp publishers, services, timers, XML launch, and catkin
shutdown. ROS 2 uses rclcpp, SensorDataQoS for wrench samples, reliable
diagnostics, a native Trigger service, and a launch description. An RDT record
does not include an acquisition timestamp, so both adapters stamp a message
with the native ROS clock after accepting the complete record.

Software bias is explicit. The core sends one bias request followed by one
continuous-stream request. RDT provides no acknowledgement, so service success
means that both datagrams were sent, not that the load condition was safe or
that the sensor changed its zero.

## ros2_control hardware plugin

The plugin requires exactly one sensor with these interfaces:

```text
<sensor_name>/force.x
<sensor_name>/force.y
<sensor_name>/force.z
<sensor_name>/torque.x
<sensor_name>/torque.y
<sensor_name>/torque.z
```

Missing, duplicate, or additional interfaces reject initialization. The exact
contract works with the standard `ForceTorqueSensorBroadcaster` and the
`ForceTorqueSensor` semantic component.

### Lifecycle ownership

- `on_init` validates the endpoint, scales, timeouts, interface set, and
  instance names.
- `on_configure` constructs the FailStop client, diagnostic evaluator,
  instance-specific auxiliary node, service, timer, and executor thread.
- `on_activate` starts acquisition and waits up to `activation_timeout` for
  the first healthy sample.
- `read()` transfers one complete sample or, after a latched fault, writes
  `NaN` to every interface and returns `ERROR`.
- `on_deactivate` and `on_error` stop acquisition and invalidate the exported
  values.
- `on_cleanup` and `on_shutdown` stop the auxiliary executor and destroy all
  instance-owned resources.

Lifecycle configure followed by activate constructs a fresh client, clears
the plugin latch, and establishes new sequence baselines. The plugin does not
call controller-manager services after a fault.

### Auxiliary ROS node

Each instance owns a lightweight rclcpp node and single-threaded executor for
its bias service and diagnostics. The default service is
`/<encoded sensor token>/bias`; the node, service, socket, receiver thread,
buffer, fault latch, and diagnostic evaluator are all instance-local. Multiple
plugin instances in one process cannot overwrite each other's measurement
state or send a command to another instance's endpoint.

The auxiliary node and default service share one collision-safe name token. A
ROS-valid sensor token is preserved unless it begins with the reserved
`netft_encoded_` prefix. Every other input, including a name beginning with
that prefix, becomes `netft_encoded_` plus the lowercase hexadecimal UTF-8 bytes
of the full original name. The mapping is injective, so names that collided
under character replacement remain isolated. Explicit service overrides are
not encoded.

The bias service accepts requests only while active. It begins a new RDT
session baseline after sending bias and stream-restart commands. Failure to
receive a healthy sample within `receive_timeout` latches a timeout fault.

### Fault contract

Healthy and monitor-condition records are accepted. RDT gaps accept the newest
sample and update loss counters; duplicates, out-of-order records, and isolated
malformed datagrams are dropped. Ten consecutive malformed datagrams are a
fatal malformed storm. FT stalls or backward movement, serious device status,
socket errors, and receive timeout are also fatal in FailStop mode.

The first fatal cause and cumulative counters remain visible in diagnostics
until lifecycle recovery. The portable controller-facing contract is six
`NaN` values, `read() == ERROR`, and an ERROR diagnostic. Humble does not
consistently propagate that hardware error into automatic controller
deactivation, so system integration must reject non-finite inputs or provide a
separate safety monitor.

## Cross-version boundary

The package uses one format-3 manifest with ROS-version conditions. ROS 1 uses
catkin and roscpp. ROS 2 uses ament_cmake, rclcpp, hardware_interface,
pluginlib, and realtime_tools.

The private `src/ros/ros2_control_compat.hpp` isolates the ros2_control API
difference, while private test-access declarations stay in
`src/ros/ros2_control_test_access.hpp`; neither header is installed. Hardware-
interface major versions below 4 use the legacy exported state-interface path
required by Humble; version 4 and later use framework-managed state interfaces.
CMake selects this at compile time from the discovered `hardware_interface`
version. Transport and fault semantics do not branch on a ROS distribution
name.

## Test boundaries

The ROS-neutral CTest suite builds `netft_core` without ROS and uses GTest for
protocol bytes, record parsing, status and sequence semantics, diagnostics,
parameter validation, client recovery, shutdown, and the bounded check tool.
ROS package GTests cover the native ROS adapters and the hardware plugin,
including invalid descriptions, activation, non-blocking reads, every fatal
class, persistent latching, lifecycle recovery, online bias, and auxiliary
thread cleanup.

Loopback graph smoke tests build and use the installed package. They verify
standalone topics, services, diagnostics, shutdown, plugin loading, the
standard broadcaster, and two isolated sensor instances. Python is retained
only for launch descriptions, loopback sensor processes, policy checks, JUnit
validation, and shell-harness orchestration; it does not implement sensor
transport or ROS node behavior.

CI runs the ROS-neutral GTest suite and the applicable installed-package and
loopback gates for Noetic, Humble, Jazzy, Kilted, Lyrical, and Rolling. These
automated paths use loopback endpoints and never contact physical hardware.
Real-hardware acceptance is a separate, explicitly authorized operator gate;
software bias additionally requires immediate confirmation that the sensor is
stationary, unloaded, and safe.
