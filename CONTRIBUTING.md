# Contributing to ROS Net F/T Driver

Thank you for helping improve `netft_driver`. Bug reports, feature proposals,
documentation fixes, tests, and code contributions are welcome.

## Reporting issues

Search [open and closed issues](https://github.com/netft/ros-netft/issues)
before filing a new report. Use the bug-report form for reproducible failures
and the feature-request form for proposed behavior.

A useful bug report includes the driver revision, ROS distribution, operating
system, installation method, sensor model, reproduction steps, expected and
actual behavior, diagnostics, and relevant logs. Remove credentials and other
sensitive information before posting.

## Development environment

[Pixi](https://pixi.sh/) provides the repository-local test environments. Run
the ROS-neutral C++ and policy suites with:

```bash
pixi run -e test core-configure
pixi run -e test core-build
pixi run -e test core-test
pixi run -e test unit
```

The native package and graph gates use only loopback fake sensors. Run every
gate for each distribution affected by the change. For example:

```bash
pixi run ros1-harness
pixi run ros2-harness
pixi run -e noetic build
pixi run -e noetic smoke
pixi run -e jazzy build
pixi run -e jazzy smoke
pixi run -e jazzy ros2-control-smoke
```

The available Pixi environments are `noetic`, `humble`, `jazzy`, `kilted`,
`lyrical`, and `rolling`. Native ROS installations may also be used; CI runs
the complete supported source matrix.

Tests should verify executable behavior, protocol rules, ROS graph contracts,
machine-readable output, or build metadata. Do not pin README, changelog,
release-note, help, diagnostic, log, or error prose. Protocol bytes, serialized
values, JSON keys, ROS interface names, and other machine-readable contracts
remain valid test inputs and assertions.

## Pull requests

Before opening a pull request:

1. Base the change on the latest `main` branch.
2. Keep the change focused and avoid unrelated formatting or refactoring.
3. Add or update tests for behavior changes.
4. Update user documentation when commands, parameters, interfaces, or safety
   behavior change. Repository content must be written in English.
5. Run the ROS-neutral C++ and policy suites plus the relevant ROS build,
   standalone smoke, and ros2_control smoke tests.
6. Complete the pull-request template and address CI results and review
   feedback.

Open an issue before starting a large feature or an interface-breaking change
so the scope can be agreed upon first.

## Core snapshot updates

1. Update and release `netft-cpp` first.
2. Copy only the library paths from an immutable upstream tag.
3. Preserve the `netft` namespace and the Apache-2.0 license.
4. Update `src/core/UPSTREAM` with the copied tag and commit.
5. Rerun the byte comparison and every snapshot and ROS test.
6. Keep ROS-specific changes out of `src/core/`; port reusable fixes upstream before copying them back.

Do not add an automated downloader or submodule for the core snapshot.

## Physical sensor safety

The automated test suite does not require a physical sensor. Do not perform
real-hardware testing for a contribution unless the sensor owner has explicitly
authorized it.

ATI RDT permits only one UDP client, so stop other RDT clients before a hardware
test. A software-bias operation changes the sensor zero and must only be
performed when the sensor is stationary, unloaded, and safe. State in the pull
request whether hardware was used, which checks were run, and whether bias was
issued. Do not publish private network details or sensor identifiers.

## License

ROS integration contributions are licensed under the repository's
[MIT License](LICENSE). The copied `src/core/` snapshot remains under its
upstream [Apache-2.0 License](src/core/LICENSE); propose reusable core fixes
upstream first, then copy them from a released immutable upstream revision.
