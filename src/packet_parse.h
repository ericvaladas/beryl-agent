#pragma once

#include <windows.h>
#include <cstdint>

// Accumulate character state from server packets. Acquires charDataMutex internally.
void ParseServerPacket(const BYTE* data, DWORD size);

// Compute walk destination from origin + direction
void ComputeWalkDestination(uint16_t originX, uint16_t originY, BYTE direction,
                             uint16_t& destX, uint16_t& destY);
