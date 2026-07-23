#include "netft/client.hpp"
#include "ros/unit_conversion.hpp"

#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <csignal>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iomanip>
#include <iostream>
#include <limits>
#include <locale>
#include <mutex>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <unistd.h>
#include <vector>

namespace {
using netft::Client;
using netft::Config;
using netft::HealthSnapshot;
using netft::Sample;

volatile std::sig_atomic_t interrupted = 0;
void handle_interrupt(int) { interrupted = 1; }

class ScopeExit {
public:
  explicit ScopeExit(std::function<void()> function) : function_(std::move(function)) {}
  ~ScopeExit() { if (function_) function_(); }
  ScopeExit(const ScopeExit &) = delete;
private:
  std::function<void()> function_;
};

class LastSampleSlot {
public:
  void store(const Sample & sample) {
    { std::lock_guard<std::mutex> lock(mutex_); sample_ = sample; }
    changed_.notify_all();
  }
  std::optional<Sample> load() const {
    std::lock_guard<std::mutex> lock(mutex_); return sample_;
  }
  void wait_for_change(std::chrono::milliseconds timeout) {
    std::unique_lock<std::mutex> lock(mutex_);
    changed_.wait_for(lock, timeout);
  }
private:
  mutable std::mutex mutex_;
  std::condition_variable changed_;
  std::optional<Sample> sample_;
};

struct Options {
  Config config;
  double duration{5.0};
  std::optional<std::string> output;
  bool help{false};
};

struct Result {
  std::string endpoint;
  double requested_duration_s{};
  double elapsed_s{};
  HealthSnapshot health;
  std::optional<Sample> last;
};

const char * usage() {
  return "usage: netft_check [--host HOST] [--port PORT] [--duration SECONDS] "
         "[--http-port PORT] [--configuration-connect-timeout SECONDS] "
         "[--configuration-timeout SECONDS] "
         "[--counts-per-force SCALE] [--counts-per-torque SCALE] "
         "[--receive-timeout SECONDS] [--output PATH] [--json]";
}

double number(const std::string & field, const std::string & value) {
  std::size_t used = 0;
  double parsed{};
  try { parsed = std::stod(value, &used); }
  catch (...) { throw std::invalid_argument{"invalid " + field + " value"}; }
  if (used != value.size()) throw std::invalid_argument{"invalid " + field + " value"};
  return parsed;
}

int integer(const std::string & field, const std::string & value) {
  std::size_t used = 0;
  long parsed{};
  try { parsed = std::stol(value, &used); }
  catch (...) { throw std::invalid_argument{"invalid " + field + " value"}; }
  if (used != value.size() || parsed < -2147483648L || parsed > 2147483647L) {
    throw std::invalid_argument{"invalid " + field + " value"};
  }
  return static_cast<int>(parsed);
}

Options parse_options(int argc, char ** argv) {
  Options options;
  std::optional<double> counts_per_force;
  std::optional<double> counts_per_torque;
  for (int index = 1; index < argc; ++index) {
    std::string argument{argv[index]};
    if (argument == "--json") continue;
    if (argument == "--help") { options.help = true; return options; }
    const auto equals = argument.find('=');
    const auto key = argument.substr(0, equals);
    std::string value;
    if (key == "--host" || key == "--port" || key == "--http-port" ||
        key == "--configuration-connect-timeout" || key == "--configuration-timeout" ||
        key == "--duration" ||
        key == "--counts-per-force" || key == "--counts-per-torque" ||
        key == "--receive-timeout" || key == "--output") {
      if (equals != std::string::npos) value = argument.substr(equals + 1);
      else if (++index < argc) value = argv[index];
      else throw std::invalid_argument{"missing value for " + key};
    } else {
      throw std::invalid_argument{"unknown argument: " + argument};
    }
    if (key == "--host") options.config.sensor_host = value;
    else if (key == "--port") options.config.rdt_port = integer("port", value);
    else if (key == "--http-port") options.config.http_port = integer("http-port", value);
    else if (key == "--configuration-connect-timeout") {
      options.config.configuration_connect_timeout =
        std::chrono::duration<double>{number("configuration-connect-timeout", value)};
    }
    else if (key == "--configuration-timeout") {
      options.config.configuration_timeout =
        std::chrono::duration<double>{number("configuration-timeout", value)};
    }
    else if (key == "--duration") options.duration = number("duration", value);
    else if (key == "--counts-per-force") {
      counts_per_force = number("counts-per-force", value);
    }
    else if (key == "--counts-per-torque") {
      counts_per_torque = number("counts-per-torque", value);
    }
    else if (key == "--receive-timeout") options.config.receive_timeout = std::chrono::duration<double>{number("receive-timeout", value)};
    else if (key == "--output") options.output = value;
  }
  if (!std::isfinite(options.duration) || options.duration <= 0.0) {
    throw std::invalid_argument{"duration must be finite and greater than zero"};
  }
  if (counts_per_force.has_value() != counts_per_torque.has_value()) {
    throw std::invalid_argument{"counts-per-force and counts-per-torque must be provided together"};
  }
  if (counts_per_force) {
    options.config.calibration_override = netft::Calibration{
      *counts_per_force, *counts_per_torque,
      netft::ForceUnit::Newton, netft::TorqueUnit::NewtonMeter};
  }
  netft::validate(options.config);
  return options;
}

Result run_check(const Options & options) {
  LastSampleSlot latest;
  Client client{options.config};
  const auto started = std::chrono::steady_clock::now();
  ScopeExit stop{[&] { client.stop(); }};
  client.start([&](const Sample & sample) { latest.store(sample); });
  const auto first_deadline = started + std::chrono::duration<double>{std::max(1.0, options.config.receive_timeout.count() * 5.0)};
  while (client.health().received_count == 0) {
    if (interrupted) throw std::runtime_error{"interrupted"};
    if (client.faulted()) throw std::runtime_error{client.health().last_error};
    if (std::chrono::steady_clock::now() >= first_deadline) {
      throw std::runtime_error{"no Net F/T sample received"};
    }
    latest.wait_for_change(std::chrono::milliseconds{10});
  }
  const auto finish = started + std::chrono::duration<double>{options.duration};
  while (std::chrono::steady_clock::now() < finish) {
    if (interrupted) throw std::runtime_error{"interrupted"};
    latest.wait_for_change(std::chrono::milliseconds{10});
  }
  client.stop();
  const auto elapsed = std::chrono::duration<double>{std::chrono::steady_clock::now() - started}.count();
  return {options.config.sensor_host + ":" + std::to_string(options.config.rdt_port), options.duration,
          elapsed, client.health(), latest.load()};
}

std::string json_string(const std::string & value) {
  std::ostringstream output; output << '"';
  for (const unsigned char character : value) {
    if (character == '"' || character == '\\') output << '\\' << character;
    else if (character == '\n') output << "\\n";
    else if (character == '\r') output << "\\r";
    else if (character == '\t') output << "\\t";
    else if (character < 0x20) output << "\\u" << std::hex << std::setw(4) << std::setfill('0') << static_cast<unsigned>(character) << std::dec;
    else output << character;
  }
  return output.str() + '"';
}

std::string json_number(double value) {
  if (!std::isfinite(value)) throw std::runtime_error{"non-finite JSON value"};
  std::ostringstream output;
  output.imbue(std::locale::classic());
  output << std::setprecision(std::numeric_limits<double>::max_digits10)
         << std::defaultfloat << value;
  if (!output) throw std::runtime_error{"unable to serialize JSON number"};
  return output.str();
}

std::string json_optional_sequence(const std::optional<std::uint32_t> & value) {
  return value ? std::to_string(*value) : "null";
}

std::string json_wrench(const std::optional<netft_driver::SiSample> & sample, bool force) {
  if (!sample) return "null";
  const auto & values = force ? sample->force : sample->torque;
  return "[" + json_number(values[0]) + ", " + json_number(values[1]) + ", " + json_number(values[2]) + "]";
}

std::string configuration_source(const netft::CalibrationSource source) {
  return source == netft::CalibrationSource::Sensor ? "sensor" : "override";
}

std::string serialize(const Result & result) {
  const auto & health = result.health;
  const auto & configuration = health.sensor_configuration;
  const auto si_sample = result.last
    ? std::optional<netft_driver::SiSample>{netft_driver::to_si_sample(*result.last)}
    : std::nullopt;
  std::ostringstream output;
  output << "{\n"
         << "  \"configuration_source\": "
         << (configuration ? json_string(configuration_source(configuration->source)) : "null")
         << ",\n"
         << "  \"force_unit\": "
         << (configuration ? json_string(std::string{netft::to_string(
              configuration->calibration.force_unit)}) : "null") << ",\n"
         << "  \"torque_unit\": "
         << (configuration ? json_string(std::string{netft::to_string(
              configuration->calibration.torque_unit)}) : "null") << ",\n"
         << "  \"counts_per_force_unit\": "
         << (configuration ? json_number(configuration->calibration.counts_per_force_unit) : "null")
         << ",\n"
         << "  \"counts_per_torque_unit\": "
         << (configuration ? json_number(configuration->calibration.counts_per_torque_unit) : "null")
         << ",\n"
         << "  \"device_error_count\": " << health.device_error_count << ",\n"
         << "  \"device_status\": \"0x" << std::hex << std::setw(8) << std::setfill('0') << health.last_status << std::dec << "\",\n"
         << "  \"duplicate_count\": " << health.duplicate_count << ",\n"
         << "  \"elapsed_s\": " << json_number(result.elapsed_s) << ",\n"
         << "  \"endpoint\": " << json_string(result.endpoint) << ",\n"
         << "  \"last_force_n\": " << json_wrench(si_sample, true) << ",\n"
         << "  \"last_ft_sequence\": " << json_optional_sequence(health.last_ft_sequence) << ",\n"
         << "  \"last_rdt_sequence\": " << json_optional_sequence(health.last_rdt_sequence) << ",\n"
         << "  \"last_torque_nm\": " << json_wrench(si_sample, false) << ",\n"
         << "  \"lost_count\": " << health.lost_count << ",\n"
         << "  \"malformed_count\": " << health.malformed_count << ",\n"
         << "  \"out_of_order_count\": " << health.out_of_order_count << ",\n"
         << "  \"receive_rate_hz\": " << json_number(result.health.received_count / result.elapsed_s) << ",\n"
         << "  \"reconnect_count\": " << health.reconnect_count << ",\n"
         << "  \"requested_duration_s\": " << json_number(result.requested_duration_s) << ",\n"
         << "  \"sample_count\": " << health.received_count << ",\n"
         << "  \"timeout_count\": " << health.timeout_count << ",\n"
         << "  \"warning_count\": " << health.warning_count << "\n}";
  return output.str();
}

std::string serialize_error(const std::string & category, const std::string & message) {
  return "{\n  \"error\": " + json_string(category) +
         ",\n  \"exit_code\": 2,\n  \"message\": " + json_string(message) + "\n}";
}

void write_output(const std::string & path, const std::string & text) {
  const std::filesystem::path output{path};
  const auto parent = output.parent_path().empty() ? std::filesystem::path{"."} : output.parent_path();
  std::string pattern = (parent / ("." + output.filename().string() + ".XXXXXX")).string();
  std::vector<char> temporary(pattern.begin(), pattern.end()); temporary.push_back('\0');
  const int descriptor = ::mkstemp(temporary.data());
  if (descriptor < 0) throw std::runtime_error{"unable to create output temporary file"};
  const std::string temporary_path{temporary.data()};
  ScopeExit cleanup{[&] { std::remove(temporary_path.c_str()); }};
  FILE * stream = ::fdopen(descriptor, "w");
  if (!stream) { ::close(descriptor); throw std::runtime_error{"unable to write output"}; }
  if (std::fwrite(text.data(), 1, text.size(), stream) != text.size() || std::fputc('\n', stream) == EOF || std::fclose(stream) != 0) {
    throw std::runtime_error{"unable to write output"};
  }
  std::filesystem::rename(temporary_path, output);
}
}  // namespace

int main(int argc, char ** argv) {
  const auto previous_handler = std::signal(SIGINT, handle_interrupt);
  ScopeExit restore_handler{[&] { std::signal(SIGINT, previous_handler); }};
  try {
    const auto options = parse_options(argc, argv);
    if (options.help) {
      std::cout << usage() << '\n';
      return 0;
    }
    const auto result = run_check(options);
    const auto text = serialize(result);
    if (options.output) write_output(*options.output, text);
    std::cout << text << '\n';
    return result.health.received_count > 0 && result.health.device_error_count == 0 &&
           result.health.warning_count == 0 ? 0 : 1;
  }
  catch (const std::invalid_argument & error) {
    std::cerr << serialize_error("invalid_arguments", error.what()) << '\n';
    return 2;
  }
  catch (const std::exception & error) {
    std::cerr << serialize_error("check_failed", error.what()) << '\n';
    return 2;
  }
}
