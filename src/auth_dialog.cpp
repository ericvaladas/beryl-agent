#include "auth_dialog.h"

#include <fstream>
#include <mutex>

#include "game_hooks.h"
#include "packet_io.h"
#include "registration.h"
#include "state.h"

bool g_authDialogActive = false;
struct mg_connection* g_pendingConn = nullptr;
std::string g_pendingOrigin;

std::set<std::string> g_allowedOrigins;
std::string g_allowedOriginsPath;

namespace {
constexpr uint16_t AUTH_PURSUIT_ID    = 0xFFFF;
constexpr uint32_t AUTH_ENTITY_ID     = 0xFFFFFFFF;
constexpr uint16_t AUTH_ENTITY_SPRITE = 0x80E8;
}

void InjectServerPacket(const std::string& pkt) {
    void* thisPtr = g_recvThis;
    if (thisPtr) {
        OrigGameRecv(thisPtr, (const BYTE*)pkt.data(), (DWORD)pkt.size());
    }
}

void ShowAuthDialog(const std::string& origin) {
    {
        std::lock_guard<std::mutex> lock(charDataMutex);
        g_suspendedDialog = g_currentDialog;
    }

    std::string pkt;
    pkt += (char)0x30;        // opcode: ShowDialog
    pkt += (char)0x02;        // DialogType::Menu
    pkt += (char)0x00;        // EntityType
    WriteBE32(pkt, AUTH_ENTITY_ID);
    pkt += (char)0x01;        // Unknown1
    WriteBE16(pkt, AUTH_ENTITY_SPRITE);
    pkt += (char)0x00;        // Color
    pkt += (char)0x01;        // Unknown2
    WriteBE16(pkt, 0x0000);   // SpriteSecondary
    pkt += (char)0x00;        // ColorSecondary
    WriteBE16(pkt, AUTH_PURSUIT_ID);
    WriteBE16(pkt, 0x0000);   // StepId
    pkt += (char)0x00;        // HasPreviousButton
    pkt += (char)0x00;        // HasNextButton
    pkt += (char)0x01;        // ShowGraphic (inverted: 1 = hide)
    WriteString8(pkt, "Beryl");
    std::string content = "Allow connections from " + origin + "?";
    WriteBE16(pkt, (uint16_t)content.size());
    pkt += content;
    pkt += (char)0x02;        // ChoiceCount
    WriteString8(pkt, "Yes");
    WriteString8(pkt, "No");

    InjectServerPacket(pkt);
    g_authDialogActive = true;
}

void CloseAuthDialog() {
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

void ResetAuthState() {
    g_authDialogActive = false;
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

bool ShouldSuppressClientPacket(const BYTE* data, DWORD size) {
    if (size < 1) return false;

    // Intercept dialog response (opcode 0x3A) for our auth dialog
    if (g_authDialogActive && data[0] == 0x3A && size >= 10) {
        uint16_t pursuitId = ReadBE16(data + 6);
        if (pursuitId != AUTH_PURSUIT_ID) return false;

        bool accepted = false;
        if (size >= 12) {
            BYTE argsType = data[10];
            BYTE menuChoice = data[11];
            accepted = (argsType == 0x01 && menuChoice == 1);
        }

        CloseAuthDialog();

        std::string toReplay;
        {
            std::lock_guard<std::mutex> lock(charDataMutex);
            toReplay.swap(g_suspendedDialog);
        }
        if (!toReplay.empty()) {
            InjectServerPacket(toReplay);
        }

        if (accepted && g_pendingConn) {
            g_allowedOrigins.insert(g_pendingOrigin);
            SaveAllowedOrigins();
            PromoteConnection(g_pendingConn);
        } else if (g_pendingConn) {
            g_pendingConn->is_closing = 1;
        }

        ResetAuthState();
        return true;
    }

    // Track real (non-auth) dialog responses so we stop tracking a dialog
    // the client has dismissed.
    if (data[0] == 0x3A) {
        std::lock_guard<std::mutex> lock(charDataMutex);
        g_currentDialog.clear();
    }

    return false;
}
