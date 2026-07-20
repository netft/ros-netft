# Contributing to ROS Net F/T Driver

Thank you for helping improve `netft_driver`. Bug reports, feature proposals,
documentation fixes, tests, and code contributions are welcome.

## Reporting issues

Search [open and closed issues](https://github.com/han-xudong/ros-netft/issues)
before filing a new report. Use the bug-report form for reproducible failures
and the feature-request form for proposed behavior.

A useful bug report includes the driver revision, ROS distribution, operating
system, installation method, sensor model, reproduction steps, expected and
actual behavior, diagnostics, and relevant logs. Remove credentials and other
sensitive information before posting.

## Development environment

[Pixi](https://pixi.sh/) provides the repository-local test environments. Run
the ROS-neutral suite and shell harnesses with:

```bash
pixi run -e test unit
pixi run ros1-harness
pixi run ros2-harness
```

Build and smoke-test the ROS distributions affected by your change. For
example:

```bash
pixi run -e noetic build
pixi run -e noetic smoke
pixi run -e jazzy build
pixi run -e jazzy smoke
```

The available Pixi environments are `noetic`, `humble`, `jazzy`, `kilted`,
`lyrical`, and `rolling`. Native ROS installations may also be used; CI runs
the complete supported source matrix.

## Pull requests

Before opening a pull request:

1. Base the change on the latest `main` branch.
2. Keep the change focused and avoid unrelated formatting or refactoring.
3. Add or update tests for behavior changes.
4. Update user documentation when commands, parameters, interfaces, or safety
   behavior change. Repository content must be written in English.
5. Run the ROS-neutral suite and the relevant ROS build and smoke tests.
6. Complete the pull-request template and address CI results and review
   feedback.

Open an issue before starting a large feature or an interface-breaking change
so the scope can be agreed upon first.

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

By contributing, you agree that your contribution is licensed under the
repository's [MIT License](LICENSE).
