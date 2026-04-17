#include "packet_io.h"

#include <cstring>

#include "state.h"

uint16_t ReadBE16(const BYTE* p) { return (p[0] << 8) | p[1]; }
uint32_t ReadBE32(const BYTE* p) { return (p[0] << 24) | (p[1] << 16) | (p[2] << 8) | p[3]; }

void WriteBE16(std::string& buf, uint16_t v) {
    buf += (char)(v >> 8);
    buf += (char)(v & 0xFF);
}

void WriteBE32(std::string& buf, uint32_t v) {
    buf += (char)(v >> 24);
    buf += (char)((v >> 16) & 0xFF);
    buf += (char)((v >> 8) & 0xFF);
    buf += (char)(v & 0xFF);
}

void WriteString8(std::string& buf, const std::string& s) {
    buf += (char)(uint8_t)s.size();
    buf += s;
}

std::string ReadString8(const BYTE* data, DWORD size, DWORD& pos) {
    if (pos >= size) return "";
    uint8_t len = data[pos++];
    if (pos + len > size) { pos = size; return ""; }
    std::string s((char*)&data[pos], len);
    pos += len;
    return s;
}

void PatchBE16(std::string& buf, size_t offset, uint16_t v) {
    buf[offset]     = (char)(v >> 8);
    buf[offset + 1] = (char)(v & 0xFF);
}

// Build a msgType-prefixed frame (used by both send paths)
static std::string WrapMessage(BYTE msgType, const BYTE* data, size_t len) {
    std::string payload(1 + len, '\0');
    payload[0] = (char)msgType;
    if (len > 0) memcpy(&payload[1], data, len);
    return payload;
}

void SendToBeryl(BYTE msgType, const BYTE* data, DWORD len) {
    if (!g_clientConn || !running || !g_wakeupId) return;
    std::string payload = WrapMessage(msgType, data, len);
    mg_wakeup(&g_mgr, g_wakeupId, payload.data(), payload.size());
}

void SendPacketToBeryl(struct mg_connection* c, const std::string& pkt) {
    std::string frame = WrapMessage(MSG_SERVER, (const BYTE*)pkt.data(), pkt.size());
    mg_ws_send(c, frame.data(), frame.size(), WEBSOCKET_OP_BINARY);
}
