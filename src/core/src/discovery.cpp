#include "netft/discovery.hpp"

#include <curl/curl.h>

#include <algorithm>
#include <cmath>
#include <limits>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>

#include "detail/xml_config.hpp"

namespace netft {
namespace {

constexpr std::size_t kMaximumResponseBytes = 65'536;

struct CurlHandleDeleter {
  void operator()(CURL *handle) const noexcept { curl_easy_cleanup(handle); }
};

using CurlHandle = std::unique_ptr<CURL, CurlHandleDeleter>;

struct CurlUrlDeleter {
  void operator()(CURLU *url) const noexcept { curl_url_cleanup(url); }
};

using CurlUrl = std::unique_ptr<CURLU, CurlUrlDeleter>;

struct ResponseBuffer {
  std::string body;
  bool too_large{false};
  bool write_failed{false};
};

bool is_ascii_blank(std::string_view value) noexcept {
  if (value.empty()) {
    return true;
  }
  return std::all_of(value.begin(), value.end(), [](const char character) {
    return character == ' ' || character == '\t' || character == '\n' || character == '\r' ||
           character == '\f' || character == '\v';
  });
}

long timeout_milliseconds(std::chrono::duration<double> timeout, std::string_view name) {
  const double seconds = timeout.count();
  if (!std::isfinite(seconds) || seconds <= 0.0) {
    throw DiscoveryError(std::string{name} + " must be finite, positive, and representable");
  }

  const long double rounded_milliseconds = std::ceil(static_cast<long double>(seconds) * 1000.0L);
  if (!std::isfinite(rounded_milliseconds) ||
      rounded_milliseconds > static_cast<long double>(std::numeric_limits<long>::max())) {
    throw DiscoveryError(std::string{name} + " must be finite, positive, and representable");
  }
  return static_cast<long>(rounded_milliseconds);
}

void validate_options(const DiscoveryOptions &options) {
  if (is_ascii_blank(options.sensor_host)) {
    throw DiscoveryError("sensor_host must not be blank");
  }
  if (options.http_port < 1 || options.http_port > 65'535) {
    throw DiscoveryError("http_port must be in the range 1..65535");
  }
  static_cast<void>(timeout_milliseconds(options.connect_timeout, "connect_timeout"));
  static_cast<void>(timeout_milliseconds(options.total_timeout, "total_timeout"));
}

CURLcode initialize_curl_once() {
  static std::once_flag initialization_flag;
  static CURLcode initialization_result = CURLE_FAILED_INIT;
  std::call_once(initialization_flag,
                 [] { initialization_result = curl_global_init(CURL_GLOBAL_DEFAULT); });
  return initialization_result;
}

template <typename Value> void set_curl_option(CURL *handle, CURLoption option, Value value) {
  if (curl_easy_setopt(handle, option, value) != CURLE_OK) {
    throw DiscoveryError("failed to configure sensor discovery request");
  }
}

void set_url_part(CURLU *url, CURLUPart part, const char *value) {
  if (curl_url_set(url, part, value, 0) != CURLUE_OK) {
    throw DiscoveryError("invalid sensor discovery URL");
  }
}

std::size_t append_response(char *data, std::size_t size, std::size_t count,
                            void *user_data) noexcept {
  auto &response = *static_cast<ResponseBuffer *>(user_data);
  if (size != 0 && count > std::numeric_limits<std::size_t>::max() / size) {
    response.too_large = true;
    return 0;
  }
  const std::size_t byte_count = size * count;
  if (byte_count > kMaximumResponseBytes - response.body.size()) {
    response.too_large = true;
    return 0;
  }
  try {
    response.body.append(data, byte_count);
  } catch (...) {
    response.write_failed = true;
    return 0;
  }
  return byte_count;
}

} // namespace

SensorConfiguration discover_sensor(const DiscoveryOptions &options) {
  validate_options(options);
  if (initialize_curl_once() != CURLE_OK) {
    throw DiscoveryError("failed to initialize HTTP transport");
  }

  CurlHandle handle{curl_easy_init()};
  if (!handle) {
    throw DiscoveryError("failed to create HTTP transport");
  }

  CurlUrl url{curl_url()};
  if (!url) {
    throw DiscoveryError("failed to create sensor discovery URL");
  }
  std::string host = options.sensor_host;
  if (host.find(':') != std::string::npos &&
      (host.size() < 2 || host.front() != '[' || host.back() != ']')) {
    host = "[" + host + "]";
  }
  const std::string port = std::to_string(options.http_port);
  set_url_part(url.get(), CURLUPART_SCHEME, "http");
  set_url_part(url.get(), CURLUPART_HOST, host.c_str());
  set_url_part(url.get(), CURLUPART_PORT, port.c_str());
  set_url_part(url.get(), CURLUPART_PATH, "/netftapi2.xml");

  ResponseBuffer response;
  set_curl_option(handle.get(), CURLOPT_CURLU, url.get());
#if LIBCURL_VERSION_NUM >= 0x075500
  set_curl_option(handle.get(), CURLOPT_PROTOCOLS_STR, "http");
#else
  set_curl_option(handle.get(), CURLOPT_PROTOCOLS, static_cast<long>(CURLPROTO_HTTP));
#endif
  set_curl_option(handle.get(), CURLOPT_FOLLOWLOCATION, 0L);
  set_curl_option(handle.get(), CURLOPT_MAXREDIRS, 0L);
  set_curl_option(handle.get(), CURLOPT_NOSIGNAL, 1L);
  set_curl_option(handle.get(), CURLOPT_PROXY, "");
  set_curl_option(handle.get(), CURLOPT_CONNECTTIMEOUT_MS,
                  timeout_milliseconds(options.connect_timeout, "connect_timeout"));
  set_curl_option(handle.get(), CURLOPT_TIMEOUT_MS,
                  timeout_milliseconds(options.total_timeout, "total_timeout"));
  set_curl_option(handle.get(), CURLOPT_WRITEFUNCTION, &append_response);
  set_curl_option(handle.get(), CURLOPT_WRITEDATA, &response);

  const CURLcode transfer_result = curl_easy_perform(handle.get());
  if (response.too_large) {
    throw DiscoveryError("sensor configuration response exceeds 65536 bytes");
  }
  if (response.write_failed) {
    throw DiscoveryError("failed to store sensor configuration response");
  }
  if (transfer_result != CURLE_OK) {
    throw DiscoveryError(std::string{"sensor configuration request failed: "} +
                         curl_easy_strerror(transfer_result));
  }

  long status{};
  if (curl_easy_getinfo(handle.get(), CURLINFO_RESPONSE_CODE, &status) != CURLE_OK) {
    throw DiscoveryError("failed to inspect sensor configuration response");
  }
  if (status != 200) {
    throw DiscoveryError("sensor configuration request returned HTTP " + std::to_string(status));
  }

  return detail::parse_sensor_configuration(response.body);
}

} // namespace netft
