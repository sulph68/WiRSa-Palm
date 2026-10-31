// mesh_spool_sd.h - Mesh texts on the SD card: /Meshtastic/<node id>/mesh.dat and mesh.old (records of mesh_spool.h, plaintext), one folder per node.
// Used from the main loop thread only (the SD card is shared with the menus and SASD): every call returns false / 0
// without touching the card when another mode owns it or no card answers.
#ifndef MESH_SPOOL_SD_H
#define MESH_SPOOL_SD_H

#include "mesh_spool.h"

namespace MeshSpoolSd {

const unsigned long FILE_MAX = 1048576UL;        // mesh.dat rolls to mesh.old at this size: 1 to 2 MB are kept
const size_t READ_MAX = 65536;                   // the newest bytes read back at a restore

bool append(const uint8_t *enc, size_t n);                     // records already encoded
size_t loadNewest(MeshSpool::Fn fn, void *ctx);                // oldest first; returns how many records
bool zero();                                                   // both files removed
void setNode(uint32_t id);                                     // the files of this node (0: none yet, nothing is read or written)
bool removeNode(uint32_t id);                                  // this node's folder and files

}  // namespace MeshSpoolSd

#endif
