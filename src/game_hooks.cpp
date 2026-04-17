#include "game_hooks.h"

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
