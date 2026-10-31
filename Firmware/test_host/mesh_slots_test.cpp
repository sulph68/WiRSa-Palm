// Host test for mesh_slots.h: finding, picking and the SD folder names.
#include <stdio.h>
#include "mesh_slots.h"
using namespace MeshSlots;

static int fails = 0;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)

static Slot mk(uint8_t st, uint8_t a, uint32_t id) {
  Slot s; s.state = st; for (int i = 0; i < 6; i++) s.addr[i] = a; s.nodeId = id; return s;
}

int main() {
  Slot t[MAX];
  CHECK(freeSlot(t) == 0 && nextActive(t, NONE) == NONE && valid(t));
  t[0] = mk(PAIRED, 1, 0xA1); t[1] = mk(ARCHIVED, 2, 0xB2); t[3] = mk(PAIRED, 4, 0xD4);
  uint8_t a2[6] = {2, 2, 2, 2, 2, 2}, a9[6] = {9, 9, 9, 9, 9, 9};
  CHECK(findAddr(t, a2) == 1 && findAddr(t, a9) == -1);
  CHECK(findId(t, 0xD4) == 3 && findId(t, 0xD4, 3) == -1 && findId(t, 0) == -1 && findId(t, 0xFF) == -1);
  CHECK(freeSlot(t) == 2);
  t[2] = mk(ARCHIVED, 3, 0xC3);
  CHECK(freeSlot(t) == -1);

  CHECK(nextActive(t, 3) == 3);                      // the background node first
  CHECK(nextActive(t, 3, 3) == 0);                   // ... unless it is the one going
  CHECK(nextActive(t, NONE) == 0);                   // else the first paired
  t[0].state = EMPTY; t[3].state = EMPTY;
  CHECK(nextActive(t, NONE) == 1);                   // else the first archived
  CHECK(nextActive(t, 1, 1) == 2);
  t[1].state = EMPTY; t[2].state = EMPTY;
  CHECK(nextActive(t, NONE) == NONE);

  t[2].state = 7;
  CHECK(!valid(t));

  char d[32];
  dirName(d, sizeof d, 0xA1B2C3D4); CHECK(!strcmp(d, "/Meshtastic/a1b2c3d4"));
  dirName(d, sizeof d, 0x2F);       CHECK(!strcmp(d, "/Meshtastic/0000002f"));

  if (fails) { printf("%d failed\n", fails); return 1; }
  printf("mesh_slots: all ok\n");
  return 0;
}
