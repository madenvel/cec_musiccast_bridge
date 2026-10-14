#include "musiccast_network.h"

#include <algorithm>
#include <arpa/inet.h>
#include <cerrno>
#include <chrono>
#include <cctype>
#include <curl/curl.h>
#include <ifaddrs.h>
#include <memory>
#include <net/if.h>
#include <poll.h>
#include <set>
#include <sstream>
#include <stdexcept>
#include <sys/socket.h>
#include <unistd.h>

namespace musiccast {
namespace {
constexpr const char *renderer = "urn:schemas-upnp-org:device:MediaRenderer:1";
std::string lower(std::string text) {
  std::transform(text.begin(), text.end(), text.begin(),
                 [](unsigned char c) { return std::tolower(c); });
  return text;
}

std::string trim(const std::string &text) {
  const auto first = text.find_first_not_of(" \t\r\n");
  if (first == std::string::npos)
    return "";
  return text.substr(first, text.find_last_not_of(" \t\r\n") - first + 1);
}

size_t appendBody(char *data, size_t size, size_t count, void *context) {
  auto &body = *static_cast<std::string *>(context);
  const size_t bytes = size * count;
  if (bytes > 1024 * 1024 || body.size() > 1024 * 1024 - bytes)
    return 0;
  try {
    body.append(data, bytes);
    return bytes;
  } catch (...) {
    return 0; // Never unwind through libcurl's C callback.
  }
}

struct Socket {
  int fd;
  explicit Socket(int descriptor) : fd(descriptor) {}
  ~Socket() { if (fd >= 0) close(fd); }
  Socket(const Socket &) = delete;
  Socket &operator=(const Socket &) = delete;
};

std::set<std::string> interfaces(const std::string &requested) {
  if (!requested.empty()) {
    in_addr address{};
    if (inet_pton(AF_INET, requested.c_str(), &address) != 1)
      throw std::runtime_error("--interface must be a local IPv4 address");
    return {requested};
  }
  ifaddrs *raw = nullptr;
  if (getifaddrs(&raw) != 0)
    throw std::runtime_error("Could not enumerate network interfaces");
  std::unique_ptr<ifaddrs, decltype(&freeifaddrs)> entries(raw, freeifaddrs);
  std::set<std::string> result;
  for (auto *entry = raw; entry; entry = entry->ifa_next) {
    if (!entry->ifa_addr || entry->ifa_addr->sa_family != AF_INET ||
        !(entry->ifa_flags & IFF_UP) || !(entry->ifa_flags & IFF_MULTICAST) ||
        (entry->ifa_flags & IFF_LOOPBACK))
      continue;
    char address[INET_ADDRSTRLEN];
    const auto *local = reinterpret_cast<sockaddr_in *>(entry->ifa_addr);
    if (inet_ntop(AF_INET, &local->sin_addr, address, sizeof(address)))
      result.insert(address);
  }
  return result;
}
} // namespace

nlohmann::json requestJson(const std::string &address, const std::string &path) {
  // Accept a hostname/IPv4 address and optional port, never a URL or credentials.
  if (address.empty() || address.find_first_not_of(
          "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789.-:")
          != std::string::npos)
    throw std::runtime_error("Invalid MusicCast address");
  std::unique_ptr<CURL, decltype(&curl_easy_cleanup)> handle(curl_easy_init(),
                                                          curl_easy_cleanup);
  if (!handle)
    throw std::runtime_error("Could not initialise HTTP client");
  const auto url = "http://" + address + "/YamahaExtendedControl/v1/" + path;
  std::string body;
  curl_easy_setopt(handle.get(), CURLOPT_URL, url.c_str());
  curl_easy_setopt(handle.get(), CURLOPT_PROXY, "");
  curl_easy_setopt(handle.get(), CURLOPT_CONNECTTIMEOUT_MS, 1500L);
  curl_easy_setopt(handle.get(), CURLOPT_TIMEOUT_MS, 3000L);
  curl_easy_setopt(handle.get(), CURLOPT_NOSIGNAL, 1L);
  curl_easy_setopt(handle.get(), CURLOPT_FAILONERROR, 1L);
  curl_easy_setopt(handle.get(), CURLOPT_WRITEFUNCTION, appendBody);
  curl_easy_setopt(handle.get(), CURLOPT_WRITEDATA, &body);
  const auto result = curl_easy_perform(handle.get());
  if (result != CURLE_OK)
    throw std::runtime_error(curl_easy_strerror(result));
  auto data = nlohmann::json::parse(body);
  if (!data.is_object() || !data.contains("response_code") ||
      data["response_code"] != 0)
    throw std::runtime_error("MusicCast returned an API error");
  return data;
}

std::optional<Device> parseDevice(const std::string &address,
                                  const nlohmann::json &data) {
  if (!data.is_object() || !data.contains("response_code") ||
      data["response_code"] != 0 || !data.contains("device_id") ||
      !data["device_id"].is_string() || !data.contains("model_name") ||
      !data["model_name"].is_string())
    return std::nullopt;
  Device device{address, data["device_id"], data["model_name"]};
  if (device.id.empty() || device.model.empty())
    return std::nullopt;
  return device;
}

std::optional<Device> probe(const std::string &address) {
  try {
    return parseDevice(address, requestJson(address, "system/getDeviceInfo"));
  } catch (const std::exception &) {
    return std::nullopt;
  }
}

bool sameId(const std::string &left, const std::string &right) {
  return lower(left) == lower(right);
}

bool isRendererResponse(const std::string &response) {
  std::istringstream lines(response);
  std::string line;
  if (!std::getline(lines, line))
    return false;
  std::istringstream status(line);
  std::string protocol;
  int code = 0;
  if (!(status >> protocol >> code) || lower(protocol) != "http/1.1" || code != 200)
    return false;
  while (std::getline(lines, line)) {
    if (trim(line).empty())
      break;
    const auto colon = line.find(':');
    if (colon != std::string::npos && lower(trim(line.substr(0, colon))) == "st" &&
        lower(trim(line.substr(colon + 1))) == lower(renderer))
      return true;
  }
  return false;
}

std::vector<Device> discover(int timeoutSeconds, const std::string &interfaceAddress) {
  if (timeoutSeconds < 1 || timeoutSeconds > 30)
    throw std::runtime_error("Discovery timeout must be between 1 and 30 seconds");
  std::vector<std::unique_ptr<Socket>> sockets;
  std::vector<pollfd> polls;
  sockaddr_in destination{};
  destination.sin_family = AF_INET;
  destination.sin_port = htons(1900);
  inet_pton(AF_INET, "239.255.255.250", &destination.sin_addr);
  const std::string message =
      "M-SEARCH * HTTP/1.1\r\nHOST: 239.255.255.250:1900\r\n"
      "MAN: \"ssdp:discover\"\r\nMX: " +
      std::to_string(std::min(timeoutSeconds, 3)) + "\r\nST: " + renderer + "\r\n\r\n";
  for (const auto &address : interfaces(interfaceAddress)) {
    auto socket = std::make_unique<Socket>(::socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0));
    if (socket->fd < 0)
      continue;
    sockaddr_in local{};
    local.sin_family = AF_INET;
    inet_pton(AF_INET, address.c_str(), &local.sin_addr);
    unsigned char ttl = 2;
    if (bind(socket->fd, reinterpret_cast<sockaddr *>(&local), sizeof(local)) != 0 ||
        setsockopt(socket->fd, IPPROTO_IP, IP_MULTICAST_IF, &local.sin_addr,
                   sizeof(local.sin_addr)) != 0 ||
        setsockopt(socket->fd, IPPROTO_IP, IP_MULTICAST_TTL, &ttl, sizeof(ttl)) != 0)
      continue;
    bool sent = false;
    for (int attempt = 0; attempt < 2; ++attempt)
      sent |= sendto(socket->fd, message.data(), message.size(), 0,
                     reinterpret_cast<sockaddr *>(&destination), sizeof(destination)) >= 0;
    if (sent) {
      polls.push_back({socket->fd, POLLIN, 0});
      sockets.push_back(std::move(socket));
    }
  }
  if (sockets.empty())
    throw std::runtime_error("No usable IPv4 multicast interface for discovery");
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(timeoutSeconds);
  std::set<std::string> candidates;
  while (std::chrono::steady_clock::now() < deadline) {
    const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
        deadline - std::chrono::steady_clock::now()).count();
    const int ready = poll(polls.data(), polls.size(), static_cast<int>(remaining));
    if (ready < 0) {
      if (errno == EINTR)
        break;
      throw std::runtime_error("SSDP receive failed");
    }
    if (ready == 0)
      break;
    for (auto &entry : polls) {
      if (!(entry.revents & POLLIN))
        continue;
      char data[8192];
      sockaddr_in peer{};
      socklen_t peerSize = sizeof(peer);
      const auto bytes = recvfrom(entry.fd, data, sizeof(data), MSG_DONTWAIT,
                                 reinterpret_cast<sockaddr *>(&peer), &peerSize);
      if (bytes > 0 && isRendererResponse(std::string(data, bytes))) {
        char address[INET_ADDRSTRLEN];
        if (inet_ntop(AF_INET, &peer.sin_addr, address, sizeof(address)))
          candidates.insert(address);
      }
    }
  }
  std::vector<Device> devices;
  for (const auto &address : candidates) {
    // Probe the sender, not an arbitrary URL supplied in an SSDP header.
    if (auto device = probe(address)) {
      if (std::none_of(devices.begin(), devices.end(), [&](const Device &existing) {
            return sameId(existing.id, device->id);
          }))
        devices.push_back(*device);
    }
  }
  return devices;
}

Device selectDevice(const std::vector<Device> &devices, const std::string &deviceId) {
  if (!deviceId.empty()) {
    for (const auto &device : devices)
      if (sameId(device.id, deviceId))
        return device;
    throw std::runtime_error("MusicCast device ID " + deviceId + " was not found");
  }
  if (devices.empty())
    throw std::runtime_error("No MusicCast devices found; check network standby and multicast");
  if (devices.size() != 1)
    throw std::runtime_error("Multiple MusicCast devices found; use --discover, then --device-id");
  return devices.front();
}
} // namespace musiccast
