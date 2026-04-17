#include "state.h"

// Logging
static std::mutex logMutex;
static HANDLE g_logFile = INVALID_HANDLE_VALUE;

void LogToFile(const std::string &message) {
  std::lock_guard<std::mutex> lock(logMutex);
  if (g_logFile == INVALID_HANDLE_VALUE) {
    g_logFile = CreateFileA(
        "C:\\Users\\Public\\hook_log.txt",
        FILE_APPEND_DATA,
        FILE_SHARE_READ | FILE_SHARE_WRITE,
        NULL,
        OPEN_ALWAYS,
        FILE_ATTRIBUTE_NORMAL,
        NULL
    );
  }
  if (g_logFile != INVALID_HANDLE_VALUE) {
    DWORD written;
    WriteFile(
        g_logFile, message.c_str(), (DWORD)message.size(), &written, NULL
    );
    WriteFile(g_logFile, "\r\n", 2, &written, NULL);
  }
}

void CloseLogFile() {
  std::lock_guard<std::mutex> lock(logMutex);
  if (g_logFile != INVALID_HANDLE_VALUE) {
    CloseHandle(g_logFile);
    g_logFile = INVALID_HANDLE_VALUE;
  }
}

std::string GetDllDirectory(HINSTANCE hInst) {
  char path[MAX_PATH];
  GetModuleFileNameA(hInst, path, MAX_PATH);
  std::string dir(path);
  size_t pos = dir.find_last_of("\\/");
  if (pos != std::string::npos) {
    dir = dir.substr(0, pos);
  }
  return dir;
}

// DLL lifecycle
DWORD pid = 0;
volatile bool running = true;
std::string g_dllDirectory;

// Mongoose
struct mg_mgr g_mgr;
struct mg_connection *g_clientListener = nullptr;
struct mg_connection *g_clientConn = nullptr;
struct mg_connection *g_registryListener = nullptr;
struct mg_connection *g_registryClientConn = nullptr;
unsigned long g_wakeupId = 0;
bool isRegistry = false;
bool registryClientConnected = false;
int clientPort = 0;

// Character data
std::mutex charDataMutex;
std::string charName;
bool charRegistered = false;
uint32_t charId = 0;
std::string storedStats;
std::map<BYTE, std::string> storedPackets;
std::map<int, std::string> storedSpells;
std::map<int, std::string> storedItems;
std::map<int, std::string> storedSkills;
std::map<int, std::string> storedEquipment;
std::map<uint32_t, std::string> storedEntities;
std::map<uint32_t, std::string> storedShowUsers;
std::string g_currentDialog;
std::string g_suspendedDialog;

// Registry
std::vector<json> registeredClients;
std::map<struct mg_connection *, DWORD> registryConnPid;
