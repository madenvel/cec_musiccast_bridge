// Exercise the bridge's receiver state without initialising a CEC adapter.
#define main cec_bridge_entrypoint
#include "../cec.cpp"
#undef main

int main(int argc, char **argv) {
  if (argc != 3)
    return 2;
  curl_global_init(CURL_GLOBAL_DEFAULT);
  MusicCast receiver({argv[1], "ABC123", "R-N602"});
  int result = 0;
  if (std::string(argv[2]) == "status") {
    result = receiver.getMainZoneStatus() ? 0 : 1;
  } else {
    // The fake server first fails a request, then offers a different receiver
    // at the old IP, and finally returns the original device. Do not send
    // commands while offline, replay the failed command or change identity.
    DeviceData options;
    options.address = argv[1];
    if (receiver.getMainZoneStatus() || receiver.isOnline())
      result = 1;
    receiver.checkConnection(options);
    if (receiver.isOnline())
      result = 1;
    receiver.volumeUp();
    receiver.checkConnection(options);
    if (!receiver.isOnline())
      result = 1;
    receiver.volumeUp();
  }
  curl_global_cleanup();
  return result;
}
