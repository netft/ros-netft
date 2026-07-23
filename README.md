# ROS Net F/T Driver

[![CI](https://github.com/netft/ros-netft/actions/workflows/ci.yml/badge.svg?branch=main)](https://github.com/netft/ros-netft/actions/workflows/ci.yml)
[![Codecov](https://codecov.io/gh/netft/ros-netft/graph/badge.svg?branch=main)](https://app.codecov.io/gh/netft/ros-netft)
[![ROS](https://img.shields.io/badge/ROS-1%20%7C%202-22314E.svg?logo=ros&logoColor=white)](https://www.ros.org/)
[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](LICENSE)

`netft_driver` acquires force and torque data from ATI Ethernet Net F/T and
Ethernet Axia sensors over the UDP Raw Data Transfer (RDT) protocol. A native
C++17 core supports both a standalone ROS node and a `ros2_control` sensor
hardware plugin.

- Native ROS 1 and ROS 2 standalone driver with wrench, diagnostics, and bias
  interfaces
- `ros2_control` `SensorInterface` for the standard force-torque broadcaster
  and controller integrations
- Sequence, receive-rate, device-status, timeout, and reconnect diagnostics

## Supported ROS distributions

| ROS distribution | Standalone driver | `ros2_control` plugin | Support policy |
|---|---:|---:|---|
| ROS 2 Lyrical | Yes | Yes | Supported |
| ROS 2 Kilted | Yes | Yes | Supported |
| ROS 2 Jazzy | Yes | Yes | Supported |
| ROS 2 Humble | Yes | Yes | Compatibility |
| ROS 2 Rolling | Yes | Yes | Development |
| ROS 1 Noetic* | Yes | N/A | Legacy source support |

\* *ROS 1 Noetic is end-of-life and supported from source only.*

## Installation

### ROS 2 source installation

Replace `lyrical` with another supported ROS 2 distribution when required.

```bash
source /opt/ros/lyrical/setup.bash
mkdir -p ~/netft_ws/src
git clone https://github.com/netft/ros-netft.git \
  ~/netft_ws/src/netft_driver
cd ~/netft_ws
rosdep install --from-paths src --ignore-src -r -y
colcon build --packages-select netft_driver
source install/setup.bash
```

### ROS 1 Noetic legacy source installation

Refresh rosdep with EOL distribution metadata enabled before resolving Noetic
dependencies.

```bash
source /opt/ros/noetic/setup.bash
mkdir -p ~/netft_ws/src
git clone https://github.com/netft/ros-netft.git \
  ~/netft_ws/src/netft_driver
cd ~/netft_ws
rosdep update --include-eol-distros
rosdep install --from-paths src --ignore-src -r -y
catkin_make
source devel/setup.bash
```

## Quick start

ATI RDT permits one UDP client. Stop the ATI Java demo and every other RDT
client before starting this driver.

The examples use ATI's [factory-default static address](https://www.ati-ia.com/app_content/Documents/9620-05-Net%20FT.pdf), `192.168.1.1`. A sensor configured for DHCP uses its network-assigned address when available and falls back to its static settings when DHCP is unavailable. Replace the example address with the address currently assigned to your sensor.

### Standalone driver

Launch ROS 2:

```bash
ros2 launch netft_driver netft.launch.py sensor_ip:=192.168.1.1 sensor_port:=49152
ros2 topic echo --once /netft/wrench
ros2 topic echo --once /diagnostics
```

Launch ROS 1:

```bash
roslaunch netft_driver netft.launch sensor_ip:=192.168.1.1 sensor_port:=49152
rostopic echo -n 1 /netft/wrench
rostopic echo -n 1 /diagnostics
```

Run a bounded, non-biasing endpoint check without starting a ROS graph:

```bash
# ROS 2
ros2 run netft_driver netft_check --host 192.168.1.1 --duration 5

# ROS 1
rosrun netft_driver netft_check --host 192.168.1.1 --duration 5
```

### ros2_control integration

The plugin class is `netft_driver/NetFTHardwareInterface`. Embed the installed
`urdf/netft.ros2_control.xacro` macro in the robot description that is already
managed by your controller manager:

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

Configure the standard broadcaster using the pattern installed at
`config/netft_ros2_control.yaml`:

```yaml
controller_manager:
  ros__parameters:
    update_rate: 1000
    wrist_ft_broadcaster:
      type: force_torque_sensor_broadcaster/ForceTorqueSensorBroadcaster

wrist_ft_broadcaster:
  ros__parameters:
    sensor_name: wrist_ft
    frame_id: wrist_ft_link
```

That YAML configures only the controller manager and broadcaster. Hardware
endpoint, calibration, and timeout values come from the robot description's
Xacro-generated `<hardware>` element.

Spawn the broadcaster through the controller manager used by the robot:

```bash
ros2 run controller_manager spawner wrist_ft_broadcaster \
  --controller-manager /controller_manager \
  --param-file /path/to/controllers.yaml
```

The repository also installs `netft_ros2_control.launch.py` as a minimal
single-sensor example. Production robot descriptions should embed the Xacro
in their existing controller-manager setup rather than start another manager.

The sensor exports exactly these six state interfaces:

```text
<sensor_name>/force.x
<sensor_name>/force.y
<sensor_name>/force.z
<sensor_name>/torque.x
<sensor_name>/torque.y
<sensor_name>/torque.z
```

Each plugin instance owns its socket, receiver thread, state buffer,
diagnostics, and default `/<encoded sensor token>/bias` service. Distinct
sensor names therefore provide isolated service and state-interface names for
multiple sensors in one controller-manager process.

For the auxiliary node and default bias service, a `sensor_name` that is a
ROS-valid token is preserved unless it starts with the reserved prefix
`netft_encoded_`. Every other name, including names that start with that
prefix, uses `netft_encoded_` followed by the lowercase hexadecimal UTF-8 bytes
of the complete original name. This injective mapping keeps names such as
`tool-ft` and `tool_ft` isolated. An explicit `bias_service` is used unchanged.

The plugin uses a fail-stop recovery policy. A fatal device, transport,
timeout, FT-sequence, or malformed-storm fault latches the first cause, writes
`NaN` to all six interfaces, and makes `read()` return `ERROR`. It does not
reconnect while active. Inspect the hardware and `/diagnostics`, correct the
cause, then recover the component through its controller-manager lifecycle.
The complete recovery path is active → inactive (deactivate) → unconfigured
(cleanup) → inactive (configure) → active (activate). The fatal fault is
cleared during configure, not during deactivate or cleanup. If the controller
manager has already moved the component to inactive or unconfigured, skip the
state transitions that are not applicable. Use the component name passed as
the Xacro `name`, for example:

```bash
ros2 control list_hardware_components
ros2 control set_hardware_component_state wrist_netft_hardware inactive
ros2 control set_hardware_component_state wrist_netft_hardware unconfigured
ros2 control set_hardware_component_state wrist_netft_hardware inactive
ros2 control set_hardware_component_state wrist_netft_hardware active
```

On Humble, controller manager does not consistently deactivate affected
controllers when a hardware `read()` returns `ERROR`. Humble integrations must
reject non-finite (`NaN`) input in their controllers or provide an independent
system-level safety monitor. This driver does not replace the robot's safety
system.

The receiver performs UDP I/O outside the controller-manager update loop and
transfers complete samples through `realtime_tools::RealtimeBuffer`. This is a
control-loop-friendly design; UDP, Linux scheduling, and sensor firmware mean
end-to-end hard real-time behavior is not guaranteed.

## Standalone interfaces

| Interface | Default name | Type and behavior |
|---|---|---|
| Wrench topic | `/netft/wrench` | `geometry_msgs/WrenchStamped`; force in N and torque in Nm |
| Software-bias service | `/netft/bias` | `std_srvs/Trigger`; sends bias, then restarts continuous streaming |
| Diagnostics topic | `/diagnostics` | `diagnostic_msgs/DiagnosticArray`; connection, device, sequence, and rate health |

ROS 1 uses a wrench publisher queue size of 10. ROS 2 wrench traffic uses
SensorDataQoS; diagnostics are reliable. RDT records have no acquisition
timestamp, so `header.stamp` uses the native ROS clock immediately after a
complete record is accepted.

Status `0x00000000` is healthy and `0x80010000` is a monitor-condition warning.
Every other nonzero status is a serious device error decoded by bit in
`/diagnostics`. Standalone nodes withhold serious records from `/netft/wrench`
unless `publish_on_error=true`.

## Configuration

### Standalone parameters

| Parameter | Default | Meaning |
|---|---:|---|
| `sensor_ip` | `192.168.1.1` | Sensor IPv4 address or host |
| `sensor_port` | `49152` | RDT UDP port |
| `frame_id` | `netft_link` | Wrench header frame |
| `wrench_topic` | `/netft/wrench` | Output topic |
| `bias_service` | `/netft/bias` | Software-bias service |
| `http_port` | `80` | Sensor HTTP configuration port |
| `use_sensor_calibration` | `true` | Read calibration and native units from the sensor over HTTP |
| `counts_per_force` | `1000000.0` | Counts per N when `use_sensor_calibration=false` |
| `counts_per_torque` | `1000000.0` | Counts per Nm when `use_sensor_calibration=false` |
| `publish_rate` | `0.0` | Zero publishes every sample; a positive value limits Hz |
| `receive_timeout` | `0.1` | Seconds without a valid record before recovery |
| `configuration_connect_timeout` | `0.5` | HTTP connection timeout in seconds |
| `configuration_timeout` | `1.0` | Total HTTP configuration-request timeout in seconds |
| `reconnect_initial_delay` | `0.25` | Initial recovery delay in seconds |
| `reconnect_max_delay` | `5.0` | Maximum recovery delay in seconds |
| `diagnostics_rate` | `1.0` | Diagnostics Hz |
| `expected_rdt_rate` | `2000.0` | Expected sensor receive Hz |
| `rate_tolerance` | `0.2` | Allowed fractional receive-rate deviation |
| `publish_on_error` | `false` | Publish serious-status samples when true |

### ros2_control parameters

The Xacro exposes the RDT and HTTP endpoints, calibration controls,
`receive_timeout`, configuration timeouts, and `activation_timeout`. Additional
hardware parameters may be placed in the generated `<hardware>` element:

| Parameter | Default | Meaning |
|---|---:|---|
| `activation_timeout` | `2.0` | Seconds to wait for the first healthy sample |
| `bias_service` | `/<encoded sensor token>/bias` | Instance-specific software-bias service; see the encoding rule above |
| `http_port` | `80` | Sensor HTTP configuration port |
| `use_sensor_calibration` | `true` | Read calibration and native units from the sensor over HTTP |
| `counts_per_force` | `1000000.0` | Counts per N when `use_sensor_calibration=false` |
| `counts_per_torque` | `1000000.0` | Counts per Nm when `use_sensor_calibration=false` |
| `configuration_connect_timeout` | `0.5` | HTTP connection timeout in seconds |
| `configuration_timeout` | `1.0` | Total HTTP configuration-request timeout in seconds |
| `diagnostics_rate` | `1.0` | Diagnostics publication rate in Hz |
| `expected_rdt_rate` | `2000.0` | Expected sensor receive rate in Hz |
| `rate_tolerance` | `0.2` | Allowed fractional receive-rate deviation |

`frame_id`, `wrench_topic`, and publication rate belong to the broadcaster in
`ros2_control`. Standalone reconnect-delay and `publish_on_error` parameters do
not apply to the fail-stop plugin.

### Calibration and HTTP configuration

Automatic sensor configuration is enabled by default. Before streaming, the driver requests the sensor configuration from `http://<sensor_ip>:<http_port>/netftapi2.xml`, using `http_port`, `configuration_connect_timeout`, and `configuration_timeout`. It uses the sensor-reported counts and native force and torque units, then converts every published wrench to N and Nm.

Set `use_sensor_calibration=false` only when an explicit manual calibration is required. This disables HTTP discovery completely and makes `counts_per_force` and `counts_per_torque` the complete calibration override: they are counts/N and counts/Nm, respectively. The override is always interpreted as N and Nm; do not enter counts for another native unit.

With automatic configuration, an unreachable HTTP endpoint, connection or total timeout, non-200 response, oversized response, or invalid configuration prevents a session from starting. The standalone driver reports the configuration failure in diagnostics and retries through its normal reconnect backoff. The `ros2_control` plugin is fail-stop, so activation fails until the configuration issue is corrected and the component is activated again.

## Operations and safety

On normal shutdown the driver sends Stop Streaming (`0x0000`) and closes its
socket. Use normal shutdown whenever possible so streaming does not remain
active at the sensor. The driver sends only start, stop, and explicitly
requested software-bias commands; it does not change persistent web,
calibration, filtering, transform, rate, unit, or network settings.

> **Warning:** Software bias changes the sensor's zero. Call it only when the
> sensor is stationary, unloaded, and safe. ATI RDT does not acknowledge the
> bias command; verify healthy wrench data and diagnostics resume before using
> the measurement for control.

Call the standalone service with:

```bash
# ROS 2
ros2 service call /netft/bias std_srvs/srv/Trigger '{}'

# ROS 1
rosservice call /netft/bias '{}'
```

For `ros2_control`, call the instance service only while the component is
active:

```bash
ros2 service call /wrist_ft/bias std_srvs/srv/Trigger '{}'
```

A successful response means only that the bias and stream-restart datagrams
were sent. If healthy data does not resume before `receive_timeout`, the plugin
latches a fail-stop fault and requires lifecycle recovery.

## Troubleshooting

| Symptom | Likely cause | Operator action |
|---|---|---|
| Repeated timeout or reconnect | Network path, power, firewall, endpoint, or competing RDT client | Check the endpoint, UDP port `49152`, sensor power, firewall, and `/diagnostics`. |
| Missing standalone wrench | No accepted record, serious-status filtering, or wrong topic | Inspect `/diagnostics`, confirm `wrench_topic`, and resolve device errors. |
| `NaN` plugin interfaces | A fail-stop fault is latched | Read the persistent ERROR diagnostic, correct the cause, and perform lifecycle recovery. |
| Wrong magnitude | Counts-per-unit or sensor units do not match | Configure the sensor's independent Counts per Force and Counts per Torque values for N and Nm. |
| Receive-rate warning | `expected_rdt_rate` differs from RDT Output Rate | Compare the parameter with the Communications page RDT Output Rate, not ADC Sampling Frequency. |
| Wrench absent after bias | Streaming did not resume or another fault intervened | Inspect timeout and device diagnostics; do not issue repeated bias commands as recovery. |

## Development

Pixi provides locked local environments for the ROS-neutral suite and every
supported distribution:

```bash
pixi run -e test core-configure
pixi run -e test core-build
pixi run -e test core-test
pixi run -e test unit
pixi run -e noetic build
pixi run -e noetic smoke
pixi run -e humble build
pixi run -e humble smoke
pixi run -e humble ros2-control-smoke
```

See [Architecture](docs/architecture.md) for implementation and lifecycle
boundaries.

## Core provenance and licenses

`src/core/` is a private, immutable snapshot of the `netft-cpp` v0.1.2 library. It is included in this source package and does not require an external `netft-cpp` build dependency. The snapshot retains the upstream `netft` namespace and is licensed under Apache-2.0; the ROS integration around it remains MIT-licensed.

## Contributing

Read [CONTRIBUTING.md](CONTRIBUTING.md) before opening an
[issue](https://github.com/netft/ros-netft/issues) or a
[pull request](https://github.com/netft/ros-netft/pulls).

## License

The ROS integration is MIT-licensed; see [LICENSE](LICENSE). `src/core/` is Apache-2.0; see [src/core/LICENSE](src/core/LICENSE).
