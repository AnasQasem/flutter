// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "flutter/lib/ui/text/font_collection.h"

#include <mutex>

#if defined(__ANDROID__)
#include <android/log.h>  // QURAN PATCH 005
#endif

#include "flutter/lib/ui/text/asset_manager_font_provider.h"
#include "flutter/lib/ui/ui_dart_state.h"
#include "flutter/lib/ui/window/platform_configuration.h"
#include "flutter/runtime/test_font_data.h"
#include "rapidjson/document.h"
#include "rapidjson/rapidjson.h"
#include "third_party/skia/include/core/SkFontMgr.h"
#include "third_party/skia/include/core/SkGraphics.h"
#include "third_party/skia/include/core/SkStream.h"
#include "third_party/skia/include/core/SkTypeface.h"
#include "third_party/tonic/dart_args.h"
#include "third_party/tonic/dart_library_natives.h"
#include "third_party/tonic/logging/dart_invoke.h"
#include "third_party/tonic/typed_data/typed_list.h"
#include "txt/asset_font_manager.h"
#include "txt/platform.h"
#include "txt/test_font_manager.h"

#if FML_OS_MACOSX || FML_OS_IOS
#include "txt/platform_mac.h"
#endif

namespace flutter {

FontCollection::FontCollection()
    : collection_(std::make_shared<txt::FontCollection>()) {
  dynamic_font_manager_ = sk_make_sp<txt::DynamicFontManager>();
  collection_->SetDynamicFontManager(dynamic_font_manager_);
}

FontCollection::~FontCollection() {
  collection_.reset();
  SkGraphics::PurgeFontCache();
}

std::shared_ptr<txt::FontCollection> FontCollection::GetFontCollection() const {
  return collection_;
}

void FontCollection::SetupDefaultFontManager(
    uint32_t font_initialization_data) {
  collection_->SetupDefaultFontManager(font_initialization_data);
}

// Font manifest yaml format:
//
// flutter:
//   fonts:
//    - family: Raleway
//      fonts:
//        - asset: fonts/Raleway-Regular.ttf
//        - asset: fonts/Raleway-Italic.ttf
//          style: italic
//    - family: RobotoMono
//      fonts:
//        - asset: fonts/RobotoMono-Regular.ttf
//        - asset: fonts/RobotoMono-Bold.ttf
//          weight: 700
//
// Structure described in https://docs.flutter.dev/cookbook/design/fonts
void FontCollection::RegisterFonts(
    const std::shared_ptr<AssetManager>& asset_manager) {
#if FML_OS_MACOSX || FML_OS_IOS
  RegisterSystemFonts(*dynamic_font_manager_);
#endif
  std::unique_ptr<fml::Mapping> manifest_mapping =
      asset_manager->GetAsMapping("FontManifest.json");
  if (manifest_mapping == nullptr) {
    FML_DLOG(WARNING) << "Could not find the font manifest in the asset store.";
    return;
  }

  rapidjson::Document document;
  static_assert(sizeof(decltype(document)::Ch) == sizeof(uint8_t), "");
  document.Parse(reinterpret_cast<const decltype(document)::Ch*>(
                     manifest_mapping->GetMapping()),
                 manifest_mapping->GetSize());

  if (document.HasParseError()) {
    FML_DLOG(WARNING) << "Error parsing the font manifest in the asset store.";
    return;
  }

  if (!document.IsArray()) {
    return;
  }

  auto font_provider =
      std::make_unique<AssetManagerFontProvider>(asset_manager);

  for (const auto& family : document.GetArray()) {
    auto family_name = family.FindMember("family");
    if (family_name == family.MemberEnd() || !family_name->value.IsString()) {
      continue;
    }

    auto family_fonts = family.FindMember("fonts");
    if (family_fonts == family.MemberEnd() || !family_fonts->value.IsArray()) {
      continue;
    }

    for (const auto& family_font : family_fonts->value.GetArray()) {
      if (!family_font.IsObject()) {
        continue;
      }

      auto font_asset = family_font.FindMember("asset");
      if (font_asset == family_font.MemberEnd() ||
          !font_asset->value.IsString()) {
        continue;
      }

      // TODO(chinmaygarde): Handle weights and styles.
      font_provider->RegisterAsset(family_name->value.GetString(),
                                   font_asset->value.GetString());
    }
  }

  collection_->SetAssetFontManager(
      sk_make_sp<txt::AssetFontManager>(std::move(font_provider)));
}

void FontCollection::RegisterTestFonts() {
  std::vector<sk_sp<SkTypeface>> test_typefaces = GetTestFontData();
  std::unique_ptr<txt::TypefaceFontAssetProvider> font_provider =
      std::make_unique<txt::TypefaceFontAssetProvider>();

  size_t index = 0;
  std::vector<std::string> names = GetTestFontFamilyNames();
  for (sk_sp<SkTypeface> typeface : test_typefaces) {
    if (typeface) {
      font_provider->RegisterTypeface(std::move(typeface), names[index]);
    }
    index++;
  }

  collection_->SetTestFontManager(
      sk_make_sp<txt::TestFontManager>(std::move(font_provider), names));

  collection_->DisableFontFallback();
}

void FontCollection::LoadFontFromList(Dart_Handle font_data_handle,
                                      Dart_Handle callback,
                                      const std::string& family_name) {
  tonic::Uint8List font_data(font_data_handle);
  UIDartState::ThrowIfUIOperationsProhibited();
  FontCollection& font_collection = UIDartState::Current()
                                        ->platform_configuration()
                                        ->client()
                                        ->GetFontCollection();

  std::unique_ptr<SkStreamAsset> font_stream = std::make_unique<SkMemoryStream>(
      font_data.data(), font_data.num_elements(), true);
  sk_sp<SkFontMgr> font_mgr = txt::GetDefaultFontManager();
  sk_sp<SkTypeface> typeface = font_mgr->makeFromStream(std::move(font_stream));
  txt::TypefaceFontAssetProvider& font_provider =
      font_collection.dynamic_font_manager_->font_provider();
  if (family_name.empty()) {
    font_provider.RegisterTypeface(typeface);
  } else {
    font_provider.RegisterTypeface(typeface, family_name);
  }
  font_collection.collection_->ClearFontFamilyCache();

  font_data.Release();
  tonic::DartInvoke(callback, {tonic::ToDart(0)});
}

void FontCollection::UnloadFont(const std::string& family_name) {
  UIDartState::ThrowIfUIOperationsProhibited();
  FontCollection& font_collection = UIDartState::Current()
                                        ->platform_configuration()
                                        ->client()
                                        ->GetFontCollection();
  txt::TypefaceFontAssetProvider& provider =
      font_collection.dynamic_font_manager_->font_provider();

  // QURAN PATCH 005: hold one ref across the drop so we can tell whether
  // anything else still pins the typeface. This matters because the font's
  // bytes hang off the typeface, not off the registration: a merged Hafs file
  // costs ~1.96 MB for the `SkMemoryStream` copy `LoadFontFromList` makes plus
  // ~3.09 MB for the sfnt FreeType rebuilds when it decompresses the WOFF.
  // While a single ref survives, `unloadFont` frees nothing at all.
  sk_sp<SkTypeface> typeface;
  if (sk_sp<SkFontStyleSet> style_set = provider.MatchFamily(family_name)) {
    if (style_set->count() > 0) {
      typeface = style_set->createTypeface(0);
    }
  }

  if (!provider.UnregisterFamily(family_name)) {
    return;
  }
  // Same reason LoadFontFromList clears it: skparagraph memoizes family
  // lookups, so a stale entry would keep handing out the typeface that was
  // just dropped.
  font_collection.collection_->ClearFontFamilyCache();

  // `unique()` is the only ref-count query Skia exposes outside SK_DEBUG, and
  // it is enough: we hold exactly one ref, so !unique() means something else
  // still pins this typeface and its bytes cannot be returned.
  const bool pinned_before_purge = typeface && !typeface->unique();

  // QURAN PATCH 005: every SkStrike owns an SkScalerContext, which holds a
  // strong ref to its typeface, and Skia's strike cache is process-global and
  // keyed by size — so a font rasterised at any size outlives its family.
  // Nothing else purges it: upstream only calls this from ~FontCollection.
  //
  // Cheap here because unloads are rare (eviction only) and this engine draws
  // Quran text as tessellated vector paths whose results the blob-path cache
  // memoises, so dropped strikes are not re-rasterised on the next frame.
  const size_t strike_bytes_before = SkGraphics::GetFontCacheUsed();
  SkGraphics::PurgeFontCache();

#if defined(__ANDROID__)
  // Same reasoning as impeller/base/quran_mem_stats.h: __android_log_print at
  // ERROR priority, because an FML_LOG(INFO) here never reached logcat.
  // pinned_after=0 means the strike cache was the only holder, so the font's
  // bytes are actually returned here; pinned_after=1 means a live DisplayList,
  // glyph atlas or Dart `ui.Paragraph` still holds it and eviction cannot win.
  __android_log_print(
      ANDROID_LOG_ERROR, "QURANMEM",
      "[unload %s] pinned_before=%d pinned_after=%d "
      "strike_cache=%lldKB->%lldKB",
      family_name.c_str(), pinned_before_purge ? 1 : 0,
      (typeface && !typeface->unique()) ? 1 : 0,
      static_cast<long long>(strike_bytes_before / 1024),
      static_cast<long long>(SkGraphics::GetFontCacheUsed() / 1024));
#else
  (void)pinned_before_purge;
  (void)strike_bytes_before;
#endif
}

}  // namespace flutter
