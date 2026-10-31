// mesh_slots.h - the paired Meshtastic nodes: up to four slots, each live (paired) or kept for its texts (archived). Pure logic, no hardware.
#ifndef MESH_SLOTS_H
#define MESH_SLOTS_H

#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

namespace MeshSlots {

constexpr int MAX = 4;
constexpr uint8_t NONE = 0xFF;
enum : uint8_t { EMPTY = 0, PAIRED = 1, ARCHIVED = 2 };

struct Slot {
  uint8_t state = EMPTY;
  uint8_t reserved = 0;            // unused (kept so the stored table keeps its layout)
  uint8_t type = 0;
  uint8_t addr[6] = {0};
  uint8_t every = 0, flush = 0;    // Background check interval and SD flush choice (settings of Mesh mode), as index + 1; 0: not set yet
  uint32_t nodeId = 0;             // 0 until the node has said who it is
  char name[24] = "";
};

static_assert(sizeof(Slot) == 40, "the stored table is read back as is: the size must not change");

inline bool used(const Slot &s) { return s.state == PAIRED || s.state == ARCHIVED; }

// A stored table is trusted only when every state is known.
inline bool valid(const Slot *s) {
  for (int i = 0; i < MAX; i++) if (s[i].state > ARCHIVED) return false;
  return true;
}

inline int findAddr(const Slot *s, const uint8_t addr[6]) {
  for (int i = 0; i < MAX; i++) if (used(s[i]) && !memcmp(s[i].addr, addr, 6)) return i;
  return -1;
}

inline int findId(const Slot *s, uint32_t id, int except = -1) {
  if (!id) return -1;
  for (int i = 0; i < MAX; i++) if (i != except && used(s[i]) && s[i].nodeId == id) return i;
  return -1;
}

inline int freeSlot(const Slot *s) {
  for (int i = 0; i < MAX; i++) if (!used(s[i])) return i;
  return -1;
}

// The slot to show when the active one goes: the background node, else a paired one, else an archived one.
inline uint8_t nextActive(const Slot *s, uint8_t bg, int except = -1) {
  if (bg < MAX && (int) bg != except && s[bg].state == PAIRED) return bg;
  for (int i = 0; i < MAX; i++) if (i != except && s[i].state == PAIRED) return (uint8_t) i;
  for (int i = 0; i < MAX; i++) if (i != except && s[i].state == ARCHIVED) return (uint8_t) i;
  return NONE;
}

// The node's folder on the SD card.
inline void dirName(char *out, size_t cap, uint32_t id) { snprintf(out, cap, "/Meshtastic/%08x", (unsigned) id); }

}  // namespace MeshSlots

#endif
