#include "game_hooks.h"

#include <deque>
#include <mutex>

#include "auth_dialog.h"
#include "packet_io.h"
#include "packet_parse.h"
#include "state.h"

namespace {
constexpr DWORD FUNC_SEND_ADDR = 0x00563E00;
constexpr DWORD FUNC_RECV_ADDR = 0x00467060;
constexpr DWORD FUNC_WALK_ADDR = 0x005F0C40;
} // namespace

GameSendFn OrigGameSend = (GameSendFn)FUNC_SEND_ADDR;
GameRecvFn OrigGameRecv = (GameRecvFn)FUNC_RECV_ADDR;
GameWalkFn GameWalk = (GameWalkFn)FUNC_WALK_ADDR;

void *g_sendThis = nullptr;
void *g_recvThis = nullptr;

CreateMutexAFn RealCreateMutexA = CreateMutexA;

BYTE *ResolvePointerChain(const BYTE *offsets, int chainLength) {
  DWORD moduleBase = (DWORD)GetModuleHandle(NULL);
  if (!moduleBase)
    return nullptr;

  DWORD addr = moduleBase + *(DWORD *)(offsets);

  for (int i = 1; i < chainLength; i++) {
    DWORD *ptr = (DWORD *)addr;
    if (IsBadReadPtr(ptr, sizeof(DWORD)))
      return nullptr;
    addr = *ptr + *(DWORD *)(offsets + i * 4);
  }

  return (BYTE *)addr;
}

void __fastcall
HookedGameSend(void *thisPtr, void * /*edx*/, const BYTE *data, DWORD size) {
  g_sendThis = thisPtr;
  if (ShouldSuppressClientPacket(data, size))
    return;
  SendToBeryl(MSG_CLIENT, data, size);
  OrigGameSend(thisPtr, data, size);
}

void __fastcall
HookedGameRecv(void *thisPtr, void * /*edx*/, const BYTE *data, DWORD size) {
  g_recvThis = thisPtr;
  ParseServerPacket(data, size);
  SendToBeryl(MSG_SERVER, data, size);
  OrigGameRecv(thisPtr, data, size);
}

namespace {
bool mutexCheckBypassed = false;
}

HANDLE WINAPI HookedCreateMutexA(
    LPSECURITY_ATTRIBUTES lpAttr, BOOL bInitialOwner, LPCSTR lpName
) {
  if (lpName && !mutexCheckBypassed) {
    mutexCheckBypassed = true;
    HANDLE h = RealCreateMutexA(lpAttr, bInitialOwner, NULL);
    SetLastError(0);
    return h;
  }
  return RealCreateMutexA(lpAttr, bInitialOwner, lpName);
}

namespace {
std::mutex g_walkQueueMutex;
std::deque<int> g_walkQueue;
// Caps backlog from a misbehaving client so DrainPendingWalks can't stall the
// game thread; the JS side already paces walks, so 16 is plenty of headroom.
constexpr size_t WALK_QUEUE_MAX = 16;
} // namespace

void EnqueueWalk(int direction) {
  std::lock_guard<std::mutex> lock(g_walkQueueMutex);
  if (g_walkQueue.size() >= WALK_QUEUE_MAX)
    return;
  g_walkQueue.push_back(direction);
}

void DrainPendingWalks() {
  while (true) {
    int direction;
    {
      std::lock_guard<std::mutex> lock(g_walkQueueMutex);
      if (g_walkQueue.empty())
        return;
      direction = g_walkQueue.front();
      g_walkQueue.pop_front();
    }
    void *thisPtr = *(void **)OBJECT_BASE_ADDR;
    if (thisPtr)
      GameWalk(thisPtr, direction);
  }
}

// Window-focus detection. EVENT_SYSTEM_FOREGROUND fires on any desktop-wide
// foreground change; WINEVENT_OUTOFCONTEXT callbacks are delivered through the
// installing thread's message pump, so the hook is installed lazily on the
// game thread (from the PeekMessage hook) and the callback runs there too.
namespace {
HWINEVENTHOOK g_focusHook = nullptr;

int CurrentFocus() {
  HWND fg = GetForegroundWindow();
  DWORD fgPid = 0;
  if (fg)
    GetWindowThreadProcessId(fg, &fgPid);
  return (fgPid == pid) ? 1 : 0;
}

void CALLBACK
FocusEventProc(HWINEVENTHOOK, DWORD event, HWND, LONG, LONG, DWORD, DWORD) {
  if (event != EVENT_SYSTEM_FOREGROUND)
    return;
  int focused = CurrentFocus();
  if (focused == g_lastFocus)
    return;
  g_lastFocus = focused;
  BYTE b = (BYTE)focused;
  SendToBeryl(MSG_WINDOW_FOCUS, &b, 1);
}
} // namespace

void InstallFocusHook() {
  if (g_focusHook)
    return;
  g_focusHook = SetWinEventHook(
      EVENT_SYSTEM_FOREGROUND,
      EVENT_SYSTEM_FOREGROUND,
      NULL,
      FocusEventProc,
      0,
      0,
      WINEVENT_OUTOFCONTEXT
  );
}

void RemoveFocusHook() {
  if (g_focusHook) {
    UnhookWinEvent(g_focusHook);
    g_focusHook = nullptr;
  }
}

void SendCurrentFocus(struct mg_connection *c) {
  g_lastFocus = CurrentFocus();
  BYTE frame[2] = {MSG_WINDOW_FOCUS, (BYTE)g_lastFocus};
  mg_ws_send(c, (const char *)frame, sizeof(frame), WEBSOCKET_OP_BINARY);
}

PeekMessageFn RealPeekMessageA = PeekMessageA;
PeekMessageFn RealPeekMessageW = PeekMessageW;

BOOL WINAPI
HookedPeekMessageA(LPMSG lpMsg, HWND hWnd, UINT min, UINT max, UINT remove) {
  InstallFocusHook();
  DrainPendingWalks();
  return RealPeekMessageA(lpMsg, hWnd, min, max, remove);
}

BOOL WINAPI
HookedPeekMessageW(LPMSG lpMsg, HWND hWnd, UINT min, UINT max, UINT remove) {
  InstallFocusHook();
  DrainPendingWalks();
  return RealPeekMessageW(lpMsg, hWnd, min, max, remove);
}
