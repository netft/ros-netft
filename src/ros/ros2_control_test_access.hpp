#pragma once

#include <cstddef>
#include <cstdint>

namespace netft_driver::ros2_control_test_access {

enum class FaultCode {
  None,
  SensorConfiguration,
  Timeout,
  Socket,
  SeriousStatus,
  FtStall,
  FtBackward,
  MalformedStorm,
  Callback,
};

struct ActivityCounters {
  std::uint64_t sample_generation;
};

void test_force_initial_sample_once() noexcept;
void test_fail_state_write_once_at(std::size_t axis) noexcept;
void test_latch_active_fault(FaultCode fault) noexcept;
bool test_read_active_instance() noexcept;
FaultCode test_active_fault_code() noexcept;
FaultCode test_active_client_fault_code() noexcept;
FaultCode test_active_latched_fault_code() noexcept;
ActivityCounters test_active_activity_counters() noexcept;
bool test_interface_write_fault_latched() noexcept;
void test_throw_executor_cancel_once() noexcept;
int test_auxiliary_thread_count() noexcept;

}  // namespace netft_driver::ros2_control_test_access
