#pragma once

#include <set>
#include <string>
#include <windows.h>

#include "mongoose.h"

extern bool g_authDialogActive;
extern struct mg_connection *g_pendingConn;
extern std::string g_pendingOrigin;

extern std::set<std::string> g_allowedOrigins;
extern std::string g_allowedOriginsPath;

void ShowAuthDialog(const std::string &origin);
void CloseAuthDialog();
void LoadAllowedOrigins();
void SaveAllowedOrigins();
void ResetAuthState();
void PromoteConnection(struct mg_connection *c);

// Called from the game-send hook; returns true if the packet was consumed
// by the auth dialog flow and must not reach the server.
bool ShouldSuppressClientPacket(const BYTE *data, DWORD size);

// Inject a server-bound packet via OrigGameRecv (uses g_recvThis).
void InjectServerPacket(const std::string &pkt);
