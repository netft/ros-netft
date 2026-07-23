#include "ros/ros2_control_test_access.hpp"

#include <dlfcn.h>
#include <link.h>

#include <cstring>
#include <cstdlib>

namespace netft_driver::ros2_control_test_access {
namespace {

constexpr char kTestingLibrary[] = "libnetft_ros2_control_testing.so";

int find_testing_library(dl_phdr_info * info, std::size_t, void * data) noexcept
{
  if (info->dlpi_name != nullptr &&
      std::strstr(info->dlpi_name, kTestingLibrary) != nullptr) {
    *static_cast<const char **>(data) = info->dlpi_name;
    return 1;
  }
  return 0;
}

template<typename Function>
Function resolve(const char * name) noexcept
{
  const char * library_path = nullptr;
  (void)::dl_iterate_phdr(find_testing_library, &library_path);
  if (library_path == nullptr) std::abort();
  const auto handle = ::dlopen(library_path, RTLD_LAZY | RTLD_NOLOAD);
  if (handle == nullptr) std::abort();
  const auto symbol = ::dlsym(handle, name);
  (void)::dlclose(handle);
  if (symbol == nullptr) std::abort();
  return reinterpret_cast<Function>(symbol);
}

}  // namespace

void test_force_initial_sample_once() noexcept
{
  resolve<void (*)() noexcept>(
    "netft_ros2_control_test_force_initial_sample_once")();
}

void test_fail_state_write_once_at(const std::size_t axis) noexcept
{
  resolve<void (*)(std::size_t) noexcept>(
    "netft_ros2_control_test_fail_state_write_once_at")(axis);
}

void test_latch_active_fault(const FaultCode fault) noexcept
{
  resolve<void (*)(FaultCode) noexcept>(
    "netft_ros2_control_test_latch_active_fault")(fault);
}

bool test_read_active_instance() noexcept
{
  return resolve<bool (*)() noexcept>(
    "netft_ros2_control_test_read_active_instance")();
}

FaultCode test_active_fault_code() noexcept
{
  return resolve<FaultCode (*)() noexcept>(
    "netft_ros2_control_test_active_fault_code")();
}

FaultCode test_active_client_fault_code() noexcept
{
  return resolve<FaultCode (*)() noexcept>(
    "netft_ros2_control_test_active_client_fault_code")();
}

FaultCode test_active_latched_fault_code() noexcept
{
  return resolve<FaultCode (*)() noexcept>(
    "netft_ros2_control_test_active_latched_fault_code")();
}

ActivityCounters test_active_activity_counters() noexcept
{
  return resolve<ActivityCounters (*)() noexcept>(
    "netft_ros2_control_test_active_activity_counters")();
}

bool test_interface_write_fault_latched() noexcept
{
  return resolve<bool (*)() noexcept>(
    "netft_ros2_control_test_interface_write_fault_latched")();
}

void test_throw_executor_cancel_once() noexcept
{
  resolve<void (*)() noexcept>(
    "netft_ros2_control_test_throw_executor_cancel_once")();
}

int test_auxiliary_thread_count() noexcept
{
  return resolve<int (*)() noexcept>(
    "netft_ros2_control_test_auxiliary_thread_count")();
}

}  // namespace netft_driver::ros2_control_test_access
