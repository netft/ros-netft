# ROS Net F/T Driver

[![CI](https://github.com/han-xudong/ros-netft/actions/workflows/ci.yml/badge.svg?branch=main)](https://github.com/han-xudong/ros-netft/actions/workflows/ci.yml)
[![ROS](https://img.shields.io/badge/ROS-1%20%7C%202-22314E.svg?logo=ros&logoColor=white)](https://www.ros.org/)
[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](LICENSE)

`netft_driver` publishes `geometry_msgs/WrenchStamped` force and torque data
from ATI Ethernet Net F/T and Ethernet Axia sensors through the UDP Raw Data
Transfer (RDT) protocol. One ROS-neutral transport core serves native ROS 1 and
ROS 2 adapters.

- Resilient RDT streaming with timeout recovery and bounded reconnect backoff
- Connection, sequence, rate, and device-status diagnostics with serious-sample
  filtering
- Explicit software bias and a bounded, non-biasing `netft_check` operator tool

## Release status

ROS binary packages are not published yet.

| ROS distribution | Branch | Source support | Binary packages |
|---|---|---|---|
| ROS 2 Lyrical | `main` | Supported | Not released |
| ROS 2 Kilted | `main` | Supported | Not released |
| ROS 2 Jazzy | `main` | Supported | Not released |
| ROS 2 Humble | `main` | Supported | Not released |
| ROS 2 Rolling | `main` | Supported | Not released |
| ROS 1 Noetic* | `main` | Legacy | Not released |

\* *ROS 1 Noetic is end-of-life and supported from source only.*

## Installation

### ROS 2 source installation

```bash
source /opt/ros/lyrical/setup.bash
mkdir -p ~/netft_ws/src
git clone https://github.com/han-xudong/ros-netft.git \
  ~/netft_ws/src/netft_driver
cd ~/netft_ws
rosdep install --from-paths src --ignore-src -r -y
colcon build --packages-select netft_driver
source install/setup.bash
```

### ROS 1 Noetic legacy source installation

Noetic is end-of-life and supported from source only. Refresh rosdep with EOL
distribution metadata enabled before resolving dependencies.

```bash
source /opt/ros/noetic/setup.bash
mkdir -p ~/netft_ws/src
git clone https://github.com/han-xudong/ros-netft.git \
  ~/netft_ws/src/netft_driver
cd ~/netft_ws
rosdep update --include-eol-distros
rosdep install --from-paths src --ignore-src -r -y
catkin_make
source devel/setup.bash
```

## Quick start

1. Connect the host to the sensor network, confirm the configured endpoint is
   reachable, and stop the ATI Java demo or any other RDT client. The sensor
   permits only one UDP client.

2. Launch the driver. ROS 2:

   ```bash
   ros2 launch netft_driver netft.launch.py sensor_ip:=192.168.31.100 sensor_port:=49152
   ```

   ROS 1:

   ```bash
   roslaunch netft_driver netft.launch sensor_ip:=192.168.31.100 sensor_port:=49152
   ```

3. Verify one wrench message and the diagnostics. ROS 2:

   ```bash
   ros2 topic echo --once /netft/wrench
   ros2 topic echo --once /diagnostics
   ```

   ROS 1:

   ```bash
   rostopic echo -n 1 /netft/wrench
   rostopic echo -n 1 /diagnostics
   ```

## Interfaces

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
`/diagnostics`. Serious records are withheld from `/netft/wrench` unless
`publish_on_error=true`.

Diagnostics report connection state and endpoint; RDT and FT sequence values;
FT progress classification; receive, publish, expected receive rates, and
tolerance; loss, duplicate, out-of-order, malformed, device-error, reconnect,
and timeout counts; last-record age; and the last exception. A serious status
or timeout recovered between timer ticks is
reported as `ERROR` for at least one diagnostic update after recovery. A
malformed-packet storm means
10 or more malformed datagrams between consecutive diagnostic updates; that
interval is `ERROR`, while every malformed datagram remains in the cumulative
count. `BACKOFF` is immediately `ERROR`.

## Configuration

| Parameter | Default | Meaning |
|---|---:|---|
| `sensor_ip` | `192.168.31.100` | Sensor IPv4 address or host |
| `sensor_port` | `49152` | RDT UDP port |
| `frame_id` | `netft_link` | Wrench header frame |
| `wrench_topic` | `/netft/wrench` | Output topic |
| `bias_service` | `/netft/bias` | Software-bias service |
| `counts_per_force` | `1000000.0` | Counts per N |
| `counts_per_torque` | `1000000.0` | Counts per Nm |
| `publish_rate` | `0.0` | Zero publishes every sample; a positive value limits Hz |
| `receive_timeout` | `0.1` | Seconds without a valid record before recovery |
| `reconnect_initial_delay` | `0.25` | Initial recovery delay in seconds |
| `reconnect_max_delay` | `5.0` | Maximum recovery delay in seconds |
| `diagnostics_rate` | `1.0` | Diagnostics Hz |
| `expected_rdt_rate` | `2000.0` | Expected sensor receive Hz |
| `rate_tolerance` | `0.2` | Allowed fractional receive-rate deviation |
| `publish_on_error` | `false` | Publish serious-status samples when true |

Force and torque counts use independent scales and are published in N and Nm.
Positive `publish_rate` limiting drops intermediate samples instead of queuing
stale wrench messages. The default endpoint, 2,000 Hz expectation, units, and
scales are configuration provenance from an inspected Ethernet Axia profile,
not a runtime validation result for another installation.

## Operations and safety

ATI RDT allows one UDP client. Stop other RDT clients before starting the
driver; a competing client can take stream ownership and cause timeout and
reconnect cycles. On normal ROS shutdown the driver sends Stop Streaming
(`0x0000`). Use normal shutdown whenever possible so streaming does not remain
active at the sensor.

The driver does not change persistent web configuration, calibration, filter,
transform, RDT rate, units, or network settings. It sends only streaming,
stop, and an explicitly requested software-bias command.

RDT gaps count as network loss. FT sequence gaps are normal because the ADC can
run faster than RDT; FT stalls and backward movement are faults. FT tracking
survives reconnects, and a backward first FT value in a new RDT session remains
an error. A sensor restart is confirmed only after a retained baseline of at
least `0x00010000` is followed by two advancing backward values in the low
window `0x00000000` through `0x0000ffff`; confirmation re-baselines tracking.
At a 7,812 Hz ADC rate, this low window spans about 8.4 seconds and keeps
ambiguous high backward values classified as errors. An advancing normal value
or a new RDT session clears an unconfirmed restart candidate. ROS-native
warning and error logs are emitted on fault transitions; a persistent fault is
repeated at most once every 10 seconds so console logging stays bounded.

Run a bounded, non-biasing operator check without starting a ROS graph. ROS 2:

```bash
ros2 run netft_driver netft_check --host 192.168.31.100 --duration 5
```

ROS 1:

```bash
rosrun netft_driver netft_check --host 192.168.31.100 --duration 5
```

> **Warning:** Software bias changes the sensor's zero. Call it only when the
> sensor is stationary, unloaded, and safe. ATI RDT does not acknowledge the
> bias command; verify the resumed wrench and diagnostics before force control.

ROS 2:

```bash
ros2 service call /netft/bias std_srvs/srv/Trigger '{}'
```

ROS 1:

```bash
rosservice call /netft/bias '{}'
```

A successful service response means the bias and stream-restart datagrams were
sent, not that the sensor acknowledged safe conditions.

## Troubleshooting

| Symptom | Likely cause | Operator action |
|---|---|---|
| Repeated timeout or reconnect | Network path, power, firewall, endpoint, or stream ownership problem | Check reachability, UDP port `49152`, sensor power, firewall, and `/diagnostics`. |
| Missing wrench | No accepted record, serious status filtering, or wrong topic | Inspect `/diagnostics`, confirm `wrench_topic`, and resolve device errors; use `publish_on_error` only after risk review. |
| Wrong magnitude | Counts-per-unit or sensor units do not match | Read Counts per Force, Counts per Torque, and units from the sensor page; configure independent N and Nm scales. |
| Receive-rate warning | `expected_rdt_rate` differs from RDT Output Rate | Compare the parameter with Communications page RDT Output Rate, not ADC Sampling Frequency. |
| Nonzero device status | Monitor condition or serious sensor fault | Inspect decoded diagnostic keys; do not use serious-fault wrench data for force control. |
| Wrench absent after bias | Stream did not resume or another fault intervened | Inspect timeout, connection, and device diagnostics; do not issue repeated bias commands as recovery. |
| Competing RDT client | Another application owns the single-client stream | Stop the ATI Java demo and every other RDT client, then allow reconnect. |

## Development

The repository-local Pixi tasks run the pure suite, shell harnesses, native
builds, and loopback graph smoke tests:

```bash
pixi run -e test unit
pixi run ros1-harness
pixi run ros2-harness
pixi run -e noetic build
pixi run -e noetic smoke
pixi run -e lyrical build
pixi run -e lyrical smoke
```

See [Architecture](docs/architecture.md) for package and lifecycle boundaries.

## Contributing

Contributions are welcome. Read the [contributing guide](CONTRIBUTING.md) before
opening an [issue](https://github.com/han-xudong/ros-netft/issues) or submitting
a [pull request](https://github.com/han-xudong/ros-netft/pulls).

## License

MIT. See [LICENSE](LICENSE).
