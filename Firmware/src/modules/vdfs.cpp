#include "vdfs.h"
#include "vdisk.h"
#include "globals.h"
#include <vfs_api.h>
extern "C" {
#include "diskio_impl.h"
#include "ff.h"
#include "esp_vfs_fat.h"
}

// FatFs disk driver over the virtual card. A file close or f_sync reaches CTRL_SYNC, which writes
// the cache back to flash.
static DSTATUS vInit(unsigned char) { return 0; }
static DSTATUS vStatus(unsigned char) { return 0; }
static DRESULT vRead(unsigned char, unsigned char *b, uint32_t sec, unsigned n) { return vdiskRead(b, sec, n) ? RES_OK : RES_ERROR; }
static DRESULT vWrite(unsigned char, const unsigned char *b, uint32_t sec, unsigned n) { return vdiskWrite(b, sec, n) ? RES_OK : RES_ERROR; }
static DRESULT vIoctl(unsigned char, unsigned char cmd, void *buf) {
  switch (cmd) {
    case CTRL_SYNC:        return vdiskFlush() ? RES_OK : RES_ERROR;
    case GET_SECTOR_COUNT: *(DWORD *) buf = vdiskSectors(); return RES_OK;
    case GET_SECTOR_SIZE:  *(WORD *) buf = 512; return RES_OK;
    case GET_BLOCK_SIZE:   *(DWORD *) buf = 8; return RES_OK;
    default:               return RES_PARERR;
  }
}
static const ff_diskio_impl_t kImpl = { vInit, vStatus, vRead, vWrite, vIoctl };

static const char kMount[] = "/ESP32";   // also the folder name the FTP server shows (ftp_rootfs.cpp)
static uint8_t gRefs = 0;               // HotSync and the FTP server can both hold it

class VdFS : public fs::FS {
 public:
  VdFS() : FS(fs::FSImplPtr(new VFSImpl())) {}
  void mountpoint(const char *p) { ((VFSImpl *) _impl.get())->mountpoint(p); }
};
static VdFS gVdFs;
static uint8_t gPdrv = 0xFF;

bool vdfsMount() {
  if (gPdrv != 0xFF) { gRefs++; return true; }
  uint8_t pdrv = 0xFF;
  if (ff_diskio_get_drive(&pdrv) != ESP_OK || pdrv == 0xFF) { LOGW("VDISK: no free FatFs drive\r\n"); return false; }
  if (!vdiskPrepare((uint32_t) ESP.getEfuseMac())) { LOGW("VDISK: could not set up the virtual card\r\n"); return false; }
  ff_diskio_register(pdrv, &kImpl);
  char drv[3] = { (char) ('0' + pdrv), ':', 0 };
  FATFS *fsx;
  esp_err_t e = esp_vfs_fat_register(kMount, drv, 5, &fsx);
  if (e != ESP_OK) { LOGW("VDISK: FatFs register failed (%d)\r\n", (int) e); ff_diskio_unregister(pdrv); vdiskClose(); return false; }
  FRESULT r = f_mount(fsx, drv, 1);
  if (r != FR_OK) {
    LOGW("VDISK: FatFs mount failed (%d)\r\n", (int) r);
    esp_vfs_fat_unregister_path(kMount);
    ff_diskio_unregister(pdrv);
    vdiskClose();
    return false;
  }
  gVdFs.mountpoint(kMount);
  gPdrv = pdrv;
  gRefs = 1;
  return true;
}

void vdfsUnmount() {
  if (gPdrv == 0xFF) return;
  if (gRefs > 1) { gRefs--; return; }
  gRefs = 0;
  char drv[3] = { (char) ('0' + gPdrv), ':', 0 };
  f_mount(NULL, drv, 0);
  esp_vfs_fat_unregister_path(kMount);
  ff_diskio_unregister(gPdrv);
  gPdrv = 0xFF;
  vdiskClose();
}

fs::FS &vdfs() { return gVdFs; }
bool vdfsMounted() { return gPdrv != 0xFF; }
