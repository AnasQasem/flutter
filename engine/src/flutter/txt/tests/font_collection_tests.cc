// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "gtest/gtest.h"

#include <sstream>

#include "third_party/skia/include/core/SkFontMgr.h"
#include "third_party/skia/include/core/SkTypeface.h"
#include "txt/font_collection.h"
#include "txt/platform.h"
#include "txt/typeface_font_asset_provider.h"

namespace txt {
namespace testing {

class FontCollectionTests : public ::testing::Test {
 public:
  FontCollectionTests() {}

  void SetUp() override {}
};

TEST_F(FontCollectionTests, SettingUpDefaultFontManagerClearsCache) {
  FontCollection font_collection;
  sk_sp<skia::textlayout::FontCollection> sk_font_collection =
      font_collection.CreateSktFontCollection();
  ASSERT_EQ(sk_font_collection->getFallbackManager().get(), nullptr);
  font_collection.SetupDefaultFontManager(0);
  sk_font_collection = font_collection.CreateSktFontCollection();
  ASSERT_NE(sk_font_collection->getFallbackManager().get(), nullptr);
}

// Dynamically loaded fonts were register-only, so their memory was monotonic:
// an app registering fonts as the user navigates could never give any back.
// Measured on a Quran reader shipping 36 merged Hafs fonts, all 36 stayed
// resident as SkMemoryStream copies — 62.6 MiB, 40% of native heap — for a
// session that reads two or three.
TEST_F(FontCollectionTests, UnregisterFamilyReleasesTheFamily) {
  TypefaceFontAssetProvider provider;
  // Any real typeface will do — what is under test is the provider's family
  // bookkeeping, not the font itself.
  sk_sp<SkTypeface> typeface =
      GetDefaultFontManager()->legacyMakeTypeface(nullptr, SkFontStyle());
  ASSERT_NE(typeface, nullptr);

  provider.RegisterTypeface(typeface, "hafs_7");
  ASSERT_EQ(provider.GetFamilyCount(), 1ul);
  ASSERT_NE(provider.MatchFamily("hafs_7"), nullptr);

  ASSERT_TRUE(provider.UnregisterFamily("hafs_7"));
  EXPECT_EQ(provider.GetFamilyCount(), 0ul);
  EXPECT_EQ(provider.MatchFamily("hafs_7"), nullptr);

  // Unregistering something that was never registered is a no-op, not a crash:
  // callers evict speculatively.
  EXPECT_FALSE(provider.UnregisterFamily("hafs_7"));
  EXPECT_FALSE(provider.UnregisterFamily("never_registered"));
  EXPECT_FALSE(provider.UnregisterFamily(""));
}

// GetFamilyName indexes straight into family_names_, which stores the ORIGINAL
// alias while the map is keyed canonically. If the erase used the wrong one,
// GetFamilyCount and GetFamilyName would disagree and the survivor would be
// unreachable by name.
TEST_F(FontCollectionTests, UnregisterFamilyKeepsNameIndexConsistent) {
  TypefaceFontAssetProvider provider;
  // Any real typeface will do — what is under test is the provider's family
  // bookkeeping, not the font itself.
  sk_sp<SkTypeface> typeface =
      GetDefaultFontManager()->legacyMakeTypeface(nullptr, SkFontStyle());
  ASSERT_NE(typeface, nullptr);

  provider.RegisterTypeface(typeface, "Hafs_One");
  provider.RegisterTypeface(typeface, "Hafs_Two");
  ASSERT_EQ(provider.GetFamilyCount(), 2ul);

  // Canonicalization is case-insensitive, so this must match "Hafs_One".
  ASSERT_TRUE(provider.UnregisterFamily("hafs_one"));
  ASSERT_EQ(provider.GetFamilyCount(), 1ul);

  EXPECT_EQ(provider.GetFamilyName(0), "Hafs_Two");
  EXPECT_NE(provider.MatchFamily("Hafs_Two"), nullptr);
  EXPECT_EQ(provider.MatchFamily("Hafs_One"), nullptr);
}
}  // namespace testing
}  // namespace txt
