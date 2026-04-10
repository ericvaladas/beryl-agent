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

// Wall tile data toggle
// Pointer chain: [OBJECT_BASE_ADDR] + 0x190 + 0x0C -> tile array
// Each tile is 6 bytes: layer1(2) + layer2(2) + layer3(2)
// Zeroing layer2/layer3 hides walls
std::vector<BYTE> wallsSavedTileData;
bool wallsHidden = false;

BYTE* GetTileArray() {
    void* base = *(void**)OBJECT_BASE_ADDR;
    if (!base) return nullptr;
    void* obj = *(void**)((BYTE*)base + 0x190);
    if (!obj) return nullptr;
    return *(BYTE**)((BYTE*)obj + 0x0C);
}

bool IsRenderedWall(uint16_t id) {
    if (id == 0) return false;
    return (id > 10012) || ((id % 10000) > 12);
}

void ToggleWalls(bool hide, uint16_t width, uint16_t height, const BYTE* wallBitmask) {
    if (hide == wallsHidden) return;

    BYTE* tiles = GetTileArray();
    if (!tiles) return;

    DWORD totalTiles = (DWORD)width * height;
    DWORD dataSize = totalTiles * 6;

    if (hide) {
        // Save original tile data
        wallsSavedTileData.assign(tiles, tiles + dataSize);
        for (DWORD i = 0; i < totalTiles; i++) {
            DWORD offset = i * 6;
            uint16_t layer2 = tiles[offset + 2] | (tiles[offset + 3] << 8);
            uint16_t layer3 = tiles[offset + 4] | (tiles[offset + 5] << 8);
            bool isWall = wallBitmask[i >> 3] & (1 << (i & 7));
            // Sprite to replace with: 1 = invisible + blocking, 0 = invisible + walkable
            BYTE replacement = isWall ? 1 : 0;

            if (IsRenderedWall(layer2)) {
                tiles[offset + 2] = replacement;
                tiles[offset + 3] = 0;
            }
            if (IsRenderedWall(layer3)) {
                tiles[offset + 4] = replacement;
                tiles[offset + 5] = 0;
            }
        }
    } else {
        // Restore original tile data
        if (wallsSavedTileData.size() == dataSize) {
            memcpy(tiles, wallsSavedTileData.data(), dataSize);
        }
        wallsSavedTileData.clear();
    }

    wallsHidden = hide;
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
constexpr BYTE MSG_CLIENT      = 0x01;  // client->server packet
constexpr BYTE MSG_SERVER      = 0x02;  // server->client packet
constexpr BYTE MSG_WALK        = 0x03;  // walk command (direction byte)
constexpr BYTE MSG_BECOME_REG  = 0x06;  // become registry server
constexpr BYTE MSG_WALLS       = 0x07;  // toggle wall rendering

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

struct SpellInfo {
    int slot, icon, targetType, lines;
    std::string name, prompt;
};
struct ItemInfo {
    int slot, sprite, color;
    std::string name;
    uint32_t quantity;
};

std::map<int, SpellInfo> charSpells;
std::map<int, ItemInfo> charInventory;
uint32_t charCurrentHP = 0, charMaxHP = 0, charCurrentMP = 0, charMaxMP = 0;

// =============================================================================
// Mongoose networking state
// =============================================================================
struct mg_mgr g_mgr;
struct mg_connection* g_clientListener = nullptr;  // Client WS listener
struct mg_connection* g_clientConn = nullptr;       // Connected Beryl client
struct mg_connection* g_registryListener = nullptr; // Registry WS listener
struct mg_connection* g_registryClientConn = nullptr; // Our connection to registry
bool isRegistry = false;
bool registryClientConnected = false;
int clientPort = 0;

std::vector<json> registeredClients;

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
// Send observed packet to Beryl via client WebSocket
// =============================================================================
void SendToBeryl(BYTE msgType, const BYTE* data, DWORD len) {
    if (!g_clientConn || !running) return;

    std::string payload(1 + len, '\0');
    payload[0] = (char)msgType;
    memcpy(&payload[1], data, len);

    mg_ws_send(g_clientConn, payload.data(), payload.size(), WEBSOCKET_OP_BINARY);
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
// Character name from game memory
// =============================================================================
std::string ReadCharName() {
    HMODULE hModule = GetModuleHandle(NULL);
    if (!hModule) return "";
    char* addr = (char*)((DWORD)hModule + 0x33D910);
    char buf[65] = {0};
    memcpy(buf, addr, 64);
    return std::string(buf);
}

// =============================================================================
// Registration payload + deferred registration
// =============================================================================
json BuildRegistrationPayload() {
    // Caller must hold charDataMutex
    return {{"pid", pid}, {"port", clientPort}, {"name", charName}};
}

// =============================================================================
// Packet reconstruction (rebuild server packets from parsed state)
// =============================================================================
std::string BuildPlayerIdPacket() {
    std::string pkt;
    pkt += (char)0x05;
    WriteBE32(pkt, charId);
    return pkt;
}

std::string BuildStatsPacket() {
    std::string pkt;
    pkt += (char)0x08;
    pkt += (char)0x30;  // bitmask: 0x20 (max stats) | 0x10 (current stats)
    // 0x20 block: 28 bytes, maxHP at offset +5, maxMP at +9
    pkt.append(5, '\0');
    WriteBE32(pkt, charMaxHP);
    WriteBE32(pkt, charMaxMP);
    pkt.append(15, '\0');  // remaining bytes to reach 28 total (5+4+4+15=28)
    // 0x10 block: 8 bytes
    WriteBE32(pkt, charCurrentHP);
    WriteBE32(pkt, charCurrentMP);
    return pkt;
}

std::string BuildSpellPacket(const SpellInfo& sp) {
    std::string pkt;
    pkt += (char)0x17;
    pkt += (char)(uint8_t)sp.slot;
    WriteBE16(pkt, (uint16_t)sp.icon);
    pkt += (char)(uint8_t)sp.targetType;
    WriteString8(pkt, sp.name);
    WriteString8(pkt, sp.prompt);
    pkt += (char)(uint8_t)sp.lines;
    return pkt;
}

std::string BuildItemPacket(const ItemInfo& item) {
    std::string pkt;
    pkt += (char)0x0F;
    pkt += (char)(uint8_t)item.slot;
    WriteBE16(pkt, (uint16_t)item.sprite);
    pkt += (char)(uint8_t)item.color;
    WriteString8(pkt, item.name);
    WriteBE32(pkt, item.quantity);
    return pkt;
}

void SendPacketToBeryl(struct mg_connection* c, const std::string& pkt) {
    std::string frame(1 + pkt.size(), '\0');
    frame[0] = (char)MSG_SERVER;
    memcpy(&frame[1], pkt.data(), pkt.size());
    mg_ws_send(c, frame.data(), frame.size(), WEBSOCKET_OP_BINARY);
}

void ReplayCharDataToBeryl(struct mg_connection* c) {
    // Caller must hold charDataMutex
    SendPacketToBeryl(c, BuildPlayerIdPacket());
    SendPacketToBeryl(c, BuildStatsPacket());
    for (auto it = charSpells.begin(); it != charSpells.end(); ++it) {
        SendPacketToBeryl(c, BuildSpellPacket(it->second));
    }
    for (auto it = charInventory.begin(); it != charInventory.end(); ++it) {
        SendPacketToBeryl(c, BuildItemPacket(it->second));
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
// Server packet parsing (accumulate character data)
// =============================================================================
void ParseServerPacket(const BYTE* data, DWORD size) {
    if (size < 1) return;
    BYTE opcode = data[0];

    std::lock_guard<std::mutex> lock(charDataMutex);

    switch (opcode) {
    case 0x05: { // playerId -- signals login complete
        if (size >= 5) {
            charId = ReadBE32(data + 1);
            charName = ReadCharName();
            TryRegister();
        }
        break;
    }
    case 0x08: { // statistics
        if (size < 2) break;
        BYTE bitmask = data[1];
        DWORD pos = 2;

        if ((bitmask & 0x20) == 0x20) {
            if (pos + 28 > size) break;
            charMaxHP = ReadBE32(data + pos + 5);
            charMaxMP = ReadBE32(data + pos + 9);
            pos += 28;
        }
        if ((bitmask & 0x10) == 0x10) {
            if (pos + 8 > size) break;
            charCurrentHP = ReadBE32(data + pos);
            charCurrentMP = ReadBE32(data + pos + 4);
        }
        break;
    }
    case 0x18: { // removeSpell
        if (size >= 2) {
            int slot = data[1];
            charSpells.erase(slot);
        }
        break;
    }
    case 0x17: { // loadSpell
        if (size < 2) break;
        DWORD pos = 1;
        int slot = data[pos++];
        if (pos + 3 > size) break;
        int icon = ReadBE16(data + pos); pos += 2;
        int targetType = data[pos++];
        std::string name = ReadString8(data, size, pos);
        std::string prompt = ReadString8(data, size, pos);
        if (pos >= size) break;
        int lines = data[pos++];
        charSpells[slot] = {slot, icon, targetType, lines, name, prompt};
        break;
    }
    case 0x0F: { // addItemToPane
        if (size < 2) break;
        DWORD pos = 1;
        int slot = data[pos++];
        if (pos + 3 > size) break;
        int sprite = ReadBE16(data + pos); pos += 2;
        int color = data[pos++];
        std::string name = ReadString8(data, size, pos);
        if (pos + 4 > size) break;
        uint32_t quantity = ReadBE32(data + pos); pos += 4;
        charInventory[slot] = {slot, sprite, color, name, quantity};
        break;
    }
    case 0x10: { // removeItemFromPane
        if (size >= 2) {
            int slot = data[1];
            charInventory.erase(slot);
        }
        break;
    }
    case 0x4C: { // logout -- reset all accumulated state
        charName.clear();
        charId = 0;
        charSpells.clear();
        charInventory.clear();
        charCurrentHP = charMaxHP = charCurrentMP = charMaxMP = 0;
        charRegistered = false;
        break;
    }
    }
}

// =============================================================================
// Hooks (__fastcall with dummy edx to intercept __thiscall)
// =============================================================================
void __fastcall HookedGameSend(void* thisPtr, void* /*edx*/, const BYTE* data, DWORD size) {
    g_sendThis = thisPtr;
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
        // LogToFile("Client WS: HTTP request received, upgrading to WebSocket");
        mg_ws_upgrade(c, (struct mg_http_message*)ev_data, NULL);
    } else if (ev == MG_EV_WS_OPEN) {
        // LogToFile("Client WS: Beryl connected");
        g_clientConn = c;

        // Send current character data so Beryl has it immediately
        std::lock_guard<std::mutex> lock(charDataMutex);
        if (!charName.empty()) {
            ReplayCharDataToBeryl(c);
        }
    } else if (ev == MG_EV_WS_MSG) {
        struct mg_ws_message* wm = (struct mg_ws_message*)ev_data;
        if (wm->data.len < 1) return;

        BYTE msgType = (BYTE)wm->data.buf[0];

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
        } else if (msgType == MSG_WALLS) {
            // body: [enable(1), width(2 LE), height(2 LE), bitmask(...)]
            if (bodyLen >= 6) {
                bool hide = body[0] != 0;
                uint16_t width = body[1] | (body[2] << 8);
                uint16_t height = body[3] | (body[4] << 8);
                const BYTE* bitmask = body + 5;
                ToggleWalls(hide, width, height, bitmask);
            }
        }
    } else if (ev == MG_EV_ERROR) {
        // LogToFile(std::string("Client error: ") + (const char*)ev_data);
    } else if (ev == MG_EV_CLOSE) {
        if (c == g_clientConn) {
            // LogToFile("Client WS: Beryl disconnected");
            g_clientConn = nullptr;
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
            } else if (type == "deregister") {
                DWORD clientPid = (DWORD)data["pid"];
                RegistryRemoveClient(clientPid);
            } else if (type == "file") {
                uint32_t requestId = (uint32_t)data["id"];
                std::string path = data["path"];
                QueueFileRequest(c, requestId, path);
            }
        } catch (...) {
            // LogToFile("Registry: exception parsing message");
        }
    } else if (ev == MG_EV_CLOSE) {
        // LogToFile("Registry: connection closed");
        // Clear file queue if this connection was sending files
        if (c == g_fileSendConn) {
            g_fileSendConn = nullptr;
            g_fileQueue.clear();
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

            clientPort = StartClientServer();
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
                if (charMaxHP > 0) {
                    charName = ReadCharName();
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
