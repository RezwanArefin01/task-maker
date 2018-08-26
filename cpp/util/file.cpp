#include "util/file.hpp"
#include "util/flags.hpp"
#include "util/sha256.hpp"

#include <cstdlib>

#include <kj/async.h>
#include <kj/debug.h>
#include <kj/exception.h>
#include <algorithm>
#include <fstream>
#include <system_error>

#if defined(__unix__) || defined(__linux__) || defined(__APPLE__)
#include <fcntl.h>
#include <ftw.h>
#include <sys/stat.h>
#include <unistd.h>

// if REMOVE_ALSO_MOUNT_POINTS is set remove also the mount points mounted in
// the sandbox when cleaning
#ifdef REMOVE_ALSO_MOUNT_POINTS
#define NFTW_EXTRA_FLAGS 0
#else
#define NFTW_EXTRA_FLAGS FTW_MOUNT
#endif

namespace {

const constexpr char* kPathSeparators = "/";

bool MkDir(const std::string& dir) {
  return mkdir(dir.c_str(), S_IRWXU | S_IRWXG | S_IXOTH) != -1 ||
         errno == EEXIST;
}

bool OsRemove(const std::string& path) { return remove(path.c_str()) != -1; }

std::vector<std::string> OsListFiles(const std::string& path) {
  thread_local std::vector<std::pair<long, std::string>> files;
  KJ_ASSERT(nftw(path.c_str(),
                 [](const char* fpath, const struct stat* sb, int typeflags,
                    struct FTW* ftwbuf) {
                   if (typeflags != FTW_F) return 0;
                   files.emplace_back(sb->st_atim.tv_sec, fpath);
                   return 0;
                 },
                 64, FTW_DEPTH | FTW_PHYS | NFTW_EXTRA_FLAGS) != -1);
  std::sort(files.begin(), files.end());
  std::vector<std::string> ret;
  ret.reserve(files.size());
  for (auto& p : files) ret.push_back(std::move(p.second));
  files.clear();
  return ret;
};

bool OsRemoveTree(const std::string& path) {
  return nftw(path.c_str(),
              [](const char* fpath, const struct stat* sb, int typeflags,
                 struct FTW* ftwbuf) { return remove(fpath); },
              64, FTW_DEPTH | FTW_PHYS | NFTW_EXTRA_FLAGS) != -1;
}

bool OsMakeExecutable(const std::string& path) {
  return chmod(path.c_str(), S_IRUSR | S_IXUSR) != -1;
}

bool OsMakeImmutable(const std::string& path) {
  return chmod(path.c_str(), S_IRUSR) != -1;
}

const size_t max_path_len = 1 << 15;
std::string OsTempDir(const std::string& path) {
  std::string tmp = util::File::JoinPath(path, "XXXXXX");
  KJ_REQUIRE(tmp.size() < max_path_len, tmp.size(), max_path_len,
             "Path too long");
  char data[max_path_len + 1];
  data[0] = 0;
  strncat(data, tmp.c_str(), max_path_len - 1);  // NOLINT
  if (mkdtemp(data) == nullptr)                  // NOLINT
    return "";
  return data;  // NOLINT
}

int OsTempFile(const std::string& path, std::string* tmp) {
#ifdef __APPLE__
  *tmp = path + ".";
  do {
    *tmp += 'a' + rand() % 26;
    int fd =
        open(tmp->c_str(), O_WRONLY | O_CREAT | O_CLOEXEC, S_IRUSR | S_IWUSR);
    if (fd == -1 && errno == EEXIST) continue;
    return fd;
  } while (true);
#else
  *tmp = path + ".XXXXXX";
  char data[max_path_len];
  data[0] = 0;
  strncat(data, tmp->c_str(), max_path_len - 1);  // NOLINT
  int fd = mkostemp(data, O_CLOEXEC);             // NOLINT
  *tmp = data;                                    // NOLINT
  return fd;
#endif
}

// Returns errno, or 0 on success.
int OsAtomicMove(const std::string& src, const std::string& dst,
                 bool overwrite = false, bool exist_ok = true) {
  // This may not have the desired effect if src is a symlink.
  if (overwrite) {
    if (rename(src.c_str(), dst.c_str()) == -1) return errno;
    return 0;
  }
  if (link(src.c_str(), dst.c_str()) == -1) {
    if (!exist_ok || errno != EEXIST) return errno;
    return 0;
  }
  if (remove(src.c_str()) == -1) return errno != ENOENT ? errno : 0;
  return 0;
}

bool OsIsLink(const std::string& path) {
  struct stat buf;
  if (lstat(path.c_str(), &buf) == -1) return false;
  return S_ISLNK(buf.st_mode);
}

// Returns errno, or 0 on success.
int OsAtomicCopy(const std::string& src, const std::string& dst,
                 bool overwrite = false, bool exist_ok = true) {
  if (link(src.c_str(), dst.c_str()) == -1) {
    if (errno != EEXIST) return errno;
    if (exist_ok) return 0;
    if (!overwrite) return errno;
    if (!OsRemove(dst)) return errno;
    if (link(src.c_str(), dst.c_str()) == -1) return errno;
    return 0;
  }
  return 0;
}

util::File::ChunkProducer OsRead(const std::string& path) {
  int fd = open(path.c_str(), O_CLOEXEC | O_RDONLY);  // NOLINT
  if (fd == -1) {
    throw std::system_error(errno, std::system_category(), "Read " + path);
  }
  return [fd, path, buf = std::array<kj::byte, util::kChunkSize>()]() mutable {
    if (fd == -1) return util::File::Chunk();
    ssize_t amount;
    try {
      while ((amount = read(fd, buf.data(), util::kChunkSize))) {  // NOLINT
        if (amount == -1 && errno == EINTR) continue;
        if (amount == -1) break;
        return util::File::Chunk(buf.data(), amount);
      }
    } catch (...) {
      fd = -1;
      close(fd);
      throw;
    }
    if (amount == -1) {
      fd = -1;
      int error = errno;
      close(fd);
      throw std::system_error(error, std::system_category(), "Read " + path);
    }
    if (close(fd) == -1) {
      fd = -1;
      throw std::system_error(errno, std::system_category(), "Read " + path);
    }
    fd = -1;
    return util::File::Chunk();
  };
}

util::File::ChunkReceiver OsWrite(const std::string& path, bool overwrite,
                                  bool exist_ok) {
  std::string temp_file;
  int fd = OsTempFile(path, &temp_file);

  if (fd == -1) {
    throw std::system_error(errno, std::system_category(), "Write " + path);
  }
  auto done = kj::heap<bool>();
  auto finalize = [done = done.get(), temp_file]() {
    if (!*done) {
      kj::UnwindDetector detector;
      detector.catchExceptionsIfUnwinding(
          [temp_file]() { util::File::Remove(temp_file); });
      KJ_LOG(WARNING, "File never finalized!");
    }
  };
  return [fd, temp_file, path, overwrite, exist_ok, done = std::move(done),
          _ = kj::defer(std::move(finalize))](util::File::Chunk chunk) mutable {
    if (fd == -1) return;
    if (chunk.size() == 0) {
      *done = true;
      if (fsync(fd) == -1 || close(fd) == -1 ||
          OsAtomicMove(temp_file, path, overwrite, exist_ok)) {
        throw std::system_error(errno, std::system_category(), "Write " + path);
      }
      return;
    }
    size_t pos = 0;
    while (pos < chunk.size()) {
      ssize_t written = write(fd, chunk.begin() + pos,  // NOLINT
                              chunk.size() - pos);
      if (written == -1 && errno == EINTR) continue;
      if (written == -1) {
        close(fd);
        fd = -1;
        throw std::system_error(errno, std::system_category(),
                                "write " + temp_file);
      }
      pos += written;
    }
  };
}

}  // namespace
#endif

namespace util {
std::vector<std::string> File::ListFiles(const std::string& path) {
  MakeDirs(path);
  return OsListFiles(path);
}

File::ChunkProducer File::Read(const std::string& path) { return OsRead(path); }
File::ChunkReceiver File::Write(const std::string& path, bool overwrite,
                                bool exist_ok) {
  MakeDirs(BaseDir(path));
  if (!overwrite && Size(path) >= 0) {
    if (exist_ok) return [](Chunk chunk) {};
    throw std::system_error(EEXIST, std::system_category(), "Write " + path);
  }
  return OsWrite(path, overwrite, exist_ok);
}

SHA256_t File::Hash(const std::string& path) {
  SHA256 hasher;
  auto producer = Read(path);
  Chunk chunk;
  while ((chunk = producer()).size()) {
    hasher.update(chunk.begin(), chunk.size());
  }
  return hasher.finalize();
}

void File::MakeDirs(const std::string& path) {
  uint64_t pos = 0;
  while (pos != std::string::npos) {
    pos = path.find_first_of(kPathSeparators, pos + 1);
    if (!MkDir(path.substr(0, pos))) {
      throw std::system_error(errno, std::system_category(), "mkdir");
    }
  }
}

void File::HardCopy(const std::string& from, const std::string& to,
                    bool overwrite, bool exist_ok, bool make_dirs) {
  if (make_dirs) MakeDirs(BaseDir(to));
  auto producer = Read(from);
  auto receiver = Write(to, overwrite, exist_ok);
  Chunk chunk;
  while ((chunk = producer()).size()) {
    receiver(chunk);
  }
  receiver(chunk);
}

void File::Copy(const std::string& from, const std::string& to, bool overwrite,
                bool exist_ok) {
  MakeDirs(BaseDir(to));
  if (OsIsLink(from) || OsAtomicCopy(from, to, overwrite, exist_ok)) {
    HardCopy(from, to, overwrite, exist_ok, false);
  }
}

void File::Move(const std::string& from, const std::string& to, bool overwrite,
                bool exist_ok) {
  if (OsIsLink(from) || !OsAtomicMove(from, to, overwrite, exist_ok)) {
    Copy(from, to, overwrite, exist_ok);
    Remove(from);
  }
}

void File::Remove(const std::string& path) {
  if (!OsRemove(path))
    throw std::system_error(errno, std::system_category(), "remove");
}

void File::RemoveTree(const std::string& path) {
  if (!OsRemoveTree(path))
    throw std::system_error(errno, std::system_category(), "removetree");
}

void File::MakeExecutable(const std::string& path) {
  if (!OsMakeExecutable(path))
    throw std::system_error(errno, std::system_category(), "chmod");
}

void File::MakeImmutable(const std::string& path) {
  if (!OsMakeImmutable(path))
    throw std::system_error(errno, std::system_category(), "chmod");
}

std::string File::PathForHash(const SHA256_t& hash) {
  std::string path = hash.Hex();
  return JoinPath(
      Flags::store_directory,
      JoinPath(JoinPath(path.substr(0, 2), path.substr(2, 2)), path));
}

std::string File::JoinPath(const std::string& first,
                           const std::string& second) {
  if (strchr(kPathSeparators, second[0]) != nullptr) return second;
  return first + kPathSeparators[0] + second;  // NOLINT
}

std::string File::BaseDir(const std::string& path) {
  return path.substr(0, path.find_last_of(kPathSeparators));
}

std::string File::BaseName(const std::string& path) {
  return path.substr(path.find_last_of(kPathSeparators) + 1);
}

int64_t File::Size(const std::string& path) {
  std::ifstream fin(path, std::ios::ate | std::ios::binary);
  if (!fin) return -1;
  return fin.tellg();
}

File::ChunkReceiver File::LazyChunkReceiver(kj::Function<ChunkReceiver()> f) {
  std::unique_ptr<File::ChunkReceiver> rec(nullptr);
  return [f = std::move(f), rec = std::move(rec)](Chunk chunk) mutable {
    if (!rec) rec = std::make_unique<File::ChunkReceiver>(f());
    (*rec)(chunk);
  };
}

TempDir::TempDir(const std::string& base) {
  File::MakeDirs(base);
  path_ = OsTempDir(base);
  if (path_.empty())
    throw std::system_error(errno, std::system_category(), "mkdtemp");
}
void TempDir::Keep() { keep_ = true; }
const std::string& TempDir::Path() const { return path_; }
TempDir::~TempDir() {
  if (!keep_ && !moved_) File::RemoveTree(path_);
}

std::string File::SHAToPath(const std::string& store_directory,
                            const SHA256_t& hash) {
  return util::File::JoinPath(store_directory, util::File::PathForHash(hash));
}

kj::Promise<void> File::Receiver::sendChunk(SendChunkContext context) {
  receiver_(context.getParams().getChunk());
  return kj::READY_NOW;
}

namespace {
kj::Promise<void> next_chunk(File::ChunkProducer producer,
                             capnproto::FileReceiver::Client receiver) {
  File::Chunk chunk = producer();
  auto req = receiver.sendChunkRequest();
  req.setChunk(chunk);
  return req.send().ignoreResult().then(
      [sz = chunk.size(), producer = std::move(producer),
       receiver = std::move(receiver)]() mutable -> kj::Promise<void> {
        if (sz)
          return next_chunk(std::move(producer), receiver);
        else
          return kj::READY_NOW;
      });
}
}  // namespace

kj::Promise<void> File::HandleRequestFile(
    const std::string& path, capnproto::FileReceiver::Client receiver) {
  // TODO: see if we can easily avoid the extra round-trips while
  // still guaranteeing in-order processing (when capnp implements streams?)
  // Possibly by using UnionPromiseBuilder?
  auto producer = Read(path);
  return next_chunk(std::move(producer), receiver);
}

}  // namespace util
