// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <chrono>
#include <fstream>

#include "flutter/display_list/testing/dl_test_snippets.h"
#include "flutter/testing/testing.h"
#include "gtest/gtest.h"
#include "impeller/core/host_buffer.h"
#include "impeller/playground/playground.h"
#include "impeller/playground/playground_test.h"
#include "impeller/tessellator/tessellator.h"
#include "impeller/typographer/backends/skia/text_frame_skia.h"
#include "impeller/typographer/backends/skia/typographer_context_skia.h"
#include "impeller/typographer/font_glyph_pair.h"
#include "impeller/typographer/lazy_glyph_atlas.h"
#include "impeller/typographer/rectangle_packer.h"
#include "third_party/skia/include/core/SkFont.h"
#include "third_party/skia/include/core/SkFontMgr.h"
#include "third_party/skia/include/core/SkRect.h"
#include "third_party/skia/include/core/SkTextBlob.h"
#include "third_party/skia/include/core/SkTypeface.h"
#include "txt/platform.h"

// TODO(zanderso): https://github.com/flutter/flutter/issues/127701
// NOLINTBEGIN(bugprone-unchecked-optional-access)

namespace impeller {
namespace testing {

using TypographerTest = PlaygroundTest;
INSTANTIATE_PLAYGROUND_SUITE(TypographerTest);

static std::shared_ptr<GlyphAtlas> CreateGlyphAtlas(
    Context& context,
    const TypographerContext* typographer_context,
    HostBuffer& data_host_buffer,
    GlyphAtlas::Type type,
    const Matrix& transform,
    const std::shared_ptr<GlyphAtlasContext>& atlas_context,
    const std::shared_ptr<TextFrame>& frame) {
  RenderableText render_frame{
      .text_frame = frame,
      .origin_transform = transform,
  };
  return typographer_context->CreateGlyphAtlas(context, type, data_host_buffer,
                                               atlas_context, {render_frame});
}

static std::shared_ptr<GlyphAtlas> CreateGlyphAtlas(
    Context& context,
    const TypographerContext* typographer_context,
    HostBuffer& data_host_buffer,
    GlyphAtlas::Type type,
    const Matrix& transform,
    const std::shared_ptr<GlyphAtlasContext>& atlas_context,
    const std::vector<std::shared_ptr<TextFrame>>& frames,
    const std::vector<std::optional<GlyphProperties>>& properties) {
  size_t offset = 0;
  std::vector<RenderableText> render_frames;
  render_frames.reserve(frames.size());
  for (auto& frame : frames) {
    render_frames.emplace_back(frame, transform, properties[offset++]);
  }
  return typographer_context->CreateGlyphAtlas(context, type, data_host_buffer,
                                               atlas_context, render_frames);
}

TEST_P(TypographerTest, CanConvertTextBlob) {
  SkFont font = flutter::testing::CreateTestFontOfSize(12);
  auto blob = SkTextBlob::MakeFromString(
      "the quick brown fox jumped over the lazy dog.", font);
  ASSERT_TRUE(blob);
  auto frame = MakeTextFrameFromTextBlobSkia(blob);
  ASSERT_EQ(frame->GetRunCount(), 1u);
  for (const auto& run : frame->GetRuns()) {
    ASSERT_TRUE(run.IsValid());
    ASSERT_EQ(run.GetGlyphCount(), 45u);
  }
}

TEST_P(TypographerTest, TextFrameReturnsColorPathsFromCreator) {
  std::vector<TextRun> runs;
  TextFrame frame(runs, Rect::MakeLTRB(0, 0, 10, 10), /*has_color=*/true,
                  /*path_creator=*/{},
                  /*color_path_creator=*/[]() {
                    std::vector<ColorGlyphLayer> layers(2);
                    layers[0].use_foreground_color = true;
                    layers[1].color = Color::Red();
                    return layers;
                  });
  std::vector<ColorGlyphLayer> layers = frame.GetColorPaths();
  ASSERT_EQ(layers.size(), 2u);
  EXPECT_TRUE(layers[0].use_foreground_color);
  EXPECT_FALSE(layers[1].use_foreground_color);
  EXPECT_EQ(layers[1].color, Color::Red());
}

TEST_P(TypographerTest, TextFrameWithoutColorPathCreatorHasNoColorPaths) {
  TextFrame frame;
  EXPECT_TRUE(frame.GetColorPaths().empty());
}

// Impeller's tessellation cache is keyed by SkPath::getGenerationID(), which is
// per-instance. Every rebuild of a widget subtree makes a NEW blob for the same
// text, so unless equal geometry yields one shared path, a page re-tessellates
// from scratch on every visit and pre-painting it can never warm anything.
TEST_P(TypographerTest, EqualBlobGeometrySharesOnePathIdentity) {
  SkFont font = flutter::testing::CreateTestFontOfSize(12);
  auto make = [&font](const char* text) {
    auto blob = SkTextBlob::MakeFromString(text, font);
    EXPECT_TRUE(blob);
    return MakeTextFrameFromTextBlobSkia(blob)->GetPath();
  };

  fml::StatusOr<flutter::DlPath> a = make("the quick brown fox");
  fml::StatusOr<flutter::DlPath> b = make("the quick brown fox");
  fml::StatusOr<flutter::DlPath> other = make("jumped over the lazy dog");
  ASSERT_TRUE(a.ok());
  ASSERT_TRUE(b.ok());
  ASSERT_TRUE(other.ok());

  // Independently built blobs, identical geometry => one cache entry.
  EXPECT_EQ(a.value().GetGeometryID(), b.value().GetGeometryID());
  EXPECT_NE(a.value().GetGeometryID(), other.value().GetGeometryID());

  // Same text at a different size is different geometry and must not collide.
  SkFont bigger = flutter::testing::CreateTestFontOfSize(24);
  auto big_blob = SkTextBlob::MakeFromString("the quick brown fox", bigger);
  ASSERT_TRUE(big_blob);
  fml::StatusOr<flutter::DlPath> big =
      MakeTextFrameFromTextBlobSkia(big_blob)->GetPath();
  ASSERT_TRUE(big.ok());
  EXPECT_NE(a.value().GetGeometryID(), big.value().GetGeometryID());
}

// The blob-path cache used to `clear()` wholesale on overflow, which threw away
// the entries for the pages currently on screen along with everything else. In
// this app that was reachable in ordinary use — 604 pages x ~15 lines against a
// 256-entry cap, i.e. every ~17 pages of swiping — and it cascaded into the
// tessellation cache, which keys on the generation id of exactly these paths.
//
// A blob that is still in use must survive an overflow.
TEST_P(TypographerTest, BlobPathCacheKeepsHotEntryAcrossOverflow) {
  SkFont font = flutter::testing::CreateTestFontOfSize(12);
  auto path_id = [&font](const std::string& text) -> uint32_t {
    auto blob = SkTextBlob::MakeFromString(text.c_str(), font);
    EXPECT_TRUE(blob);
    fml::StatusOr<flutter::DlPath> path =
        MakeTextFrameFromTextBlobSkia(blob)->GetPath();
    EXPECT_TRUE(path.ok());
    return path.value().GetGeometryID();
  };

  // Letters only: the test font has no digit glyphs, and an all-notdef blob
  // yields an empty outline that never reaches the cache.
  auto filler = [](int i) {
    std::string text = "fill";
    for (int n = i; text.size() < 12u; n /= 26) {
      text += static_cast<char>('a' + (n % 26));
      if (n < 26) {
        break;
      }
    }
    return text;
  };

  const std::string hot = "the quick brown fox";
  const uint32_t hot_id = path_id(hot);

  // kMaxCachedBlobPaths is 256. Overflow it several times over, touching the
  // hot entry often enough that it is never the least recently used.
  for (int i = 0; i < 900; i++) {
    path_id(filler(i));
    if (i % 8 == 0) {
      ASSERT_EQ(path_id(hot), hot_id) << "hot entry evicted at filler " << i;
    }
  }

  // A clear-all would have rebuilt this with a fresh SkPath and a new
  // generation id, silently invalidating the tessellation cache with it.
  EXPECT_EQ(path_id(hot), hot_id);
}

// The rasterizer asks for these on every frame that repaints the text, and
// extraction walks the COLR table plus every glyph outline. Recomputing it per
// frame dominated the cost of scrolling color text, so the result is cached.
TEST_P(TypographerTest, ColorPathsAreExtractedOnlyOnce) {
  std::vector<TextRun> runs;
  int extractions = 0;
  TextFrame frame(runs, Rect::MakeLTRB(0, 0, 10, 10), /*has_color=*/true,
                  /*path_creator=*/{},
                  /*color_path_creator=*/[&extractions]() {
                    extractions++;
                    std::vector<ColorGlyphLayer> layers(1);
                    layers[0].color = Color::Red();
                    return layers;
                  });

  ASSERT_EQ(frame.GetColorPaths().size(), 1u);
  ASSERT_EQ(frame.GetColorPaths().size(), 1u);
  frame.GetColorPaths();
  EXPECT_EQ(extractions, 1);
}

TEST_P(TypographerTest, NonColrColorFontHasNoColorPaths) {
#if FML_OS_MACOSX
  auto mapping = flutter::testing::OpenFixtureAsSkData("Apple Color Emoji.ttc");
#else
  auto mapping = flutter::testing::OpenFixtureAsSkData("NotoColorEmoji.ttf");
#endif
  ASSERT_TRUE(mapping);
  sk_sp<SkFontMgr> font_mgr = txt::GetDefaultFontManager();
  SkFont emoji_font(font_mgr->makeFromData(mapping), 50.0);
  auto frame = MakeTextFrameFromTextBlobSkia(
      SkTextBlob::MakeFromString("😀 ", emoji_font));
  ASSERT_TRUE(frame->HasColor());
  // Bitmap emoji formats (CBDT/sbix) carry no COLRv0 layer list, so color
  // path extraction must come back empty and rendering falls back to the
  // color glyph atlas.
  EXPECT_TRUE(frame->GetColorPaths().empty());
}

// MEASUREMENT (not a correctness test): where does the cost of drawing a page
// of COLR text actually go? Splits outline decode from layer extraction from
// tessellation, using a real 2500-upem COLR Quran font. Needs /tmp/hafs_1.ttf
// and /tmp/hafs_glyphs.txt; skips silently otherwise.
TEST_P(TypographerTest, MeasureColorTextCost) {
  sk_sp<SkData> font_data = SkData::MakeFromFileName("/tmp/hafs_1.ttf");
  if (!font_data) {
    GTEST_SKIP() << "no /tmp/hafs_1.ttf";
  }
  std::ifstream gid_file("/tmp/hafs_glyphs.txt");
  if (!gid_file) {
    GTEST_SKIP() << "no /tmp/hafs_glyphs.txt";
  }
  std::vector<SkGlyphID> gids;
  for (int gid = 0; gid_file >> gid;) {
    gids.push_back(static_cast<SkGlyphID>(gid));
  }
  ASSERT_FALSE(gids.empty());

  sk_sp<SkFontMgr> font_mgr = txt::GetDefaultFontManager();
  sk_sp<SkTypeface> typeface = font_mgr->makeFromData(font_data);
  ASSERT_TRUE(typeface);

  using Clock = std::chrono::steady_clock;
  auto ms = [](Clock::duration d) {
    return std::chrono::duration<double, std::milli>(d).count();
  };

  // One "page": every glyph laid out on a grid, at a typical reading size.
  auto build_blob = [&](Scalar size) {
    SkFont font(typeface, size);
    SkTextBlobBuilder builder;
    const SkTextBlobBuilder::RunBuffer& run =
        builder.allocRunPos(font, gids.size());
    for (size_t i = 0; i < gids.size(); i++) {
      run.glyphs[i] = gids[i];
      run.points()[i] =
          SkPoint::Make((i % 20) * size * 1.2f, (i / 20) * size * 1.6f);
    }
    return builder.make();
  };

  // Cold: a size never extracted before, so Skia must decode every outline.
  sk_sp<SkTextBlob> cold_blob = build_blob(25.0f);
  auto t0 = Clock::now();
  std::shared_ptr<TextFrame> cold_frame =
      MakeTextFrameFromTextBlobSkia(cold_blob);
  auto t1 = Clock::now();
  size_t layer_count = cold_frame->GetColorPaths().size();
  auto t2 = Clock::now();

  // Warm: same size again in a NEW frame — Skia's strike still holds the
  // outlines, so this isolates extraction from decode.
  sk_sp<SkTextBlob> warm_blob = build_blob(25.0f);
  auto t3 = Clock::now();
  std::shared_ptr<TextFrame> warm_frame =
      MakeTextFrameFromTextBlobSkia(warm_blob);
  size_t warm_layers = warm_frame->GetColorPaths().size();
  auto t4 = Clock::now();

  // A DIFFERENT size, as the app's per-line FittedBox produces: does Skia
  // re-decode every outline for each size?
  sk_sp<SkTextBlob> other_size = build_blob(31.0f);
  auto t5 = Clock::now();
  std::shared_ptr<TextFrame> other_frame =
      MakeTextFrameFromTextBlobSkia(other_size);
  other_frame->GetColorPaths();
  auto t6 = Clock::now();

  // Per-frame cost: tessellating every layer path, as the rasterizer does.
  std::vector<Point> points;
  std::vector<uint16_t> indices;
  auto t7 = Clock::now();
  for (const ColorGlyphLayer& layer : cold_frame->GetColorPaths()) {
    Tessellator::TessellateConvexInternal(layer.path, points, indices, 3.5f);
  }
  auto t8 = Clock::now();

  // The same work through the real entry point, which now caches by path
  // identity + tolerance. First pass populates, second pass should be a copy.
  std::shared_ptr<HostBuffer> vtx_buffer = HostBuffer::Create(
      GetContext()->GetResourceAllocator(), GetContext()->GetIdleWaiter(),
      GetContext()->GetCapabilities()->GetMinimumUniformAlignment());
  std::shared_ptr<HostBuffer> idx_buffer = HostBuffer::Create(
      GetContext()->GetResourceAllocator(), GetContext()->GetIdleWaiter(),
      GetContext()->GetCapabilities()->GetMinimumUniformAlignment());
  Tessellator tessellator;
  auto tessellate_page = [&]() {
    for (const ColorGlyphLayer& layer : cold_frame->GetColorPaths()) {
      tessellator.TessellateConvex(layer.path, *vtx_buffer, *idx_buffer, 3.5f,
                                   /*supports_primitive_restart=*/false,
                                   /*supports_triangle_fan=*/false);
    }
  };
  auto t11 = Clock::now();
  tessellate_page();
  auto t12 = Clock::now();
  tessellate_page();
  auto t13 = Clock::now();
  tessellate_page();
  auto t14 = Clock::now();

  // Vulkan (i.e. Android) takes the primitive-restart streaming path instead,
  // which fills caller-sized storage — measure and exercise that variant too.
  Tessellator restart_tessellator;
  auto tessellate_page_restart = [&]() {
    for (const ColorGlyphLayer& layer : cold_frame->GetColorPaths()) {
      restart_tessellator.TessellateConvex(layer.path, *vtx_buffer, *idx_buffer,
                                           3.5f,
                                           /*supports_primitive_restart=*/true,
                                           /*supports_triangle_fan=*/true);
    }
  };
  auto t15 = Clock::now();
  tessellate_page_restart();
  auto t16 = Clock::now();
  tessellate_page_restart();
  auto t17 = Clock::now();

  // Second call on the memoized frame: proves GetColorPaths is not re-running.
  auto t9 = Clock::now();
  cold_frame->GetColorPaths();
  auto t10 = Clock::now();

  FML_LOG(ERROR)
      << "\n=== COLR page cost (" << gids.size() << " glyphs, " << layer_count
      << " layers) ===\n"
      << "  MakeTextFrame (has_color scan): " << ms(t1 - t0) << " ms\n"
      << "  extract COLD (decode+build):    " << ms(t2 - t1) << " ms\n"
      << "  frame+extract WARM strike:      " << ms(t4 - t3) << " ms\n"
      << "  frame+extract NEW SIZE:         " << ms(t6 - t5) << " ms\n"
      << "  tessellate all layers:          " << ms(t8 - t7) << " ms\n"
      << "  memoized GetColorPaths:         " << ms(t10 - t9) << " ms\n"
      << "  TessellateConvex pass 1 (cold): " << ms(t12 - t11) << " ms\n"
      << "  TessellateConvex pass 2 (cache):" << ms(t13 - t12) << " ms\n"
      << "  TessellateConvex pass 3 (cache):" << ms(t14 - t13) << " ms\n"
      << "  restart/fan pass 1 (cold):      " << ms(t16 - t15) << " ms\n"
      << "  restart/fan pass 2 (cache):     " << ms(t17 - t16) << " ms\n";
  EXPECT_EQ(layer_count, warm_layers);
}

TEST_P(TypographerTest, CanCreateRenderContext) {
  auto context = TypographerContextSkia::Make();
  ASSERT_TRUE(context && context->IsValid());
}

TEST_P(TypographerTest, CanCreateGlyphAtlas) {
  auto context = TypographerContextSkia::Make();
  auto atlas_context =
      context->CreateGlyphAtlasContext(GlyphAtlas::Type::kAlphaBitmap);
  auto data_host_buffer = HostBuffer::Create(
      GetContext()->GetResourceAllocator(), GetContext()->GetIdleWaiter(),
      GetContext()->GetCapabilities()->GetMinimumUniformAlignment());
  ASSERT_TRUE(context && context->IsValid());
  SkFont sk_font = flutter::testing::CreateTestFontOfSize(12);
  auto blob = SkTextBlob::MakeFromString("hello", sk_font);
  ASSERT_TRUE(blob);
  auto atlas =
      CreateGlyphAtlas(*GetContext(), context.get(), *data_host_buffer,
                       GlyphAtlas::Type::kAlphaBitmap, Matrix(), atlas_context,
                       MakeTextFrameFromTextBlobSkia(blob));

  ASSERT_NE(atlas, nullptr);
  ASSERT_NE(atlas->GetTexture(), nullptr);
  ASSERT_EQ(atlas->GetType(), GlyphAtlas::Type::kAlphaBitmap);
  ASSERT_EQ(atlas->GetGlyphCount(), 4llu);

  std::optional<impeller::ScaledFont> first_scaled_font;
  std::optional<impeller::SubpixelGlyph> first_glyph;
  Rect first_rect;
  atlas->IterateGlyphs([&](const ScaledFont& scaled_font,
                           const SubpixelGlyph& glyph,
                           const Rect& rect) -> bool {
    first_scaled_font = scaled_font;
    first_glyph = glyph;
    first_rect = rect;
    return false;
  });

  ASSERT_TRUE(first_scaled_font.has_value());
  ASSERT_TRUE(atlas
                  ->FindFontGlyphBounds(
                      {first_scaled_font.value(), first_glyph.value()})
                  .has_value());
}

TEST_P(TypographerTest, LazyAtlasTracksColor) {
  auto data_host_buffer = HostBuffer::Create(
      GetContext()->GetResourceAllocator(), GetContext()->GetIdleWaiter(),
      GetContext()->GetCapabilities()->GetMinimumUniformAlignment());
#if FML_OS_MACOSX
  auto mapping = flutter::testing::OpenFixtureAsSkData("Apple Color Emoji.ttc");
#else
  auto mapping = flutter::testing::OpenFixtureAsSkData("NotoColorEmoji.ttf");
#endif
  ASSERT_TRUE(mapping);
  sk_sp<SkFontMgr> font_mgr = txt::GetDefaultFontManager();
  SkFont emoji_font(font_mgr->makeFromData(mapping), 50.0);
  SkFont sk_font = flutter::testing::CreateTestFontOfSize(12);

  auto blob = SkTextBlob::MakeFromString("hello", sk_font);
  ASSERT_TRUE(blob);
  auto frame = MakeTextFrameFromTextBlobSkia(blob);

  ASSERT_FALSE(frame->GetAtlasType() == GlyphAtlas::Type::kColorBitmap);

  LazyGlyphAtlas lazy_atlas(TypographerContextSkia::Make());

  lazy_atlas.AddTextFrame(frame, {0, 0}, Matrix(), {});

  frame = MakeTextFrameFromTextBlobSkia(
      SkTextBlob::MakeFromString("😀 ", emoji_font));

  ASSERT_TRUE(frame->GetAtlasType() == GlyphAtlas::Type::kColorBitmap);

  lazy_atlas.AddTextFrame(frame, {0, 0}, Matrix(), {});

  // Creates different atlases for color and red bitmap.
  auto color_atlas = lazy_atlas.CreateOrGetGlyphAtlas(
      *GetContext(), *data_host_buffer, GlyphAtlas::Type::kColorBitmap);

  auto bitmap_atlas = lazy_atlas.CreateOrGetGlyphAtlas(
      *GetContext(), *data_host_buffer, GlyphAtlas::Type::kAlphaBitmap);

  ASSERT_FALSE(color_atlas == bitmap_atlas);
}

TEST_P(TypographerTest, GlyphAtlasWithOddUniqueGlyphSize) {
  auto context = TypographerContextSkia::Make();
  auto atlas_context =
      context->CreateGlyphAtlasContext(GlyphAtlas::Type::kAlphaBitmap);
  auto data_host_buffer = HostBuffer::Create(
      GetContext()->GetResourceAllocator(), GetContext()->GetIdleWaiter(),
      GetContext()->GetCapabilities()->GetMinimumUniformAlignment());
  ASSERT_TRUE(context && context->IsValid());
  SkFont sk_font = flutter::testing::CreateTestFontOfSize(12);
  auto blob = SkTextBlob::MakeFromString("AGH", sk_font);
  ASSERT_TRUE(blob);
  auto atlas =
      CreateGlyphAtlas(*GetContext(), context.get(), *data_host_buffer,
                       GlyphAtlas::Type::kAlphaBitmap, Matrix(), atlas_context,
                       MakeTextFrameFromTextBlobSkia(blob));
  ASSERT_NE(atlas, nullptr);
  ASSERT_NE(atlas->GetTexture(), nullptr);

  EXPECT_EQ(atlas->GetTexture()->GetSize().width, 4096u);
  EXPECT_EQ(atlas->GetTexture()->GetSize().height, 1024u);
}

TEST_P(TypographerTest, GlyphAtlasIsRecycledIfUnchanged) {
  auto context = TypographerContextSkia::Make();
  auto atlas_context =
      context->CreateGlyphAtlasContext(GlyphAtlas::Type::kAlphaBitmap);
  auto data_host_buffer = HostBuffer::Create(
      GetContext()->GetResourceAllocator(), GetContext()->GetIdleWaiter(),
      GetContext()->GetCapabilities()->GetMinimumUniformAlignment());
  ASSERT_TRUE(context && context->IsValid());
  SkFont sk_font = flutter::testing::CreateTestFontOfSize(12);
  auto blob = SkTextBlob::MakeFromString("spooky skellingtons", sk_font);
  ASSERT_TRUE(blob);
  auto atlas =
      CreateGlyphAtlas(*GetContext(), context.get(), *data_host_buffer,
                       GlyphAtlas::Type::kAlphaBitmap, Matrix(), atlas_context,
                       MakeTextFrameFromTextBlobSkia(blob));
  ASSERT_NE(atlas, nullptr);
  ASSERT_NE(atlas->GetTexture(), nullptr);
  ASSERT_EQ(atlas, atlas_context->GetGlyphAtlas());

  // now attempt to re-create an atlas with the same text blob.

  auto next_atlas =
      CreateGlyphAtlas(*GetContext(), context.get(), *data_host_buffer,
                       GlyphAtlas::Type::kAlphaBitmap, Matrix(), atlas_context,
                       MakeTextFrameFromTextBlobSkia(blob));
  ASSERT_EQ(atlas, next_atlas);
  ASSERT_EQ(atlas_context->GetGlyphAtlas(), atlas);
}

TEST_P(TypographerTest, GlyphAtlasWithLotsOfdUniqueGlyphSize) {
  auto data_host_buffer = HostBuffer::Create(
      GetContext()->GetResourceAllocator(), GetContext()->GetIdleWaiter(),
      GetContext()->GetCapabilities()->GetMinimumUniformAlignment());
  auto context = TypographerContextSkia::Make();
  auto atlas_context =
      context->CreateGlyphAtlasContext(GlyphAtlas::Type::kAlphaBitmap);
  ASSERT_TRUE(context && context->IsValid());

  const char* test_string =
      "QWERTYUIOPASDFGHJKLZXCVBNMqewrtyuiopasdfghjklzxcvbnm,.<>[]{};':"
      "2134567890-=!@#$%^&*()_+"
      "œ∑´®†¥¨ˆøπ““‘‘åß∂ƒ©˙∆˚¬…æ≈ç√∫˜µ≤≥≥≥≥÷¡™£¢∞§¶•ªº–≠⁄€‹›ﬁﬂ‡°·‚—±Œ„´‰Á¨Ø∏”’/"
      "* Í˝ */¸˛Ç◊ı˜Â¯˘¿";

  SkFont sk_font = flutter::testing::CreateTestFontOfSize(12);
  auto blob = SkTextBlob::MakeFromString(test_string, sk_font);
  ASSERT_TRUE(blob);

  size_t size_count = 8;
  std::vector<RenderableText> render_frames;
  for (size_t index = 0; index < size_count; index += 1) {
    Scalar scale = 6.0f * index / 10.0f;
    render_frames.emplace_back(MakeTextFrameFromTextBlobSkia(blob),
                               Matrix::MakeScale({scale, scale, 1.0f}),
                               GlyphProperties{});
  };
  auto atlas = context->CreateGlyphAtlas(
      *GetContext(), GlyphAtlas::Type::kAlphaBitmap, *data_host_buffer,
      atlas_context, render_frames);
  ASSERT_NE(atlas, nullptr);
  ASSERT_NE(atlas->GetTexture(), nullptr);

  std::set<uint16_t> unique_glyphs;
  std::vector<uint16_t> total_glyphs;
  atlas->IterateGlyphs([&](const ScaledFont& scaled_font,
                           const SubpixelGlyph& glyph, const Rect& rect) {
    unique_glyphs.insert(glyph.glyph.index);
    total_glyphs.push_back(glyph.glyph.index);
    return true;
  });

  // These numbers may be different due to subpixel positions.
  EXPECT_LE(unique_glyphs.size() * size_count, atlas->GetGlyphCount());
  EXPECT_EQ(total_glyphs.size(), atlas->GetGlyphCount());

  EXPECT_TRUE(atlas->GetGlyphCount() > 0);
  EXPECT_TRUE(atlas->GetTexture()->GetSize().width > 0);
  EXPECT_TRUE(atlas->GetTexture()->GetSize().height > 0);
}

TEST_P(TypographerTest, GlyphAtlasTextureIsRecycledIfUnchanged) {
  auto data_host_buffer = HostBuffer::Create(
      GetContext()->GetResourceAllocator(), GetContext()->GetIdleWaiter(),
      GetContext()->GetCapabilities()->GetMinimumUniformAlignment());
  auto context = TypographerContextSkia::Make();
  auto atlas_context =
      context->CreateGlyphAtlasContext(GlyphAtlas::Type::kAlphaBitmap);
  ASSERT_TRUE(context && context->IsValid());
  SkFont sk_font = flutter::testing::CreateTestFontOfSize(12);
  auto blob = SkTextBlob::MakeFromString("spooky 1", sk_font);
  ASSERT_TRUE(blob);
  auto atlas =
      CreateGlyphAtlas(*GetContext(), context.get(), *data_host_buffer,
                       GlyphAtlas::Type::kAlphaBitmap, Matrix(), atlas_context,
                       MakeTextFrameFromTextBlobSkia(blob));
  auto old_packer = atlas_context->GetRectPacker();

  ASSERT_NE(atlas, nullptr);
  ASSERT_NE(atlas->GetTexture(), nullptr);
  ASSERT_EQ(atlas, atlas_context->GetGlyphAtlas());

  auto* first_texture = atlas->GetTexture().get();

  // Now create a new glyph atlas with a nearly identical blob.

  auto blob2 = SkTextBlob::MakeFromString("spooky 2", sk_font);
  auto next_atlas =
      CreateGlyphAtlas(*GetContext(), context.get(), *data_host_buffer,
                       GlyphAtlas::Type::kAlphaBitmap, Matrix(), atlas_context,
                       MakeTextFrameFromTextBlobSkia(blob2));
  ASSERT_EQ(atlas, next_atlas);
  auto* second_texture = next_atlas->GetTexture().get();

  auto new_packer = atlas_context->GetRectPacker();

  ASSERT_EQ(second_texture, first_texture);
  ASSERT_EQ(old_packer, new_packer);
}

TEST_P(TypographerTest, GlyphColorIsPartOfCacheKey) {
  auto data_host_buffer = HostBuffer::Create(
      GetContext()->GetResourceAllocator(), GetContext()->GetIdleWaiter(),
      GetContext()->GetCapabilities()->GetMinimumUniformAlignment());
#if FML_OS_MACOSX
  auto mapping = flutter::testing::OpenFixtureAsSkData("Apple Color Emoji.ttc");
#else
  auto mapping = flutter::testing::OpenFixtureAsSkData("NotoColorEmoji.ttf");
#endif
  ASSERT_TRUE(mapping);
  sk_sp<SkFontMgr> font_mgr = txt::GetDefaultFontManager();
  SkFont emoji_font(font_mgr->makeFromData(mapping), 50.0);

  auto context = TypographerContextSkia::Make();
  auto atlas_context =
      context->CreateGlyphAtlasContext(GlyphAtlas::Type::kColorBitmap);

  // Create two frames with the same character and a different color, expect
  // that it adds a character.
  auto frame = MakeTextFrameFromTextBlobSkia(
      SkTextBlob::MakeFromString("😂", emoji_font));
  auto frame_2 = MakeTextFrameFromTextBlobSkia(
      SkTextBlob::MakeFromString("😂", emoji_font));
  std::vector<std::optional<GlyphProperties>> properties = {
      GlyphProperties{.color = Color::Red()},
      GlyphProperties{.color = Color::Blue()},
  };

  auto next_atlas =
      CreateGlyphAtlas(*GetContext(), context.get(), *data_host_buffer,
                       GlyphAtlas::Type::kColorBitmap, Matrix(), atlas_context,
                       {frame, frame_2}, properties);

  EXPECT_EQ(next_atlas->GetGlyphCount(), 2u);
}

TEST_P(TypographerTest, GlyphColorIsIgnoredForNonEmojiFonts) {
  auto data_host_buffer = HostBuffer::Create(
      GetContext()->GetResourceAllocator(), GetContext()->GetIdleWaiter(),
      GetContext()->GetCapabilities()->GetMinimumUniformAlignment());
  sk_sp<SkFontMgr> font_mgr = txt::GetDefaultFontManager();
  sk_sp<SkTypeface> typeface =
      font_mgr->matchFamilyStyle("Arial", SkFontStyle::Normal());
  SkFont sk_font(typeface, 0.5f);

  auto context = TypographerContextSkia::Make();
  auto atlas_context =
      context->CreateGlyphAtlasContext(GlyphAtlas::Type::kColorBitmap);

  // Create two frames with the same character and a different color, but as a
  // non-emoji font the text frame constructor will ignore it.
  auto frame =
      MakeTextFrameFromTextBlobSkia(SkTextBlob::MakeFromString("A", sk_font));
  auto frame_2 =
      MakeTextFrameFromTextBlobSkia(SkTextBlob::MakeFromString("A", sk_font));
  std::vector<std::optional<GlyphProperties>> properties = {
      GlyphProperties{},
      GlyphProperties{},
  };

  auto next_atlas =
      CreateGlyphAtlas(*GetContext(), context.get(), *data_host_buffer,
                       GlyphAtlas::Type::kColorBitmap, Matrix(), atlas_context,
                       {frame, frame_2}, properties);

  EXPECT_EQ(next_atlas->GetGlyphCount(), 1u);
}

TEST_P(TypographerTest, RectanglePackerAddsNonoverlapingRectangles) {
  auto packer = RectanglePacker::Factory(200, 100);
  ASSERT_NE(packer, nullptr);
  ASSERT_EQ(packer->PercentFull(), 0);

  const SkIRect packer_area = SkIRect::MakeXYWH(0, 0, 200, 100);

  IPoint16 first_output = {-1, -1};  // Fill with sentinel values
  ASSERT_TRUE(packer->AddRect(20, 20, &first_output));
  // Make sure the rectangle is placed such that it is inside the bounds of
  // the packer's area.
  const SkIRect first_rect =
      SkIRect::MakeXYWH(first_output.x(), first_output.y(), 20, 20);
  ASSERT_TRUE(SkIRect::Intersects(packer_area, first_rect));

  // Initial area was 200 x 100 = 20_000
  // We added 20x20 = 400. 400 / 20_000 == 0.02 == 2%
  ASSERT_TRUE(flutter::testing::NumberNear(packer->PercentFull(), 0.02));

  IPoint16 second_output = {-1, -1};
  ASSERT_TRUE(packer->AddRect(140, 90, &second_output));
  const SkIRect second_rect =
      SkIRect::MakeXYWH(second_output.x(), second_output.y(), 140, 90);
  // Make sure the rectangle is placed such that it is inside the bounds of
  // the packer's area but not in the are of the first rectangle.
  ASSERT_TRUE(SkIRect::Intersects(packer_area, second_rect));
  ASSERT_FALSE(SkIRect::Intersects(first_rect, second_rect));

  // We added another 90 x 140 = 12_600 units, now taking us to 13_000
  // 13_000 / 20_000 == 0.65 == 65%
  ASSERT_TRUE(flutter::testing::NumberNear(packer->PercentFull(), 0.65));

  // There's enough area to add this rectangle, but no space big enough for
  // the 50 units of width.
  IPoint16 output;
  ASSERT_FALSE(packer->AddRect(50, 50, &output));
  // Should be unchanged.
  ASSERT_TRUE(flutter::testing::NumberNear(packer->PercentFull(), 0.65));

  packer->Reset();
  // Should be empty now.
  ASSERT_EQ(packer->PercentFull(), 0);
}

TEST(TypographerTest, RectanglePackerFillsRows) {
  auto skyline = RectanglePacker::Factory(257, 256);

  // Fill up the first row.
  IPoint16 loc;
  for (auto i = 0u; i < 16; i++) {
    skyline->AddRect(16, 16, &loc);
  }
  // Last rectangle still in first row.
  EXPECT_EQ(loc.x(), 256 - 16);
  EXPECT_EQ(loc.y(), 0);

  // Fill up second row.
  for (auto i = 0u; i < 16; i++) {
    skyline->AddRect(16, 16, &loc);
  }

  EXPECT_EQ(loc.x(), 256 - 16);
  EXPECT_EQ(loc.y(), 16);
}

TEST_P(TypographerTest, GlyphAtlasTextureWillGrowTilMaxTextureSize) {
  if (GetBackend() == PlaygroundBackend::kOpenGLES) {
    GTEST_SKIP() << "Atlas growth isn't supported for OpenGLES currently.";
  }

  auto data_host_buffer = HostBuffer::Create(
      GetContext()->GetResourceAllocator(), GetContext()->GetIdleWaiter(),
      GetContext()->GetCapabilities()->GetMinimumUniformAlignment());
  auto context = TypographerContextSkia::Make();
  auto atlas_context =
      context->CreateGlyphAtlasContext(GlyphAtlas::Type::kAlphaBitmap);
  ASSERT_TRUE(context && context->IsValid());
  SkFont sk_font = flutter::testing::CreateTestFontOfSize(12);
  auto blob = SkTextBlob::MakeFromString("A", sk_font);
  ASSERT_TRUE(blob);
  auto atlas =
      CreateGlyphAtlas(*GetContext(), context.get(), *data_host_buffer,
                       GlyphAtlas::Type::kAlphaBitmap, Matrix(), atlas_context,
                       MakeTextFrameFromTextBlobSkia(blob));
  // Continually append new glyphs until the glyph size grows to the maximum.
  // Note that the sizes here are more or less experimentally determined, but
  // the important expectation is that the atlas size will shrink again after
  // growing to the maximum size.
  constexpr ISize expected_sizes[13] = {
      {4096, 4096},   //
      {4096, 4096},   //
      {4096, 8192},   //
      {4096, 8192},   //
      {4096, 8192},   //
      {4096, 8192},   //
      {4096, 16384},  //
      {4096, 16384},  //
      {4096, 16384},  //
      {4096, 16384},  //
      {4096, 16384},  //
      {4096, 16384},  //
      {4096, 4096}    // Shrinks!
  };

  SkFont sk_font_small = flutter::testing::CreateTestFontOfSize(10);

  for (int i = 0; i < 13; i++) {
    SkTextBlobBuilder builder;

    auto add_char = [&](const SkFont& sk_font, char c) {
      int count = sk_font.countText(&c, 1, SkTextEncoding::kUTF8);
      auto buffer = builder.allocRunPos(sk_font, count);
      sk_font.textToGlyphs(&c, 1, SkTextEncoding::kUTF8,
                           {buffer.glyphs, count});
      sk_font.getPos({buffer.glyphs, count}, {buffer.points(), count},
                     {0, 0} /*=origin*/);
    };

    SkFont sk_font = flutter::testing::CreateTestFontOfSize(50 + i);
    add_char(sk_font, 'A');
    add_char(sk_font_small, 'B');
    auto blob = builder.make();

    Matrix transform = Matrix::MakeScale({50.0f + i, 50.0f + i, 1.0f});
    atlas =
        CreateGlyphAtlas(*GetContext(), context.get(), *data_host_buffer,
                         GlyphAtlas::Type::kAlphaBitmap, transform,
                         atlas_context, MakeTextFrameFromTextBlobSkia(blob));
    ASSERT_TRUE(!!atlas);
    EXPECT_EQ(atlas->GetTexture()->GetTextureDescriptor().size,
              expected_sizes[i]);
  }

  // The final atlas should contain both the "A" glyph (which was not present
  // in the previous atlas) and the "B" glyph (which existed in the previous
  // atlas).
  ASSERT_EQ(atlas->GetGlyphCount(), 2u);
}

TEST_P(TypographerTest, InvalidAtlasForcesRepopulation) {
  SkFont font = flutter::testing::CreateTestFontOfSize(12);
  auto blob = SkTextBlob::MakeFromString(
      "the quick brown fox jumped over the lazy dog.", font);
  ASSERT_TRUE(blob);
  auto frame = MakeTextFrameFromTextBlobSkia(blob);

  auto context = TypographerContextSkia::Make();
  auto atlas_context =
      context->CreateGlyphAtlasContext(GlyphAtlas::Type::kAlphaBitmap);
  auto data_host_buffer = HostBuffer::Create(
      GetContext()->GetResourceAllocator(), GetContext()->GetIdleWaiter(),
      GetContext()->GetCapabilities()->GetMinimumUniformAlignment());

  auto atlas = CreateGlyphAtlas(*GetContext(), context.get(), *data_host_buffer,
                                GlyphAtlas::Type::kAlphaBitmap, Matrix(),
                                atlas_context, frame);

  auto second_context = TypographerContextSkia::Make();
  auto second_atlas_context =
      second_context->CreateGlyphAtlasContext(GlyphAtlas::Type::kAlphaBitmap);

  EXPECT_FALSE(second_atlas_context->GetGlyphAtlas()->IsValid());

  atlas = CreateGlyphAtlas(*GetContext(), second_context.get(),
                           *data_host_buffer, GlyphAtlas::Type::kAlphaBitmap,
                           Matrix(), second_atlas_context, frame);

  EXPECT_TRUE(second_atlas_context->GetGlyphAtlas()->IsValid());
}

}  // namespace testing
}  // namespace impeller

// NOLINTEND(bugprone-unchecked-optional-access)
