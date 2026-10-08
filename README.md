# ROS Net F/T Driver

[![CI](https://github.com/netft/ros-netft/actions/workflows/ci.yml/badge.svg?branch=main)](https://github.com/netft/ros-netft/actions/workflows/ci.yml)
[![Release](https://img.shields.io/github/v/release/netft/ros-netft?display_name=tag&sort=semver)](https://github.com/netft/ros-netft/releases)
[![CodeQL](https://github.com/netft/ros-netft/actions/workflows/codeql.yml/badge.svg?branch=main)](https://github.com/netft/ros-netft/actions/workflows/codeql.yml)
[![Coverage](https://codecov.io/gh/netft/ros-netft/graph/badge.svg?branch=main)](https://codecov.io/gh/netft/ros-netft)
[![ROS](https://img.shields.io/badge/ROS-1%20%7C%202-22314E.svg?logo=ros&logoColor=white)](https://www.ros.org/)
[![License](https://img.shields.io/github/license/netft/ros-netft?label=license)](LICENSE)

`netft_driver` is a native ROS driver for ATI Ethernet Net F/T and Ethernet
Axia force/torque sensors. It supports a standalone ROS 1 or ROS 2 node and a
ROS 2 `ros2_control` sensor hardware plugin.

## Features

- Publishes calibrated `geometry_msgs/WrenchStamped` data in N and Nm.
- Reports connection, sequence, receive-rate, and device-status diagnostics.
- Provides an explicit software-bias service.
- Integrates with the standard ROS 2 force-torque sensor broadcaster.
- Carries a reviewed private snapshot of the `netft-cpp` acquisition core.

## Supported ROS distributions

| ROS distribution | Standalone driver | `ros2_control` plugin | Support policy |
| --- | ---: | ---: | --- |
| ROS 2 Lyrical | Yes | Yes | Supported |
| ROS 2 Kilted | Yes | Yes | Supported |
| ROS 2 Jazzy | Yes | Yes | Supported |
| ROS 2 Humble | Yes | Yes | Compatibility |
| ROS 2 Rolling | Yes | Yes | Development |
| ROS 1 Noetic\* | Yes | N/A | Legacy source support |

\* _ROS 1 Noetic is end-of-life and supported from source only._

## Installation

### ROS 2

Replace `lyrical` with another supported ROS 2 distribution when required.

```bash
source /opt/ros/lyrical/setup.bash
mkdir -p ~/netft_ws/src
git clone https://github.com/netft/ros-netft.git ~/netft_ws/src/netft_driver
cd ~/netft_ws
rosdep install --from-paths src --ignore-src -r -y
colcon build --packages-select netft_driver
source install/setup.bash
```

### ROS 1 Noetic

```bash
source /opt/ros/noetic/setup.bash
mkdir -p ~/netft_ws/src
git clone https://github.com/netft/ros-netft.git ~/netft_ws/src/netft_driver
cd ~/netft_ws
rosdep update --include-eol-distros
rosdep install --from-paths src --ignore-src -r -y
catkin_make
source devel/setup.bash
```

See the [installation guide](https://netft.dev/docs/get-started/installation)
for prerequisites and package availability.

## Quick start

ATI RDT permits one UDP client. Stop other RDT clients before starting the
driver. The examples use `192.168.1.1`, ATI's factory-default static address;
replace it with the address assigned to your sensor.

### Standalone driver

ROS 2:

```bash
ros2 launch netft_driver netft.launch.py sensor_ip:=192.168.1.1
ros2 topic echo --once /netft/wrench
```

ROS 1:

```bash
roslaunch netft_driver netft.launch sensor_ip:=192.168.1.1
rostopic echo -n 1 /netft/wrench
```

The default bias service is `/netft/bias`. Bias changes the measurement zero;
call it only when the sensor and connected mechanism are in a safe state.

### ros2_control

The plugin class is `netft_driver/NetFTHardwareInterface`. Include the installed
Xacro macro in the robot description managed by your controller manager:

```xml
<xacro:include filename="$(find netft_driver)/urdf/netft.ros2_control.xacro"/>
<xacro:netft_ros2_control
  name="wrist_netft_hardware"
  sensor_name="wrist_ft"
  sensor_ip="192.168.1.1"
  sensor_port="49152"
  receive_timeout="0.1"
  activation_timeout="2.0"/>
```

Configure and spawn `force_torque_sensor_broadcaster` through the robot's
existing controller manager. The repository also installs
`netft_ros2_control.launch.py` as a minimal single-sensor example.

## Documentation

- [Standalone ROS driver tutorial](https://netft.dev/docs/tutorials/ros/standalone)
- [`ros2_control` tutorial](https://netft.dev/docs/tutorials/ros/ros2-control)
- [Standalone interface reference](https://netft.dev/docs/references/ros/standalone)
- [`ros2_control` reference](https://netft.dev/docs/references/ros/ros2-control)
- [Diagnostics and lifecycle](https://netft.dev/docs/references/ros/diagnostics-and-lifecycle)
- [Security and safety](https://netft.dev/docs/references/security-and-safety)

Automatic sensor configuration is enabled by default. Validate the discovered
calibration, units, status, freshness, and finite values before using wrench
data for control. This driver is not a safety-rated control path.

## Contributing

See [CONTRIBUTING.md](CONTRIBUTING.md) for development environments, tests,
hardware authorization, and native-core synchronization. Report security
issues through [SECURITY.md](SECURITY.md).

## License

The repository is licensed under [Apache-2.0](LICENSE). The private
`netft-cpp` snapshot retains its upstream license at
[src/core/LICENSE](src/core/LICENSE).

## Candidate runtime contracts

Version 0.4.0 adds `allow_bias` for both standalone nodes and the
ros2_control hardware parameters. It defaults to true to preserve existing
service availability; set it to false for a read-only deployment. A disabled
adapter does not create the bias service. ROS 2 standalone example:

```sh
ros2 run netft_driver netft_node --ros-args -p allow_bias:=false
```

For ros2_control use `<param name="allow_bias">false</param>` in the hardware
block, or `allow_bias="false"` on the supplied xacro macro. ROS 1 uses the private
`~allow_bias` parameter. Restart/reconfigure for parameter changes; the toggle
is not a dynamic authorization mechanism. Bias is a device command, and callers
must own permission to alter the measurement reference.

`docs/bias-policy.example.xml` illustrates separate SROS2 driver, reader and
operator identities: readers cannot request bias, operators can. Customize node,
namespace, topic and service names and add lifecycle permissions if needed.
The example includes logging, parameter-event publication/subscription and each node's own
parameter-service replies; it does not grant clients parameter-service requests.
It is a policy for adaptation, not an installed keystore. Enforce signed permissions with
`ROS_SECURITY_ENABLE=true`, `ROS_SECURITY_STRATEGY=Enforce` and your provisioned
`ROS_SECURITY_KEYSTORE`; test denied and allowed calls against a loopback sensor.
The bounded `test/integration/ros2_sros2_smoke.py --node-executable <installed-node>`
checks secure sample delivery, a denied reader bias endpoint and an allowed
operator call, including the fake device's command count. Run it in an isolated
container/CI host with SROS2 and Fast DDS installed; it creates temporary keys for
the selected `ROS_DOMAIN_ID` and deletes them on exit. Jazzy CI runs it once with
the graph smoke's installed node and leased domain, without an extra build.
See the [official access-control policy design](https://design.ros2.org/articles/ros2_access_control_policies.html)
and [SROS2 sample policy](https://github.com/ros2/sros2/blob/rolling/sros2/test/policies/sample.policy.xml).
ROS 1 deployments must isolate the master, nodes and device network and disable
bias for readers; a ROS_DOMAIN_ID or service name alone does not authorize users.

Wrench header stamps are ROS publication time. `received_at`, SDK age and control
freshness use the host steady clock, so a paused or jumping simulated ROS clock
does not make stale force data fresh. Diagnostics use wall timers; ros2_control
read ignores its ROS-time argument for freshness. Callback SI overflow is
rejected, and auxiliary executor failure latches a control fault and invalidates
state interfaces. The candidate compiled in a disposable Kilted environment; standalone and controller tests, including SI overflow, disabled bias and executor failure, passed. Other distributions and DDS policy enforcement remain release acceptance checks.

The existing CI contract classifies Jazzy and Lyrical as primary, Humble as
compatibility, Kilted as active short-lived, Rolling as development, and Noetic as
legacy/EOL source-only. A configured matrix is not evidence that every candidate
passed it; keep per-distribution results with the release.
