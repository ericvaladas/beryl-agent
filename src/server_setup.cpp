#include "server_setup.h"

#include <string>

#include "state.h"
#include "ws_client.h"
#include "ws_registry.h"

int StartClientServer() {
  for (int port = CLIENT_PORT_START; port <= CLIENT_PORT_END; port++) {
    std::string url = "http://127.0.0.1:" + std::to_string(port);
    struct mg_connection *c =
        mg_http_listen(&g_mgr, url.c_str(), ClientHandler, NULL);
    if (c) {
      g_clientListener = c;
      return port;
    }
  }
  return 0;
}

bool StartRegistryServer() {
  std::string url = "http://127.0.0.1:" + std::to_string(REGISTRY_PORT);
  struct mg_connection *c =
      mg_http_listen(&g_mgr, url.c_str(), RegistryHandler, NULL);
  if (c) {
    g_registryListener = c;
    return true;
  }
  return false;
}

void ConnectToRegistry() {
  std::string url = "ws://127.0.0.1:" + std::to_string(REGISTRY_PORT);
  struct mg_connection *c =
      mg_ws_connect(&g_mgr, url.c_str(), RegistryClientHandler, NULL, NULL);
  if (c) {
    g_registryClientConn = c;
  }
}
