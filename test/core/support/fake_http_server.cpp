#include "support/fake_http_server.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <condition_variable>
#include <cstring>
#include <mutex>
#include <stdexcept>
#include <string_view>
#include <thread>
#include <utility>

namespace {

void close_socket(int socket) noexcept {
  if (socket >= 0) {
    ::close(socket);
  }
}

void send_all(int socket, std::string_view data) noexcept {
  while (!data.empty()) {
    const auto sent = ::send(socket, data.data(), data.size(), MSG_NOSIGNAL);
    if (sent <= 0) {
      return;
    }
    data.remove_prefix(static_cast<std::size_t>(sent));
  }
}

} // namespace

struct FakeHttpServer::Impl {
  explicit Impl(std::string initial_body, int initial_status)
      : body(std::move(initial_body)), status(initial_status) {
    listener = ::socket(AF_INET, SOCK_STREAM, 0);
    if (listener < 0) {
      throw std::runtime_error("failed to create fake HTTP server socket");
    }

    const int reuse_address = 1;
    ::setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &reuse_address, sizeof(reuse_address));

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = 0;
    if (::bind(listener, reinterpret_cast<const sockaddr *>(&address), sizeof(address)) != 0 ||
        ::listen(listener, 8) != 0) {
      const auto error = std::string{std::strerror(errno)};
      close_socket(listener);
      listener = -1;
      throw std::runtime_error("failed to bind fake HTTP server: " + error);
    }

    socklen_t address_length = sizeof(address);
    if (::getsockname(listener, reinterpret_cast<sockaddr *>(&address), &address_length) != 0) {
      const auto error = std::string{std::strerror(errno)};
      close_socket(listener);
      listener = -1;
      throw std::runtime_error("failed to inspect fake HTTP server: " + error);
    }
    listening_port = ntohs(address.sin_port);
    worker = std::thread([this] { serve(); });
  }

  ~Impl() {
    stopping.store(true);
    response_changed.notify_all();
    if (listener >= 0) {
      ::shutdown(listener, SHUT_RDWR);
      close_socket(listener);
    }
    const int client = active_client.exchange(-1);
    if (client >= 0) {
      ::shutdown(client, SHUT_RDWR);
    }
    if (worker.joinable()) {
      worker.join();
    }
  }

  void serve() noexcept {
    while (!stopping.load()) {
      const int client = ::accept(listener, nullptr, nullptr);
      if (client < 0) {
        if (stopping.load()) {
          return;
        }
        continue;
      }
      active_client.store(client);
      accepted_connections.fetch_add(1);
      if (stopping.load()) {
        ::shutdown(client, SHUT_RDWR);
      }
      handle_request(client);
      int expected_client = client;
      static_cast<void>(active_client.compare_exchange_strong(expected_client, -1));
      close_socket(client);
    }
  }

  void handle_request(int client) noexcept {
    std::string request;
    char buffer[1024];
    while (request.size() < 8192 && request.find("\r\n\r\n") == std::string::npos) {
      const auto received = ::recv(client, buffer, sizeof(buffer), 0);
      if (received <= 0) {
        return;
      }
      request.append(buffer, static_cast<std::size_t>(received));
    }
    requests.fetch_add(1);

    std::string response_body;
    std::string response_location;
    int response_status{};
    std::chrono::milliseconds response_delay{};
    {
      std::lock_guard<std::mutex> lock(response_mutex);
      response_body = body;
      response_location = redirect_location;
      response_status = status;
      response_delay = delay;
    }

    if (request.rfind("GET /netftapi2.xml ", 0) != 0) {
      response_body.clear();
      response_status = 404;
    }

    if (response_delay.count() > 0) {
      std::unique_lock<std::mutex> lock(response_mutex);
      if (response_changed.wait_for(lock, response_delay, [this] { return stopping.load(); })) {
        return;
      }
    }

    const std::string reason = response_status == 200 ? "OK" : "Error";
    const std::string location_header =
        response_location.empty() ? std::string{} : "Location: " + response_location + "\r\n";
    const std::string headers = "HTTP/1.1 " + std::to_string(response_status) + " " + reason +
                                "\r\n" + "Content-Type: application/xml\r\n" + location_header +
                                "Content-Length: " + std::to_string(response_body.size()) + "\r\n" +
                                "Connection: close\r\n\r\n";
    send_all(client, headers);
    send_all(client, response_body);
  }

  std::string body;
  std::string redirect_location;
  int status;
  std::chrono::milliseconds delay{};
  std::mutex response_mutex;
  std::condition_variable response_changed;
  std::atomic<bool> stopping{false};
  std::atomic<std::uint64_t> requests{0};
  std::atomic<std::uint64_t> accepted_connections{0};
  std::atomic<int> active_client{-1};
  int listener{-1};
  int listening_port{};
  std::thread worker;
};

FakeHttpServer::FakeHttpServer(std::string body, int status)
    : impl_(std::make_unique<Impl>(std::move(body), status)) {}

FakeHttpServer::~FakeHttpServer() = default;

std::string FakeHttpServer::host() const { return "127.0.0.1"; }

int FakeHttpServer::port() const { return impl_->listening_port; }

std::uint64_t FakeHttpServer::request_count() const noexcept { return impl_->requests.load(); }

std::uint64_t FakeHttpServer::accepted_connection_count() const noexcept {
  return impl_->accepted_connections.load();
}

void FakeHttpServer::set_response(std::string body, int status) {
  std::lock_guard<std::mutex> lock(impl_->response_mutex);
  impl_->body = std::move(body);
  impl_->status = status;
}

void FakeHttpServer::set_response_delay(std::chrono::milliseconds delay) {
  std::lock_guard<std::mutex> lock(impl_->response_mutex);
  impl_->delay = delay;
}

void FakeHttpServer::set_redirect_location(std::string location) {
  std::lock_guard<std::mutex> lock(impl_->response_mutex);
  impl_->redirect_location = std::move(location);
}
