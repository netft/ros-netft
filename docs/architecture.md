# Architecture

This document is an implementation overview for maintainers and contributors.
For installation, configuration, interfaces, and operator safety guidance, see
the repository README.

## Package boundaries

The package keeps sensor transport independent from ROS. `protocol.py` owns
network-byte-order request and record encoding. `status.py` classifies device
status words and tracks RDT and FT sequence progress. `client.py` owns the UDP
socket, worker thread, reconnect loop, sample conversion, sequence accounting,
and health snapshots. `node_common.py` owns shared parameter validation and
diagnostic policy.

`ros1_node.py` and `ros2_node.py` are thin adapters for native clocks,
publishers, services, diagnostics, logging, and shutdown. The installed
`netft_node` script selects the adapter from `ROS_VERSION`; `netft_check` runs
a bounded acquisition without ROS graph traffic or software bias. The
transport and policy core contains no ROS imports.

## RDT protocol

Requests use the `>HHI` layout: header, command, and sample count. Records use
`>III6i`: RDT sequence, FT sequence, status, three signed force counts, and
three signed torque counts. All fields use network byte order. Force and torque
axes are divided by their independently configured counts-per-unit values.

ATI RDT permits one active UDP client. A competing client can take ownership
of the stream and cause timeout/reconnect cycles. The driver sends only RDT
start, stop, and software-bias commands; it does not modify persistent web,
calibration, filter, transform, rate, or network settings.

## RDT session lifecycle

The client moves through `STOPPED`, `CONNECTING`, `STREAMING`, and `BACKOFF`.
Starting creates the worker thread; that thread creates the socket and owns
normal receive and reconnect activity. A valid record enters `STREAMING`.
Timeouts or socket failures close the session and enter bounded exponential
backoff before reconnecting.

Normal shutdown asks the sensor to stop, closes the socket, wakes blocked
receive, joins the worker with a finite deadline, and leaves a completed worker
in `STOPPED`. Software bias is explicit: while streaming, the client sends
exactly one bias request and then restarts continuous streaming. RDT has no
bias acknowledgement, so success means the datagrams were sent, not that load
conditions were safe.

## Sequence and fault accounting

RDT sequence comparison uses unsigned 32-bit wraparound rules. Forward gaps
increase packet loss, equality increases duplicates, and backward movement
increases out-of-order counts. FT sequence progress is tracked independently:
FT gaps are normal, while stalls and backward movement are faults.

FT tracking survives reconnects. A possible sensor restart is confirmed only
by two advancing low-window FT values after a retained high baseline;
ambiguous backward values remain errors. Serious device status words are
counted and, unless `publish_on_error` is true, withheld from the wrench topic.
Serious status, timeout, malformed-storm, FT progress, and backoff events are
reported by diagnostics; counter deltas preserve recovered events until the
next diagnostic evaluation.

## ROS adapters

Both adapters expose `/netft/wrench` as `geometry_msgs/WrenchStamped`,
`/netft/bias` as `std_srvs/Trigger`, and `/diagnostics` as
`diagnostic_msgs/DiagnosticArray`, with the same fifteen parameters and
defaults. An RDT record has no acquisition timestamp, so adapters stamp it
with the native ROS clock immediately after a complete record is accepted.

ROS 1 uses rospy publishers, a queue size of 10, a native service, timer,
logging, XML launch, and catkin shutdown. ROS 2 uses rclpy, SensorDataQoS for
wrench traffic, reliable diagnostics, a native service, Python launch, and
explicit node/context shutdown. Neither adapter owns UDP protocol rules.

## Test boundaries

Pure pytest covers encoding, status, accounting, configuration, diagnostics,
and lifecycle state. Loopback UDP tests exercise the client against the fake
sensor. ROS graph smoke tests build and launch each locked distribution against
loopback only. Shell harnesses inject shutdown failures without starting ROS.

Real-hardware checks are manual, bounded, non-biasing by default, and require
explicit authorization for software bias. Default pytest selection, CTest,
Pixi tasks, and GitHub Actions never contact a hardware endpoint.
