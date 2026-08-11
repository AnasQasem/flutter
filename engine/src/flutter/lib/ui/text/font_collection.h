// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef FLUTTER_LIB_UI_TEXT_FONT_COLLECTION_H_
#define FLUTTER_LIB_UI_TEXT_FONT_COLLECTION_H_

#include <memory>
#include <vector>

#include "flutter/assets/asset_manager.h"
#include "flutter/fml/macros.h"
#include "flutter/fml/memory/ref_ptr.h"
#include "third_party/tonic/typed_data/typed_list.h"
#include "txt/font_collection.h"

namespace flutter {

class FontCollection {
 public:
  FontCollection();

  virtual ~FontCollection();

  std::shared_ptr<txt::FontCollection> GetFontCollection() const;

  void SetupDefaultFontManager(uint32_t font_initialization_data);

  // Virtual for testing.
  virtual void RegisterFonts(
      const std::shared_ptr<AssetManager>& asset_manager);

  void RegisterTestFonts();

  static void LoadFontFromList(Dart_Handle font_data_handle,
                               Dart_Handle callback,
                               const std::string& family_name);

  /// Releases a family previously registered by [LoadFontFromList], freeing
  /// the font bytes it copied.
  ///
  /// [LoadFontFromList] copies the whole font into an `SkMemoryStream`
  /// (`copyData=true`) and the typeface owns that copy for the life of the
  /// process, because until now nothing could take a dynamically loaded font
  /// back out. For an app that registers fonts as the user navigates, resident
  /// font memory could therefore only grow.
  ///
  /// Text already laid out with this family keeps rendering: the typeface is
  /// refcounted, so live paragraphs hold it alive and only the provider's
  /// reference is dropped. New layouts fall back until the family is loaded
  /// again, so callers must be able to re-load it on demand.
  static void UnloadFont(const std::string& family_name);

 private:
  std::shared_ptr<txt::FontCollection> collection_;
  sk_sp<txt::DynamicFontManager> dynamic_font_manager_;

  FML_DISALLOW_COPY_AND_ASSIGN(FontCollection);
};

}  // namespace flutter

#endif  // FLUTTER_LIB_UI_TEXT_FONT_COLLECTION_H_
