// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "txt/typeface_font_asset_provider.h"

#include "flutter/fml/logging.h"
#include "third_party/skia/include/core/SkString.h"
#include "third_party/skia/include/core/SkTypeface.h"

namespace txt {

TypefaceFontAssetProvider::TypefaceFontAssetProvider() = default;

TypefaceFontAssetProvider::~TypefaceFontAssetProvider() = default;

// |FontAssetProvider|
size_t TypefaceFontAssetProvider::GetFamilyCount() const {
  return family_names_.size();
}

// |FontAssetProvider|
std::string TypefaceFontAssetProvider::GetFamilyName(int index) const {
  return family_names_[index];
}

// |FontAssetProvider|
sk_sp<SkFontStyleSet> TypefaceFontAssetProvider::MatchFamily(
    const std::string& family_name) {
  auto found = registered_families_.find(CanonicalFamilyName(family_name));
  if (found == registered_families_.end()) {
    return nullptr;
  }
  return found->second;
}

void TypefaceFontAssetProvider::RegisterTypeface(sk_sp<SkTypeface> typeface) {
  if (typeface == nullptr) {
    return;
  }

  SkString sk_family_name;
  typeface->getFamilyName(&sk_family_name);

  std::string family_name(sk_family_name.c_str(), sk_family_name.size());
  RegisterTypeface(std::move(typeface), family_name);
}

void TypefaceFontAssetProvider::RegisterTypeface(
    sk_sp<SkTypeface> typeface,
    const std::string& family_name_alias) {
  if (family_name_alias.empty()) {
    return;
  }

  std::string canonical_name = CanonicalFamilyName(family_name_alias);
  auto family_it = registered_families_.find(canonical_name);
  if (family_it == registered_families_.end()) {
    family_names_.push_back(family_name_alias);
    auto value =
        std::make_pair(canonical_name, sk_make_sp<TypefaceFontStyleSet>());
    family_it = registered_families_.emplace(value).first;
  }
  family_it->second->registerTypeface(std::move(typeface));
}

bool TypefaceFontAssetProvider::UnregisterFamily(
    const std::string& family_name) {
  if (family_name.empty()) {
    return false;
  }

  const std::string canonical_name = CanonicalFamilyName(family_name);
  auto family_it = registered_families_.find(canonical_name);
  if (family_it == registered_families_.end()) {
    return false;
  }
  registered_families_.erase(family_it);

  // `family_names_` holds the ORIGINAL alias, not the canonical form, and
  // GetFamilyName indexes straight into it — so the erase has to compare
  // canonically but remove the original entry, or GetFamilyCount and
  // GetFamilyName disagree.
  for (auto it = family_names_.begin(); it != family_names_.end(); ++it) {
    if (CanonicalFamilyName(*it) == canonical_name) {
      family_names_.erase(it);
      break;
    }
  }
  return true;
}

TypefaceFontStyleSet::TypefaceFontStyleSet() = default;

TypefaceFontStyleSet::~TypefaceFontStyleSet() = default;

void TypefaceFontStyleSet::registerTypeface(sk_sp<SkTypeface> typeface) {
  if (typeface == nullptr) {
    return;
  }
  typefaces_.emplace_back(std::move(typeface));
}

int TypefaceFontStyleSet::count() {
  return typefaces_.size();
}

void TypefaceFontStyleSet::getStyle(int index,
                                    SkFontStyle* style,
                                    SkString* name) {
  FML_DCHECK(static_cast<size_t>(index) < typefaces_.size());
  if (style) {
    *style = typefaces_[index]->fontStyle();
  }
  if (name) {
    name->reset();
  }
}

sk_sp<SkTypeface> TypefaceFontStyleSet::createTypeface(int i) {
  size_t index = i;
  if (index >= typefaces_.size()) {
    return nullptr;
  }
  return typefaces_[index];
}

sk_sp<SkTypeface> TypefaceFontStyleSet::matchStyle(const SkFontStyle& pattern) {
  return matchStyleCSS3(pattern);
}

}  // namespace txt
