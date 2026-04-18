#pragma once

#include <map>
#include <mutex>
#include <set>
#include <string>
#include <vector>
#include <windows.h>

#include "json.h"
#include "mongoose.h"

// Message types (must match Beryl side)
constexpr BYTE MSG_CLIENT = 0x01;
constexpr BYTE MSG_SERVER = 0x02;
constexpr BYTE MSG_WALK = 0x03;
constexpr BYTE MSG_READ_MEMORY = 0x04;
constexpr BYTE MSG_WRITE_MEMORY = 0x05;
constexpr BYTE MSG_BECOME_REG = 0x06;
constexpr BYTE MSG_READY = 0x07;

constexpr int REGISTRY_PORT = 21000;
constexpr int CLIENT_PORT_START = 21001;
constexpr int CLIENT_PORT_END = 21020;

// DLL lifecycle
extern DWORD pid;
extern volatile bool running;
extern std::string g_dllDirectory;

// Mongoose connections / state
extern struct mg_mgr g_mgr;
extern struct mg_connection *g_clientListener;
extern struct mg_connection *g_clientConn;
extern struct mg_connection *g_registryListener;
extern struct mg_connection *g_registryClientConn;
extern unsigned long g_wakeupId;
extern bool isRegistry;
extern bool registryClientConnected;
extern int clientPort;

// Character data (all access guarded by charDataMutex)
extern std::mutex charDataMutex;
extern std::string charName;
extern bool charRegistered;
extern uint32_t charId;
extern std::string storedStats;
extern std::map<BYTE, std::string> storedPackets;
extern std::map<int, std::string> storedSpells;
extern std::map<int, std::string> storedItems;
extern std::map<int, std::string> storedSkills;
extern std::map<int, std::string> storedEquipment;
extern std::map<uint32_t, std::string> storedEntities;
extern std::map<uint32_t, std::string> storedShowUsers;
extern std::string g_currentDialog;
extern std::string g_suspendedDialog;

// Registry bookkeeping
extern std::vector<json> registeredClients;
extern std::map<struct mg_connection *, std::string> registryConnName;

// DLL directory helper
std::string GetDllDirectory(HINSTANCE hInst);
