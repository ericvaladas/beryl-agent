#include "ws_client.h"

#include <cstring>
#include <mutex>
#include <vector>

#include "auth_dialog.h"
#include "game_hooks.h"
#include "packet_io.h"
#include "registration.h"
#include "server_setup.h"
#include "state.h"
#include "ws_registry.h"

// ============================================================================
// File serving (binary response to Beryl file requests)
// ============================================================================
namespace {
struct FileRequest {
  struct mg_connection *c;
  uint32_t requestId;
  std::string path;
};
std::vector<FileRequest> g_fileQueue;

void SendFileResponse(
    struct mg_connection *c,
    uint32_t requestId,
    uint8_t status,
    const char *data = nullptr,
    size_t dataLen = 0
) {
  std::string payload(5 + dataLen, '\0');
  memcpy(&payload[0], &requestId, 4); // LE uint32
  payload[4] = (char)status;
  if (data && dataLen > 0) {
    memcpy(&payload[5], data, dataLen);
  }
  mg_ws_send(c, payload.data(), payload.size(), WEBSOCKET_OP_BINARY);
}

void HandleFileRequest(
    struct mg_connection *c, uint32_t requestId, const std::string &relativePath
) {
  std::string winPath = relativePath;
  for (auto &ch : winPath) {
    if (ch == '/')
      ch = '\\';
  }

  // Canonicalize the requested path and require it to sit under the DLL's
  // directory. GetFullPathNameA collapses ".." and resolves absolute, UNC, and
  // drive-relative paths — so one prefix check covers all the escape variants.
  std::string joined = g_dllDirectory + "\\" + winPath;
  char resolved[MAX_PATH];
  DWORD rLen = GetFullPathNameA(joined.c_str(), MAX_PATH, resolved, NULL);
  std::string basePrefix = g_dllDirectory + "\\";
  if (rLen == 0 || rLen >= MAX_PATH ||
      _strnicmp(resolved, basePrefix.c_str(), basePrefix.size()) != 0) {
    SendFileResponse(c, requestId, 0x02);
    return;
  }

  HANDLE h = CreateFileA(
      resolved,
      GENERIC_READ,
      FILE_SHARE_READ,
      NULL,
      OPEN_EXISTING,
      FILE_ATTRIBUTE_NORMAL,
      NULL
  );
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
} // namespace

struct mg_connection *g_fileSendConn = nullptr;

void ProcessNextFile() {
  if (g_fileQueue.empty()) {
    g_fileSendConn = nullptr;
    return;
  }
  FileRequest req = std::move(g_fileQueue.front());
  g_fileQueue.erase(g_fileQueue.begin());
  HandleFileRequest(req.c, req.requestId, req.path);
}

void QueueFileRequest(
    struct mg_connection *c, uint32_t requestId, const std::string &path
) {
  if (g_fileSendConn == nullptr) {
    HandleFileRequest(c, requestId, path);
  } else {
    g_fileQueue.push_back({c, requestId, path});
  }
}

// ============================================================================
// Client WebSocket handler (per-DLL, binary packets to/from Beryl)
// ============================================================================
static void
HandleMemoryRead(struct mg_connection *c, const BYTE *body, DWORD bodyLen) {
  if (bodyLen < 6)
    return;
  BYTE requestId = body[0];
  BYTE chainLength = body[1];
  DWORD expectedLen = 2 + (DWORD)chainLength * 4 + 4;
  if (bodyLen < expectedLen || chainLength == 0)
    return;

  const BYTE *offsets = body + 2;
  DWORD size = *(DWORD *)(body + 2 + chainLength * 4);

  BYTE *addr = ResolvePointerChain(offsets, chainLength);

  std::vector<BYTE> response(2 + size);
  response[0] = MSG_READ_MEMORY;
  response[1] = requestId;
  if (addr && !IsBadReadPtr(addr, size)) {
    memcpy(response.data() + 2, addr, size);
  }
  mg_ws_send(
      c, (const char *)response.data(), response.size(), WEBSOCKET_OP_BINARY
  );
}

static void
HandleMemoryWrite(struct mg_connection *c, const BYTE *body, DWORD bodyLen) {
  if (bodyLen < 6)
    return;
  BYTE requestId = body[0];
  BYTE chainLength = body[1];
  DWORD headerLen = 2 + (DWORD)chainLength * 4 + 4;
  if (bodyLen < headerLen || chainLength == 0)
    return;

  DWORD size = *(DWORD *)(body + 2 + chainLength * 4);
  const BYTE *data = body + headerLen;

  BYTE status = 1; // failed
  const BYTE *offsets = body + 2;
  if (bodyLen >= headerLen + size) {
    BYTE *addr = ResolvePointerChain(offsets, chainLength);
    if (addr && !IsBadWritePtr(addr, size)) {
      memcpy(addr, data, size);
      status = 0; // ok
    }
  }

  BYTE response[3] = {MSG_WRITE_MEMORY, requestId, status};
  mg_ws_send(c, (const char *)response, 3, WEBSOCKET_OP_BINARY);
}

void ClientHandler(struct mg_connection *c, int ev, void *ev_data) {
  if (ev == MG_EV_HTTP_MSG) {
    struct mg_http_message *hm = (struct mg_http_message *)ev_data;
    struct mg_str *origin = mg_http_get_header(hm, "Origin");
    g_pendingOrigin =
        origin ? std::string(origin->buf, origin->len) : "unknown";
    mg_ws_upgrade(c, hm, NULL);
  } else if (ev == MG_EV_WS_OPEN) {
    if (g_authDialogActive) {
      if (!g_pendingConn && g_pendingOrigin == g_dialogOrigin) {
        g_pendingConn = c;
        return;
      }
      c->is_closing = 1;
      return;
    }

    if (g_clientConn) {
      c->is_closing = 1;
      return;
    }

    if (!g_recvThis) {
      c->is_closing = 1;
      return;
    }

    if (g_allowedOrigins.count(g_pendingOrigin)) {
      PromoteConnection(c);
      return;
    }

    g_pendingConn = c;
    ShowAuthDialog(g_pendingOrigin);
  } else if (ev == MG_EV_WS_MSG) {
    struct mg_ws_message *wm = (struct mg_ws_message *)ev_data;
    if (wm->data.len < 1)
      return;

    BYTE msgType = (BYTE)wm->data.buf[0];

    // Only allow MSG_BECOME_REG from unapproved connections
    if (c != g_clientConn && msgType != MSG_BECOME_REG)
      return;

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

    if (wm->data.len < 2)
      return;

    const BYTE *body = (const BYTE *)wm->data.buf + 1;
    DWORD bodyLen = (DWORD)wm->data.len - 1;

    if (msgType == MSG_CLIENT) {
      void *thisPtr = g_sendThis;
      if (!thisPtr)
        thisPtr = *(void **)SEND_THIS_ADDR;
      if (thisPtr) {
        OrigGameSend(thisPtr, body, bodyLen);
      }
    } else if (msgType == MSG_SERVER) {
      void *thisPtr = g_recvThis;
      if (thisPtr) {
        OrigGameRecv(thisPtr, body, bodyLen);
      }
    } else if (msgType == MSG_WALK) {
      if (bodyLen >= 1) {
        void *thisPtr = *(void **)OBJECT_BASE_ADDR;
        if (thisPtr) {
          GameWalk(thisPtr, (int)body[0]);
        }
      }
    } else if (msgType == MSG_READ_MEMORY) {
      HandleMemoryRead(c, body, bodyLen);
    } else if (msgType == MSG_WRITE_MEMORY) {
      HandleMemoryWrite(c, body, bodyLen);
    }
  } else if (ev == MG_EV_WAKEUP) {
    // Forwarded from game thread via mg_wakeup — send to Beryl on mongoose
    // thread
    struct mg_str *data = (struct mg_str *)ev_data;
    if (g_clientConn) {
      mg_ws_send(g_clientConn, data->buf, data->len, WEBSOCKET_OP_BINARY);
    }
  } else if (ev == MG_EV_CLOSE) {
    if (c == g_clientConn) {
      g_clientConn = nullptr;
      g_clientOrigin.clear();
      if (c == g_fileSendConn) {
        g_fileSendConn = nullptr;
        g_fileQueue.clear();
      }
    } else if (c == g_pendingConn) {
      g_pendingConn = nullptr;
    }
  }
}
