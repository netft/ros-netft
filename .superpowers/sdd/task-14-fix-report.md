# Task 14 final-review fix report

## Status

The final-review findings against `227ee20` are implemented and verified on
the requested ROS 2 distribution matrix. No earlier commit was rewritten, and
`src/core/` remains byte-identical to upstream `netft-cpp` v0.1.2.

## RED evidence

The configuration, launch, and coverage contract tests were changed before
their production/configuration inputs. This command:

```bash
pixi run -e test python -m pytest -q \
  test/test_config_validation.py \
  test/test_launch_validation.py \
  test/test_workflow_validation.py \
  test/test_manifest.py
```

failed with 3 failures and 6 passes:

- `test_ros2_control_yaml_contains_only_controller_configuration` found the
  inert `netft_hardware` YAML block;
- `test_ros2_control_launch_passes_native_strings_for_every_hardware_argument`
  found no launch defaults for `counts_per_force`, `counts_per_torque`,
  `receive_timeout`, or `activation_timeout`;
- `test_core_coverage_installs_every_native_build_dependency` found no
  `libcurl4-openssl-dev`.

The actual pre-fix production DSO was then tested with:

```bash
pixi run -e humble bash \
  test/integration/audit_ros2_control_symbols.sh \
  build/task11-humble/libnetft_ros2_control.so
```

It failed and printed the leaked archive surface, including
`netft::Client`, `netft::detail::*`, calibration/discovery/status functions,
`netft_driver::DiagnosticEvaluator`, `FaultLogThrottle`, and SI conversion
functions.

The first localization build confirmed that `--exclude-libs` removed all
archive-owned leaks but left RTTI emitted directly by the plugin translation
unit:

```bash
pixi run -e humble bash -lc \
  'cmake -S . -B build/task14-humble-symbol -DROS_VERSION=2 \
   -DBUILD_TESTING=ON -DCMAKE_BUILD_TYPE=Release &&
   cmake --build build/task14-humble-symbol \
     --target netft_ros2_control --parallel 2 &&
   bash test/integration/audit_ros2_control_symbols.sh \
     build/task14-humble-symbol/libnetft_ros2_control.so'
```

The audit failed only on `typeinfo`, `typeinfo name`, and `vtable` for
`netft::NotConnectedError`. `nm` showed those weak definitions came from
`netft_hardware_interface.cpp.o`, where the plugin itself threw that private
core exception. The plugin-side guard now throws `std::runtime_error`; no
snapshot source was changed.

## Implementation

- Localized `netft_core` and `netft_ros_support` archive symbols at both ROS 2
  plugin DSO link boundaries with a targeted Linux GNU/Clang
  `LINKER:--exclude-libs,libnetft_core.a:libnetft_ros_support.a` option.
- Added an `nm -D --defined-only --demangle` audit which requires the
  production plugin export and rejects direct core, diagnostics, throttle,
  and SI-conversion exports.
- Registered the build-tree production DSO audit with CTest. The package build
  also audits the build-tree and installed production DSOs and rejects an
  installed testing DSO.
- Removed the inert `netft_hardware.ros__parameters` YAML block. The installed
  YAML now contains controller-manager and broadcaster configuration only.
- Added native string launch substitutions for the endpoint, automatic/manual
  calibration, receive/configuration/activation timeout, and sensor-name
  arguments, including the new manual-count launch defaults.
- Retained the structured defaults and hardware forwarding in the installed
  Xacro.
- Added `libcurl4-openssl-dev` to the coverage job and a structured workflow
  command/package test.
- Reworded the contribution license boundary and the private architecture
  boundary, and clarified in the README that ros2_control hardware values
  come from the robot description rather than controller YAML.
- Changed the forbidden-coupling test to discover tracked and non-ignored
  repository inputs recursively with `git ls-files`: nested `CMakeLists.txt`,
  `*.cmake`, workflows, ROS/package/build manifests, and lockfiles. Generated
  and ignored trees and human-facing prose are outside the scan.

## Focused GREEN evidence

```text
pixi run -e test python -m pytest -q \
  test/test_config_validation.py test/test_launch_validation.py \
  test/test_urdf_validation.py test/test_workflow_validation.py \
  test/test_manifest.py
11 passed
```

```text
pixi run -e humble bash -lc \
  'cmake --build build/task14-humble-symbol \
     --target netft_ros2_control --parallel 2 &&
   bash test/integration/audit_ros2_control_symbols.sh \
     build/task14-humble-symbol/libnetft_ros2_control.so &&
   ctest --test-dir build/task14-humble-symbol \
     -R netft_ros2_control_symbol_audit --output-on-failure'
1/1 CTest passed
```

## ROS-neutral verification

| Command | Result |
| --- | --- |
| `pixi run unit` | PASS: 142 passed, 1 skipped |
| `pixi run -e test core-configure` | PASS |
| `pixi run -e test core-test` | PASS: 13/13 CTests |

## ROS 2 distribution matrix

Every `build` command ran the build-tree and installed production DSO audits,
confirmed that the testing DSO was non-installed, and loaded the instrumented
test plugin in the hardware suite. Every ros2_control smoke built an installed
package and loaded the production plugin.

| Distribution | `pixi run -e <distro> build` | `smoke` | `ros2-control-smoke` |
| --- | --- | --- | --- |
| Humble | PASS: 186 tests; 31 hardware tests; 1 skipped | PASS: full graph and shutdown scenarios | PASS: `netft_control_result=pass` |
| Jazzy | PASS: 187 tests; 32 hardware tests; 1 skipped | PASS: full graph and shutdown scenarios | PASS: `netft_control_result=pass` |
| Kilted | PASS: 187 tests; 32 hardware tests; 1 skipped | PASS: full graph and shutdown scenarios | PASS: `netft_control_result=pass` |
| Lyrical | PASS: 187 tests; 32 hardware tests; 1 skipped | PASS: full graph and shutdown scenarios | PASS: `netft_control_result=pass` |
| Rolling | PASS: 187 tests; 32 hardware tests; 1 skipped | PASS: full graph and shutdown scenarios | PASS: `netft_control_result=pass` |

## Install and repository audits

An explicit Humble staged install was built and installed with:

```bash
pixi run -e humble bash -lc \
  'cmake --build build/task14-humble-symbol --parallel 2 &&
   stage_root=$(mktemp -d) &&
   trap '\''rm -rf -- "$stage_root"'\'' EXIT &&
   cmake --install build/task14-humble-symbol --prefix "$stage_root" &&
   bash test/integration/audit_ros2_control_symbols.sh \
     "$stage_root/lib/libnetft_ros2_control.so" &&
   test ! -e "$stage_root/lib/libnetft_ros2_control_testing.so" &&
   echo staged_install_symbol_audit=pass &&
   echo staged_test_dso_absent=pass'
```

Result:

```text
staged_install_symbol_audit=pass
staged_test_dso_absent=pass
```

The recursive coupling command used `repository_dependency_inputs()` from
`test/test_manifest.py` and reported:

```text
dependency_inputs_scanned=10
forbidden_coupling_files=0
```

The tracked-address audit excluded the generated lockfile and historical SDD
prose. Its complete address inventory is:

```text
127.0.0.1
127.0.0.7
192.168.1.1
tracked_address_audit=pass
```

An archive made directly from local upstream tag `v0.1.2` was compared with
`src/core/include/netft`, `src/core/src`, and `src/core/LICENSE` using
`diff -ru` and `cmp`:

```text
provenance_commit=bb7df8d211ecc36ebc1434da3686897d7b3d73cc
snapshot_byte_comparison=pass
```

`git diff -- src/core` is empty. The recorded hashes remain:

```text
0e05bd1ef40cbd15da438e2aab71ffe33236661558505dd45d98cf0bbf200edd  src/core/UPSTREAM
c71d239df91726fc519c6eb72d318ec65820627232b2f796219e87dcf35d0ab4  src/core/LICENSE
```

## Review notes

The matrix emitted existing toolchain/dependency warnings: GTest CMake
deprecation warnings, realtime_tools/Boost CMake policy warnings on newer
distributions, Jazzy's `ROS_LOCALHOST_ONLY` deprecation, and Humble compiler
warnings in rclcpp templates. They did not cause a build, test, plugin load,
symbol audit, or smoke failure. No unresolved fix-wave concern remains.
