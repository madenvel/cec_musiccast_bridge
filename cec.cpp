#include <iostream>
#include <libcec/cec.h>
#include <optional>
#include <signal.h>
#include <sstream>
#include <unistd.h>
#include <vector>

#include <curlpp/Easy.hpp>
#include <curlpp/Options.hpp>
#include <curlpp/cURLpp.hpp>

#include <nlohmann/json.hpp>

#include <boost/program_options.hpp>

void sighandler(int signal) {
  std::cerr << "Caught SIGINT - interrupting" << std::endl;
}

struct DeviceData {
  std::string inputName;
  int volume;
  std::string address;
};

using json = nlohmann::json;

class MusicCast {

  json get(const std::string &url) const {
    try {
      curlpp::Cleanup myCleanup;
      curlpp::Easy myRequest;
      myRequest.setOpt<curlpp::options::Url>(baseUrl + url);
      std::ostringstream os;
      os << myRequest;
      return json::parse(os.str());
    } catch (curlpp::RuntimeError &e) {
      std::cerr << "Failed to perform MusicCast request: " << e.what()
                << std::endl;
    } catch (curlpp::LogicError &e) {
      std::cerr << "Failed to perform MusicCast request: " << e.what()
                << std::endl;
    }
    return json{};
  }

  const std::string baseUrl;

  int currentVolume;
  int maxVolume;

public:
  struct ZoneStatus {
    std::string power;
    int sleep;
    int volume;
    int max_volume;
    bool mute;
    std::string input;
  };

  MusicCast(const std::string &address)
      : baseUrl("http://" + address + "/YamahaExtendedControl/v1/") {}

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
    auto data = get("main/getStatus");
    if (data["response_code"] != 0) {
      return std::nullopt;
    }
    return ZoneStatus{.power = data["power"],
                      .sleep = data["sleep"],
                      .volume = data["volume"],
                      .max_volume = data["max_volume"],
                      .mute = data["mute"],
                      .input = data["input"]};
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
    po::options_description desc(
        "This tool make Yamaha MusicCast device respond to TV commands"
        "inluding power on / off and volume control");
    desc.add_options()("help,h", "Show this information")(
        "input,i", po::value(&deviceData.inputName)->required(),
        "MusicCast input name to set when power on")(
        "address,a", po::value(&deviceData.address)->required(),
        "The address of MusicCast device")(
        "volume,v", po::value(&deviceData.volume)->default_value(90));

    po::variables_map vm;
    po::store(po::parse_command_line(argc, argv, desc), vm);

    if (vm.count("help")) {
      std::cout << desc << std::endl;
      exit(1);
    }
    po::notify(vm);
    std::cout << "DeviceData: input=" << deviceData.inputName
              << ", address=" << deviceData.address
              << ", volume=" << deviceData.volume << std::endl;

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
    }
  }

private:
  void processGetSystemAudioModeRequest(const CEC::cec_datapacket &parameters) {
    std::cout << "Got system audio mode request - turning on MusicCast"
              << std::endl;
    uint16_t physAddr =
        ((uint16_t)parameters.data[1] << 8) + parameters.data[0];
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
    }

    CEC::cec_command command;
    CEC::cec_command::Format(command, CEC::CECDEVICE_AUDIOSYSTEM,
                             CEC::CECDEVICE_TV,
                             CEC::CEC_OPCODE_REPORT_AUDIO_STATUS);
    int volume = (100 * zoneStatus->volume / zoneStatus->max_volume) & 0x7f;
    command.parameters.data[0] =
        (zoneStatus->mute ? 0xf0 : 0x00) | (uint8_t)volume;
    command.parameters.size = 1;

    if (!adapter->Transmit(command)) {
      std::cerr << "Failed to transmit the command" << std::endl;
    }
  }
};

int main(int argc, char *argv[]) {
  if (signal(SIGINT, sighandler) == SIG_ERR) {
    std::cerr << "Can't register sighandler" << std::endl;
    return -1;
  }

  DeviceData deviceData = parseCmdlineOpts(argc, argv);
  MusicCast musicCast(deviceData.address);
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
    exit(-1);
  }

  try {
    std::vector<CEC::cec_adapter_descriptor> deviceList(5);
    auto size = adapter->DetectAdapters(deviceList.data(), 5);
    deviceList.resize(size);

    if (deviceList.empty()) {
      throw std::runtime_error("Failed to find adapters");
    }

    std::cout << "Opening " << deviceList[0].strComName << " path "
              << deviceList[0].strComPath << std::endl;

    bool rc = adapter->Open(deviceList[0].strComName);
    if (!rc) {
      throw std::runtime_error("Error connecting to adapter");
    }

    std::cout << "Connected successfully." << std::endl;
    pause();
  } catch (const std::exception &e) {
    std::cerr << "Error: " << e.what() << std::endl;
  }

  adapter->Close();
  CECDestroy(adapter);

  return 0;
}
