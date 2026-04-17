#include "registration.h"

#include "packet_io.h"
#include "state.h"
#include "ws_registry.h"

json BuildRegistrationPayload() {
    return {{"pid", pid}, {"port", clientPort}, {"name", charName}};
}

void ReplayCharDataToBeryl(struct mg_connection* c) {
    for (auto it = storedPackets.begin(); it != storedPackets.end(); ++it) {
        SendPacketToBeryl(c, it->second);
    }

    if (!storedStats.empty()) SendPacketToBeryl(c, storedStats);

    for (auto& [slot, pkt] : storedSpells)    SendPacketToBeryl(c, pkt);
    for (auto& [slot, pkt] : storedItems)     SendPacketToBeryl(c, pkt);
    for (auto& [slot, pkt] : storedSkills)    SendPacketToBeryl(c, pkt);
    for (auto& [slot, pkt] : storedEquipment) SendPacketToBeryl(c, pkt);

    // Replay stored entities as a single 0x07 packet
    if (!storedEntities.empty()) {
        std::string pkt;
        pkt += (char)0x07;
        WriteBE16(pkt, (uint16_t)storedEntities.size());
        for (auto it = storedEntities.begin(); it != storedEntities.end(); ++it) {
            pkt += it->second;
        }
        SendPacketToBeryl(c, pkt);
    }

    for (auto it = storedShowUsers.begin(); it != storedShowUsers.end(); ++it) {
        SendPacketToBeryl(c, it->second);
    }
}

void Deregister() {
    if (!charRegistered) return;
    if (isRegistry) {
        RegistryRemoveClient(pid);
    } else if (registryClientConnected && g_registryClientConn) {
        json msg = {{"type", "deregister"}, {"pid", pid}};
        std::string s = msg.dump();
        mg_ws_send(g_registryClientConn, s.c_str(), s.size(), WEBSOCKET_OP_TEXT);
    }
    charRegistered = false;
}

void TryRegister() {
    if (charRegistered || charName.empty()) return;
    charRegistered = true;

    json payload = BuildRegistrationPayload();

    if (isRegistry) {
        RegistryAddClient(payload);
    } else if (registryClientConnected && g_registryClientConn) {
        json msg = payload;
        msg["type"] = "register";
        std::string s = msg.dump();
        mg_ws_send(g_registryClientConn, s.c_str(), s.size(), WEBSOCKET_OP_TEXT);
    } else if (g_clientConn) {
        ReplayCharDataToBeryl(g_clientConn);
    }
}
