#include <windows.h>

#include <detours.h>
#include <mutex>
#include <string>
#include <thread>

#include "auth_dialog.h"
#include "game_hooks.h"
#include "mongoose.h"
#include "registration.h"
#include "server_setup.h"
#include "state.h"
#include "ws_client.h"
#include "ws_registry.h"

// wininet proxy (wininet_proxy.cpp)
extern void LoadRealWininet();
extern void UnloadRealWininet();

namespace {
HANDLE hMutex = NULL;
std::thread *wsThread = nullptr;

void WebSocketThread() {
  mg_mgr_init(&g_mgr);
  mg_wakeup_init(&g_mgr);

  clientPort = StartClientServer();
  if (g_clientListener) {
    g_wakeupId = g_clientListener->id;
  }
  if (clientPort == 0) {
    LogToFile("Failed to start client WS server on any port");
    mg_mgr_free(&g_mgr);
    return;
  }

  // Try to become the registry; otherwise connect to the existing one.
  isRegistry = StartRegistryServer();
  if (!isRegistry) {
    ConnectToRegistry();
  }

  // If we were injected while already logged in, register now.
  {
    std::lock_guard<std::mutex> lock(charDataMutex);
    if (!storedStats.empty()) {
      TryRegister();
    }
  }

  while (running) {
    mg_mgr_poll(&g_mgr, 50);

    // Process next queued file when current one has drained
    if (g_fileSendConn != nullptr && g_fileSendConn->send.len == 0) {
      ProcessNextFile();
    }

    // Registry reconnect
    if (g_registryReconnectTime > 0 && !isRegistry &&
        !registryClientConnected && running) {
      if (mg_millis() >= g_registryReconnectTime) {
        g_registryReconnectTime = 0;
        ConnectToRegistry();
      }
    }
  }

  mg_mgr_free(&g_mgr);
}

void AttachHooks() {
  DetourTransactionBegin();
  DetourUpdateThread(GetCurrentThread());
  DetourAttach(
      reinterpret_cast<PVOID *>(&RealCreateMutexA),
      reinterpret_cast<PVOID>(HookedCreateMutexA)
  );
  DetourAttach(
      reinterpret_cast<PVOID *>(&OrigGameSend),
      reinterpret_cast<PVOID>(HookedGameSend)
  );
  DetourAttach(
      reinterpret_cast<PVOID *>(&OrigGameRecv),
      reinterpret_cast<PVOID>(HookedGameRecv)
  );
  DetourTransactionCommit();
}

void DetachHooks() {
  DetourTransactionBegin();
  DetourUpdateThread(GetCurrentThread());
  DetourDetach(
      reinterpret_cast<PVOID *>(&RealCreateMutexA),
      reinterpret_cast<PVOID>(HookedCreateMutexA)
  );
  DetourDetach(
      reinterpret_cast<PVOID *>(&OrigGameSend),
      reinterpret_cast<PVOID>(HookedGameSend)
  );
  DetourDetach(
      reinterpret_cast<PVOID *>(&OrigGameRecv),
      reinterpret_cast<PVOID>(HookedGameRecv)
  );
  DetourTransactionCommit();
}
} // namespace

BOOL APIENTRY DllMain(HINSTANCE hInst, DWORD reason, LPVOID) {
  if (reason == DLL_PROCESS_ATTACH) {
    LoadRealWininet();
    g_dllDirectory = GetDllDirectory(hInst);

    std::string appdata = std::string(getenv("LOCALAPPDATA")) + "\\beryl";
    CreateDirectoryA(appdata.c_str(), NULL);
    g_allowedOriginsPath = appdata + "\\settings.json";
    LoadAllowedOrigins();
    pid = GetCurrentProcessId();

    std::string mutexStr = "ProxyInjected_" + std::to_string(pid);
    const char *mutexName = mutexStr.c_str();
    HANDLE existing = OpenMutexA(MUTEX_ALL_ACCESS, FALSE, mutexName);
    if (existing) {
      CloseHandle(existing);
      return FALSE;
    }
    hMutex = CreateMutexA(NULL, FALSE, mutexName);

    running = true;

    DisableThreadLibraryCalls(hInst);

    AttachHooks();

    wsThread = new std::thread(WebSocketThread);
  } else if (reason == DLL_PROCESS_DETACH) {
    running = false;

    Deregister();

    if (wsThread && wsThread->joinable()) {
      wsThread->join();
      delete wsThread;
      wsThread = nullptr;
    }

    registeredClients.clear();
    CloseLogFile();
    if (hMutex)
      CloseHandle(hMutex);

    DetachHooks();

    UnloadRealWininet();
  }

  return TRUE;
}
