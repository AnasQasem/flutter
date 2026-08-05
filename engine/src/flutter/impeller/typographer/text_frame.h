// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef FLUTTER_IMPELLER_TYPOGRAPHER_TEXT_FRAME_H_
#define FLUTTER_IMPELLER_TYPOGRAPHER_TEXT_FRAME_H_

#include <cstdint>
#include <mutex>
#include <optional>
#include <vector>

#include "flutter/display_list/geometry/dl_path.h"
#include "fml/status_or.h"
#include "impeller/geometry/color.h"
#include "impeller/geometry/point.h"
#include "impeller/geometry/rational.h"
#include "impeller/typographer/glyph.h"
#include "impeller/typographer/glyph_atlas.h"
#include "impeller/typographer/text_run.h"

namespace impeller {

using PathCreator = std::function<fml::StatusOr<flutter::DlPath>()>;

//------------------------------------------------------------------------------
/// @brief      One drawable layer of a color (COLR) text frame: a vector glyph
///             outline plus where it sits in the frame, paired with the color
///             it should be filled with.
///
///             Rendering these layers as real paths (instead of sampling the
///             color glyph atlas) keeps COLR text crisp at any scale, matching
///             how monochrome text is drawn via [TextFrame::GetPath].
struct ColorGlyphLayer {
  /// The layer glyph's outline in GLYPH-LOCAL space — i.e. NOT pre-translated
  /// to [offset]. Untranslated so the copy-on-write outline owned by the Skia
  /// strike can be shared instead of duplicated per glyph per frame; the
  /// translation rides on the draw transform instead.
  flutter::DlPath path;
  /// This glyph's origin within the text frame. Apply as a translation on top
  /// of the frame's transform when drawing [path].
  Point offset;
  /// Uniform scale to apply to [path] BEFORE [offset]. Outlines are extracted
  /// once at a canonical size and shared across every size the same glyph is
  /// drawn at (a page of Quran text asks for ~10 sizes), so the size shows up
  /// here rather than baked into the geometry.
  Scalar scale = 1.0f;
  Color color;
  /// When true, fill with the current paint color instead of [color] (COLR
  /// palette index 0xFFFF = "text foreground color").
  bool use_foreground_color = false;
};

using ColorPathCreator = std::function<std::vector<ColorGlyphLayer>()>;

//------------------------------------------------------------------------------
/// @brief      Represents a collection of shaped text runs.
///
///             This object is typically the entrypoint in the Impeller type
///             rendering subsystem.
class TextFrame {
 public:
  TextFrame();

  TextFrame(std::vector<TextRun>& runs,
            Rect bounds,
            bool has_color,
            const PathCreator& path_creator = {},
            const ColorPathCreator& color_path_creator = {});

  ~TextFrame();

  static SubpixelPosition ComputeSubpixelPosition(
      const TextRun::GlyphPosition& glyph_position,
      AxisAlignment alignment,
      const Matrix& transform);

  static Rational RoundScaledFontSize(Scalar scale);
  static Rational RoundScaledFontSize(Rational scale);

  //----------------------------------------------------------------------------
  /// @brief      The conservative bounding box for this text frame.
  ///
  /// @return     The bounds rectangle. If there are no glyphs in this text
  ///             frame an empty Rectangle is returned instead.
  ///
  Rect GetBounds() const;

  //----------------------------------------------------------------------------
  /// @brief      The number of runs in this text frame.
  ///
  /// @return     The run count.
  ///
  size_t GetRunCount() const;

  //----------------------------------------------------------------------------
  /// @brief      Returns a reference to all the text runs in this frame.
  ///
  /// @return     The runs in this frame.
  ///
  const std::vector<TextRun>& GetRuns() const;

  //----------------------------------------------------------------------------
  /// @brief      Returns whether any glyph in any run in this TextFrame
  ///             is colored and so would be cached with color already
  ///             baked in to the colored glyph.
  ///
  ///             Non-bitmap/COLR fonts only store an alpha bitmap, but
  ///             COLR fonts can potentially use the paint color in the glyph
  ///             atlas, so the color the text is being rendered with must
  ///             be considered as part of the cache key.
  bool HasColor() const;

  //----------------------------------------------------------------------------
  /// @brief      The type of atlas this run should be place in.
  ///
  ///             This return value depends primarily on the HasColor
  ///             property.
  GlyphAtlas::Type GetAtlasType() const;

  /// @brief If this text frame contains a single glyph (such as for an Icon),
  ///        then return it, otherwise std::nullopt.
  std::optional<Glyph> AsSingleGlyph() const;

  /// @brief Return the font of the first glyph run.
  const Font& GetFont() const;

  fml::StatusOr<flutter::DlPath> GetPath() const;

  /// @brief Toggle the platform-specific contrast and gamma correction in the
  ///        fragment shader.
  ///
  ///        By default, this is true on Linux to compensate for FreeType
  ///        rasterization in linear space, and false elsewhere. Setting a
  ///        value overrides this default behavior.
  void SetEnableGammaCorrection(std::optional<bool> value) {
    enable_gamma_correction_ = value;
  }
  std::optional<bool> GetEnableGammaCorrection() const {
    return enable_gamma_correction_;
  }
  //----------------------------------------------------------------------------
  /// @brief      For color (COLR) frames, the per-layer colored vector paths
  ///             for every glyph, each paired with its offset in the frame.
  ///
  ///             Empty if this frame has no color glyphs or no color-path
  ///             creator was supplied (in which case callers fall back to the
  ///             glyph atlas). Drawing these keeps COLR text sharp at any
  ///             scale.
  ///
  ///             Computed once and cached: the result depends only on this
  ///             frame's immutable blob and font tables, but the caller is the
  ///             rasterizer, which re-runs on EVERY frame that repaints this
  ///             text (Impeller has no picture raster cache). Extraction walks
  ///             the COLR table and every glyph outline, so recomputing it per
  ///             frame dominated the cost of scrolling color text.
  const std::vector<ColorGlyphLayer>& GetColorPaths() const;

 private:
  std::vector<TextRun> runs_;
  Rect bounds_;
  bool has_color_;
  const PathCreator path_creator_;
  const ColorPathCreator color_path_creator_;
  std::optional<bool> enable_gamma_correction_ = std::nullopt;
  // Lazily built by GetColorPaths() / GetPath(). call_once (rather than a bare
  // bool) keeps them safe if two rasterizer threads ever share a frame; after
  // the first call the cost is an atomic load.
  mutable std::once_flag color_paths_once_;
  mutable std::vector<ColorGlyphLayer> color_paths_;
  mutable std::once_flag path_once_;
  mutable std::optional<fml::StatusOr<flutter::DlPath>> path_;
};

}  // namespace impeller

#endif  // FLUTTER_IMPELLER_TYPOGRAPHER_TEXT_FRAME_H_
