#pragma once
// The virtual card (vdisk.h) as a FAT volume the ESP can open files on, for code written against
// fs::FS (HotSync's installs). Mounted only while it is in use; vdfsUnmount() flushes and frees it.
#include <FS.h>

// Each vdfsMount() that returns true needs one vdfsUnmount(); the volume goes when the last is released.
bool vdfsMount();          // prepares the disk (formats it if blank), registers it with FatFs; false on failure
void vdfsUnmount();        // close every File first; flushes the disk and releases its memory
bool vdfsMounted();
fs::FS &vdfs();            // the volume, valid between mount and unmount
