#pragma once

#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <vector>

namespace musiccast {
struct Device {
  std::string address;
  std::string id;
  std::string model;
};

// Call curl_global_init before using these functions, and cleanup after all
// CEC callbacks have stopped. Each request owns its own easy handle.
nlohmann::json requestJson(const std::string &address, const std::string &path);
std::optional<Device> probe(const std::string &address);
std::optional<Device> parseDevice(const std::string &address,
                                  const nlohmann::json &data);
bool isRendererResponse(const std::string &response);
std::vector<Device> discover(int timeoutSeconds,
                             const std::string &interfaceAddress = "");
Device selectDevice(const std::vector<Device> &devices,
                    const std::string &deviceId = "");
bool sameId(const std::string &left, const std::string &right);
} // namespace musiccast
