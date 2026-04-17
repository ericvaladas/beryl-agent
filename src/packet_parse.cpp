#include "packet_parse.h"

#include <cstring>
#include <map>
#include <mutex>

#include "packet_io.h"
#include "registration.h"
#include "state.h"

void ComputeWalkDestination(
    uint16_t originX,
    uint16_t originY,
    BYTE direction,
    uint16_t &destX,
    uint16_t &destY
) {
  destX = originX;
  destY = originY;
  switch (direction) {
  case 0:
    destY = originY - 1;
    break;
  case 1:
    destX = originX + 1;
    break;
  case 2:
    destY = originY + 1;
    break;
  case 3:
    destX = originX - 1;
    break;
  }
}

// Map opcode -> (add target, remove target) for slot-keyed inventory packets
namespace {
struct SlotMapBinding {
  BYTE addOp;
  BYTE removeOp;
  std::map<int, std::string> *store;
};

const SlotMapBinding kSlotBindings[] = {
    {0x17, 0x18, &storedSpells},
    {0x0F, 0x10, &storedItems},
    {0x2C, 0x2D, &storedSkills},
    {0x37, 0x38, &storedEquipment},
};

// Caller holds charDataMutex. Update entity position+direction in
// storedShowUsers.
void PatchShowUserPosition(
    uint32_t entityId, uint16_t destX, uint16_t destY, BYTE direction
) {
  auto sit = storedShowUsers.find(entityId);
  if (sit != storedShowUsers.end() && sit->second.size() >= 10) {
    PatchBE16(sit->second, 1, destX);
    PatchBE16(sit->second, 3, destY);
    sit->second[5] = (char)direction;
  }
}

// Caller holds charDataMutex. Update entity direction in storedShowUsers.
void PatchShowUserDirection(uint32_t entityId, BYTE direction) {
  auto sit = storedShowUsers.find(entityId);
  if (sit != storedShowUsers.end() && sit->second.size() >= 6) {
    sit->second[5] = (char)direction;
  }
}
} // namespace

void ParseServerPacket(const BYTE *data, DWORD size) {
  if (size < 1)
    return;
  BYTE opcode = data[0];

  std::lock_guard<std::mutex> lock(charDataMutex);

  // Table-driven add/remove for slot-keyed inventory packets
  for (const auto &b : kSlotBindings) {
    if (opcode == b.addOp) {
      if (size < 2)
        return;
      (*b.store)[data[1]] = std::string((char *)data, size);
      return;
    }
    if (opcode == b.removeOp) {
      if (size < 2)
        return;
      b.store->erase(data[1]);
      return;
    }
  }

  switch (opcode) {
  case 0x03: { // redirect — extract character name, deregister if changed
    if (size < 10)
      break;
    DWORD pos = 9;
    BYTE keyLength = data[pos++];
    pos += keyLength;
    std::string newName = ReadString8(data, size, pos);
    if (newName != charName) {
      Deregister();
    }
    charName = newName;
    break;
  }
  case 0x05: { // playerId — signals login complete
    if (size >= 5) {
      storedPackets[opcode] = std::string((char *)data, size);
      charId = ReadBE32(data + 1);
      TryRegister();
    }
    break;
  }
  case 0x08: { // statistics — patch into stored 0x30 packet
    if (size < 2)
      break;
    BYTE bitmask = data[1];
    DWORD pos = 2;

    if (storedStats.empty()) {
      storedStats.assign(38, '\0');
      storedStats[0] = (char)0x08;
      storedStats[1] = (char)0x30;
    }

    if ((bitmask & 0x20) == 0x20) {
      if (pos + 28 > size)
        break;
      memcpy(&storedStats[2], data + pos, 28);
      pos += 28;
    }
    if ((bitmask & 0x10) == 0x10) {
      if (pos + 8 > size)
        break;
      memcpy(&storedStats[30], data + pos, 8);
    }
    break;
  }
  case 0x4C: { // logout — deregister and reset all accumulated state
    Deregister();
    charName.clear();
    charId = 0;
    storedPackets.clear();
    storedStats.clear();
    storedSpells.clear();
    storedItems.clear();
    storedSkills.clear();
    storedEquipment.clear();
    storedEntities.clear();
    storedShowUsers.clear();
    g_currentDialog.clear();
    g_suspendedDialog.clear();
    break;
  }
  case 0x15: // mapInfo — store and clear entities (new map)
    storedPackets[opcode] = std::string((char *)data, size);
    storedEntities.clear();
    storedShowUsers.clear();
    break;
  case 0x04: // mapLocation
  case 0x20: // lightLevel
  case 0x32: // mapDoor
  case 0x6f: // metadata
    storedPackets[opcode] = std::string((char *)data, size);
    break;
  case 0x33: { // showUser
    if (size >= 10) {
      uint32_t entityId = ReadBE32(data + 6);
      storedShowUsers[entityId] = std::string((char *)data, size);
    }
    break;
  }
  case 0x07: { // addEntity — split into per-entity slices keyed by ID
    if (size < 3)
      break;
    uint16_t entityCount = ReadBE16(data + 1);
    DWORD pos = 3;
    for (uint16_t i = 0; i < entityCount; i++) {
      DWORD entityStart = pos;
      if (pos + 10 > size)
        break;
      uint32_t entityId = ReadBE32(data + pos + 4);
      uint16_t sprite = ReadBE16(data + pos + 8);
      pos += 10;
      if (sprite & 0x4000) { // creature
        if (pos + 7 > size)
          break;
        BYTE creatureType = data[pos + 6];
        pos += 7;
        if (creatureType == 2) {
          if (pos >= size)
            break;
          uint8_t nameLen = data[pos];
          pos += 1 + nameLen;
        }
      } else if (sprite & 0x8000) { // item
        pos += 3;
      } else {
        break;
      }
      if (pos > size)
        break;
      storedEntities[entityId] =
          std::string((char *)data + entityStart, pos - entityStart);
    }
    break;
  }
  case 0x0B: { // walkResponse — update current player's stored ShowUser
    if (size < 6)
      break;
    BYTE direction = data[1];
    uint16_t originX = ReadBE16(data + 2);
    uint16_t originY = ReadBE16(data + 4);
    uint16_t destX, destY;
    ComputeWalkDestination(originX, originY, direction, destX, destY);
    PatchShowUserPosition(charId, destX, destY, direction);
    break;
  }
  case 0x0C: { // entityWalk — update stored entity position and direction
    if (size < 10)
      break;
    uint32_t entityId = ReadBE32(data + 1);
    uint16_t originX = ReadBE16(data + 5);
    uint16_t originY = ReadBE16(data + 7);
    BYTE direction = data[9];
    uint16_t destX, destY;
    ComputeWalkDestination(originX, originY, direction, destX, destY);

    auto eit = storedEntities.find(entityId);
    if (eit != storedEntities.end() && eit->second.size() >= 15) {
      uint16_t sprite = ReadBE16((const BYTE *)eit->second.data() + 8);
      if (sprite & 0x4000) {
        PatchBE16(eit->second, 0, destX);
        PatchBE16(eit->second, 2, destY);
        eit->second[14] = (char)direction;
      }
    }
    PatchShowUserPosition(entityId, destX, destY, direction);
    break;
  }
  case 0x11: { // entityTurn — update stored entity direction
    if (size < 6)
      break;
    uint32_t entityId = ReadBE32(data + 1);
    BYTE direction = data[5];

    auto eit = storedEntities.find(entityId);
    if (eit != storedEntities.end() && eit->second.size() >= 15) {
      uint16_t sprite = ReadBE16((const BYTE *)eit->second.data() + 8);
      if (sprite & 0x4000) {
        eit->second[14] = (char)direction;
      }
    }
    PatchShowUserDirection(entityId, direction);
    break;
  }
  case 0x30: { // ShowDialog — track for auth dialog replay
    if (size >= 2 && data[1] == 0x0A) {
      g_currentDialog.clear();
    } else {
      g_currentDialog.assign((char *)data, size);
    }
    break;
  }
  case 0x31: // ShowDialogMenu — track for auth dialog replay
    g_currentDialog.assign((char *)data, size);
    break;
  case 0x0E: { // removeEntity — erase from both maps
    if (size < 5)
      break;
    uint32_t entityId = ReadBE32(data + 1);
    storedEntities.erase(entityId);
    storedShowUsers.erase(entityId);
    break;
  }
  }
}
