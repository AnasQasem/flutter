// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef FLUTTER_IMPELLER_ENTITY_RENDER_TARGET_CACHE_H_
#define FLUTTER_IMPELLER_ENTITY_RENDER_TARGET_CACHE_H_

#include <string_view>

#include "impeller/renderer/render_target.h"

namespace impeller {

/// @brief An implementation of the [RenderTargetAllocator] that caches all
///        allocated texture data for at least one frame.
///
///        Any textures unused after [keep_alive_frame_count] frames are
///        discarded.
class RenderTargetCache : public RenderTargetAllocator {
 public:
  /// QURAN PATCH 002: byte ceiling for the cache.
  ///
  /// Upstream bounds this cache by TIME ONLY (`keep_alive_frame_count`) and
  /// keeps `render_target_data_` in an unbounded vector, so its size is
  /// whatever the last few frames happened to allocate. That is fine for a UI
  /// that draws a couple of offscreens; this app issues ~8 `saveLayer`s per
  /// frame, and each one that misses the cache adds a full offscreen for at
  /// least 4 frames.
  ///
  /// 32 MB is ~7 full-screen 1080x2408 RGBA targets — comfortably more than the
  /// working set of one frame, so the steady state is unaffected and only a
  /// pathological burst gets trimmed.
  static constexpr size_t kDefaultMaxCachedBytes = 32u * 1024u * 1024u;

  explicit RenderTargetCache(std::shared_ptr<Allocator> allocator,
                             uint32_t keep_alive_frame_count = 4,
                             size_t max_cached_bytes = kDefaultMaxCachedBytes);

  ~RenderTargetCache() = default;

  // |RenderTargetAllocator|
  void Start() override;

  // |RenderTargetAllocator|
  void End() override;

  // |RenderTargetAllocator|
  void DisableCache() override;

  // |RenderTargetAllocator|
  void EnableCache() override;

  RenderTarget CreateOffscreen(
      const Context& context,
      ISize size,
      int mip_count,
      std::string_view label = "Offscreen",
      RenderTarget::AttachmentConfig color_attachment_config =
          RenderTarget::kDefaultColorAttachmentConfig,
      std::optional<RenderTarget::AttachmentConfig> stencil_attachment_config =
          RenderTarget::kDefaultStencilAttachmentConfig,
      const std::shared_ptr<Texture>& existing_color_texture = nullptr,
      const std::shared_ptr<Texture>& existing_depth_stencil_texture = nullptr,
      std::optional<PixelFormat> target_pixel_format = std::nullopt) override;

  RenderTarget CreateOffscreenMSAA(
      const Context& context,
      ISize size,
      int mip_count,
      std::string_view label = "Offscreen MSAA",
      RenderTarget::AttachmentConfigMSAA color_attachment_config =
          RenderTarget::kDefaultColorAttachmentConfigMSAA,
      std::optional<RenderTarget::AttachmentConfig> stencil_attachment_config =
          RenderTarget::kDefaultStencilAttachmentConfig,
      const std::shared_ptr<Texture>& existing_color_msaa_texture = nullptr,
      const std::shared_ptr<Texture>& existing_color_resolve_texture = nullptr,
      const std::shared_ptr<Texture>& existing_depth_stencil_texture = nullptr,
      std::optional<PixelFormat> target_pixel_format = std::nullopt) override;

  // visible for testing.
  size_t CachedTextureCount() const;

  /// Device-resident bytes currently held by the cache. Transient (memoryless)
  /// attachments are excluded because they never get real storage.
  /// Visible for testing.
  size_t CachedTextureBytes() const;

 private:
  struct RenderTargetData {
    bool used_this_frame;
    uint32_t keep_alive_frame_count;
    RenderTargetConfig config;
    RenderTarget render_target;
  };

  bool CacheEnabled() const;

  /// Device-resident byte cost of one render target.
  static size_t RenderTargetBytes(const RenderTarget& render_target);

  /// Drop cached targets, nearest-to-expiry first, until the cache is inside
  /// its byte budget. Never drops a target used in the frame just ended.
  void TrimToByteBudget();

  std::vector<RenderTargetData> render_target_data_;
  uint32_t keep_alive_frame_count_;
  size_t max_cached_bytes_;
  uint32_t cache_disabled_count_ = 0;

  RenderTargetCache(const RenderTargetCache&) = delete;

  RenderTargetCache& operator=(const RenderTargetCache&) = delete;

 public:
  /// Visible for testing.
  std::vector<RenderTargetData>::const_iterator GetRenderTargetDataBegin()
      const {
    return render_target_data_.begin();
  }

  /// Visible for testing.
  std::vector<RenderTargetData>::const_iterator GetRenderTargetDataEnd() const {
    return render_target_data_.end();
  }
};

}  // namespace impeller

#endif  // FLUTTER_IMPELLER_ENTITY_RENDER_TARGET_CACHE_H_
