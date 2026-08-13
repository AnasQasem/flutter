// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "impeller/entity/render_target_cache.h"
#include <algorithm>
#include "impeller/base/quran_mem_stats.h"
#include "impeller/core/formats.h"
#include "impeller/core/texture.h"
#include "impeller/core/texture_descriptor.h"
#include "impeller/renderer/render_target.h"

namespace impeller {

RenderTargetCache::RenderTargetCache(std::shared_ptr<Allocator> allocator,
                                     uint32_t keep_alive_frame_count,
                                     size_t max_cached_bytes)
    : RenderTargetAllocator(std::move(allocator)),
      keep_alive_frame_count_(keep_alive_frame_count),
      max_cached_bytes_(max_cached_bytes) {}

size_t RenderTargetCache::RenderTargetBytes(const RenderTarget& render_target) {
  size_t bytes = 0u;
  render_target.IterateAllAttachments([&bytes](const Attachment& attachment) {
    const Texture* textures[] = {attachment.texture.get(),
                                 attachment.resolve_texture.get()};
    for (const Texture* texture : textures) {
      if (texture == nullptr) {
        continue;
      }
      const TextureDescriptor& desc = texture->GetTextureDescriptor();
      // Transient attachments are memoryless — they never receive device
      // storage, so counting them would evict real textures to make room for
      // bytes that do not exist. MSAA colour and depth/stencil both default to
      // kDeviceTransient (`render_target.h`).
      if (desc.storage_mode == StorageMode::kDeviceTransient) {
        continue;
      }
      bytes += desc.GetByteSizeOfBaseMipLevel();
    }
    return true;
  });
  return bytes;
}

size_t RenderTargetCache::CachedTextureBytes() const {
  size_t bytes = 0u;
  for (const RenderTargetData& td : render_target_data_) {
    bytes += RenderTargetBytes(td.render_target);
  }
  return bytes;
}

void RenderTargetCache::TrimToByteBudget() {
  size_t total = CachedTextureBytes();
  if (total <= max_cached_bytes_) {
    return;
  }

  // Only targets that went unused in the frame just ended are candidates, and
  // the ones nearest expiry go first — `keep_alive_frame_count` counts DOWN, so
  // the smallest value is the entry that was about to be dropped anyway. This
  // keeps the eviction order identical to the time-based policy and only makes
  // it happen sooner.
  std::vector<size_t> candidates;
  candidates.reserve(render_target_data_.size());
  for (size_t i = 0u; i < render_target_data_.size(); i++) {
    if (!render_target_data_[i].used_this_frame) {
      candidates.push_back(i);
    }
  }
  std::sort(candidates.begin(), candidates.end(), [this](size_t a, size_t b) {
    return render_target_data_[a].keep_alive_frame_count <
           render_target_data_[b].keep_alive_frame_count;
  });

  std::vector<bool> evict(render_target_data_.size(), false);
  bool any = false;
  for (size_t i : candidates) {
    if (total <= max_cached_bytes_) {
      break;
    }
    total -= RenderTargetBytes(render_target_data_[i].render_target);
    evict[i] = true;
    any = true;
  }
  if (!any) {
    return;
  }

  std::vector<RenderTargetData> kept;
  kept.reserve(render_target_data_.size());
  for (size_t i = 0u; i < render_target_data_.size(); i++) {
    if (!evict[i]) {
      kept.push_back(std::move(render_target_data_[i]));
    }
  }
  render_target_data_.swap(kept);
}

void RenderTargetCache::Start() {
  cache_disabled_count_ = 0;
  for (auto& td : render_target_data_) {
    td.used_this_frame = false;
  }
}

void RenderTargetCache::End() {
  cache_disabled_count_ = 0;
  std::vector<RenderTargetData> retain;

  for (RenderTargetData& td : render_target_data_) {
    if (td.used_this_frame) {
      retain.push_back(td);
    } else if (td.keep_alive_frame_count > 0) {
      td.keep_alive_frame_count--;
      retain.push_back(td);
    }
  }
  render_target_data_.swap(retain);
  // QURAN PATCH 002: the time-based pass above has no idea how many bytes it
  // just decided to keep. Enforce the ceiling as well.
  TrimToByteBudget();
  // QURAN PATCH 004: publish for the low-memory dump.
  QuranRenderTargetBytes().store(static_cast<int64_t>(CachedTextureBytes()));
}

void RenderTargetCache::DisableCache() {
  cache_disabled_count_++;
}

bool RenderTargetCache::CacheEnabled() const {
  return cache_disabled_count_ == 0;
}

void RenderTargetCache::EnableCache() {
  FML_DCHECK(cache_disabled_count_ > 0);
  if (cache_disabled_count_ == 0) {
    return;
  }
  cache_disabled_count_--;
}

RenderTarget RenderTargetCache::CreateOffscreen(
    const Context& context,
    ISize size,
    int mip_count,
    std::string_view label,
    RenderTarget::AttachmentConfig color_attachment_config,
    std::optional<RenderTarget::AttachmentConfig> stencil_attachment_config,
    const std::shared_ptr<Texture>& existing_color_texture,
    const std::shared_ptr<Texture>& existing_depth_stencil_texture,
    std::optional<PixelFormat> target_pixel_format) {
  if (size.IsEmpty()) {
    return {};
  }

  FML_DCHECK(existing_color_texture == nullptr &&
             existing_depth_stencil_texture == nullptr);
  auto config = RenderTargetConfig{
      .size = size,
      .mip_count = static_cast<size_t>(mip_count),
      .has_msaa = false,
      .has_depth_stencil = stencil_attachment_config.has_value(),
  };

  if (CacheEnabled()) {
    for (RenderTargetData& render_target_data : render_target_data_) {
      const RenderTargetConfig other_config = render_target_data.config;
      if (!render_target_data.used_this_frame && other_config == config) {
        render_target_data.used_this_frame = true;
        render_target_data.keep_alive_frame_count = keep_alive_frame_count_;
        ColorAttachment color0 =
            render_target_data.render_target.GetColorAttachment(0);
        std::optional<DepthAttachment> depth =
            render_target_data.render_target.GetDepthAttachment();
        std::shared_ptr<Texture> depth_tex = depth ? depth->texture : nullptr;
        return RenderTargetAllocator::CreateOffscreen(
            context, size, mip_count, label, color_attachment_config,
            stencil_attachment_config, color0.texture, depth_tex,
            target_pixel_format);
      }
    }
  }
  RenderTarget created_target = RenderTargetAllocator::CreateOffscreen(
      context, size, mip_count, label, color_attachment_config,
      stencil_attachment_config, nullptr, nullptr, target_pixel_format);
  if (!created_target.IsValid()) {
    return created_target;
  }
  if (CacheEnabled()) {
    render_target_data_.push_back(RenderTargetData{
        .used_this_frame = true,                            //
        .keep_alive_frame_count = keep_alive_frame_count_,  //
        .config = config,                                   //
        .render_target = created_target                     //
    });
  }
  return created_target;
}

RenderTarget RenderTargetCache::CreateOffscreenMSAA(
    const Context& context,
    ISize size,
    int mip_count,
    std::string_view label,
    RenderTarget::AttachmentConfigMSAA color_attachment_config,
    std::optional<RenderTarget::AttachmentConfig> stencil_attachment_config,
    const std::shared_ptr<Texture>& existing_color_msaa_texture,
    const std::shared_ptr<Texture>& existing_color_resolve_texture,
    const std::shared_ptr<Texture>& existing_depth_stencil_texture,
    std::optional<PixelFormat> target_pixel_format) {
  if (size.IsEmpty()) {
    return {};
  }

  FML_DCHECK(existing_color_msaa_texture == nullptr &&
             existing_color_resolve_texture == nullptr &&
             existing_depth_stencil_texture == nullptr);
  auto config = RenderTargetConfig{
      .size = size,
      .mip_count = static_cast<size_t>(mip_count),
      .has_msaa = true,
      .has_depth_stencil = stencil_attachment_config.has_value(),
  };
  if (CacheEnabled()) {
    for (RenderTargetData& render_target_data : render_target_data_) {
      const RenderTargetConfig other_config = render_target_data.config;
      if (!render_target_data.used_this_frame && other_config == config) {
        render_target_data.used_this_frame = true;
        render_target_data.keep_alive_frame_count = keep_alive_frame_count_;
        ColorAttachment color0 =
            render_target_data.render_target.GetColorAttachment(0);
        std::optional<DepthAttachment> depth =
            render_target_data.render_target.GetDepthAttachment();
        std::shared_ptr<Texture> depth_tex = depth ? depth->texture : nullptr;
        return RenderTargetAllocator::CreateOffscreenMSAA(
            context, size, mip_count, label, color_attachment_config,
            stencil_attachment_config, color0.texture, color0.resolve_texture,
            depth_tex, target_pixel_format);
      }
    }
  }
  RenderTarget created_target = RenderTargetAllocator::CreateOffscreenMSAA(
      context, size, mip_count, label, color_attachment_config,
      stencil_attachment_config, nullptr, nullptr, nullptr,
      target_pixel_format);
  if (!created_target.IsValid()) {
    return created_target;
  }
  if (CacheEnabled()) {
    render_target_data_.push_back(RenderTargetData{
        .used_this_frame = true,                            //
        .keep_alive_frame_count = keep_alive_frame_count_,  //
        .config = config,                                   //
        .render_target = created_target                     //
    });
  }
  return created_target;
}

size_t RenderTargetCache::CachedTextureCount() const {
  return render_target_data_.size();
}

}  // namespace impeller
