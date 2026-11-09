#pragma once

// Local hardware adaptation for the FAST'23 ROLEX prototype.
//
// The paper's memory pool assumes (i) pre-registered huge pages and (ii)
// one-sided 8-byte RDMA atomics for leaf allocation and table locking.  The
// zongshu hosts expose an mlx5 RoCE HCA but currently have zero hugetlb pages,
// and the official compute-side path is incomplete.  This header keeps the
// same allocation/locking *semantics* while lowering the operations onto the
// primitives actually available on the host:
//
//   * mmap(MAP_HUGETLB) when the kernel has reserved huge pages,
//     otherwise an aligned private anonymous mapping;
//   * 8-byte CAS/FAA through GCC's __atomic builtins (single-node analogue of
//     IBV_WR_ATOMIC_CMP_AND_SWP / IBV_WR_ATOMIC_FETCH_AND_ADD).
//
// No zongshu source is changed; this file lives only in fuxian/rolex.

#include <atomic>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>

#include <sys/mman.h>
#include <unistd.h>

namespace rolex {
namespace port {

struct HardwareProfile {
  bool hugetlb_available{false};
  bool hugetlb_used{false};
  bool mapped{false};
  std::uint64_t bytes{0};
  std::string mapping{"none"};
  std::string atomic_backend{"gcc-__atomic"};
  std::string rdma_backend{"cpu-local (official compute path incomplete)"};
  std::string fallback_reason;
};

inline std::uint64_t read_ulong_file(const char* path) {
  std::ifstream in(path);
  std::uint64_t value = 0;
  if (!(in >> value)) return 0;
  return value;
}

inline bool hugetlb_available() {
  return read_ulong_file("/proc/sys/vm/nr_hugepages") > 0;
}

inline std::uint64_t align_up(std::uint64_t value, std::uint64_t alignment) {
  return (value + alignment - 1) / alignment * alignment;
}

// RAII page-aligned region.  This is deliberately not std::vector: the leaf
// arena needs stable addresses, exactly like a registered remote MR.
class MappedRegion {
 public:
  MappedRegion() = default;
  MappedRegion(const MappedRegion&) = delete;
  MappedRegion& operator=(const MappedRegion&) = delete;
  MappedRegion(MappedRegion&& other) noexcept { swap(other); }
  MappedRegion& operator=(MappedRegion&& other) noexcept {
    if (this != &other) {
      release();
      swap(other);
    }
    return *this;
  }
  ~MappedRegion() { release(); }

  static MappedRegion create(std::uint64_t bytes, bool prefer_hugetlb,
                             HardwareProfile* profile) {
    MappedRegion region;
    region.bytes_ = align_up(bytes ? bytes : 1, 2ull << 20);
    region.available_ = prefer_hugetlb && hugetlb_available();
    if (profile) {
      profile->hugetlb_available = hugetlb_available();
      profile->bytes = region.bytes_;
    }

    int flags = MAP_PRIVATE | MAP_ANONYMOUS;
    if (region.available_) {
      region.ptr_ = ::mmap(nullptr, region.bytes_, PROT_READ | PROT_WRITE,
                           flags | MAP_HUGETLB, -1, 0);
      if (region.ptr_ != MAP_FAILED) {
        region.huge_ = true;
        if (profile) {
          profile->hugetlb_used = true;
          profile->mapping = "mmap(MAP_HUGETLB-2MiB)";
        }
        return region;
      }
      region.ptr_ = nullptr;
      if (profile) {
        profile->fallback_reason =
            "MAP_HUGETLB failed: " + std::string(std::strerror(errno));
      }
    } else if (profile) {
      profile->fallback_reason =
          prefer_hugetlb ? "nr_hugepages=0" : "hugetlb not requested";
    }

    region.ptr_ = ::mmap(nullptr, region.bytes_, PROT_READ | PROT_WRITE, flags,
                         -1, 0);
    if (region.ptr_ == MAP_FAILED) {
      region.ptr_ = nullptr;
      region.bytes_ = 0;
      if (profile) profile->mapping = "mmap failed";
      return region;
    }
    region.huge_ = false;
    if (profile) profile->mapping = "mmap(MAP_ANONYMOUS-page-aligned)";
    return region;
  }

  void* data() const { return ptr_; }
  std::uint64_t size() const { return bytes_; }
  bool valid() const { return ptr_ != nullptr; }
  bool huge() const { return huge_; }

 private:
  void release() {
    if (ptr_) {
      ::munmap(ptr_, bytes_);
      ptr_ = nullptr;
    }
    bytes_ = 0;
    huge_ = false;
  }
  void swap(MappedRegion& other) noexcept {
    std::swap(ptr_, other.ptr_);
    std::swap(bytes_, other.bytes_);
    std::swap(huge_, other.huge_);
    std::swap(available_, other.available_);
  }

  void* ptr_{nullptr};
  std::uint64_t bytes_{0};
  bool huge_{false};
  bool available_{false};
};

// 8-byte single-node equivalents of the two RDMA atomics used by ROLEX.
// They operate on naturally aligned addresses in registered/local memory.
inline bool atomic_cas_u64(std::uint64_t* address, std::uint64_t expected,
                           std::uint64_t desired) {
  return __atomic_compare_exchange_n(address, &expected, desired, false,
                                     __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE);
}

inline std::uint64_t atomic_faa_u64(std::uint64_t* address,
                                    std::uint64_t increment) {
  return __atomic_fetch_add(address, increment, __ATOMIC_ACQ_REL);
}

inline std::uint64_t atomic_load_u64(const std::uint64_t* address) {
  return __atomic_load_n(address, __ATOMIC_ACQUIRE);
}

}  // namespace port
}  // namespace rolex
