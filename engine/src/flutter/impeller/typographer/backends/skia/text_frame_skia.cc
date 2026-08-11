// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "impeller/typographer/backends/skia/text_frame_skia.h"

#include <algorithm>
#include <cstring>
#include <memory>
#include <mutex>
#include <optional>
#include <unordered_map>
#include <vector>

#include "flutter/display_list/geometry/dl_path.h"
#include "flutter/fml/logging.h"
#include "flutter/fml/trace_event.h"
#include "fml/status.h"
#include "impeller/typographer/backends/skia/typeface_skia.h"
#include "impeller/typographer/font.h"
#include "impeller/typographer/glyph.h"
#include "third_party/skia/include/core/SkData.h"
#include "third_party/skia/include/core/SkFont.h"
#include "third_party/skia/include/core/SkFontMetrics.h"
#include "third_party/skia/include/core/SkMatrix.h"
#include "third_party/skia/include/core/SkPaint.h"
#include "third_party/skia/include/core/SkPath.h"
#include "third_party/skia/include/core/SkRect.h"
#include "third_party/skia/include/core/SkTypeface.h"
#include "third_party/skia/modules/skparagraph/include/Paragraph.h"  // nogncheck
#include "third_party/skia/src/core/SkStrikeSpec.h"    // nogncheck
#include "third_party/skia/src/core/SkTextBlobPriv.h"  // nogncheck

namespace impeller {

static Font ToFont(const SkTextBlobRunIterator& run, AxisAlignment alignment) {
  auto& font = run.font();
  auto typeface = std::make_shared<TypefaceSkia>(font.refTypeface());

  SkFontMetrics sk_metrics;
  font.getMetrics(&sk_metrics);

  Font::Metrics metrics;
  metrics.point_size = font.getSize();
  metrics.embolden = font.isEmbolden();
  metrics.skewX = font.getSkewX();
  metrics.scaleX = font.getScaleX();

  return Font{std::move(typeface), metrics, alignment};
}

static Rect ToRect(const SkRect& rect) {
  return Rect::MakeLTRB(rect.fLeft, rect.fTop, rect.fRight, rect.fBottom);
}

namespace {

// Minimal big-endian OpenType readers for the COLR (v0) and CPAL tables.
// iOS uses the CoreText backend, which rasterizes the *composite* color glyph
// to a bitmap and never exposes COLR layers as vector data. But COLRv0 layers
// are just references to ordinary glyphs whose outlines CoreText *does*
// provide, so we parse the tables ourselves and render each layer as a colored
// path.
uint16_t ReadU16(const uint8_t* p) {
  return static_cast<uint16_t>((static_cast<uint16_t>(p[0]) << 8) | p[1]);
}
uint32_t ReadU32(const uint8_t* p) {
  return (static_cast<uint32_t>(p[0]) << 24) |
         (static_cast<uint32_t>(p[1]) << 16) |
         (static_cast<uint32_t>(p[2]) << 8) | static_cast<uint32_t>(p[3]);
}

// The first (index 0) CPAL palette, as impeller Colors. Empty if unavailable.
std::vector<Color> ParseCpalPalette0(const sk_sp<SkData>& data) {
  if (!data || data->size() < 14) {
    return {};
  }
  const uint8_t* b = data->bytes();
  const size_t size = data->size();
  uint16_t num_palette_entries = ReadU16(b + 2);
  uint16_t num_palettes = ReadU16(b + 4);
  uint16_t num_color_records = ReadU16(b + 6);
  uint32_t color_records_offset = ReadU32(b + 8);
  if (num_palettes == 0) {
    return {};
  }
  // colorRecordIndices[] starts at offset 12 (u16 each); palette 0 is the
  // first.
  uint16_t first_record = ReadU16(b + 12);
  std::vector<Color> colors;
  colors.reserve(num_palette_entries);
  for (uint16_t i = 0; i < num_palette_entries; i++) {
    uint32_t rec = first_record + i;
    if (rec >= num_color_records) {
      break;
    }
    size_t off = color_records_offset + static_cast<size_t>(rec) * 4;
    if (off + 4 > size) {
      break;
    }
    // CPAL color records are stored BGRA, 8 bits each.
    Scalar blue = b[off + 0] / 255.0f;
    Scalar green = b[off + 1] / 255.0f;
    Scalar red = b[off + 2] / 255.0f;
    Scalar alpha = b[off + 3] / 255.0f;
    colors.push_back(Color(red, green, blue, alpha));
  }
  return colors;
}

// A parsed COLR (v0) table header; layer records are read on demand.
struct ColrV0 {
  const uint8_t* data = nullptr;
  size_t size = 0;
  uint16_t num_base_glyph_records = 0;
  uint32_t base_glyph_records_offset = 0;
  uint32_t layer_records_offset = 0;
  uint16_t num_layer_records = 0;
  bool ok = false;
};

ColrV0 ParseColrV0(const sk_sp<SkData>& data) {
  ColrV0 c;
  if (!data || data->size() < 14) {
    return c;
  }
  const uint8_t* b = data->bytes();
  // Only the v0 base/layer records are handled (v1 keeps them at the same
  // offsets, so v1 fonts still get their v0 layers rendered).
  c.data = b;
  c.size = data->size();
  c.num_base_glyph_records = ReadU16(b + 2);
  c.base_glyph_records_offset = ReadU32(b + 4);
  c.layer_records_offset = ReadU32(b + 8);
  c.num_layer_records = ReadU16(b + 12);
  c.ok = true;
  return c;
}

// Binary-search the (glyphID-sorted) base glyph records for `glyph_id`.
bool FindColrLayers(const ColrV0& c,
                    uint16_t glyph_id,
                    uint16_t* first_layer,
                    uint16_t* num_layers) {
  if (!c.ok || c.num_base_glyph_records == 0) {
    return false;
  }
  int lo = 0;
  int hi = static_cast<int>(c.num_base_glyph_records) - 1;
  while (lo <= hi) {
    int mid = (lo + hi) / 2;
    size_t off = c.base_glyph_records_offset + static_cast<size_t>(mid) * 6;
    if (off + 6 > c.size) {
      return false;
    }
    uint16_t gid = ReadU16(c.data + off);
    if (gid == glyph_id) {
      *first_layer = ReadU16(c.data + off + 2);
      *num_layers = ReadU16(c.data + off + 4);
      return true;
    }
    if (gid < glyph_id) {
      lo = mid + 1;
    } else {
      hi = mid - 1;
    }
  }
  return false;
}

bool GetColrLayerRecord(const ColrV0& c,
                        uint16_t layer_index,
                        uint16_t* glyph_id,
                        uint16_t* palette_index) {
  if (layer_index >= c.num_layer_records) {
    return false;
  }
  size_t off = c.layer_records_offset + static_cast<size_t>(layer_index) * 4;
  if (off + 4 > c.size) {
    return false;
  }
  *glyph_id = ReadU16(c.data + off);
  *palette_index = ReadU16(c.data + off + 2);
  return true;
}

/// A cache of `V` keyed by a 64-bit identity, bounded by entry count, which
/// drops the least recently used half when it overflows.
///
/// Both caches in this file used to `clear()` on overflow. That threw away the
/// entries for the pages currently on SCREEN along with everything else, so the
/// frame right after the cap was crossed had to re-extract every visible glyph
/// and re-merge every visible line — a stall proportional to a whole page,
/// recurring every time the working set crossed the bound. The blob-path cache
/// made that reachable in ordinary use: the app has 604 pages x ~15 lines
/// (~9060 distinct blobs) against a 256-entry cap, i.e. roughly every 17 pages
/// of swiping.
///
/// It also cascaded. The merged paths rebuilt after a clear are fresh SkPaths,
/// so their `getGenerationID()` values differ, which silently invalidates the
/// tessellation cache for that content too (it keys on exactly that id).
///
/// Halving by age keeps the on-screen working set — 3 mounted pages is ~45
/// blobs, far inside the cap — and amortizes each eviction over many inserts.
/// Mirrors `ConvexTessellatorImpl::EvictLeastRecentlyUsed`, which is bounded by
/// bytes rather than by count.
///
/// Not internally synchronized: callers hold their own mutex, as they did for
/// the raw maps this replaces.
template <typename V>
class LruCache {
 public:
  explicit LruCache(size_t max_entries) : max_entries_(max_entries) {}

  /// Returns a COPY, deliberately: the caller uses the value after dropping the
  /// lock, and a later insert can evict and invalidate any reference into the
  /// map. Both instantiations copy a refcount (`shared_ptr`, `DlPath`).
  std::optional<V> Find(uint64_t key) {
    auto it = entries_.find(key);
    if (it == entries_.end()) {
      return std::nullopt;
    }
    it->second.last_used = ++tick_;
    return it->second.value;
  }

  V Insert(uint64_t key, V value) {
    if (entries_.size() >= max_entries_) {
      EvictOlderHalf(key);
    }
    Entry& entry = entries_[key];
    entry.value = std::move(value);
    entry.last_used = ++tick_;
    return entry.value;
  }

 private:
  struct Entry {
    V value;
    uint64_t last_used = 0u;
  };

  /// Drops the older half of the cache by last use, never `keep`.
  void EvictOlderHalf(uint64_t keep) {
    std::vector<uint64_t> by_age;
    by_age.reserve(entries_.size());
    for (const auto& [key, entry] : entries_) {
      if (key != keep) {
        by_age.push_back(key);
      }
    }
    std::sort(by_age.begin(), by_age.end(), [&](uint64_t a, uint64_t b) {
      return entries_.at(a).last_used < entries_.at(b).last_used;
    });
    const size_t target = max_entries_ / 2u;
    for (uint64_t key : by_age) {
      if (entries_.size() <= target) {
        break;
      }
      entries_.erase(key);
    }
  }

  std::unordered_map<uint64_t, Entry> entries_;
  const size_t max_entries_;
  uint64_t tick_ = 0u;
};

// One COLR layer of one glyph, in glyph-local space at kCanonicalGlyphSize.
struct CachedLayer {
  flutter::DlPath path;
  Color color;
  bool use_foreground_color = false;
};

// Everything needed to draw one glyph, independent of the size it is drawn at.
struct CachedGlyph {
  std::vector<CachedLayer> layers;
  // True for a color glyph with no COLRv0 layer list (bitmap emoji, COLRv1):
  // paths cannot represent it, so the whole frame must fall back to the atlas.
  bool unsupported_color_glyph = false;
};

// Outlines are extracted at this size and scaled per use. Legitimate because
// extraction disables hinting and uses linear metrics, so an outline at size S
// is exactly (S / kCanonicalGlyphSize) times this one.
constexpr Scalar kCanonicalGlyphSize = 100.0f;

// Appends `glyph_id`'s outline as a layer filled with `color` (or the paint's
// color when `use_foreground`).
void AppendGlyphPath(SkBulkGlyphMetricsAndPaths& paths,
                     SkGlyphID glyph_id,
                     Color color,
                     bool use_foreground,
                     std::vector<CachedLayer>* out) {
  SkSpan<const SkGlyph*> span = paths.glyphs(SkSpan(&glyph_id, 1));
  if (span.empty() || span[0] == nullptr) {
    return;
  }
  const SkPath* path = span[0]->path();
  if (path == nullptr || path->isEmpty()) {
    return;
  }
  CachedLayer layer;
  // SkPath is copy-on-write, so this shares the strike's outline data rather
  // than duplicating every point.
  layer.path = flutter::DlPath(*path);
  layer.color = color;
  layer.use_foreground_color = use_foreground;
  out->push_back(std::move(layer));
}

// True when `glyph_id` renders as color pixels rather than as its own outline —
// a bitmap emoji (CBDT/sbix) or a COLRv1 glyph. Such a glyph cannot be drawn
// from paths at all: its outline is either empty or a placeholder, so filling
// it would paint a black silhouette where the color glyph belongs.
bool GlyphIsColor(SkBulkGlyphMetricsAndPaths& paths, SkGlyphID glyph_id) {
  SkSpan<const SkGlyph*> span = paths.glyphs(SkSpan(&glyph_id, 1));
  return !span.empty() && span[0] != nullptr && span[0]->isColor();
}

// The COLR/CPAL tables of one typeface, parsed once.
struct ColrTables {
  // Owns the bytes `colr` points into.
  sk_sp<SkData> colr_data;
  ColrV0 colr;
  std::vector<Color> palette;
};

// COLR/CPAL are immutable font data, but `copyTableData` memcpy's the whole
// table on every call (~26 KB for a 4.5k-glyph COLR font) and extraction reads
// them once per text run. Parse once per typeface instead.
//
// Shared ownership, not a bare reference: the caller keeps using the tables
// after the lock is dropped, so an eviction must not be able to pull them out
// from under it.
std::shared_ptr<const ColrTables> GetColrTables(SkTypeface* typeface) {
  // Typeface count is small in practice (one COLR font per Quran page, say),
  // but bound it anyway — entries are cheap to rebuild.
  static constexpr size_t kMaxCachedTypefaces = 64;
  static std::mutex mutex;
  static LruCache<std::shared_ptr<const ColrTables>> cache(kMaxCachedTypefaces);

  std::lock_guard<std::mutex> lock(mutex);
  const uint64_t key = typeface->uniqueID();
  if (std::optional<std::shared_ptr<const ColrTables>> hit = cache.Find(key);
      hit.has_value()) {
    return *hit;
  }
  auto tables = std::make_shared<ColrTables>();
  tables->colr_data =
      typeface->copyTableData(SkSetFourByteTag('C', 'O', 'L', 'R'));
  tables->colr = ParseColrV0(tables->colr_data);
  tables->palette = ParseCpalPalette0(
      typeface->copyTableData(SkSetFourByteTag('C', 'P', 'A', 'L')));
  return cache.Insert(key, std::move(tables));
}

/// Walks `glyph_id`'s COLRv0 layer list and turns each layer into a colored
/// path, taking outlines from `paths` (whatever size that strike was built at).
std::shared_ptr<const CachedGlyph> ExtractGlyph(
    SkTypeface* typeface,
    SkGlyphID glyph_id,
    const ColrV0& colr,
    const std::vector<Color>& palette,
    SkBulkGlyphMetricsAndPaths& paths) {
  auto glyph = std::make_shared<CachedGlyph>();
  uint16_t first_layer = 0;
  uint16_t num_layers = 0;
  if (colr.ok && FindColrLayers(colr, glyph_id, &first_layer, &num_layers) &&
      num_layers > 0) {
    for (uint16_t l = 0; l < num_layers; l++) {
      uint16_t layer_gid = 0;
      uint16_t palette_index = 0;
      if (!GetColrLayerRecord(colr, first_layer + l, &layer_gid,
                              &palette_index)) {
        continue;
      }
      bool use_foreground =
          palette_index == 0xFFFF || palette_index >= palette.size();
      Color color = use_foreground ? Color::Black() : palette[palette_index];
      AppendGlyphPath(paths, layer_gid, color, use_foreground, &glyph->layers);
    }
  } else if (GlyphIsColor(paths, glyph_id)) {
    glyph->unsupported_color_glyph = true;
  } else {
    // A non-color glyph inside a color frame (e.g. a fallback-font space or
    // digit): draw its own outline in the foreground (paint) color.
    AppendGlyphPath(paths, glyph_id, Color::Black(),
                    /*use_foreground=*/true, &glyph->layers);
  }
  return glyph;
}

// The COLR layers of one glyph, extracted once and reused at every size and on
// every page that draws it.
//
// Without this, every text frame re-walks the COLR table and re-wraps an
// outline for every layer of every glyph — measured at ~15 ms for one page of
// Quran text on an M-series Mac, recurring whenever that page's DisplayList is
// recorded. Skia's own strike cache does not save us: warm and cold measured
// the same, because the cost is the per-layer lookup and wrapping, not the
// outline decode.
//
// Shared ownership so an eviction cannot pull the layers out from under a
// caller.
std::shared_ptr<const CachedGlyph> GetCachedGlyph(
    SkTypeface* typeface,
    SkGlyphID glyph_id,
    const ColrV0& colr,
    const std::vector<Color>& palette,
    SkBulkGlyphMetricsAndPaths& canonical_paths) {
  // A page uses ~260 glyphs, so this holds several pages' worth. Bounded
  // because the app ships 36 Quran fonts with ~1500 color glyphs each.
  static constexpr size_t kMaxCachedGlyphs = 4096;
  static std::mutex mutex;
  static LruCache<std::shared_ptr<const CachedGlyph>> cache(kMaxCachedGlyphs);

  const uint64_t key = (static_cast<uint64_t>(typeface->uniqueID()) << 16) |
                       static_cast<uint64_t>(glyph_id);
  {
    std::lock_guard<std::mutex> lock(mutex);
    if (std::optional<std::shared_ptr<const CachedGlyph>> hit = cache.Find(key);
        hit.has_value()) {
      return *hit;
    }
  }

  // Cache miss only — one slice per never-seen (typeface, glyph). A page's
  // worth of these in a settle frame means the COLR extraction is the cold
  // cost.
  TRACE_EVENT0("impeller", "ExtractColorGlyph");
  std::shared_ptr<const CachedGlyph> glyph =
      ExtractGlyph(typeface, glyph_id, colr, palette, canonical_paths);

  std::lock_guard<std::mutex> lock(mutex);
  return cache.Insert(key, std::move(glyph));
}

uint64_t MixKey(uint64_t hash, uint64_t value) {
  return hash ^ (value + 0x9e3779b97f4a7c15ull + (hash << 6) + (hash >> 2));
}

uint64_t FloatBits(float value) {
  uint32_t bits = 0;
  memcpy(&bits, &value, sizeof(bits));
  return bits;
}

/// A key that fully determines the outline `Paragraph::GetPath` will build for
/// `blob`: every font property that shapes a glyph, each glyph id and position,
/// and the translate applied to the result. 0 means "not cacheable".
uint64_t BlobGeometryKey(const SkTextBlob* blob) {
  uint64_t hash = 0xcbf29ce484222325ull;
  const SkRect bounds = blob->bounds();
  hash = MixKey(hash, FloatBits(bounds.left()));
  hash = MixKey(hash, FloatBits(bounds.top()));
  for (SkTextBlobRunIterator run(blob); !run.done(); run.next()) {
    // Only full positioning carries an explicit point per glyph. Anything else
    // derives positions from advances this key does not capture, so refuse to
    // cache rather than risk two different layouts sharing one outline.
    if (run.positioning() != SkTextBlobRunIterator::kFull_Positioning) {
      return 0;
    }
    const SkFont& font = run.font();
    hash =
        MixKey(hash, font.getTypeface() ? font.getTypeface()->uniqueID() : 0u);
    hash = MixKey(hash, FloatBits(font.getSize()));
    hash = MixKey(hash, FloatBits(font.getScaleX()));
    hash = MixKey(hash, FloatBits(font.getSkewX()));
    hash = MixKey(hash, (static_cast<uint64_t>(font.getEdging()) << 8) |
                            static_cast<uint64_t>(font.getHinting()));
    hash = MixKey(hash, (font.isEmbolden() ? 0b001u : 0u) |
                            (font.isLinearMetrics() ? 0b010u : 0u) |
                            (font.isSubpixel() ? 0b100u : 0u));
    hash = MixKey(hash, run.glyphCount());
    const SkGlyphID* ids = run.glyphs();
    const SkPoint* points = run.points();
    for (uint32_t i = 0; i < run.glyphCount(); i++) {
      hash = MixKey(hash, ids[i]);
      hash = MixKey(
          hash, (FloatBits(points[i].x()) << 32) | FloatBits(points[i].y()));
    }
  }
  // 0 is the "not cacheable" sentinel; a real hash that lands there can just
  // take the slow path.
  return hash;
}

/// The merged outline of one blob, shared by every blob that encodes the same
/// geometry.
///
/// Impeller's tessellation cache is keyed by `SkPath::getGenerationID()`, which
/// is per-instance, and `Paragraph::GetPath` builds a FRESH SkPath on every
/// call. Any rebuild of a widget subtree produces a new blob for the same text,
/// so the same page paid a full re-tessellation on every visit and pre-painting
/// one subtree could never warm another — measured on an S23 Ultra as 34 ms to
/// paint a page that had just been pre-painted invisibly, with no improvement
/// across four swipes. Handing back one shared SkPath for identical geometry
/// gives that cache a stable id (DlPath copies share the SkPath copy-on-write),
/// so a glyph set is walked and tessellated once instead of once per subtree.
std::optional<flutter::DlPath> GetSharedBlobPath(SkTextBlob* blob) {
  // A page of Quran text is ~15 blobs (one per line), so this holds a dozen
  // pages of merged outlines. Bounded: text geometry is unbounded in general.
  static constexpr size_t kMaxCachedBlobPaths = 256;
  static std::mutex mutex;
  static LruCache<flutter::DlPath> cache(kMaxCachedBlobPaths);

  const uint64_t key = BlobGeometryKey(blob);
  if (key != 0) {
    std::lock_guard<std::mutex> lock(mutex);
    if (std::optional<flutter::DlPath> hit = cache.Find(key); hit.has_value()) {
      return *hit;
    }
  }
  // Cache miss only. Traced because the cost that shows up as a cold page paint
  // is otherwise invisible: it lands inside SurfaceFrame::Encode, which has no
  // slices of its own. If a settle frame is expensive and this slice is NOT in
  // it, the merged-path build is not the problem.
  TRACE_EVENT0("impeller", "BuildMergedBlobPath");
  SkPath path = skia::textlayout::Paragraph::GetPath(blob);
  if (path.isEmpty()) {
    return std::nullopt;
  }
  flutter::DlPath result(path.makeTransform(
      SkMatrix::Translate(blob->bounds().left(), blob->bounds().top())));
  if (key == 0) {
    return result;
  }
  std::lock_guard<std::mutex> lock(mutex);
  return cache.Insert(key, std::move(result));
}

}  // namespace

std::shared_ptr<TextFrame> MakeTextFrameFromTextBlobSkia(
    const sk_sp<SkTextBlob>& blob) {
  bool has_color = false;
  std::vector<TextRun> runs;
  for (SkTextBlobRunIterator run(blob.get()); !run.done(); run.next()) {
    SkStrikeSpec strikeSpec = SkStrikeSpec::MakeWithNoDevice(run.font());
    // Metrics only. Everything below needs just `isColor()` (the mask format)
    // and the glyph ids; asking for paths here also prepares an outline for
    // every glyph in the blob, on the UI thread, at DisplayList record time —
    // measured at ~8 ms per page of Quran text on an M-series Mac.
    SkBulkGlyphMetrics metrics_only{strikeSpec};
    SkSpan<const SkGlyph*> glyphs =
        metrics_only.glyphs(SkSpan(run.glyphs(), run.glyphCount()));

    for (const auto& glyph : glyphs) {
      has_color |= glyph->isColor();
    }

    AxisAlignment alignment = AxisAlignment::kNone;
    if (run.font().isSubpixel() && run.font().isBaselineSnap() && !has_color) {
      alignment = AxisAlignment::kX;
    }

    switch (run.positioning()) {
      case SkTextBlobRunIterator::kFull_Positioning: {
        std::vector<TextRun::GlyphPosition> positions;
        positions.reserve(run.glyphCount());
        for (auto i = 0u; i < run.glyphCount(); i++) {
          // kFull_Positioning has two scalars per glyph.
          const SkPoint* glyph_points = run.points();
          const SkPoint* point = glyph_points + i;
          Glyph::Type type =
              glyphs[i]->isColor() ? Glyph::Type::kBitmap : Glyph::Type::kPath;
          positions.emplace_back(TextRun::GlyphPosition{
              Glyph{glyphs[i]->getGlyphID(), type}, Point{
                                                        point->x(),
                                                        point->y(),
                                                    }});
        }
        TextRun text_run(ToFont(run, alignment), positions);
        runs.emplace_back(text_run);
        break;
      }
      default:
        FML_DLOG(ERROR) << "Unimplemented.";
        continue;
    }
  }
  return std::make_shared<TextFrame>(
      runs, ToRect(blob->bounds()), has_color,
      [blob]() -> fml::StatusOr<flutter::DlPath> {
        // Shared across blobs with identical geometry so the tessellation cache
        // sees a stable path id — see GetSharedBlobPath.
        std::optional<flutter::DlPath> path = GetSharedBlobPath(blob.get());
        if (!path.has_value()) {
          return fml::Status(fml::StatusCode::kCancelled, "No path available");
        }
        return path.value();
      },
      [blob]() -> std::vector<ColorGlyphLayer> {
        // Build per-layer colored vector paths for every glyph in the frame so
        // COLR text can be drawn as paths (crisp at any scale) instead of the
        // color glyph atlas. See ColorGlyphLayer / TextFrame::GetColorPaths.
        //
        // Per-glyph layers come from a process-wide cache keyed by typeface and
        // glyph, extracted at kCanonicalGlyphSize and scaled here, so a page
        // that draws the same glyph at ten different line sizes — and the next
        // page that draws it again — do no extraction work at all.
        std::vector<ColorGlyphLayer> layers;
        for (SkTextBlobRunIterator run(blob.get()); !run.done(); run.next()) {
          if (run.positioning() != SkTextBlobRunIterator::kFull_Positioning) {
            continue;
          }
          SkTypeface* typeface = run.font().getTypeface();
          if (typeface == nullptr) {
            continue;
          }
          // A skewed, stretched or emboldened font does not scale linearly from
          // the canonical outline, so those runs are not cached: extract them
          // at their own size instead.
          const bool cacheable = run.font().getScaleX() == 1.0f &&
                                 run.font().getSkewX() == 0.0f &&
                                 !run.font().isEmbolden();
          const Scalar extraction_size =
              cacheable ? kCanonicalGlyphSize : run.font().getSize();
          const Scalar scale = run.font().getSize() / extraction_size;

          // Parsed once per typeface, not once per run per extraction. Held by
          // shared_ptr for the duration of this run's extraction.
          std::shared_ptr<const ColrTables> tables = GetColrTables(typeface);

          // Fetch outlines unhinted with linear metrics: these paths are
          // rendered as scalable vectors, and grid-fitting (e.g. FreeType on
          // Android) displaces the base glyph and its color layers by
          // different amounts, which reads as a visible misalignment once
          // zoomed. Unhinted linear outlines are also what makes extracting at
          // one canonical size and scaling exact. (CoreText never hints, so the
          // hinting part is a no-op on iOS/macOS.)
          SkFont path_font = run.font();
          path_font.setSize(extraction_size);
          path_font.setHinting(SkFontHinting::kNone);
          path_font.setLinearMetrics(true);
          SkStrikeSpec strike_spec = SkStrikeSpec::MakeWithNoDevice(path_font);
          SkBulkGlyphMetricsAndPaths paths{strike_spec};

          const SkGlyphID* glyph_ids = run.glyphs();
          const SkPoint* points = run.points();
          for (uint32_t i = 0; i < run.glyphCount(); i++) {
            std::shared_ptr<const CachedGlyph> glyph =
                cacheable ? GetCachedGlyph(typeface, glyph_ids[i], tables->colr,
                                           tables->palette, paths)
                          : ExtractGlyph(typeface, glyph_ids[i], tables->colr,
                                         tables->palette, paths);
            if (glyph->unsupported_color_glyph) {
              // A color glyph with no COLRv0 layer list: bitmap emoji, or a
              // COLRv1-only font. Paths cannot represent it, and a partial path
              // frame would hide it (the canvas draws the layers and returns),
              // so give up on the whole frame and let the color glyph atlas
              // draw it as before.
              return std::vector<ColorGlyphLayer>{};
            }
            for (const CachedLayer& cached : glyph->layers) {
              layers.push_back(ColorGlyphLayer{
                  .path = cached.path,
                  .offset = Point(points[i].x(), points[i].y()),
                  .scale = scale,
                  .color = cached.color,
                  .use_foreground_color = cached.use_foreground_color,
              });
            }
          }
        }
        return layers;
      });
}

}  // namespace impeller
