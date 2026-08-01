# Security Policy

## Supported versions

Security fixes are applied to the latest released version and the `main`
branch. Older releases may not receive backports.

## Reporting a vulnerability

Do not open a public issue for a suspected vulnerability. Use
[GitHub private vulnerability reporting](https://github.com/netft/ros-netft/security/advisories/new)
when available. If private reporting is unavailable, contact the maintainer at
[hanxudong159@126.com](mailto:hanxudong159@126.com).

Include the affected package and ROS versions, deployment assumptions, a
minimal reproduction or data-flow description, and the security impact.
Remove sensor addresses, robot descriptions, credentials, measurements, and
private network details before submitting a report.

## Network security model

ATI Net F/T devices expose configuration through HTTP and stream RDT records
through UDP. `ros-netft` implements those device protocols and does not add
transport encryption, peer authentication, or message integrity.
Configuration discovery and RDT data must not be treated as authenticated
solely because they came from the configured sensor address.

The supported deployment model places the sensor, ROS host, and controller on
a trusted, isolated network segment. Do not expose the sensor HTTP or RDT
ports directly to the Internet or an untrusted shared network. If traffic must
cross an untrusted network, place the complete sensor connection inside an
authenticated boundary such as a managed VPN or an equivalent
industrial-network gateway.

Recommended controls include:

- isolate the sensor network with a dedicated interface or VLAN;
- restrict traffic with host and network firewalls to expected hosts;
- prevent untrusted devices from joining, routing to, or bridging the sensor
  segment; and
- monitor unexpected calibration, unit, endpoint, and connection changes.

The native core disables HTTP redirects and proxy use and strictly validates
the configuration response. Those controls reduce exposure but do not
authenticate its origin.

## Configuration integrity and robot safety

Automatic discovery reads calibration scales and units over unauthenticated
HTTP. Verify discovered values against the intended sensor configuration
before enabling a controller. A complete manual override avoids HTTP
discovery but does not authenticate the UDP measurement stream.

Force/torque data can affect robot motion, protective limits, and controller
state. Reject stale, faulted, or non-finite measurements, use lifecycle
fail-stop behavior where appropriate, and retain an independent safety-rated
control path. This package is not a substitute for an emergency stop or other
safety system.

Software bias changes the sensor zero and RDT does not acknowledge the
command. Invoke bias only with explicit operator authorization, a verified
unloaded sensor, and a mechanically safe robot.

## Known protocol limitations

A report that depends only on the absence of HTTPS or authenticated UDP in the
ATI device protocol may describe a known protocol limitation rather than a
defect this package can correct independently. Reports remain valuable when
they demonstrate an unexpected exposure, a bypass of the stated
trusted-network model, unsafe handling of unauthenticated data, or a practical
mitigation compatible with the device.
