#pragma once
// Virtual card: a raw 12 MB flash partition ("vdisk") presented as a 512-byte-sector block device
// with an MBR and one FAT16 volume. Sectors are cached 4 KB at a time in PSRAM and written back on
// vdiskFlush() / vdiskClose(); nothing is kept in RAM between sessions.
#include <stdint.h>
#include <stddef.h>

struct VdiskBackend {            // raw flash access; offsets are within the partition
  bool (*read)(uint32_t off, void *buf, size_t n);
  bool (*erase)(uint32_t off, size_t n);          // n and off multiples of 4096
  bool (*write)(uint32_t off, const void *buf, size_t n);
  uint32_t size;
};

void vdiskSetBackend(const VdiskBackend *b);     // host tests; on the device vdiskOpen() finds the partition
bool vdiskOpen();                                // allocate the cache; false if no partition / no memory
void vdiskClose();                               // flush and free the cache
bool vdiskPresent();                             // the partition exists
uint32_t vdiskSectors();                         // 512-byte sectors (0 if absent)
bool vdiskIsFormatted();                         // MBR + FAT16 boot sector look valid
bool vdiskUsage(uint64_t *used, uint64_t *total);  // bytes in the FAT16 volume: used and total (formatted disk)
bool vdiskPrepare(uint32_t serial);               // open, then format if blank / add the label if missing; true when ready
bool vdiskEnsureLabel();                         // volume name "ESP32" on a formatted disk (keeps its files)
bool vdiskFormat(uint32_t serial);               // write MBR, FAT16 boot sector, FATs, empty root
bool vdiskRead(uint8_t *buf, uint32_t sector, uint32_t count);
bool vdiskWrite(const uint8_t *buf, uint32_t sector, uint32_t count);
bool vdiskFlush();                               // write back every dirty block
uint32_t vdiskDirtyBlocks();
