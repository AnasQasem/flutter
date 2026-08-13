// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef FLUTTER_IMPELLER_BASE_QURAN_MEM_STATS_H_
#define FLUTTER_IMPELLER_BASE_QURAN_MEM_STATS_H_

#include <atomic>
#include <cstdint>

#if defined(__ANDROID__)
#include <android/log.h>
#endif

/// QURAN PATCH 004 (instrumentation): name the live engine memory.
///
/// Measurement showed ~70 MB of *live* native allocation persists after one
/// broad search — not allocator retention (`HeapAlloc` barely moved across two
/// `mallopt(M_PURGE_ALL)` calls). Four caches could hold it and guessing
/// between them is not acceptable, so each one reports here.
///
/// Why counters and not a profiler: heapprofd resolved 1 of 8820 engine frames
/// on this project, and trace counters do not survive DevTools' export. Why
/// `__android_log_print` and not `FML_LOG`: an `FML_LOG(INFO)` added to
/// `AllocatorVK` in a previous session never appeared in logcat and the reason
/// was never found (handoff §7.5). This bypasses fml's log plumbing entirely
/// and uses ERROR priority so nothing filters it out.
///
/// Header-only on purpose: `inline` function-local statics give one instance
/// across translation units with no new build target, so there is no BUILD.gn
/// change to re-resolve on every upstream rebase.
///
/// Read it with:
///   adb logcat -c && <trigger low memory> && adb logcat -d -s QURANMEM
namespace impeller {

// Exact byte counts — these caches already track their own size.
inline std::atomic<int64_t>& QuranTessellationBytes() {
  static std::atomic<int64_t> v{0};
  return v;
}
inline std::atomic<int64_t>& QuranRenderTargetBytes() {
  static std::atomic<int64_t> v{0};
  return v;
}

// Entry counts — these caches hold opaque geometry whose byte size is not
// cheaply computable. Saturation against the cap is the signal; the byte cost
// is then obtained empirically by lowering the cap and re-measuring RSS.
inline std::atomic<int64_t>& QuranBlobPathEntries() {
  static std::atomic<int64_t> v{0};
  return v;
}
/// Approximate bytes of merged line paths built (points*8 + verbs). Monotonic:
/// evictions are not subtracted, so it is an upper bound on what is resident.
inline std::atomic<int64_t>& QuranBlobPathBytes() {
  static std::atomic<int64_t> v{0};
  return v;
}
inline std::atomic<int64_t>& QuranColrGlyphEntries() {
  static std::atomic<int64_t> v{0};
  return v;
}
inline std::atomic<int64_t>& QuranColrTableEntries() {
  static std::atomic<int64_t> v{0};
  return v;
}
/// Total bytes of the COLR/CPAL table blobs copied out of each typeface. These
/// are `SkData` copies, so unlike the path caches their size IS known.
inline std::atomic<int64_t>& QuranColrTableBytes() {
  static std::atomic<int64_t> v{0};
  return v;
}

/// QURAN PATCH 006: live bytes of Vulkan DeviceBuffers (created minus
/// destroyed).
///
/// `Rasterizer::Teardown` was measured freeing 249 MB of live malloc on an
/// A146P, all of it inside `aiks_context_.reset()` — so a ContentContext-owned
/// object holds it, and every cache with a counter accounts for under 30 MB.
/// Device buffers are the remaining candidate: host-visible Vulkan memory on
/// Mali is malloc-backed, so it lands in `HeapAlloc`. Counted at the allocator
/// so it catches HostBuffer's 1 MB blocks, the one-off oversized buffers
/// `EmplaceInternal` hands out, and anything else, regardless of owner.
inline std::atomic<int64_t>& QuranDeviceBufferBytes() {
  static std::atomic<int64_t> v{0};
  return v;
}
inline std::atomic<int64_t>& QuranDeviceBufferCount() {
  static std::atomic<int64_t> v{0};
  return v;
}

/// Same, for Vulkan texture allocations. Buffers came back at only 4 MB, so if
/// the 212 MB is graphics memory at all it is here.
inline std::atomic<int64_t>& QuranDeviceTextureBytes() {
  static std::atomic<int64_t> v{0};
  return v;
}
inline std::atomic<int64_t>& QuranDeviceTextureCount() {
  static std::atomic<int64_t> v{0};
  return v;
}

inline void DumpQuranMemStats(const char* when) {
  const int64_t tess = QuranTessellationBytes().load();
  const int64_t rt = QuranRenderTargetBytes().load();
  const int64_t colr_bytes = QuranColrTableBytes().load();
#if defined(__ANDROID__)
  __android_log_print(
      ANDROID_LOG_ERROR, "QURANMEM",
      "[%s] tessellation=%lldKB blob_paths=%lldKB render_targets=%lldKB "
      "colr_tables=%lldKB device_buffers=%lldKB/%lld textures=%lldKB/%lld | "
      "entries: "
      "blob_paths=%lld/256 colr_glyphs=%lld/4096 colr_tables=%lld/64",
      when, static_cast<long long>(tess / 1024),
      static_cast<long long>(QuranBlobPathBytes().load() / 1024),
      static_cast<long long>(rt / 1024),
      static_cast<long long>(colr_bytes / 1024),
      static_cast<long long>(QuranDeviceBufferBytes().load() / 1024),
      static_cast<long long>(QuranDeviceBufferCount().load()),
      static_cast<long long>(QuranDeviceTextureBytes().load() / 1024),
      static_cast<long long>(QuranDeviceTextureCount().load()),
      static_cast<long long>(QuranBlobPathEntries().load()),
      static_cast<long long>(QuranColrGlyphEntries().load()),
      static_cast<long long>(QuranColrTableEntries().load()));
#else
  (void)when;
  (void)tess;
  (void)rt;
  (void)colr_bytes;
#endif
}

}  // namespace impeller

#endif  // FLUTTER_IMPELLER_BASE_QURAN_MEM_STATS_H_
