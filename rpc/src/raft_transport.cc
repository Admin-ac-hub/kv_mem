#include "kv_raft/transport.h"
#include <arpa/inet.h>
#include <cerrno>
#include <charconv>
#include <fcntl.h>
#include <fstream>
#include <poll.h>
#include <set>
#include <sys/socket.h>
#include <unistd.h>
#include "minirpc/protocol/body_codec.h"
#include "minirpc/protocol/codec.h"
#include "minirpc/core/status.h"

namespace kv::raft {
namespace {
bool Endpoint(const std::string& text, minirpc::Endpoint* endpoint) {
  const auto colon = text.find(':');
  if (colon == std::string::npos) return false;
  unsigned port = 0;
  const auto result = std::from_chars(text.data() + colon + 1, text.data() + text.size(), port);
  in_addr address{};
  endpoint->host = text.substr(0, colon);
  if (result.ec != std::errc{} || result.ptr != text.data() + text.size() ||
      port == 0 || port > 65535 || inet_pton(AF_INET, endpoint->host.c_str(), &address) != 1)
    return false;
  endpoint->port = static_cast<std::uint16_t>(port);
  return true;
}
struct Socket {
  int fd = -1;
  ~Socket() { if (fd >= 0) ::close(fd); }
};
}

Status Config::Validate() const {
  if (cluster.empty() || cluster.size() > 128 ||
      cluster.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-_") != std::string::npos)
    return Status::InvalidArgument("cluster ID must be 1..128 alphanumeric, '-' or '_' characters");
  std::set<std::string> addresses;
  for (const auto* group : {&clients, &peers}) {
    for (const auto& endpoint : *group) {
      minirpc::Endpoint parsed;
      if (!Endpoint(endpoint.ToString(), &parsed) || endpoint.host == "0.0.0.0" ||
          !addresses.insert(endpoint.ToString()).second)
        return Status::InvalidArgument("six distinct explicit IPv4:port endpoints required");
    }
  }
  return Status::OK();
}
std::string Config::Identity(unsigned id) const {
  std::string identity = cluster + "\n" + std::to_string(id);
  for (unsigned i = 0; i < 3; ++i)
    identity += "\n" + clients[i].ToString() + " " + peers[i].ToString();
  return identity;
}
Status Config::Load(const std::filesystem::path& path, Config* config) {
  std::ifstream input(path);
  Config parsed;
  if (!(input >> parsed.cluster)) return Status::InvalidArgument("cannot read cluster config");
  for (unsigned i = 0; i < 3; ++i) {
    unsigned id = 0;
    std::string client, peer;
    if (!(input >> id >> client >> peer) || id != i + 1 ||
        !Endpoint(client, &parsed.clients[i]) || !Endpoint(peer, &parsed.peers[i]))
      return Status::InvalidArgument("expected lines: ID CLIENT_IPV4:PORT PEER_IPV4:PORT, IDs 1/2/3");
  }
  std::string extra;
  if (input >> extra) return Status::InvalidArgument("unexpected extra cluster configuration");
  auto status = parsed.Validate();
  if (status.ok()) *config = std::move(parsed);
  return status;
}

minirpc::RpcResponse Exchange(const minirpc::Endpoint& endpoint,
                             const char* service, const char* method,
                             const std::string& payload,
                             std::chrono::milliseconds timeout) {
  minirpc::RpcResponse failure;
  failure.status_code = static_cast<int>(minirpc::StatusCode::kNetworkError);
  failure.error_message = "bounded RPC failed or timed out";
  if (timeout <= std::chrono::milliseconds::zero()) return failure;
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  Socket socket;
  socket.fd = ::socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
  if (socket.fd < 0) return failure;
  auto wait = [&](short events) {
    for (;;) {
      auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
          deadline - std::chrono::steady_clock::now()).count();
      if (remaining <= 0) return false;
      pollfd descriptor{socket.fd, events, 0};
      const int result = ::poll(&descriptor, 1, static_cast<int>(remaining));
      if (result < 0 && errno == EINTR) continue;
      return result > 0 && (descriptor.revents & events) != 0;
    }
  };
  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_port = htons(endpoint.port);
  if (inet_pton(AF_INET, endpoint.host.c_str(), &address.sin_addr) != 1) return failure;
  const int connected = ::connect(socket.fd, reinterpret_cast<sockaddr*>(&address), sizeof(address));
  if (connected != 0) {
    if (errno != EINPROGRESS || !wait(POLLOUT)) return failure;
    int error = 0;
    socklen_t length = sizeof(error);
    if (getsockopt(socket.fd, SOL_SOCKET, SO_ERROR, &error, &length) != 0 || error != 0) return failure;
  }
  try {
    minirpc::RpcRequest request;
    request.request_id = 1;
    request.service_name = service;
    request.method_name = method;
    request.payload = payload;
    request.deadline_unix_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch() + timeout).count();
    minirpc::ProtocolFrame frame;
    frame.request_id = 1;
    frame.body = minirpc::EncodeRequestBody(request);
    minirpc::RpcCodec codec(2 * 1024 * 1024);
    const auto bytes = codec.Encode(frame);
    std::size_t sent = 0;
    while (sent < bytes.size()) {
      if (!wait(POLLOUT)) return failure;
      const auto count = ::send(socket.fd, bytes.data() + sent, bytes.size() - sent, MSG_NOSIGNAL);
      if (count < 0 && (errno == EAGAIN || errno == EINTR)) continue;
      if (count <= 0) return failure;
      sent += static_cast<std::size_t>(count);
    }
    std::string buffer, error;
    for (;;) {
      if (!wait(POLLIN)) return failure;
      char chunk[16384];
      const auto count = ::recv(socket.fd, chunk, sizeof(chunk), 0);
      if (count < 0 && (errno == EAGAIN || errno == EINTR)) continue;
      if (count <= 0) return failure;
      buffer.append(chunk, static_cast<std::size_t>(count));
      const auto decoded = codec.TryDecode(buffer, &frame, &error);
      if (decoded == minirpc::DecodeResult::kProtocolError) return failure;
      if (decoded == minirpc::DecodeResult::kSuccess) {
        if (frame.request_id != 1 || frame.message_type != minirpc::MessageType::kResponse ||
            frame.codec_type != minirpc::CodecType::kProtobuf || !buffer.empty()) return failure;
        return minirpc::DecodeResponseBody(frame.request_id, frame.body);
      }
    }
  } catch (const std::exception& error) {
    failure.error_message = error.what();
    return failure;
  }
}
}  // namespace kv::raft
