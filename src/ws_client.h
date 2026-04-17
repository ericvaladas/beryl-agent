#pragma once

#include <cstdint>
#include <string>

#include "mongoose.h"

void ClientHandler(struct mg_connection *c, int ev, void *ev_data);

// File-serving queue (mongoose thread). ProcessNextFile is driven by the poll
// loop.
extern struct mg_connection *g_fileSendConn;
void QueueFileRequest(
    struct mg_connection *c, uint32_t requestId, const std::string &path
);
void ProcessNextFile();
