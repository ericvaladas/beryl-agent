#include "mongoose.h"

#include <windows.h>
#include <detours.h>
#include <thread>
#include <mutex>
#include <vector>
#include <map>
#include <string>
#include <cstring>
#include <algorithm>
#include <set>
#include <fstream>

#include "json.h"

// wininet proxy (wininet_proxy.cpp)
extern void LoadRealWininet();
extern void UnloadRealWininet();

// =============================================================================
// Logging
// =============================================================================
std::mutex logMutex;
HANDLE g_logFile = INVALID_HANDLE_VALUE;

void LogToFile(const std::string& message) {
    std::lock_guard<std::mutex> lock(logMutex);
    if (g_logFile == INVALID_HANDLE_VALUE) {
        g_logFile = CreateFileA("C:\\Users\\Public\\hook_log.txt",
            FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
            NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    }
    if (g_logFile != INVALID_HANDLE_VALUE) {
        DWORD written;
        WriteFile(g_logFile, message.c_str(), (DWORD)message.size(), &written, NULL);
        WriteFile(g_logFile, "\r\n", 2, &written, NULL);
    }
}

// =============================================================================
// Game addresses (post-decryption packet functions)
// =============================================================================
constexpr DWORD FUNC_SEND_ADDR = 0x00563E00;
constexpr DWORD FUNC_RECV_ADDR = 0x00467060;
constexpr DWORD SEND_THIS_ADDR = 0x0073D958;
constexpr DWORD FUNC_WALK_ADDR = 0x005F0C40;
constexpr DWORD OBJECT_BASE_ADDR = 0x00882E68;

// Generic memory read/write via pointer chain resolution
// Chain: moduleBase + offsets[0], then dereference + add each subsequent offset
BYTE* ResolvePointerChain(const BYTE* offsets, int chainLength) {
    DWORD moduleBase = (DWORD)GetModuleHandle(NULL);
    if (!moduleBase) return nullptr;

    DWORD addr = moduleBase + *(DWORD*)(offsets);

    for (int i = 1; i < chainLength; i++) {
        DWORD* ptr = (DWORD*)addr;
        if (IsBadReadPtr(ptr, sizeof(DWORD))) return nullptr;
        addr = *ptr + *(DWORD*)(offsets + i * 4);
    }

    return (BYTE*)addr;
}

// __thiscall: ECX = this, args on stack
typedef void(__thiscall* GameSendFn)(void* thisPtr, const BYTE* data, DWORD size);
typedef void(__thiscall* GameRecvFn)(void* thisPtr, const BYTE* data, DWORD size);

typedef void(__thiscall* GameWalkFn)(void* thisPtr, int direction);

GameSendFn OrigGameSend = (GameSendFn)FUNC_SEND_ADDR;
GameRecvFn OrigGameRecv = (GameRecvFn)FUNC_RECV_ADDR;
GameWalkFn GameWalk = (GameWalkFn)FUNC_WALK_ADDR;

// =============================================================================
// Message types (must match Beryl side)
// =============================================================================
constexpr BYTE MSG_CLIENT       = 0x01;  // client->server packet
constexpr BYTE MSG_SERVER       = 0x02;  // server->client packet
constexpr BYTE MSG_WALK         = 0x03;  // walk command (direction byte)
constexpr BYTE MSG_READ_MEMORY  = 0x04;  // read memory via pointer chain
constexpr BYTE MSG_WRITE_MEMORY = 0x05;  // write memory via pointer chain
constexpr BYTE MSG_BECOME_REG   = 0x06;  // become registry server
constexpr BYTE MSG_READY        = 0x07;  // connection approved, client may proceed

// DLL's directory (Dark Ages install dir), used for serving game files over WS
std::string g_dllDirectory;

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

// =============================================================================
// State
// =============================================================================
DWORD pid;
volatile bool running = true;

void* g_sendThis = nullptr;
void* g_recvThis = nullptr;

HANDLE hMutex = NULL;

// =============================================================================
// Character data accumulation (parsed from recv hook)
// =============================================================================
std::mutex charDataMutex;
std::string charName;
bool charRegistered = false;
uint32_t charId = 0;

// Raw packet storage for replay
// storedStats: fixed 38-byte packet with bitmask 0x30 (both 0x20 and 0x10 blocks)
// Layout: [0x08][0x30][28-byte 0x20 block][8-byte 0x10 block]
std::string storedStats;
std::map<BYTE, std::string> storedPackets;           // opcode -> raw packet bytes
std::map<int, std::string> storedSpells;             // slot -> raw 0x17 packet
std::map<int, std::string> storedItems;              // slot -> raw 0x0F packet
std::map<int, std::string> storedSkills;             // slot -> raw 0x2C packet
std::map<int, std::string> storedEquipment;          // slot -> raw 0x37 packet
std::map<uint32_t, std::string> storedEntities;      // entityId -> per-entity byte slice from 0x07
std::map<uint32_t, std::string> storedShowUsers;     // entityId -> full raw 0x33 packet

// =============================================================================
// Mongoose networking state
// =============================================================================
struct mg_mgr g_mgr;
struct mg_connection* g_clientListener = nullptr;  // Client WS listener
struct mg_connection* g_clientConn = nullptr;       // Connected Beryl client
struct mg_connection* g_registryListener = nullptr; // Registry WS listener
struct mg_connection* g_registryClientConn = nullptr; // Our connection to registry
unsigned long g_wakeupId = 0;                        // ID for mg_wakeup target
bool isRegistry = false;
bool registryClientConnected = false;
int clientPort = 0;

std::vector<json> registeredClients;
std::map<struct mg_connection*, DWORD> registryConnPid;  // track which PID registered from which connection

// Connection consent state
struct mg_connection* g_pendingConn = nullptr;
std::string g_pendingOrigin;
bool g_consentDialogActive = false;
constexpr uint16_t CONSENT_PURSUIT_ID = 0xFFFF;
constexpr uint32_t CONSENT_ENTITY_ID = 0xFFFFFFFF;
constexpr uint16_t CONSENT_ENTITY_SPRITE = 0x80E8;

std::set<std::string> g_allowedOrigins;
std::string g_allowedOriginsPath;

// Threading
std::thread* wsThread = nullptr;

constexpr int REGISTRY_PORT = 21000;
constexpr int CLIENT_PORT_START = 21001;
constexpr int CLIENT_PORT_END = 21020;

// Forward declarations
void ClientHandler(struct mg_connection* c, int ev, void* ev_data);
void RegistryHandler(struct mg_connection* c, int ev, void* ev_data);
void RegistryClientHandler(struct mg_connection* c, int ev, void* ev_data);
bool StartRegistryServer();
void ConnectToRegistry();

// =============================================================================
// Registry: broadcast to all connected Beryl clients
// =============================================================================
void RegistryBroadcast(const std::string& message) {
    if (!g_registryListener) return;
    for (struct mg_connection* c = g_mgr.conns; c != NULL; c = c->next) {
        if (c->is_websocket && c->fn == (mg_event_handler_t)RegistryHandler) {
            mg_ws_send(c, message.c_str(), message.size(), WEBSOCKET_OP_TEXT);
        }
    }
}

void RegistrySendClientList(struct mg_connection* c) {
    json msg = {{"type", "init"}, {"clients", registeredClients}};
    std::string s = msg.dump();
    mg_ws_send(c, s.c_str(), s.size(), WEBSOCKET_OP_TEXT);
}

void RegistryAddClient(const json& clientData) {
    registeredClients.push_back(clientData);

    json msg = {{"type", "add"}, {"client", clientData}};
    RegistryBroadcast(msg.dump());
}

void RegistryRemoveClient(DWORD clientPid) {
    registeredClients.erase(
        std::remove_if(registeredClients.begin(), registeredClients.end(),
            [clientPid](const json& c) { return c["pid"] == clientPid; }),
        registeredClients.end());

    json msg = {{"type", "remove"}, {"pid", clientPid}};
    RegistryBroadcast(msg.dump());
}

// =============================================================================
// Send observed packet to Beryl via client WebSocket (thread-safe)
// =============================================================================
void SendToBeryl(BYTE msgType, const BYTE* data, DWORD len) {
    if (!g_clientConn || !running || !g_wakeupId) return;

    std::string payload(1 + len, '\0');
    payload[0] = (char)msgType;
    memcpy(&payload[1], data, len);

    mg_wakeup(&g_mgr, g_wakeupId, payload.data(), payload.size());
}

// =============================================================================
// Packet reading helpers (big-endian, matching Beryl's packet.js)
// =============================================================================
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

// =============================================================================
// Registration payload + deferred registration
// =============================================================================
json BuildRegistrationPayload() {
    // Caller must hold charDataMutex
    return {{"pid", pid}, {"port", clientPort}, {"name", charName}};
}

void SendPacketToBeryl(struct mg_connection* c, const std::string& pkt) {
    std::string frame(1 + pkt.size(), '\0');
    frame[0] = (char)MSG_SERVER;
    memcpy(&frame[1], pkt.data(), pkt.size());
    mg_ws_send(c, frame.data(), frame.size(), WEBSOCKET_OP_BINARY);
}

void ReplayCharDataToBeryl(struct mg_connection* c) {
    // Caller must hold charDataMutex
    // Replay stored raw packets (player id, map info, location, light level, doors, metadata)
    for (auto it = storedPackets.begin(); it != storedPackets.end(); ++it) {
        SendPacketToBeryl(c, it->second);
    }

    if (!storedStats.empty()) SendPacketToBeryl(c, storedStats);

    for (auto& [slot, pkt] : storedSpells)  SendPacketToBeryl(c, pkt);
    for (auto& [slot, pkt] : storedItems)   SendPacketToBeryl(c, pkt);
    for (auto& [slot, pkt] : storedSkills)  SendPacketToBeryl(c, pkt);
    for (auto& [slot, pkt] : storedEquipment) SendPacketToBeryl(c, pkt);

    // Replay stored entities as a single 0x07 packet
    if (!storedEntities.empty()) {
        std::string pkt;
        pkt += (char)0x07;
        WriteBE16(pkt, (uint16_t)storedEntities.size());
        for (auto it = storedEntities.begin(); it != storedEntities.end(); ++it) {
            pkt += it->second;
        }
        SendPacketToBeryl(c, pkt);
    }

    // Replay stored ShowUser packets
    for (auto it = storedShowUsers.begin(); it != storedShowUsers.end(); ++it) {
        SendPacketToBeryl(c, it->second);
    }
}

void TryRegister() {
    // Caller must hold charDataMutex
    if (charRegistered || charName.empty()) return;
    charRegistered = true;

    json payload = BuildRegistrationPayload();

    if (isRegistry) {
        RegistryAddClient(payload);
    } else if (registryClientConnected && g_registryClientConn) {
        json msg = payload;
        msg["type"] = "register";
        std::string s = msg.dump();
        mg_ws_send(g_registryClientConn, s.c_str(), s.size(), WEBSOCKET_OP_TEXT);
    } else if (g_clientConn) {
        // Registry is down -- replay character data over the per-client WS
        ReplayCharDataToBeryl(g_clientConn);
    }
}

// =============================================================================
// Walk destination helper
// =============================================================================
void ComputeWalkDestination(uint16_t originX, uint16_t originY, BYTE direction, uint16_t& destX, uint16_t& destY) {
    destX = originX;
    destY = originY;
    switch (direction) {
    case 0: destY = originY - 1; break; // Up
    case 1: destX = originX + 1; break; // Right
    case 2: destY = originY + 1; break; // Down
    case 3: destX = originX - 1; break; // Left
    }
}

// Write a BE16 value into a string at a given offset
void PatchBE16(std::string& buf, size_t offset, uint16_t v) {
    buf[offset]     = (char)(v >> 8);
    buf[offset + 1] = (char)(v & 0xFF);
}

// =============================================================================
// Server packet parsing (accumulate character data)
// =============================================================================
void ParseServerPacket(const BYTE* data, DWORD size) {
    if (size < 1) return;
    BYTE opcode = data[0];

    std::lock_guard<std::mutex> lock(charDataMutex);

    switch (opcode) {
    case 0x03: { // redirect -- extract character name
        if (size < 10) break;
        DWORD pos = 9; // skip opcode(1) + Address(4) + Port(2) + RemainingCount(1) + Seed(1)
        BYTE keyLength = data[pos++];
        pos += keyLength;
        charName = ReadString8(data, size, pos);
        break;
    }
    case 0x05: { // playerId -- signals login complete
        if (size >= 5) {
            storedPackets[opcode] = std::string((char*)data, size);
            charId = ReadBE32(data + 1);
            TryRegister();
        }
        break;
    }
    case 0x08: { // statistics -- patch into stored 0x30 packet
        if (size < 2) break;
        BYTE bitmask = data[1];
        DWORD pos = 2;

        // Initialize storedStats if empty: [0x08][0x30][28 zeros][8 zeros] = 38 bytes
        if (storedStats.empty()) {
            storedStats.assign(38, '\0');
            storedStats[0] = (char)0x08;
            storedStats[1] = (char)0x30;
        }

        if ((bitmask & 0x20) == 0x20) {
            if (pos + 28 > size) break;
            memcpy(&storedStats[2], data + pos, 28);
            pos += 28;
        }
        if ((bitmask & 0x10) == 0x10) {
            if (pos + 8 > size) break;
            memcpy(&storedStats[30], data + pos, 8);
        }
        break;
    }
    case 0x17: // addSpell -- store raw packet keyed by slot
    case 0x0F: // addItem
    case 0x2C: // addSkill
    case 0x37: // setEquipment
    {
        if (size < 2) break;
        int slot = data[1];
        std::string raw((char*)data, size);
        if (opcode == 0x17)      storedSpells[slot] = raw;
        else if (opcode == 0x0F) storedItems[slot] = raw;
        else if (opcode == 0x2C) storedSkills[slot] = raw;
        else                     storedEquipment[slot] = raw;
        break;
    }
    case 0x18: // removeSpell
    case 0x10: // removeItem
    case 0x2D: // removeSkill
    case 0x38: // removeEquipment
    {
        if (size < 2) break;
        int slot = data[1];
        if (opcode == 0x18)      storedSpells.erase(slot);
        else if (opcode == 0x10) storedItems.erase(slot);
        else if (opcode == 0x2D) storedSkills.erase(slot);
        else                     storedEquipment.erase(slot);
        break;
    }
    case 0x4C: { // logout -- deregister and reset all accumulated state
        if (charRegistered) {
            if (isRegistry) {
                RegistryRemoveClient(pid);
            } else if (registryClientConnected && g_registryClientConn) {
                json msg = {{"type", "deregister"}, {"pid", pid}};
                std::string s = msg.dump();
                mg_ws_send(g_registryClientConn, s.c_str(), s.size(), WEBSOCKET_OP_TEXT);
            }
        }
        charName.clear();
        charId = 0;
        charRegistered = false;
        storedPackets.clear();
        storedStats.clear();
        storedSpells.clear();
        storedItems.clear();
        storedSkills.clear();
        storedEquipment.clear();
        storedEntities.clear();
        storedShowUsers.clear();
        break;
    }
    case 0x15: // mapInfo -- store and clear entities (new map)
        storedPackets[opcode] = std::string((char*)data, size);
        storedEntities.clear();
        storedShowUsers.clear();
        break;
    case 0x04: // mapLocation
    case 0x20: // lightLevel
    case 0x32: // mapDoor
    case 0x6f: // metadata
        storedPackets[opcode] = std::string((char*)data, size);
        break;
    case 0x33: { // showUser -- store keyed by entity ID
        if (size >= 10) {
            uint32_t entityId = ReadBE32(data + 6);
            storedShowUsers[entityId] = std::string((char*)data, size);
        }
        break;
    }
    case 0x07: { // addEntity -- split into per-entity slices keyed by ID
        if (size < 3) break;
        uint16_t entityCount = ReadBE16(data + 1);
        DWORD pos = 3;
        for (uint16_t i = 0; i < entityCount; i++) {
            DWORD entityStart = pos;
            if (pos + 10 > size) break; // X(2)+Y(2)+Id(4)+Sprite(2)
            uint32_t entityId = ReadBE32(data + pos + 4);
            uint16_t sprite = ReadBE16(data + pos + 8);
            pos += 10;
            if (sprite & 0x4000) { // creature
                if (pos + 7 > size) break; // Unknown(4)+Dir(1)+Unknown(1)+CreatureType(1)
                BYTE creatureType = data[pos + 6];
                pos += 7;
                if (creatureType == 2) { // Mundane -- has String8 name
                    if (pos >= size) break;
                    uint8_t nameLen = data[pos];
                    pos += 1 + nameLen;
                }
            } else if (sprite & 0x8000) { // item
                pos += 3; // Color(1)+Unknown(2)
            } else {
                break; // unknown entity type
            }
            if (pos > size) break;
            storedEntities[entityId] = std::string((char*)data + entityStart, pos - entityStart);
        }
        break;
    }
    case 0x0B: { // walkResponse -- update current player's stored ShowUser
        if (size < 6) break; // opcode(1)+Dir(1)+PrevX(2)+PrevY(2)
        BYTE direction = data[1];
        uint16_t originX = ReadBE16(data + 2);
        uint16_t originY = ReadBE16(data + 4);
        uint16_t destX, destY;
        ComputeWalkDestination(originX, originY, direction, destX, destY);

        auto sit = storedShowUsers.find(charId);
        if (sit != storedShowUsers.end() && sit->second.size() >= 10) {
            PatchBE16(sit->second, 1, destX);
            PatchBE16(sit->second, 3, destY);
            sit->second[5] = (char)direction;
        }
        break;
    }
    case 0x0C: { // entityWalk -- update stored entity position and direction
        if (size < 10) break; // opcode(1)+Id(4)+OriginX(2)+OriginY(2)+Dir(1)
        uint32_t entityId = ReadBE32(data + 1);
        uint16_t originX = ReadBE16(data + 5);
        uint16_t originY = ReadBE16(data + 7);
        BYTE direction = data[9];
        uint16_t destX, destY;
        ComputeWalkDestination(originX, originY, direction, destX, destY);

        auto eit = storedEntities.find(entityId);
        if (eit != storedEntities.end() && eit->second.size() >= 15) {
            uint16_t sprite = ReadBE16((const BYTE*)eit->second.data() + 8);
            if (sprite & 0x4000) { // creature
                PatchBE16(eit->second, 0, destX);
                PatchBE16(eit->second, 2, destY);
                eit->second[14] = (char)direction;
            }
        }
        auto sit = storedShowUsers.find(entityId);
        if (sit != storedShowUsers.end() && sit->second.size() >= 10) {
            PatchBE16(sit->second, 1, destX);
            PatchBE16(sit->second, 3, destY);
            sit->second[5] = (char)direction;
        }
        break;
    }
    case 0x11: { // entityTurn -- update stored entity direction
        if (size < 6) break; // opcode(1)+Id(4)+Dir(1)
        uint32_t entityId = ReadBE32(data + 1);
        BYTE direction = data[5];

        auto eit = storedEntities.find(entityId);
        if (eit != storedEntities.end() && eit->second.size() >= 15) {
            uint16_t sprite = ReadBE16((const BYTE*)eit->second.data() + 8);
            if (sprite & 0x4000) { // creature
                eit->second[14] = (char)direction;
            }
        }
        auto sit = storedShowUsers.find(entityId);
        if (sit != storedShowUsers.end() && sit->second.size() >= 6) {
            sit->second[5] = (char)direction;
        }
        break;
    }
    case 0x0E: { // removeEntity -- erase from both maps
        if (size < 5) break; // opcode(1)+Id(4)
        uint32_t entityId = ReadBE32(data + 1);
        storedEntities.erase(entityId);
        storedShowUsers.erase(entityId);
        break;
    }
    }
}

// =============================================================================
// Connection consent dialog
// =============================================================================
void InjectServerPacket(const std::string& pkt) {
    void* thisPtr = g_recvThis;
    if (thisPtr) {
        OrigGameRecv(thisPtr, (const BYTE*)pkt.data(), (DWORD)pkt.size());
    }
}

void ShowConsentDialog(const std::string& origin) {
    std::string pkt;
    pkt += (char)0x30;        // opcode: ShowDialog
    pkt += (char)0x02;        // DialogType::Menu
    pkt += (char)0x00;        // EntityType
    WriteBE32(pkt, CONSENT_ENTITY_ID);
    pkt += (char)0x01;        // Unknown1
    WriteBE16(pkt, CONSENT_ENTITY_SPRITE);
    pkt += (char)0x00;        // Color
    pkt += (char)0x01;        // Unknown2
    WriteBE16(pkt, 0x0000);   // SpriteSecondary
    pkt += (char)0x00;        // ColorSecondary
    WriteBE16(pkt, CONSENT_PURSUIT_ID);
    WriteBE16(pkt, 0x0000);   // StepId
    pkt += (char)0x00;        // HasPreviousButton
    pkt += (char)0x00;        // HasNextButton
    pkt += (char)0x01;        // ShowGraphic (inverted: 1 = hide)
    WriteString8(pkt, "Beryl");
    std::string content = "Allow connections from " + origin + "?";
    // String16: 2-byte BE length prefix
    WriteBE16(pkt, (uint16_t)content.size());
    pkt += content;
    pkt += (char)0x02;        // ChoiceCount
    WriteString8(pkt, "Yes");
    WriteString8(pkt, "No");

    InjectServerPacket(pkt);
    g_consentDialogActive = true;
}

void CloseConsentDialog() {
    std::string pkt;
    pkt += (char)0x30;  // opcode: ShowDialog
    pkt += (char)0x0A;  // DialogType::CloseDialog
    pkt += (char)0x00;  // padding
    InjectServerPacket(pkt);
}

void LoadAllowedOrigins() {
    g_allowedOrigins.clear();
    std::ifstream file(g_allowedOriginsPath);
    if (!file.is_open()) return;

    std::string content((std::istreambuf_iterator<char>(file)),
                         std::istreambuf_iterator<char>());
    file.close();

    try {
        json j = json::parse(content);
        for (const auto& item : j["allowedOrigins"]) {
            g_allowedOrigins.insert(item.str());
        }
    } catch (...) {}
}

void SaveAllowedOrigins() {
    json arr;
    for (const auto& origin : g_allowedOrigins) {
        arr.push_back(json(origin));
    }

    json j = {{"allowedOrigins", arr}};

    std::ofstream file(g_allowedOriginsPath);
    if (file.is_open()) {
        file << j.dump();
    }
}

void ResetConsentState() {
    g_consentDialogActive = false;
    g_pendingConn = nullptr;
    g_pendingOrigin.clear();
}

void PromoteConnection(struct mg_connection* c) {
    g_clientConn = c;
    BYTE ready = MSG_READY;
    mg_ws_send(g_clientConn, (const char*)&ready, 1, WEBSOCKET_OP_BINARY);
    std::lock_guard<std::mutex> lock(charDataMutex);
    if (!charName.empty()) {
        ReplayCharDataToBeryl(g_clientConn);
    }
}

// =============================================================================
// Hooks (__fastcall with dummy edx to intercept __thiscall)
// =============================================================================
// Returns true if the packet should be suppressed (not sent to the game server)
bool ShouldSuppressClientPacket(const BYTE* data, DWORD size) {
    if (size < 1) return false;

    // Intercept dialog response (opcode 0x3A) for our consent dialog
    if (g_consentDialogActive && data[0] == 0x3A && size >= 10) {
        // Body after opcode: EntityType(1) + EntityId(4) + PursuitId(2) + StepId(2)
        uint16_t pursuitId = ReadBE16(data + 6);
        if (pursuitId != CONSENT_PURSUIT_ID) return false;

        bool accepted = false;
        if (size >= 12) {
            BYTE argsType = data[10];
            BYTE menuChoice = data[11];
            accepted = (argsType == 0x01 && menuChoice == 1);  // "Yes"
        }

        CloseConsentDialog();

        if (accepted && g_pendingConn) {
            g_allowedOrigins.insert(g_pendingOrigin);
            SaveAllowedOrigins();
            PromoteConnection(g_pendingConn);
        } else if (g_pendingConn) {
            g_pendingConn->is_closing = 1;
        }

        ResetConsentState();
        return true;
    }

    return false;
}

void __fastcall HookedGameSend(void* thisPtr, void* /*edx*/, const BYTE* data, DWORD size) {
    g_sendThis = thisPtr;
    if (ShouldSuppressClientPacket(data, size)) return;
    SendToBeryl(MSG_CLIENT, data, size);
    OrigGameSend(thisPtr, data, size);
}

void __fastcall HookedGameRecv(void* thisPtr, void* /*edx*/, const BYTE* data, DWORD size) {
    g_recvThis = thisPtr;

    // Parse packet data to accumulate character state
    ParseServerPacket(data, size);

    SendToBeryl(MSG_SERVER, data, size);
    OrigGameRecv(thisPtr, data, size);
}

// =============================================================================
// WebSocket file serving (binary response to Beryl file requests)
// =============================================================================
struct FileRequest {
    struct mg_connection* c;
    uint32_t requestId;
    std::string path;
};
std::vector<FileRequest> g_fileQueue;
struct mg_connection* g_fileSendConn = nullptr;  // connection currently draining

void SendFileResponse(struct mg_connection* c, uint32_t requestId, uint8_t status, const char* data = nullptr, size_t dataLen = 0) {
    std::string payload(5 + dataLen, '\0');
    memcpy(&payload[0], &requestId, 4);  // LE uint32
    payload[4] = (char)status;
    if (data && dataLen > 0) {
        memcpy(&payload[5], data, dataLen);
    }
    mg_ws_send(c, payload.data(), payload.size(), WEBSOCKET_OP_BINARY);
}

void HandleFileRequest(struct mg_connection* c, uint32_t requestId, const std::string& relativePath) {
    // Sanitize: reject path traversal
    if (relativePath.find("..") != std::string::npos) {
        SendFileResponse(c, requestId, 0x02);
        return;
    }

    // Convert forward slashes to backslashes for Windows
    std::string winPath = relativePath;
    for (auto& ch : winPath) {
        if (ch == '/') ch = '\\';
    }

    std::string filePath = g_dllDirectory + "\\" + winPath;

    HANDLE h = CreateFileA(filePath.c_str(), GENERIC_READ, FILE_SHARE_READ,
        NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) {
        SendFileResponse(c, requestId, 0x01);
        return;
    }

    DWORD size = GetFileSize(h, NULL);
    if (size == INVALID_FILE_SIZE) {
        CloseHandle(h);
        SendFileResponse(c, requestId, 0x02);
        return;
    }

    std::string body((size_t)size, '\0');
    DWORD bytesRead;
    BOOL ok = ReadFile(h, &body[0], size, &bytesRead, NULL);
    CloseHandle(h);

    if (!ok || bytesRead != size) {
        SendFileResponse(c, requestId, 0x02);
        return;
    }

    SendFileResponse(c, requestId, 0x00, body.data(), body.size());
    g_fileSendConn = c;
}

void ProcessNextFile() {
    if (g_fileQueue.empty()) {
        g_fileSendConn = nullptr;
        return;
    }
    FileRequest req = std::move(g_fileQueue.front());
    g_fileQueue.erase(g_fileQueue.begin());
    HandleFileRequest(req.c, req.requestId, req.path);
}

void QueueFileRequest(struct mg_connection* c, uint32_t requestId, const std::string& path) {
    if (g_fileSendConn == nullptr) {
        // No file currently draining, process immediately
        HandleFileRequest(c, requestId, path);
    } else {
        // Queue it — will be processed when current file finishes draining
        g_fileQueue.push_back({c, requestId, path});
    }
}

// =============================================================================
// Client WebSocket handler (per-DLL, binary packets to/from Beryl)
// =============================================================================
void ClientHandler(struct mg_connection* c, int ev, void* ev_data) {
    if (ev == MG_EV_HTTP_MSG) {
        struct mg_http_message* hm = (struct mg_http_message*)ev_data;
        struct mg_str* origin = mg_http_get_header(hm, "Origin");
        g_pendingOrigin = origin ? std::string(origin->buf, origin->len) : "unknown";
        mg_ws_upgrade(c, hm, NULL);
    } else if (ev == MG_EV_WS_OPEN) {
        // Close any previous pending connection
        if (g_pendingConn) {
            g_pendingConn->is_closing = 1;
            ResetConsentState();
        }

        // If already connected, reject new connection
        if (g_clientConn) {
            c->is_closing = 1;
            return;
        }

        // If not logged in, can't show dialog — reject
        if (!g_recvThis) {
            c->is_closing = 1;
            return;
        }

        // Auto-approve if origin was previously allowed
        if (g_allowedOrigins.count(g_pendingOrigin)) {
            PromoteConnection(c);
            return;
        }

        g_pendingConn = c;
        ShowConsentDialog(g_pendingOrigin);
    } else if (ev == MG_EV_WS_MSG) {
        struct mg_ws_message* wm = (struct mg_ws_message*)ev_data;
        if (wm->data.len < 1) return;

        BYTE msgType = (BYTE)wm->data.buf[0];

        // Only allow MSG_BECOME_REG from unapproved connections
        if (c != g_clientConn && msgType != MSG_BECOME_REG) return;

        if (msgType == MSG_BECOME_REG) {
            if (!isRegistry) {
                isRegistry = StartRegistryServer();
                if (isRegistry) {
                    std::lock_guard<std::mutex> lock(charDataMutex);
                    if (charRegistered) {
                        RegistryAddClient(BuildRegistrationPayload());
                    }
                }
            }
            return;
        }

        if (wm->data.len < 2) return;

        const BYTE* body = (const BYTE*)wm->data.buf + 1;
        DWORD bodyLen = (DWORD)wm->data.len - 1;

        if (msgType == MSG_CLIENT) {
            void* thisPtr = g_sendThis;
            if (!thisPtr) thisPtr = *(void**)SEND_THIS_ADDR;
            if (thisPtr) {
                OrigGameSend(thisPtr, body, bodyLen);
            }
        } else if (msgType == MSG_SERVER) {
            void* thisPtr = g_recvThis;
            if (thisPtr) {
                OrigGameRecv(thisPtr, body, bodyLen);
            }
        } else if (msgType == MSG_WALK) {
            if (bodyLen >= 1) {
                void* thisPtr = *(void**)OBJECT_BASE_ADDR;
                if (thisPtr) {
                    GameWalk(thisPtr, (int)body[0]);
                }
            }
        } else if (msgType == MSG_READ_MEMORY) {
            // body: [request_id(1), chain_length(1), offsets(chain_length*4 LE), size(4 LE)]
            if (bodyLen >= 6) {
                BYTE requestId = body[0];
                BYTE chainLength = body[1];
                DWORD expectedLen = 2 + (DWORD)chainLength * 4 + 4;
                if (bodyLen >= expectedLen && chainLength > 0) {
                    const BYTE* offsets = body + 2;
                    DWORD size = *(DWORD*)(body + 2 + chainLength * 4);

                    BYTE* addr = ResolvePointerChain(offsets, chainLength);

                    // Response: [MSG_READ_MEMORY, request_id, data...]
                    std::vector<BYTE> response(2 + size);
                    response[0] = MSG_READ_MEMORY;
                    response[1] = requestId;
                    if (addr && !IsBadReadPtr(addr, size)) {
                        memcpy(response.data() + 2, addr, size);
                    }
                    // else: zeros (default from vector init)

                    mg_ws_send(c, (const char*)response.data(), response.size(), WEBSOCKET_OP_BINARY);
                }
            }
        } else if (msgType == MSG_WRITE_MEMORY) {
            // body: [request_id(1), chain_length(1), offsets(chain_length*4 LE), size(4 LE), data(size)]
            if (bodyLen >= 6) {
                BYTE requestId = body[0];
                BYTE chainLength = body[1];
                DWORD headerLen = 2 + (DWORD)chainLength * 4 + 4;
                if (bodyLen >= headerLen && chainLength > 0) {
                    DWORD size = *(DWORD*)(body + 2 + chainLength * 4);
                    const BYTE* data = body + headerLen;

                    BYTE status = 1; // failed
                    const BYTE* offsets = body + 2;
                    if (bodyLen >= headerLen + size) {
                        BYTE* addr = ResolvePointerChain(offsets, chainLength);
                        if (addr && !IsBadWritePtr(addr, size)) {
                            memcpy(addr, data, size);
                            status = 0; // ok
                        }
                    }

                    // Response: [MSG_WRITE_MEMORY, request_id, status]
                    BYTE response[3] = { MSG_WRITE_MEMORY, requestId, status };
                    mg_ws_send(c, (const char*)response, 3, WEBSOCKET_OP_BINARY);
                }
            }
        }
    } else if (ev == MG_EV_WAKEUP) {
        // Forwarded from game thread via mg_wakeup — send to Beryl on mongoose thread
        struct mg_str* data = (struct mg_str*)ev_data;
        if (g_clientConn) {
            mg_ws_send(g_clientConn, data->buf, data->len, WEBSOCKET_OP_BINARY);
        }
    } else if (ev == MG_EV_ERROR) {
        // LogToFile(std::string("Client error: ") + (const char*)ev_data);
    } else if (ev == MG_EV_CLOSE) {
        if (c == g_clientConn) {
            g_clientConn = nullptr;
            if (c == g_fileSendConn) {
                g_fileSendConn = nullptr;
                g_fileQueue.clear();
            }
        } else if (c == g_pendingConn) {
            ResetConsentState();
        }
    }
}

// =============================================================================
// Registry WebSocket handler (first DLL only, JSON messages)
// =============================================================================
void RegistryHandler(struct mg_connection* c, int ev, void* ev_data) {
    if (ev == MG_EV_HTTP_MSG) {
        mg_ws_upgrade(c, (struct mg_http_message*)ev_data, NULL);
    } else if (ev == MG_EV_WS_OPEN) {
        // LogToFile("Registry: WS client connected");
        RegistrySendClientList(c);
    } else if (ev == MG_EV_WS_MSG) {
        struct mg_ws_message* wm = (struct mg_ws_message*)ev_data;

        try {
            json data = json::parse(std::string(wm->data.buf, wm->data.len));
            std::string type = data["type"];

            if (type == "register") {
                json clientData = data;
                clientData.erase("type");
                RegistryAddClient(clientData);
                registryConnPid[c] = (DWORD)clientData["pid"];
            } else if (type == "deregister") {
                DWORD clientPid = (DWORD)data["pid"];
                RegistryRemoveClient(clientPid);
            } else if (type == "file") {
                uint32_t requestId = (uint32_t)data["id"];
                std::string path = data["path"];
                QueueFileRequest(c, requestId, path);
            } else if (type == "getSettings" && g_clientConn) {
                std::ifstream settingsFile(g_allowedOriginsPath);
                std::string content = "{}";
                if (settingsFile.is_open()) {
                    content.assign((std::istreambuf_iterator<char>(settingsFile)),
                                    std::istreambuf_iterator<char>());
                    settingsFile.close();
                }
                // Wrap in a message with type
                json resp = json::parse(content);
                resp["type"] = "settings";
                std::string out = resp.dump();
                mg_ws_send(c, out.c_str(), out.size(), WEBSOCKET_OP_TEXT);
            } else if (type == "setSettings" && g_clientConn) {
                // Update in-memory allowed origins if present
                const json& origins = data["allowedOrigins"];
                if (origins.type() == json::t_array) {
                    g_allowedOrigins.clear();
                    for (const auto& item : origins) {
                        g_allowedOrigins.insert(item.str());
                    }
                }

                // Write full settings to disk (strip "type" key)
                json settings = data;
                settings.erase("type");
                std::ofstream settingsFile(g_allowedOriginsPath);
                if (settingsFile.is_open()) {
                    settingsFile << settings.dump();
                }
            }
        } catch (...) {
            // LogToFile("Registry: exception parsing message");
        }
    } else if (ev == MG_EV_CLOSE) {
        auto it = registryConnPid.find(c);
        if (it != registryConnPid.end()) {
            RegistryRemoveClient(it->second);
            registryConnPid.erase(it);
        }
    }
}

// =============================================================================
// Registry client handler (for non-registry DLLs)
// =============================================================================
uint64_t g_registryReconnectTime = 0;  // millis timestamp for next reconnect attempt

void RegistryClientHandler(struct mg_connection* c, int ev, void* ev_data) {
    if (ev == MG_EV_WS_OPEN) {
        g_registryClientConn = c;
        registryClientConnected = true;

        // Re-register with the new registry
        std::lock_guard<std::mutex> lock(charDataMutex);
        if (!charName.empty()) {
            charRegistered = false;
            TryRegister();
        }
    } else if (ev == MG_EV_CLOSE) {
        if (c == g_registryClientConn) {
            g_registryClientConn = nullptr;
            registryClientConnected = false;
            if (running && !isRegistry) {
                g_registryReconnectTime = mg_millis() + 2000;
            }
        }
    } else if (ev == MG_EV_ERROR) {
        // Connection failed
        registryClientConnected = false;
        g_registryClientConn = nullptr;
        if (running && !isRegistry) {
            g_registryReconnectTime = mg_millis() + 2000;
        }
    }
}

// =============================================================================
// Server setup
// =============================================================================
int StartClientServer() {
    for (int port = CLIENT_PORT_START; port <= CLIENT_PORT_END; port++) {
        std::string url = "http://127.0.0.1:" + std::to_string(port);
        struct mg_connection* c = mg_http_listen(&g_mgr, url.c_str(), ClientHandler, NULL);
        if (c) {
            g_clientListener = c;
            // LogToFile("StartClientServer: bound on " + url);
            return port;
        } else {
            // LogToFile("StartClientServer: failed on " + url);
        }
    }
    return 0;
}

bool StartRegistryServer() {
    std::string url = "http://127.0.0.1:" + std::to_string(REGISTRY_PORT);
    struct mg_connection* c = mg_http_listen(&g_mgr, url.c_str(), RegistryHandler, NULL);
    if (c) {
        g_registryListener = c;
        // LogToFile("Registry server bound on " + url);
        return true;
    }
    // LogToFile("Registry server failed to bind on " + url);
    return false;
}

void ConnectToRegistry() {
    std::string url = "ws://127.0.0.1:" + std::to_string(REGISTRY_PORT);
    struct mg_connection* c = mg_ws_connect(&g_mgr, url.c_str(), RegistryClientHandler, NULL, NULL);
    if (c) {
        g_registryClientConn = c;
    }
}

// =============================================================================
// Multi-instance: hook CreateMutexA to neuter the single-instance check
// =============================================================================
typedef HANDLE(WINAPI* CreateMutexAFn)(LPSECURITY_ATTRIBUTES, BOOL, LPCSTR);
CreateMutexAFn RealCreateMutexA = CreateMutexA;

bool mutexCheckBypassed = false;

HANDLE WINAPI HookedCreateMutexA(LPSECURITY_ATTRIBUTES lpAttr, BOOL bInitialOwner, LPCSTR lpName) {
    if (lpName && !mutexCheckBypassed) {
        mutexCheckBypassed = true;
        HANDLE h = RealCreateMutexA(lpAttr, bInitialOwner, NULL);
        SetLastError(0);
        return h;
    }
    return RealCreateMutexA(lpAttr, bInitialOwner, lpName);
}

// =============================================================================
// DllMain
// =============================================================================
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
        const char* mutexName = mutexStr.c_str();
        HANDLE existing = OpenMutexA(MUTEX_ALL_ACCESS, FALSE, mutexName);
        if (existing) {
            CloseHandle(existing);
            return FALSE;
        }
        hMutex = CreateMutexA(NULL, FALSE, mutexName);

        running = true;

        DisableThreadLibraryCalls(hInst);

        // Install hooks (including CreateMutexA for multi-instance)
        DetourTransactionBegin();
        DetourUpdateThread(GetCurrentThread());
        DetourAttach(reinterpret_cast<PVOID*>(&RealCreateMutexA), reinterpret_cast<PVOID>(HookedCreateMutexA));
        DetourAttach(reinterpret_cast<PVOID*>(&OrigGameSend), reinterpret_cast<PVOID>(HookedGameSend));
        DetourAttach(reinterpret_cast<PVOID*>(&OrigGameRecv), reinterpret_cast<PVOID>(HookedGameRecv));
        DetourTransactionCommit();

        // Start WebSocket servers on a background thread
        wsThread = new std::thread([]() {
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

            // LogToFile("Client WS server listening on port " + std::to_string(clientPort));

            // Try to become the registry
            isRegistry = StartRegistryServer();
            if (isRegistry) {
                // LogToFile("This DLL is the registry");
            } else {
                // LogToFile("Another DLL is the registry, connecting as client");
                ConnectToRegistry();
            }

            // Check if already logged in
            {
                std::lock_guard<std::mutex> lock(charDataMutex);
                if (!storedStats.empty()) {
                    TryRegister();
                }
            }

            // Event loop
            while (running) {
                mg_mgr_poll(&g_mgr, 50);

                // Process next queued file when current one has drained
                if (g_fileSendConn != nullptr && g_fileSendConn->send.len == 0) {
                    ProcessNextFile();
                }

                // Registry client reconnect logic
                if (g_registryReconnectTime > 0 && !isRegistry && !registryClientConnected && running) {
                    if (mg_millis() >= g_registryReconnectTime) {
                        g_registryReconnectTime = 0;
                        ConnectToRegistry();
                    }
                }
            }

            mg_mgr_free(&g_mgr);
        });
    }
    else if (reason == DLL_PROCESS_DETACH) {
        running = false;

        // Notify registry of our departure
        if (isRegistry) {
            RegistryRemoveClient(pid);
        } else if (registryClientConnected && g_registryClientConn) {
            json msg = {{"type", "deregister"}, {"pid", pid}};
            std::string s = msg.dump();
            mg_ws_send(g_registryClientConn, s.c_str(), s.size(), WEBSOCKET_OP_TEXT);
        }

        if (wsThread && wsThread->joinable()) {
            wsThread->join();
            delete wsThread;
        }

        registeredClients.clear();
        if (g_logFile != INVALID_HANDLE_VALUE) { CloseHandle(g_logFile); g_logFile = INVALID_HANDLE_VALUE; }
        if (hMutex) CloseHandle(hMutex);

        DetourTransactionBegin();
        DetourUpdateThread(GetCurrentThread());
        DetourDetach(reinterpret_cast<PVOID*>(&RealCreateMutexA), reinterpret_cast<PVOID>(HookedCreateMutexA));
        DetourDetach(reinterpret_cast<PVOID*>(&OrigGameSend), reinterpret_cast<PVOID>(HookedGameSend));
        DetourDetach(reinterpret_cast<PVOID*>(&OrigGameRecv), reinterpret_cast<PVOID>(HookedGameRecv));
        DetourTransactionCommit();

        UnloadRealWininet();
    }

    return TRUE;
}
