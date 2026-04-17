#pragma once

#include "json.h"
#include "mongoose.h"

// All of these are meant to be called with charDataMutex held.
json BuildRegistrationPayload();
void TryRegister();
void Deregister();

// Replay accumulated character state over a specific Beryl client connection.
void ReplayCharDataToBeryl(struct mg_connection *c);
