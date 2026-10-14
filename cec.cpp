#include <iostream>
#include <atomic>
#include <chrono>
#include <fstream>
#include <libcec/cec.h>
#include <mutex>
#include <optional>
#include <signal.h>
#include <sstream>
#include <unistd.h>
#include <vector>

#include <curl/curl.h>
#include "musiccast_network.h"

#include <nlohmann/json.hpp>

#include <boost/program_options.hpp>

volatile sig_atomic_t stopping = 0;
void sighandler(int) { stopping = 1; }

struct DeviceData {
  std::string inputName;
  int volume;
  std::string address;
  std::string deviceId;
  std::string interfaceAddress;
  int discoveryTimeout = 5;
  bool discoverOnly = false;
};

using json = nlohmann::json;

class MusicCast {
  mutable std::mutex requestMutex;
  std::string address;
  const std::string deviceId;
  mutable std::atomic<bool> online{true};

  json get(const std::string &url) const {
    std::lock_guard<std::mutex> lock(requestMutex);
    if (!online)
      return json{};
    try {
      return musiccast::requestJson(address, url);
    } catch (const std::exception &e) {
      online = false;
      std::cerr << "Failed to perform MusicCast request: " << e.what()
                << std::endl;
    }
    return json{};
  }

public:
  struct ZoneStatus {
    std::string power;
    int sleep;
    int volume;
    int max_volume;
    bool mute;
    std::string input;
  };

  explicit MusicCast(const musiccast::Device &device)
      : address(device.address), deviceId(device.id) {}

  bool isOnline() const { return online; }

  void checkConnection(const DeviceData &options) {
    // The endpoint can change while CEC callbacks run; replace it under the
    // same lock used for requests. Always retain the selected device identity.
    std::string previousAddress;
    {
      std::lock_guard<std::mutex> lock(requestMutex);
      previousAddress = address;
    }
    auto device = musiccast::probe(previousAddress);
    if (!device || !musiccast::sameId(device->id, deviceId)) {
      online = false;
      if (options.address == "auto") {
        try {
          device = musiccast::selectDevice(
              musiccast::discover(options.discoveryTimeout, options.interfaceAddress),
              deviceId);
        } catch (const std::exception &e) {
          std::cerr << "Waiting for MusicCast: " << e.what() << std::endl;
          return;
        }
      } else {
        std::cerr << "Waiting for MusicCast at " << previousAddress << std::endl;
        return;
      }
    }
    std::lock_guard<std::mutex> lock(requestMutex);
    if (!online || address != device->address)
      std::cout << "MusicCast reconnected: " << device->id << " at "
                << device->address << std::endl;
    address = device->address;
    online = true;
  }

  void setPower(const std::string &power) const {
    std::cout << "Setting MusicCast power status to " << power << std::endl;
    get("main/setPower?power=" + power);
  }

  void setInput(const std::string &inputName) const {
    std::cout << "Setting MusicCast input to " << inputName << std::endl;
    get("main/setInput?input=" + inputName);
  }

  void setVolume(int vol) {
    std::cout << "Setting MusicCast volume to " << vol << std::endl;
    get("main/setVolume?volume=" + std::to_string(vol));
  }

  void volumeUp() { get("main/setVolume?volume=up"); }

  void volumeDown() { get("main/setVolume?volume=down"); }

  std::optional<ZoneStatus> getMainZoneStatus() const {
    try {
      const auto data = get("main/getStatus");
      if (!data.is_object() || data.value("response_code", -1) != 0)
        return std::nullopt;
      ZoneStatus status{data.at("power"), data.at("sleep"), data.at("volume"),
                        data.at("max_volume"), data.at("mute"), data.at("input")};
      if (status.max_volume <= 0)
        return std::nullopt;
      return status;
    } catch (const json::exception &e) {
      std::cerr << "Invalid MusicCast status: " << e.what() << std::endl;
      return std::nullopt;
    }
  }
};

bool isOn(CEC::cec_power_status powerStatus) {
  switch (powerStatus) {
  case CEC::CEC_POWER_STATUS_ON:
    return true;
  case CEC::CEC_POWER_STATUS_STANDBY:
  case CEC::CEC_POWER_STATUS_IN_TRANSITION_STANDBY_TO_ON:
  case CEC::CEC_POWER_STATUS_IN_TRANSITION_ON_TO_STANDBY:
  case CEC::CEC_POWER_STATUS_UNKNOWN:
    break;
  }
  return false;
}

DeviceData parseCmdlineOpts(int argc, char *argv[]) {
  try {
    DeviceData deviceData;

    namespace po = boost::program_options;
    std::string configFile;
    po::options_description settings("MusicCast settings");
    settings.add_options()(
        "input,i", po::value(&deviceData.inputName)->default_value("optical1"),
        "MusicCast input name to set when power on")(
        "address,a", po::value(&deviceData.address)->default_value("auto"),
        "MusicCast hostname/IPv4 address, or auto for SSDP discovery")(
        "volume,v", po::value(&deviceData.volume)->default_value(90),
        "Power-on volume in Yamaha units, not percent")(
        "device-id", po::value(&deviceData.deviceId)->default_value(""),
        "Select the stable device ID reported by --discover")(
        "discovery-timeout", po::value(&deviceData.discoveryTimeout)->default_value(5),
        "SSDP listening time in seconds (1-30), plus API probes")(
        "interface", po::value(&deviceData.interfaceAddress)->default_value(""),
        "Local IPv4 address for discovery (default: all multicast interfaces)");
    po::options_description desc("Bridge TV HDMI-CEC commands to Yamaha MusicCast");
    desc.add_options()("help,h", "Show this information")(
        "config,c", po::value(&configFile), "Read configuration file; CLI takes precedence")(
        "discover", po::bool_switch(&deviceData.discoverOnly),
        "Print discovered devices as JSON and exit without opening CEC");
    desc.add(settings);

    po::variables_map vm;
    po::store(po::parse_command_line(argc, argv, desc), vm);

    if (vm.count("help")) {
      std::cout << desc << std::endl;
      exit(0);
    }
    if (vm.count("config")) {
      std::ifstream stream(vm["config"].as<std::string>());
      if (!stream)
        throw std::runtime_error("Cannot read configuration file");
      po::store(po::parse_config_file(stream, settings), vm);
    }
    po::notify(vm);
    if (deviceData.discoveryTimeout < 1 || deviceData.discoveryTimeout > 30)
      throw std::runtime_error("--discovery-timeout must be between 1 and 30");
    if (deviceData.volume < 0)
      throw std::runtime_error("--volume must be a nonnegative number of Yamaha units");
    if (deviceData.inputName.empty() || deviceData.inputName.find_first_not_of(
          "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_") != std::string::npos)
      throw std::runtime_error("Invalid MusicCast input name");

    return deviceData;
  } catch (std::exception &e) {
    std::cerr << "Error: " << e.what() << std::endl;
  } catch (...) {
    std::cerr << "Unknown error!" << std::endl;
  }
  exit(-1);

  return {};
}

class DevicePowerStatus {
  bool isOn;
  MusicCast &musicCast;
  const DeviceData &deviceData;

public:
  DevicePowerStatus(MusicCast &musicCast, const DeviceData &deviceData)
      : isOn(false), musicCast(musicCast), deviceData(deviceData) {}

  void update(bool newIsOn) {
    if (!musicCast.isOnline())
      return;
    if (isOn == newIsOn) {
      return;
    }
    if (newIsOn) {
      musicCast.setPower("on");
      musicCast.setInput(deviceData.inputName);
      // Make sure automatic volume is set by device before we try
      sleep(3);
      musicCast.setVolume(deviceData.volume);
    } else {
      // Make sure we don't turn off the device if the TV input
      // is not the active one
      auto status = musicCast.getMainZoneStatus();
      if (!status || status->input == deviceData.inputName) {
        musicCast.setPower("standby");
      } else {
        std::cout << "Not sending updates to MusicCast device as TV is not the "
                     "active input"
                  << std::endl;
      }
    }
    if (musicCast.isOnline())
      isOn = newIsOn;
  }
};

struct CommandReceviedCallback {
  DevicePowerStatus *dps;
  MusicCast *musicCast;
  CEC::ICECAdapter *adapter;

  void commandReceived(const CEC::cec_command *command) {
    // std::cout << "Received command opcode=0x" << std::hex << command->opcode
    //           << std::dec << std::endl;
    // std::cout << "From device: " << command->initiator << " to "
    //           << command->destination << std::endl;
    // if (command->opcode == CEC::CEC_OPCODE_REPORT_POWER_STATUS) {
    //   dps->update(isOn(
    //       static_cast<CEC::cec_power_status>(command->parameters.data[0])));
    // }
    auto parameters = command->parameters;
    switch (command->opcode) {
    case CEC::CEC_OPCODE_SYSTEM_AUDIO_MODE_REQUEST:
      processGetSystemAudioModeRequest(parameters);
      break;
    case CEC::CEC_OPCODE_USER_CONTROL_PRESSED:
      processUserControlPressed(parameters);
      break;
    case CEC::CEC_OPCODE_STANDBY:
      dps->update(false);
      break;
    case CEC::CEC_OPCODE_GIVE_AUDIO_STATUS:
      std::cout << "Requested to report audio status" << std::endl;
      transmitReportAudioStatus();
      break;
    default:
      break;
    }
  }

private:
  void processGetSystemAudioModeRequest(const CEC::cec_datapacket &parameters) {
    std::cout << "Got system audio mode request - turning on MusicCast"
              << std::endl;
    bool on = parameters.size != 0;
    dps->update(on);
    CEC::cec_command command;
    CEC::cec_command::Format(command, CEC::CECDEVICE_AUDIOSYSTEM,
                             CEC::CECDEVICE_TV,
                             CEC::CEC_OPCODE_SET_SYSTEM_AUDIO_MODE);
    command.parameters.data[0] = !!on;
    command.parameters.size = 1;

    if (!adapter->Transmit(command)) {
      std::cerr << "Failed to transmit the command" << std::endl;
    }
  }

  void processUserControlPressed(const CEC::cec_datapacket &parameters) {
    if (parameters.size == 0)
      return;
    const uint8_t keycode = parameters.data[0];
    switch (keycode) {
    case CEC::CEC_USER_CONTROL_CODE_VOLUME_DOWN:
      musicCast->volumeDown();
      break;
    case CEC::CEC_USER_CONTROL_CODE_VOLUME_UP:
      musicCast->volumeUp();
      break;
    }
  }

  void transmitReportAudioStatus() {
    auto zoneStatus = musicCast->getMainZoneStatus();
    if (!zoneStatus) {
      std::cerr << "Error while retrieving MusicCast status" << std::endl;
      return;
    }

    CEC::cec_command command;
    CEC::cec_command::Format(command, CEC::CECDEVICE_AUDIOSYSTEM,
                             CEC::CECDEVICE_TV,
                             CEC::CEC_OPCODE_REPORT_AUDIO_STATUS);
    int volume = (100 * zoneStatus->volume / zoneStatus->max_volume) & 0x7f;
    command.parameters.data[0] =
        (zoneStatus->mute ? 0x80 : 0x00) | (uint8_t)volume;
    command.parameters.size = 1;

    if (!adapter->Transmit(command)) {
      std::cerr << "Failed to transmit the command" << std::endl;
    }
  }
};

int main(int argc, char *argv[]) {
  if (signal(SIGINT, sighandler) == SIG_ERR || signal(SIGTERM, sighandler) == SIG_ERR) {
    std::cerr << "Can't register sighandler" << std::endl;
    return -1;
  }

  DeviceData deviceData = parseCmdlineOpts(argc, argv);
  if (curl_global_init(CURL_GLOBAL_DEFAULT) != CURLE_OK)
    return 1;
  struct CurlCleanup { ~CurlCleanup() { curl_global_cleanup(); } } curlCleanup;
  musiccast::Device selected;
  try {
    if (deviceData.discoverOnly || deviceData.address == "auto") {
      const auto devices = musiccast::discover(deviceData.discoveryTimeout,
                                               deviceData.interfaceAddress);
      if (deviceData.discoverOnly) {
        auto output = json::array();
        for (const auto &device : devices)
          output.push_back({{"address", device.address}, {"device_id", device.id},
                            {"model_name", device.model}});
        std::cout << output.dump(2) << std::endl;
        return devices.empty() ? 1 : 0;
      }
      selected = musiccast::selectDevice(devices, deviceData.deviceId);
    } else {
      auto device = musiccast::probe(deviceData.address);
      if (!device)
        throw std::runtime_error("No MusicCast API at " + deviceData.address);
      selected = musiccast::selectDevice({*device}, deviceData.deviceId);
    }
  } catch (const std::exception &e) {
    std::cerr << "Error: " << e.what() << std::endl;
    return 1;
  }
  if (stopping)
    return 0;
  std::cout << "MusicCast: " << selected.model << " (" << selected.id << ") at "
            << selected.address << ", input=" << deviceData.inputName
            << ", volume=" << deviceData.volume << std::endl;
  MusicCast musicCast(selected);
  DevicePowerStatus dps(musicCast, deviceData);

  CEC::libcec_configuration config;
  config.bActivateSource = 0;
  config.clientVersion = CEC::LIBCEC_VERSION_CURRENT;
  config.deviceTypes.Add(CEC::CEC_DEVICE_TYPE_AUDIO_SYSTEM);
  CEC::ICECCallbacks callbacks;
  callbacks.commandReceived = [](void *cbparam,
                                 const CEC::cec_command *command) {
    static_cast<CommandReceviedCallback *>(cbparam)->commandReceived(command);
  };

  CEC::ICECAdapter *adapter = CECInitialise(&config);
  if (adapter == nullptr) {
    std::cerr << "Error initialising CEC" << std::endl;
    return -1;
  }

  CommandReceviedCallback callback = {
      .dps = &dps, .musicCast = &musicCast, .adapter = adapter};

  if (!adapter->SetCallbacks(&callbacks, &callback)) {
    std::cerr << "Failed to enable the callbacks" << std::endl;
    CECDestroy(adapter);
    return 1;
  }

  int result = 0;
  try {
    std::vector<CEC::cec_adapter_descriptor> deviceList(5);
    auto size = adapter->DetectAdapters(deviceList.data(), 5);
    if (size <= 0) {
      throw std::runtime_error("Failed to find adapters");
    }
    deviceList.resize(size);

    std::cout << "Opening " << deviceList[0].strComName << " path "
              << deviceList[0].strComPath << std::endl;

    bool rc = adapter->Open(deviceList[0].strComName);
    if (!rc) {
      throw std::runtime_error("Error connecting to adapter");
    }

    std::cout << "Connected successfully." << std::endl;
    auto nextCheck = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    while (!stopping) {
      sleep(1);
      if (!stopping && std::chrono::steady_clock::now() >= nextCheck) {
        musicCast.checkConnection(deviceData);
        nextCheck = std::chrono::steady_clock::now() +
                    std::chrono::seconds(musicCast.isOnline() ? 30 : 10);
      }
    }
  } catch (const std::exception &e) {
    std::cerr << "Error: " << e.what() << std::endl;
    result = 1;
  }

  adapter->Close();
  CECDestroy(adapter);

  return result;
}
