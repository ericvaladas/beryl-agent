#pragma once

#include <cstdint>
#include <string>

#include "json.h"
#include "mongoose.h"

void RegistryHandler(struct mg_connection* c, int ev, void* ev_data);
void RegistryClientHandler(struct mg_connection* c, int ev, void* ev_data);

void RegistryBroadcast(const std::string& message);
void RegistrySendClientList(struct mg_connection* c);
void RegistryAddClient(const json& clientData);
void RegistryRemoveClient(DWORD clientPid);

// Millis timestamp for next reconnect attempt (0 = no pending reconnect)
extern uint64_t g_registryReconnectTime;
