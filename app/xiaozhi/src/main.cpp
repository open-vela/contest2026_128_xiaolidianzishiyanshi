#include <nuttx/config.h>

#include <cstdio>
#include <utility>

#include "xiaozhi/application.h"

extern "C" int main(int argc, char *argv[]) {
  xiaozhi::ApplicationConfig config;
  config.ota_url = CONFIG_CONTEST2026_128_XIAOZHI_OTA_URL;
  config.websocket_url = CONFIG_CONTEST2026_128_XIAOZHI_WEBSOCKET_URL;
  config.token = CONFIG_CONTEST2026_128_XIAOZHI_TOKEN;
  config.capture_path = CONFIG_CONTEST2026_128_XIAOZHI_CAPTURE_DEVPATH;
  config.playback_path = CONFIG_CONTEST2026_128_XIAOZHI_PLAYBACK_DEVPATH;
  config.client_id_path = CONFIG_CONTEST2026_128_XIAOZHI_CLIENT_ID_PATH;
  config.framebuffer_path = CONFIG_CONTEST2026_128_XIAOZHI_FB_DEVPATH;
  config.input_path = CONFIG_CONTEST2026_128_XIAOZHI_INPUT_DEVPATH;
  config.protocol_version = CONFIG_CONTEST2026_128_XIAOZHI_PROTOCOL_VERSION;
  config.language = CONFIG_CONTEST2026_128_XIAOZHI_LANGUAGE;
  config.board_type = CONFIG_CONTEST2026_128_XIAOZHI_BOARD_TYPE;
  config.board_name = CONFIG_CONTEST2026_128_XIAOZHI_BOARD_NAME;
  config.board_manufacturer = CONFIG_CONTEST2026_128_XIAOZHI_BOARD_MANUFACTURER;
  config.application_version = CONFIG_CONTEST2026_128_XIAOZHI_APP_VERSION;

  if (argc > 1) {
    config.websocket_url = argv[1];
    config.provision_from_server = false;
  }
  if (argc > 2) {
    config.token = argv[2];
  }

  std::printf("xiaozhi: OpenVela/NuttX client starting\n");
  xiaozhi::Application application(std::move(config));
  return application.Run();
}
