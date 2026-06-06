#pragma once

#include <windows.h>

// Post-decryption packet function thunks (addresses hardcoded to Dark Ages
// client)
typedef void(__thiscall *GameSendFn)(
    void *thisPtr, const BYTE *data, DWORD size
);
typedef void(__thiscall *GameRecvFn)(
    void *thisPtr, const BYTE *data, DWORD size
);
typedef void(__thiscall *GameWalkFn)(void *thisPtr, int direction);

extern GameSendFn OrigGameSend;
extern GameRecvFn OrigGameRecv;
extern GameWalkFn GameWalk;

// Captured `this` pointers observed at the first hook invocation
extern void *g_sendThis;
extern void *g_recvThis;

constexpr DWORD SEND_THIS_ADDR = 0x0073D958;
constexpr DWORD OBJECT_BASE_ADDR = 0x00882E68;

// Follow moduleBase + offsets[0], dereference and add each subsequent offset.
BYTE *ResolvePointerChain(const BYTE *offsets, int chainLength);

void __fastcall
HookedGameSend(void *thisPtr, void *edx, const BYTE *data, DWORD size);
void __fastcall
HookedGameRecv(void *thisPtr, void *edx, const BYTE *data, DWORD size);

// CreateMutexA hook (neuters single-instance check)
typedef HANDLE(WINAPI *CreateMutexAFn)(LPSECURITY_ATTRIBUTES, BOOL, LPCSTR);
extern CreateMutexAFn RealCreateMutexA;

HANDLE WINAPI HookedCreateMutexA(
    LPSECURITY_ATTRIBUTES lpAttr, BOOL bInitialOwner, LPCSTR lpName
);

// Cross-thread walk dispatch. The mongoose thread calls EnqueueWalk; the
// PeekMessage hook (game thread) calls DrainPendingWalks so GameWalk only ever
// runs on the thread that owns the game state.
void EnqueueWalk(int direction);
void DrainPendingWalks();

typedef BOOL(WINAPI *PeekMessageFn)(LPMSG, HWND, UINT, UINT, UINT);
extern PeekMessageFn RealPeekMessageA;
extern PeekMessageFn RealPeekMessageW;
BOOL WINAPI
HookedPeekMessageA(LPMSG lpMsg, HWND hWnd, UINT min, UINT max, UINT remove);
BOOL WINAPI
HookedPeekMessageW(LPMSG lpMsg, HWND hWnd, UINT min, UINT max, UINT remove);

// Window-focus push. InstallFocusHook is idempotent and must be called from the
// game thread (it is, via the PeekMessage hook); RemoveFocusHook tears it down
// at unload. SendCurrentFocus pushes the current state to a connection from the
// mongoose thread (used to sync a newly connected client).
void InstallFocusHook();
void RemoveFocusHook();
void SendCurrentFocus(struct mg_connection *c);
