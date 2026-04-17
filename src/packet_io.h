#pragma once

#include <windows.h>
#include <cstdint>
#include <string>

#include "mongoose.h"

// Big-endian packet helpers (match Beryl's packet.js)
uint16_t ReadBE16(const BYTE* p);
uint32_t ReadBE32(const BYTE* p);

void WriteBE16(std::string& buf, uint16_t v);
void WriteBE32(std::string& buf, uint32_t v);
void WriteString8(std::string& buf, const std::string& s);
std::string ReadString8(const BYTE* data, DWORD size, DWORD& pos);
void PatchBE16(std::string& buf, size_t offset, uint16_t v);

// Send a msgType-prefixed payload to the connected Beryl client.
// Called from the game thread — dispatches via mg_wakeup to the mongoose thread.
void SendToBeryl(BYTE msgType, const BYTE* data, DWORD len);

// Send a server-packet frame directly on a specific connection (mongoose thread).
void SendPacketToBeryl(struct mg_connection* c, const std::string& pkt);
