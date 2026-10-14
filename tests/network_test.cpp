#include "musiccast_network.h"
#include <curl/curl.h>
#include <iostream>
#include <stdexcept>

void require(bool condition, const std::string &message) {
  if (!condition)
    throw std::runtime_error(message);
}

int main(int argc, char **argv) {
  curl_global_init(CURL_GLOBAL_DEFAULT);
  int result = 0;
  try {
    if (argc == 3) {
      if (std::string(argv[2]) == "probe") {
        auto device = musiccast::probe(argv[1]);
        if (!device)
          return 1;
        std::cout << device->id << std::endl;
      } else {
        std::cout << musiccast::requestJson(argv[1], argv[2]).dump() << std::endl;
      }
    } else {
      using namespace musiccast;
      const std::string response = "HTTP/1.1 200 OK\r\nLocation: http://host/desc.xml\r\n"
          "sT: urn:schemas-upnp-org:device:MediaRenderer:1\r\n\r\n";
      require(isRendererResponse(response), "mixed-case SSDP headers");
      require(!isRendererResponse("NOTIFY * HTTP/1.1\r\n"), "ignore advertisements");
      require(!isRendererResponse("HTTP/1.1 404 Missing\r\n" + response.substr(17)), "ignore errors");
      require(!isRendererResponse("HTTP/1.1 200 OK\r\n\r\nST: urn:schemas-upnp-org:device:MediaRenderer:1"), "ignore body headers");
      require(!isRendererResponse("HTTP/1.1 200 OK\r\nST: ssdp:all\r\n\r\n"), "ignore other targets");
      const nlohmann::json valid{{"response_code", 0}, {"device_id", "ABC123"},
                                  {"model_name", "R-N602"}};
      require(parseDevice("192.0.2.1", valid).has_value(), "accept MusicCast API");
      for (const auto &invalid : std::vector<nlohmann::json>{
               nullptr, nlohmann::json::array(), {{"response_code", 0}},
               {{"response_code", 1}, {"device_id", "A"}, {"model_name", "B"}},
               {{"response_code", 0}, {"device_id", 123}, {"model_name", "B"}},
               {{"response_code", 0}, {"device_id", ""}, {"model_name", "B"}}})
        require(!parseDevice("192.0.2.1", invalid), "reject non-MusicCast or malformed API");
      const Device first{"192.0.2.1", "ABC123", "R-N602"};
      const Device second{"192.0.2.2", "DEF456", "R-N803"};
      require(selectDevice({first}).address == first.address, "select single device");
      require(selectDevice({first, second}, "def456").address == second.address,
              "select ID without case sensitivity");
      for (const auto &devices : std::vector<std::vector<Device>>{{}, {first, second}}) {
        bool threw = false;
        try { selectDevice(devices); } catch (const std::runtime_error &) { threw = true; }
        require(threw, "reject absent or ambiguous devices");
      }
      bool threw = false;
      try { selectDevice({first}, "missing"); } catch (const std::runtime_error &) { threw = true; }
      require(threw, "never fall back to a different device ID");
      std::cout << "Network unit checks passed" << std::endl;
    }
  } catch (const std::exception &error) {
    std::cerr << error.what() << std::endl;
    result = 1;
  }
  curl_global_cleanup();
  return result;
}
