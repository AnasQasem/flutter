// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef FLUTTER_IMPELLER_TYPOGRAPHER_LAZY_GLYPH_ATLAS_H_
#define FLUTTER_IMPELLER_TYPOGRAPHER_LAZY_GLYPH_ATLAS_H_

#include "impeller/geometry/rational.h"
#include "impeller/renderer/context.h"
#include "impeller/typographer/glyph_atlas.h"
#include "impeller/typographer/text_frame.h"
#include "impeller/typographer/typographer_context.h"

namespace impeller {

class LazyGlyphAtlas {
 public:
  explicit LazyGlyphAtlas(
      std::shared_ptr<TypographerContext> typographer_context);

  ~LazyGlyphAtlas();

  void AddTextFrame(const std::shared_ptr<TextFrame>& frame,
                    Point position,
                    const Matrix& transform,
                    const GlyphProperties& properties);

  void ResetTextFrames();

  //----------------------------------------------------------------------------
  /// @brief   QURAN PATCH 006: drop the accumulated atlas state entirely.
  ///
  ///          `ResetTextFrames` clears only the per-frame list and the current
  ///          atlas handle — the `GlyphAtlasContext` survives, and with it the
  ///          atlas's `ScaledFont` -> `FontGlyphAtlas` maps, the rect packer
  ///          and the height adjustment. Those accumulate an entry per (font,
  ///          glyph, scale) for the life of the surface and are never evicted.
  ///
  ///          MEASURED on an A146P: after a broad search plus scrolling every
  ///          result, this held **262 MB** of live malloc — by far the largest
  ///          single consumer, and more than every other instrumented cache
  ///          combined (device buffers 4 MB, textures 26 MB, tessellation 6 MB,
  ///          blob paths 5.6 MB, render targets 0.5 MB). It fits: this app
  ///          ships 36 word-ligature Hafs fonts of ~4600 glyphs each, so a few
  ///          scales apiece is on the order of a million map entries.
  ///
  ///          Destroying the surface is what used to reclaim it, which meant
  ///          the app had to be backgrounded. This releases the same state in
  ///          place. Call only between frames — the raster task runner already
  ///          serialises `Rasterizer::NotifyLowMemoryWarning` against
  ///          rasterization. The next frame rebuilds whatever it still needs.
  void ClearAtlasContexts();

  const std::shared_ptr<GlyphAtlas>& CreateOrGetGlyphAtlas(
      Context& context,
      HostBuffer& host_buffer,
      GlyphAtlas::Type type);

 private:
  std::shared_ptr<TypographerContext> typographer_context_;

  struct AtlasData {
    explicit AtlasData(std::shared_ptr<GlyphAtlasContext> context);

    ~AtlasData();

    std::vector<RenderableText> renderable_frames;
    std::shared_ptr<GlyphAtlasContext> context;
    std::shared_ptr<GlyphAtlas> atlas;

    void reset();
  };

  AtlasData alpha_data_;
  AtlasData color_data_;

  AtlasData& GetData(GlyphAtlas::Type type);

  LazyGlyphAtlas(const LazyGlyphAtlas&) = delete;

  LazyGlyphAtlas& operator=(const LazyGlyphAtlas&) = delete;
};

}  // namespace impeller

#endif  // FLUTTER_IMPELLER_TYPOGRAPHER_LAZY_GLYPH_ATLAS_H_
