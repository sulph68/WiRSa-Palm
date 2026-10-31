#include "ftp_rootfs.h"
#include "vdfs.h"
#include <SD.h>
#include <vfs_api.h>
#include <strings.h>
#include <memory>

// Folder names at the root, and the VFS mount points they stand for (sd_mount.h, vdfs.cpp).
static const char kSdName[] = "SDCard";
static const char kVdName[] = "ESP32";

static bool sdPresent() { return SD.cardType() != CARD_NONE; }

enum { P_ROOT = 0, P_SD = 1, P_VD = 2, P_NONE = -1 };

// Which volume a path belongs to; *rest is what follows the volume's folder name ("" or "/...").
static int classify(const char *path, const char **rest) {
  *rest = "";
  if (!path || path[0] != '/') return P_NONE;
  const char *p = path + 1;
  if (!*p) return P_ROOT;
  size_t n = strcspn(p, "/");
  *rest = p + n;
  if (n == strlen(kSdName) && !strncasecmp(p, kSdName, n)) return sdPresent() ? P_SD : P_NONE;
  if (n == strlen(kVdName) && !strncasecmp(p, kVdName, n)) return vdfsMounted() ? P_VD : P_NONE;
  return P_NONE;
}

// The real VFS path: canonical folder name + the rest ("/" for the volume's own root).
static String forward(int vol, const char *rest) {
  String s = "/";
  s += vol == P_SD ? kSdName : kVdName;
  s += (*rest && !(rest[0] == '/' && !rest[1])) ? rest : "/";
  return s;
}

static bool atVolumeRoot(const char *rest) { return !*rest || (rest[0] == '/' && !rest[1]); }

// One folder at the root.
class RootEntry : public fs::FileImpl {
  String _name, _path;
 public:
  explicit RootEntry(const char *name) : _name(name), _path(String("/") + name) {}
  size_t write(const uint8_t *, size_t) override { return 0; }
  size_t read(uint8_t *, size_t) override { return 0; }
  void flush() override {}
  bool seek(uint32_t, SeekMode) override { return false; }
  size_t position() const override { return 0; }
  size_t size() const override { return 0; }
  bool setBufferSize(size_t) override { return false; }
  void close() override {}
  time_t getLastWrite() override { return 0; }
  const char *path() const override { return _path.c_str(); }
  const char *name() const override { return _name.c_str(); }
  boolean isDirectory() override { return true; }
  fs::FileImplPtr openNextFile(const char *) override { return fs::FileImplPtr(); }
  boolean seekDir(long) override { return false; }
  String getNextFileName() override { return String(); }
  String getNextFileName(bool *) override { return String(); }
  void rewindDirectory() override {}
  operator bool() override { return true; }
};

// The root directory: its entries are the volumes that are present.
class RootDir : public RootEntry {
  const char *_names[2];
  int _n = 0, _i = 0;
 public:
  RootDir() : RootEntry("/") {
    if (sdPresent()) _names[_n++] = kSdName;
    if (vdfsMounted()) _names[_n++] = kVdName;
  }
  fs::FileImplPtr openNextFile(const char *) override {
    return _i < _n ? fs::FileImplPtr(new RootEntry(_names[_i++])) : fs::FileImplPtr();
  }
  String getNextFileName() override { return _i < _n ? String(_names[_i++]) : String(); }
  String getNextFileName(bool *isDir) override { if (isDir) *isDir = true; return getNextFileName(); }
  void rewindDirectory() override { _i = 0; }
};

class RootImpl : public VFSImpl {
 public:
  RootImpl() { mountpoint(""); }   // paths go to the VFS as given: /SDCard/... and /ESP32/...

  fs::FileImplPtr open(const char *path, const char *mode, const bool create) override {
    const char *rest;
    int v = classify(path, &rest);
    if (v == P_ROOT) return (mode && mode[0] == 'r') ? fs::FileImplPtr(new RootDir()) : fs::FileImplPtr();
    if (v == P_NONE) return fs::FileImplPtr();
    return VFSImpl::open(forward(v, rest).c_str(), mode, create);
  }
  bool exists(const char *path) override {
    const char *rest;
    int v = classify(path, &rest);
    if (v == P_ROOT) return true;
    if (v == P_NONE) return false;
    return atVolumeRoot(rest) ? true : VFSImpl::exists(forward(v, rest).c_str());
  }
  // The volume folders themselves are not changeable; inside one, the volume's own rules apply.
  bool rename(const char *from, const char *to) override {
    const char *r1, *r2;
    int a = classify(from, &r1), b = classify(to, &r2);
    if (a < P_SD || a != b || atVolumeRoot(r1) || atVolumeRoot(r2)) return false;
    return VFSImpl::rename(forward(a, r1).c_str(), forward(b, r2).c_str());
  }
  bool remove(const char *path) override { return inside(path, 0); }
  bool mkdir(const char *path) override { return inside(path, 1); }
  bool rmdir(const char *path) override { return inside(path, 2); }

 private:
  bool inside(const char *path, int what) {
    const char *rest;
    int v = classify(path, &rest);
    if (v < P_SD || atVolumeRoot(rest)) return false;
    String f = forward(v, rest);
    return what == 0 ? VFSImpl::remove(f.c_str()) : what == 1 ? VFSImpl::mkdir(f.c_str()) : VFSImpl::rmdir(f.c_str());
  }
};

FtpRootFs::FtpRootFs() : FS(fs::FSImplPtr(new RootImpl())) {}
FtpRootFs gFtpRootFs;
