#pragma once

#include <chrono>
#include <cstdint>
#include <memory>
#include <string>

class FakeHttpServer {
public:
  explicit FakeHttpServer(std::string body, int status = 200);
  ~FakeHttpServer();

  FakeHttpServer(const FakeHttpServer &) = delete;
  FakeHttpServer &operator=(const FakeHttpServer &) = delete;

  std::string host() const;
  int port() const;
  std::uint64_t request_count() const noexcept;
  std::uint64_t accepted_connection_count() const noexcept;
  void set_response(std::string body, int status = 200);
  void set_response_delay(std::chrono::milliseconds delay);
  void set_redirect_location(std::string location);

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
