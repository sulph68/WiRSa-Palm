#pragma once
// (In the library folder so the library's own translation unit finds it too.)
// The file system the FTP server serves (the library's STORAGE_MANAGER, see FtpServerKey.h): a root
// directory that lists "SDCard" (when a card is mounted) and "ESP32" (the virtual card, when
// mounted), with each volume's own tree underneath. Nothing but the root is synthetic.
#include <FS.h>

class FtpRootFs : public fs::FS {
 public:
  FtpRootFs();
};

extern FtpRootFs gFtpRootFs;
