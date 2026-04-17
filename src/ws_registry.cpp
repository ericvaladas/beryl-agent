#include "ws_registry.h"

#include <algorithm>
#include <fstream>
#include <mutex>

#include "auth_dialog.h"
#include "registration.h"
#include "state.h"
#include "ws_client.h"

uint64_t g_registryReconnectTime = 0;

void RegistryBroadcast(const std::string& message) {
    if (!g_registryListener) return;
    for (struct mg_connection* c = g_mgr.conns; c != NULL; c = c->next) {
        if (c->is_websocket && c->fn == (mg_event_handler_t)RegistryHandler) {
            mg_ws_send(c, message.c_str(), message.size(), WEBSOCKET_OP_TEXT);
        }
    }
}

void RegistrySendClientList(struct mg_connection* c) {
    json msg = {{"type", "init"}, {"clients", registeredClients}};
    std::string s = msg.dump();
    mg_ws_send(c, s.c_str(), s.size(), WEBSOCKET_OP_TEXT);
}

void RegistryAddClient(const json& clientData) {
    registeredClients.push_back(clientData);

    json msg = {{"type", "add"}, {"client", clientData}};
    RegistryBroadcast(msg.dump());
}

void RegistryRemoveClient(DWORD clientPid) {
    registeredClients.erase(
        std::remove_if(registeredClients.begin(), registeredClients.end(),
            [clientPid](const json& c) { return c["pid"] == clientPid; }),
        registeredClients.end());

    json msg = {{"type", "remove"}, {"pid", clientPid}};
    RegistryBroadcast(msg.dump());
}

void RegistryHandler(struct mg_connection* c, int ev, void* ev_data) {
    if (ev == MG_EV_HTTP_MSG) {
        mg_ws_upgrade(c, (struct mg_http_message*)ev_data, NULL);
    } else if (ev == MG_EV_WS_OPEN) {
        RegistrySendClientList(c);
    } else if (ev == MG_EV_WS_MSG) {
        struct mg_ws_message* wm = (struct mg_ws_message*)ev_data;

        try {
            json data = json::parse(std::string(wm->data.buf, wm->data.len));
            std::string type = data["type"];

            if (type == "register") {
                json clientData = data;
                clientData.erase("type");
                RegistryAddClient(clientData);
                registryConnPid[c] = (DWORD)clientData["pid"];
            } else if (type == "deregister") {
                DWORD clientPid = (DWORD)data["pid"];
                RegistryRemoveClient(clientPid);
            } else if (type == "file") {
                uint32_t requestId = (uint32_t)data["id"];
                std::string path = data["path"];
                QueueFileRequest(c, requestId, path);
            } else if (type == "getSettings" && g_clientConn) {
                std::ifstream settingsFile(g_allowedOriginsPath);
                std::string content = "{}";
                if (settingsFile.is_open()) {
                    content.assign((std::istreambuf_iterator<char>(settingsFile)),
                                    std::istreambuf_iterator<char>());
                    settingsFile.close();
                }
                json resp = json::parse(content);
                resp["type"] = "settings";
                std::string out = resp.dump();
                mg_ws_send(c, out.c_str(), out.size(), WEBSOCKET_OP_TEXT);
            } else if (type == "setSettings" && g_clientConn) {
                const json& origins = data["allowedOrigins"];
                if (origins.type() == json::t_array) {
                    g_allowedOrigins.clear();
                    for (const auto& item : origins) {
                        g_allowedOrigins.insert(item.str());
                    }
                }

                json settings = data;
                settings.erase("type");
                std::ofstream settingsFile(g_allowedOriginsPath);
                if (settingsFile.is_open()) {
                    settingsFile << settings.dump();
                }
            }
        } catch (...) {}
    } else if (ev == MG_EV_CLOSE) {
        auto it = registryConnPid.find(c);
        if (it != registryConnPid.end()) {
            RegistryRemoveClient(it->second);
            registryConnPid.erase(it);
        }
    }
}

void RegistryClientHandler(struct mg_connection* c, int ev, void* /*ev_data*/) {
    if (ev == MG_EV_WS_OPEN) {
        g_registryClientConn = c;
        registryClientConnected = true;

        std::lock_guard<std::mutex> lock(charDataMutex);
        if (!charName.empty()) {
            charRegistered = false;
            TryRegister();
        }
    } else if (ev == MG_EV_CLOSE) {
        if (c == g_registryClientConn) {
            g_registryClientConn = nullptr;
            registryClientConnected = false;
            if (running && !isRegistry) {
                g_registryReconnectTime = mg_millis() + 2000;
            }
        }
    } else if (ev == MG_EV_ERROR) {
        registryClientConnected = false;
        g_registryClientConn = nullptr;
        if (running && !isRegistry) {
            g_registryReconnectTime = mg_millis() + 2000;
        }
    }
}
