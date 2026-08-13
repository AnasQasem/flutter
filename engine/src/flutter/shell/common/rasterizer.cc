// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "flutter/shell/common/rasterizer.h"

#include <algorithm>
#include <memory>
#include <utility>

// Must precede the FML_OS_ANDROID test below — the macro comes from here, and
// this include sits above the fml headers that would otherwise define it.
#include "flutter/fml/build_config.h"

#if defined(FML_OS_ANDROID)
#include <android/log.h>  // QURAN PATCH 006 instrumentation.
#include <malloc.h>       // mallopt / M_PURGE_ALL — see PurgeNativeAllocator.
#endif

#if defined(FML_OS_IOS) || defined(FML_OS_MACOSX)
#include <mach/mach.h>  // QURAN PATCH 006: phys_footprint via task_info.
#endif

#include "display_list/dl_builder.h"
#include "flow/frame_timings.h"
#include "flutter/common/constants.h"
#include "flutter/common/graphics/persistent_cache.h"
#include "flutter/flow/layers/offscreen_surface.h"
#include "flutter/fml/time/time_delta.h"
#include "flutter/fml/time/time_point.h"
#include "flutter/shell/common/base64.h"
#include "flutter/shell/common/serialization_callbacks.h"
#include "fml/closure.h"
#include "fml/make_copyable.h"
#include "fml/synchronization/waitable_event.h"
#include "impeller/renderer/context.h"
#include "third_party/skia/include/core/SkColorSpace.h"
#include "third_party/skia/include/core/SkData.h"
#include "third_party/skia/include/core/SkGraphics.h"  // QURAN PATCH 005
#include "third_party/skia/include/core/SkImage.h"
#include "third_party/skia/include/core/SkImageInfo.h"
#include "third_party/skia/include/core/SkMatrix.h"
#include "third_party/skia/include/core/SkPictureRecorder.h"
#include "third_party/skia/include/core/SkRect.h"
#include "third_party/skia/include/core/SkSerialProcs.h"
#include "third_party/skia/include/core/SkSize.h"
#include "third_party/skia/include/core/SkSurface.h"
#include "third_party/skia/include/encode/SkPngEncoder.h"
#include "third_party/skia/include/gpu/GpuTypes.h"
#include "third_party/skia/include/gpu/ganesh/GrBackendSurface.h"
#include "third_party/skia/include/gpu/ganesh/GrDirectContext.h"
#include "third_party/skia/include/gpu/ganesh/GrTypes.h"
#include "third_party/skia/include/gpu/ganesh/SkSurfaceGanesh.h"

#if IMPELLER_SUPPORTS_RENDERING
#include "impeller/base/quran_mem_stats.h"             // nogncheck
#include "impeller/core/formats.h"                     // nogncheck
#include "impeller/display_list/aiks_context.h"        // nogncheck
#include "impeller/display_list/dl_dispatcher.h"       // nogncheck
#include "impeller/entity/contents/content_context.h"  // nogncheck QURAN PATCH 006
#include "impeller/typographer/lazy_glyph_atlas.h"  // nogncheck QURAN PATCH 006
#endif

namespace flutter {

// The rasterizer will tell Skia to purge cached resources that have not been
// used within this interval.
[[maybe_unused]] static constexpr std::chrono::milliseconds
    kSkiaCleanupExpiration(15000);

namespace {

/// QURAN PATCH 001: return decommitted pages to the operating system.
///
/// Freeing a block only hands it back to the allocator's arena. Scudo
/// (Android's allocator since API 29) keeps that arena mapped, so a process
/// that briefly peaks stays at the peak in RSS for the rest of its life.
/// `mallopt(M_PURGE_ALL)` is the documented way to force the released pages
/// back.
///
/// Cheap but not free — it walks the allocator's regions — so it belongs on a
/// pressure signal, never on a frame path.
void PurgeNativeAllocator() {
#if defined(FML_OS_ANDROID)
  // M_PURGE_ALL is API 31+ and is missing from older NDK headers (the sysroot
  // this engine builds against defines M_PURGE but not M_PURGE_ALL), while the
  // devices we target run API 35. Declare it rather than depend on the header,
  // and fall back at RUNTIME: mallopt returns 0 for an option the platform does
  // not know, so this costs one failed call on old devices and never
  // misbehaves.
  //
  // The distinction matters here: M_PURGE only releases the calling thread's
  // cache, and the memory we want back was freed on other threads (UI, IO,
  // raster), so M_PURGE_ALL is the one that actually reclaims it.
#ifndef M_PURGE_ALL
#define M_PURGE_ALL (-104)
#endif
  // mallopt is __INTRODUCED_IN(26) and therefore a weak symbol; our minSdk is
  // 29 so it is always bound, but check anyway — a null weak symbol would
  // crash.
  if (mallopt != nullptr && mallopt(M_PURGE_ALL, 0) == 0) {
    mallopt(M_PURGE, 0);
  }
#endif  // FML_OS_ANDROID
}

/// QURAN PATCH 006 (instrumentation): live malloc bytes, so the teardown path
/// can be attributed step by step.
///
/// Measured on an A146P: after a broad search plus scrolling every result,
/// closing the search leaves ~372 MB of `HeapAlloc`, and the pressure path
/// (Dart_NotifyLowMemory + the font-cache purge) frees none of it — but
/// backgrounding drops it to ~97 MB and it STAYS there on resume. So ~270 MB is
/// released by something on the teardown path only. Logging each stage names it
/// instead of guessing; `dumpsys meminfo` cannot see inside a single call.
size_t QuranLiveMallocKB() {
#if defined(FML_OS_ANDROID)
  // bionic's uordblks is size_t, so it does not overflow at these sizes the way
  // glibc's int-based mallinfo would.
  struct mallinfo info = mallinfo();
  return static_cast<size_t>(info.uordblks) / 1024;
#elif defined(FML_OS_IOS) || defined(FML_OS_MACOSX)
  // phys_footprint, not resident_size: it is the figure iOS actually enforces
  // its memory limit against, and the one Xcode's gauge shows.
  task_vm_info_data_t info = {};
  mach_msg_type_number_t count = TASK_VM_INFO_COUNT;
  if (task_info(mach_task_self(), TASK_VM_INFO,
                reinterpret_cast<task_info_t>(&info), &count) != KERN_SUCCESS) {
    return 0;
  }
  return static_cast<size_t>(info.phys_footprint) / 1024;
#else
  return 0;
#endif
}

void QuranLogMalloc(const char* where, size_t before) {
  const size_t now = QuranLiveMallocKB();
  const long long delta =
      static_cast<long long>(now) - static_cast<long long>(before);
#if defined(FML_OS_ANDROID)
  __android_log_print(ANDROID_LOG_ERROR, "QURANMEM",
                      "[%s] live_malloc %zuKB -> %zuKB (delta %lldKB)", where,
                      before, now, delta);
#elif defined(FML_OS_IOS) || defined(FML_OS_MACOSX)
  // FML_LOG(ERROR) rather than __android_log_print, and ERROR rather than INFO
  // so it survives to the device console / `flutter run` output.
  FML_LOG(ERROR) << "QURANMEM [" << where << "] phys_footprint " << before
                 << "KB -> " << now << "KB (delta " << delta << "KB)";
#else
  (void)where;
  (void)before;
  (void)delta;
#endif
}

}  // namespace

Rasterizer::Rasterizer(Delegate& delegate,
                       MakeGpuImageBehavior gpu_image_behavior)
    : delegate_(delegate),
      gpu_image_behavior_(gpu_image_behavior),
      compositor_context_(std::make_unique<flutter::CompositorContext>(*this)),
      snapshot_controller_(
          SnapshotController::Make(*this, delegate.GetSettings())),
      weak_factory_(this) {
  FML_DCHECK(compositor_context_);
}

Rasterizer::~Rasterizer() = default;

fml::TaskRunnerAffineWeakPtr<Rasterizer> Rasterizer::GetWeakPtr() const {
  return weak_factory_.GetWeakPtr();
}

fml::TaskRunnerAffineWeakPtr<SnapshotDelegate> Rasterizer::GetSnapshotDelegate()
    const {
  return weak_factory_.GetWeakPtr();
}

void Rasterizer::SetImpellerContext(
    std::shared_ptr<impeller::ImpellerContextFuture> impeller_context) {
  impeller_context_ = std::move(impeller_context);
}

void Rasterizer::Setup(std::unique_ptr<Surface> surface) {
  surface_ = std::move(surface);

  if (max_cache_bytes_.has_value()) {
    SetResourceCacheMaxBytes(max_cache_bytes_.value(),
                             user_override_resource_cache_bytes_);
  }

  auto context_switch = surface_->MakeRenderContextCurrent();
  if (context_switch->GetResult()) {
    compositor_context_->OnGrContextCreated();
  }

  if (external_view_embedder_ &&
      external_view_embedder_->SupportsDynamicThreadMerging() &&
      !raster_thread_merger_) {
    const auto platform_id =
        delegate_.GetTaskRunners().GetPlatformTaskRunner()->GetTaskQueueId();
    const auto gpu_id =
        delegate_.GetTaskRunners().GetRasterTaskRunner()->GetTaskQueueId();
    raster_thread_merger_ = fml::RasterThreadMerger::CreateOrShareThreadMerger(
        delegate_.GetParentRasterThreadMerger(), platform_id, gpu_id);
  }
  if (raster_thread_merger_) {
    raster_thread_merger_->SetMergeUnmergeCallback([this]() {
      // Clear the GL context after the thread configuration has changed.
      if (surface_) {
        surface_->ClearRenderContext();
      }
    });
  }
}

void Rasterizer::TeardownExternalViewEmbedder() {
  if (external_view_embedder_) {
    external_view_embedder_->Teardown();
  }
}

void Rasterizer::Teardown() {
  // QURAN PATCH 006 (instrumentation): stage-by-stage, to find which of these
  // releases the ~270 MB that the pressure path does not.
  size_t quran_mark = QuranLiveMallocKB();
  QuranLogMalloc("teardown:enter", quran_mark);
  is_torn_down_ = true;
  if (surface_) {
    auto context_switch = surface_->MakeRenderContextCurrent();
    if (context_switch->GetResult()) {
      quran_mark = QuranLiveMallocKB();
      compositor_context_->OnGrContextDestroyed();
      QuranLogMalloc("teardown:OnGrContextDestroyed", quran_mark);
#if !SLIMPELLER
      if (auto* context = surface_->GetContext()) {
        context->purgeUnlockedResources(GrPurgeResourceOptions::kAllResources);
      }
#endif  //  !SLIMPELLER
    }
    quran_mark = QuranLiveMallocKB();
    surface_.reset();
    QuranLogMalloc("teardown:surface_reset", quran_mark);
  }

  quran_mark = QuranLiveMallocKB();
  view_records_.clear();
  QuranLogMalloc("teardown:view_records_clear", quran_mark);

  if (raster_thread_merger_.get() != nullptr &&
      raster_thread_merger_.get()->IsMerged()) {
    FML_DCHECK(raster_thread_merger_->IsEnabled());
    raster_thread_merger_->UnMergeNowIfLastOne();
    raster_thread_merger_->SetMergeUnmergeCallback(nullptr);
  }
}

bool Rasterizer::IsTornDown() {
  return is_torn_down_;
}

std::optional<DrawSurfaceStatus> Rasterizer::GetLastDrawStatus(
    int64_t view_id) {
  auto found = view_records_.find(view_id);
  if (found != view_records_.end()) {
    return found->second.last_draw_status;
  } else {
    return std::optional<DrawSurfaceStatus>();
  }
}

void Rasterizer::EnableThreadMergerIfNeeded() {
  if (raster_thread_merger_) {
    raster_thread_merger_->Enable();
  }
}

void Rasterizer::DisableThreadMergerIfNeeded() {
  if (raster_thread_merger_) {
    raster_thread_merger_->Disable();
  }
}

void Rasterizer::NotifyLowMemoryWarning() const {
#if !SLIMPELLER
  // Skia-only path. `Surface::GetContext()` returns a `GrDirectContext*`, and
  // every Impeller surface returns nullptr from it ("Impeller != Skia",
  // gpu_surface_vulkan_impeller.cc) — so on Impeller this whole block is a
  // no-op and always has been. Upstream therefore does nothing at all for GPU
  // memory when the platform reports pressure on an Impeller build.
  if (surface_) {
    if (auto context = surface_->GetContext()) {
      auto context_switch = surface_->MakeRenderContextCurrent();
      if (context_switch->GetResult()) {
        context->performDeferredCleanup(std::chrono::milliseconds(0));
      }
    } else {
      FML_DLOG(INFO) << "Rasterizer::NotifyLowMemoryWarning: no GrContext "
                        "(expected on Impeller).";
    }
  }
#endif  //  !SLIMPELLER

  // QURAN PATCH 001: hand freed pages back to the OS.
  //
  // Nothing in the engine has ever called into the allocator to release memory
  // (`grep -rn "mallopt\|malloc_trim"` outside third_party finds nothing), so
  // every free the engine performs — font unregistration, cache eviction, Dart
  // GC, deferred GPU cleanup above — returns the block to Scudo's arena and
  // leaves RSS at its high-water mark. Measured consequence: releasing ~14 MB
  // of typefaces moved `Heap Alloc` down by 14 MB while `Native Heap` PSS did
  // not move at all.
  //
  // This is the difference between "the engine freed it" and "the device got it
  // back", so it gates the value of every other memory change.
  //
  // MEASURED 2026-08-12 (A146P): this does NOT recover the ~70 MB a broad
  // search adds. `HeapAlloc` (live bytes) moved only 148 -> 139 MB across two
  // purges and `HeapFree` never collapsed, so that memory is genuinely live in
  // engine caches, not parked in the allocator. Kept because it is free and
  // does return what IS free; it is simply not the lever it was hypothesised to
  // be.
#if IMPELLER_SUPPORTS_RENDERING
  impeller::DumpQuranMemStats("before-purge");
#endif

  // QURAN PATCH 005: drop Skia's strike cache before handing pages back.
  //
  // A strike owns an SkScalerContext, which holds a strong ref to its typeface,
  // so a font rasterised at any size outlives the family being unregistered.
  // MEASURED (A146P): purging on unload cut peak RssAnon 505 -> 417 MB even
  // though `GetFontCacheUsed()` reported under 2 MB of strike data — the bytes
  // are not the strikes, they are the ~5 MB of font data per merged Hafs file
  // (the SkMemoryStream copy plus FreeType's WOFF-decompressed sfnt) that the
  // strikes were pinning.
  //
  // It belongs here too, and must run BEFORE PurgeNativeAllocator: freeing the
  // typefaces only returns the blocks to Scudo's arena, and the mallopt below
  // is what returns them to the OS. Without this the pressure path had nothing
  // to purge, which is exactly why Patch 001 measured as a no-op.
  // QURAN PATCH 006: release the accumulated glyph-atlas state.
  //
  // This is the memory the pressure path was missing. Attribution on an A146P
  // after a broad search: `Rasterizer::Teardown` freed 272 MB, ALL of it inside
  // `aiks_context_.reset()`, and inside that 262 MB was `lazy_glyph_atlas_`.
  // Nothing else came close, and every other cache already has a counter.
  //
  // Before this, reclaiming it required destroying the surface — i.e. the user
  // had to background the app. Closing a memory-heavy screen could not.
  //
  // Safe here: NotifyLowMemoryWarning is posted to the raster task runner, so
  // it cannot land mid-frame, and the next frame repopulates what it needs.
#if IMPELLER_SUPPORTS_RENDERING
  if (surface_) {
    if (std::shared_ptr<impeller::AiksContext> aiks =
            surface_->GetAiksContext()) {
      size_t atlas_mark = QuranLiveMallocKB();
      aiks->GetContentContext().GetLazyGlyphAtlas()->ClearAtlasContexts();
      QuranLogMalloc("pressure:ClearAtlasContexts", atlas_mark);
    }
  }
#endif  // IMPELLER_SUPPORTS_RENDERING

  // QURAN PATCH 006 (instrumentation): same units and tag as the teardown:*
  // lines, so the two paths can be compared directly.
  size_t quran_mark = QuranLiveMallocKB();
  SkGraphics::PurgeFontCache();
  QuranLogMalloc("pressure:PurgeFontCache", quran_mark);

  quran_mark = QuranLiveMallocKB();
  PurgeNativeAllocator();
  QuranLogMalloc("pressure:PurgeNativeAllocator", quran_mark);
#if IMPELLER_SUPPORTS_RENDERING
  impeller::DumpQuranMemStats("after-purge");
#endif
}

void Rasterizer::CollectView(int64_t view_id) {
  if (external_view_embedder_) {
    external_view_embedder_->CollectView(view_id);
  }
  view_records_.erase(view_id);
}

std::shared_ptr<flutter::TextureRegistry> Rasterizer::GetTextureRegistry() {
  return compositor_context_->texture_registry();
}

GrDirectContext* Rasterizer::GetGrContext() {
  return surface_ ? surface_->GetContext() : nullptr;
}

flutter::LayerTree* Rasterizer::GetLastLayerTree(int64_t view_id) {
  auto found = view_records_.find(view_id);
  if (found == view_records_.end()) {
    return nullptr;
  }
  auto& last_task = found->second.last_successful_task;
  if (last_task == nullptr) {
    return nullptr;
  }
  return last_task->layer_tree.get();
}

void Rasterizer::DrawLastLayerTrees(
    std::unique_ptr<FrameTimingsRecorder> frame_timings_recorder) {
  if (!surface_) {
    return;
  }
  std::vector<std::unique_ptr<LayerTreeTask>> tasks;
  for (auto& [view_id, view_record] : view_records_) {
    if (view_record.last_successful_task) {
      tasks.push_back(std::move(view_record.last_successful_task));
    }
  }
  if (tasks.empty()) {
    return;
  }

  DoDrawResult result =
      DrawToSurfaces(*frame_timings_recorder, std::move(tasks));

  // EndFrame should perform cleanups for the external_view_embedder.
  if (external_view_embedder_ && external_view_embedder_->GetUsedThisFrame()) {
    bool should_resubmit_frame = ShouldResubmitFrame(result);
    external_view_embedder_->SetUsedThisFrame(false);
    external_view_embedder_->EndFrame(should_resubmit_frame,
                                      raster_thread_merger_);
  }
}

DrawStatus Rasterizer::Draw(const std::shared_ptr<FramePipeline>& pipeline) {
  TRACE_EVENT0("flutter", "GPURasterizer::Draw");
  if (raster_thread_merger_ &&
      !raster_thread_merger_->IsOnRasterizingThread()) {
    // we yield and let this frame be serviced on the right thread.
    return DrawStatus::kYielded;
  }
  FML_DCHECK(delegate_.GetTaskRunners()
                 .GetRasterTaskRunner()
                 ->RunsTasksOnCurrentThread());

  DoDrawResult draw_result;
  FramePipeline::Consumer consumer = [&draw_result,
                                      this](std::unique_ptr<FrameItem> item) {
    draw_result = DoDraw(std::move(item->frame_timings_recorder),
                         std::move(item->layer_tree_tasks));
  };

  PipelineConsumeResult consume_result = pipeline->Consume(consumer);
  if (consume_result == PipelineConsumeResult::NoneAvailable) {
    return DrawStatus::kPipelineEmpty;
  }
  // if the raster status is to resubmit the frame, we push the frame to the
  // front of the queue and also change the consume status to more available.

  bool should_resubmit_frame = ShouldResubmitFrame(draw_result);
  if (should_resubmit_frame) {
    FML_CHECK(draw_result.resubmitted_item);
    auto front_continuation = pipeline->ProduceIfEmpty();
    PipelineProduceResult pipeline_result =
        front_continuation.Complete(std::move(draw_result.resubmitted_item));
    if (pipeline_result.success) {
      consume_result = PipelineConsumeResult::MoreAvailable;
    }
  } else if (draw_result.status == DoDrawStatus::kEnqueuePipeline) {
    consume_result = PipelineConsumeResult::MoreAvailable;
  }

  // EndFrame should perform cleanups for the external_view_embedder.
  if (external_view_embedder_ && external_view_embedder_->GetUsedThisFrame()) {
    external_view_embedder_->SetUsedThisFrame(false);
    external_view_embedder_->EndFrame(should_resubmit_frame,
                                      raster_thread_merger_);
  }

  // Consume as many pipeline items as possible. But yield the event loop
  // between successive tries.
  switch (consume_result) {
    case PipelineConsumeResult::MoreAvailable: {
      delegate_.GetTaskRunners().GetRasterTaskRunner()->PostTask(
          [weak_this = weak_factory_.GetWeakPtr(), pipeline]() {
            if (weak_this) {
              weak_this->Draw(pipeline);
            }
          });
      break;
    }
    default:
      break;
  }

  return ToDrawStatus(draw_result.status);
}

bool Rasterizer::ShouldResubmitFrame(const DoDrawResult& result) {
  if (result.resubmitted_item) {
    FML_CHECK(!result.resubmitted_item->layer_tree_tasks.empty());
    return true;
  }
  return false;
}

DrawStatus Rasterizer::ToDrawStatus(DoDrawStatus status) {
  switch (status) {
    case DoDrawStatus::kEnqueuePipeline:
      return DrawStatus::kDone;
    case DoDrawStatus::kNotSetUp:
      return DrawStatus::kNotSetUp;
    case DoDrawStatus::kGpuUnavailable:
      return DrawStatus::kGpuUnavailable;
    case DoDrawStatus::kDone:
      return DrawStatus::kDone;
  }
  FML_UNREACHABLE();
}

#if !SLIMPELLER
namespace {
std::unique_ptr<SnapshotDelegate::GpuImageResult> MakeBitmapImage(
    const sk_sp<DisplayList>& display_list,
    const SkImageInfo& image_info) {
  FML_DCHECK(display_list);
  // Use 16384 as a proxy for the maximum texture size for a GPU image.
  // This is meant to be large enough to avoid false positives in test contexts,
  // but not so artificially large to be completely unrealistic on any platform.
  // This limit is taken from the Metal specification. D3D, Vulkan, and GL
  // generally have lower limits.
  if (image_info.width() > 16384 || image_info.height() > 16384) {
    return std::make_unique<SnapshotDelegate::GpuImageResult>(
        GrBackendTexture(), nullptr, nullptr,
        "unable to create bitmap render target at specified size " +
            std::to_string(image_info.width()) + "x" +
            std::to_string(image_info.height()));
  };

  sk_sp<SkSurface> surface = SkSurfaces::Raster(image_info);
  auto canvas = DlSkCanvasAdapter(surface->getCanvas());
  canvas.Clear(DlColor::kTransparent());
  canvas.DrawDisplayList(display_list);

  sk_sp<SkImage> image = surface->makeImageSnapshot();
  return std::make_unique<SnapshotDelegate::GpuImageResult>(
      GrBackendTexture(), nullptr, image,
      image ? "" : "Unable to create image");
}
}  // namespace
#endif  //  !SLIMPELLER

std::unique_ptr<Rasterizer::GpuImageResult> Rasterizer::MakeSkiaGpuImage(
    sk_sp<DisplayList> display_list,
    const SkImageInfo& image_info) {
#if SLIMPELLER
  FML_LOG(FATAL) << "Impeller opt-out unavailable.";
  return nullptr;
#else   // SLIMPELLER
  TRACE_EVENT0("flutter", "Rasterizer::MakeGpuImage");
  FML_DCHECK(display_list);

  std::unique_ptr<SnapshotDelegate::GpuImageResult> result;
  delegate_.GetIsGpuDisabledSyncSwitch()->Execute(
      fml::SyncSwitch::Handlers()
          .SetIfTrue([&result, &image_info, &display_list] {
            // TODO(dnfield): This isn't safe if display_list contains any GPU
            // resources like an SkImage_gpu.
            result = MakeBitmapImage(display_list, image_info);
          })
          .SetIfFalse([&result, &image_info, &display_list,
                       surface = surface_.get(),
                       gpu_image_behavior = gpu_image_behavior_] {
            if (!surface ||
                gpu_image_behavior == MakeGpuImageBehavior::kBitmap) {
              // TODO(dnfield): This isn't safe if display_list contains any GPU
              // resources like an SkImage_gpu.
              result = MakeBitmapImage(display_list, image_info);
              return;
            }

            auto context_switch = surface->MakeRenderContextCurrent();
            if (!context_switch->GetResult()) {
              result = MakeBitmapImage(display_list, image_info);
              return;
            }

            auto* context = surface->GetContext();
            if (!context) {
              result = MakeBitmapImage(display_list, image_info);
              return;
            }

            GrBackendTexture texture = context->createBackendTexture(
                image_info.width(), image_info.height(), image_info.colorType(),
                skgpu::Mipmapped::kNo, GrRenderable::kYes);
            if (!texture.isValid()) {
              result = std::make_unique<SnapshotDelegate::GpuImageResult>(
                  GrBackendTexture(), nullptr, nullptr,
                  "unable to create texture render target at specified size " +
                      std::to_string(image_info.width()) + "x" +
                      std::to_string(image_info.height()));
              return;
            }

            sk_sp<SkSurface> sk_surface = SkSurfaces::WrapBackendTexture(
                context, texture, kTopLeft_GrSurfaceOrigin, /*sampleCnt=*/0,
                image_info.colorType(), image_info.refColorSpace(), nullptr);
            if (!sk_surface) {
              result = std::make_unique<SnapshotDelegate::GpuImageResult>(
                  GrBackendTexture(), nullptr, nullptr,
                  "unable to create rendering surface for image");
              return;
            }

            auto canvas = DlSkCanvasAdapter(sk_surface->getCanvas());
            canvas.Clear(DlColor::kTransparent());
            canvas.DrawDisplayList(display_list);

            result = std::make_unique<SnapshotDelegate::GpuImageResult>(
                texture, sk_ref_sp(context), nullptr, "");
          }));
  return result;
#endif  //  !SLIMPELLER
}

void Rasterizer::MakeSkiaSnapshot(sk_sp<DisplayList> display_list,
                                  DlISize picture_size,
                                  std::function<void(sk_sp<SkImage>)> callback,
                                  SnapshotPixelFormat pixel_format) {
  return snapshot_controller_->MakeSkiaSnapshot(
      display_list, picture_size, std::move(callback), pixel_format);
}

sk_sp<SkImage> Rasterizer::MakeSkiaSnapshotSync(
    sk_sp<DisplayList> display_list,
    DlISize picture_size,
    SnapshotPixelFormat pixel_format) {
  return snapshot_controller_->MakeSkiaSnapshotSync(std::move(display_list),
                                                    picture_size, pixel_format);
}

void Rasterizer::MakeImpellerSnapshot(
    sk_sp<DisplayList> display_list,
    DlISize picture_size,
    std::function<void(std::shared_ptr<impeller::Texture>)> callback,
    SnapshotPixelFormat pixel_format) {
  return snapshot_controller_->MakeImpellerSnapshot(
      display_list, picture_size, std::move(callback), pixel_format);
}

std::shared_ptr<impeller::Texture> Rasterizer::MakeImpellerSnapshotSync(
    sk_sp<DisplayList> display_list,
    DlISize picture_size,
    SnapshotPixelFormat pixel_format) {
  return snapshot_controller_->MakeImpellerSnapshotSync(
      std::move(display_list), picture_size, pixel_format);
}

sk_sp<SkImage> Rasterizer::ConvertToRasterImage(sk_sp<SkImage> image) {
  TRACE_EVENT0("flutter", __FUNCTION__);
  return snapshot_controller_->ConvertToRasterImage(image);
}

sk_sp<SkImage> Rasterizer::MakeSkiaTextureImage(
    sk_sp<SkImage> image,
    SnapshotPixelFormat pixel_format) {
  TRACE_EVENT0("flutter", __FUNCTION__);
  return snapshot_controller_->MakeSkiaTextureImage(image, pixel_format);
}

std::shared_ptr<impeller::Texture> Rasterizer::MakeImpellerTextureImage(
    sk_sp<SkImage> image,
    SnapshotPixelFormat pixel_format) {
  TRACE_EVENT0("flutter", __FUNCTION__);
  return snapshot_controller_->MakeImpellerTextureImage(image, pixel_format);
}

// |SnapshotDelegate|
void Rasterizer::CacheRuntimeStage(
    const std::shared_ptr<impeller::RuntimeStage>& runtime_stage) {
  if (snapshot_controller_) {
    snapshot_controller_->CacheRuntimeStage(runtime_stage);
  }
}

// |SnapshotDelegate|
bool Rasterizer::MakeRenderContextCurrent() {
  return snapshot_controller_->MakeRenderContextCurrent();
}

fml::Milliseconds Rasterizer::GetFrameBudget() const {
  return delegate_.GetFrameBudget();
};

Rasterizer::DoDrawResult Rasterizer::DoDraw(
    std::unique_ptr<FrameTimingsRecorder> frame_timings_recorder,
    std::vector<std::unique_ptr<LayerTreeTask>> tasks) {
  TRACE_EVENT_WITH_FRAME_NUMBER(frame_timings_recorder, "flutter",
                                "Rasterizer::DoDraw", /*flow_id_count=*/0,
                                /*flow_ids=*/nullptr);
  FML_DCHECK(delegate_.GetTaskRunners()
                 .GetRasterTaskRunner()
                 ->RunsTasksOnCurrentThread());
  frame_timings_recorder->AssertInState(FrameTimingsRecorder::State::kBuildEnd);

  if (tasks.empty()) {
    return DoDrawResult{DoDrawStatus::kDone};
  }
  if (!surface_) {
    return DoDrawResult{DoDrawStatus::kNotSetUp};
  }

#if !SLIMPELLER
  PersistentCache* persistent_cache = PersistentCache::GetCacheForProcess();
  persistent_cache->ResetStoredNewShaders();
#endif  //  !SLIMPELLER

  DoDrawResult result =
      DrawToSurfaces(*frame_timings_recorder, std::move(tasks));

  FML_DCHECK(result.status != DoDrawStatus::kEnqueuePipeline);
  if (result.status == DoDrawStatus::kGpuUnavailable) {
    return DoDrawResult{DoDrawStatus::kGpuUnavailable};
  }

#if !SLIMPELLER
  if (persistent_cache->IsDumpingSkp() &&
      persistent_cache->StoredNewShaders()) {
    auto screenshot =
        ScreenshotLastLayerTree(ScreenshotType::SkiaPicture, false);
    persistent_cache->DumpSkp(*screenshot.data);
  }
#endif  //  !SLIMPELLER

  // TODO(liyuqian): in Fuchsia, the rasterization doesn't finish when
  // Rasterizer::DoDraw finishes. Future work is needed to adapt the timestamp
  // for Fuchsia to capture SceneUpdateContext::ExecutePaintTasks.
  delegate_.OnFrameRasterized(frame_timings_recorder->GetRecordedTime());

// SceneDisplayLag events are disabled on Fuchsia.
// see: https://github.com/flutter/flutter/issues/56598
#if !defined(OS_FUCHSIA)
  const fml::TimePoint raster_finish_time =
      frame_timings_recorder->GetRasterEndTime();
  fml::TimePoint frame_target_time =
      frame_timings_recorder->GetVsyncTargetTime();
  if (raster_finish_time > frame_target_time) {
    fml::TimePoint latest_frame_target_time =
        delegate_.GetLatestFrameTargetTime();
    const auto frame_budget_millis = delegate_.GetFrameBudget().count();
    if (latest_frame_target_time < raster_finish_time) {
      latest_frame_target_time =
          latest_frame_target_time +
          fml::TimeDelta::FromMillisecondsF(frame_budget_millis);
    }
    const auto frame_lag =
        (latest_frame_target_time - frame_target_time).ToMillisecondsF();
    const int vsync_transitions_missed = round(frame_lag / frame_budget_millis);
    fml::tracing::TraceEventAsyncComplete(
        "flutter",                    // category
        "SceneDisplayLag",            // name
        raster_finish_time,           // begin_time
        latest_frame_target_time,     // end_time
        "frame_target_time",          // arg_key_1
        frame_target_time,            // arg_val_1
        "current_frame_target_time",  // arg_key_2
        latest_frame_target_time,     // arg_val_2
        "vsync_transitions_missed",   // arg_key_3
        vsync_transitions_missed      // arg_val_3
    );
  }
#endif

  // Pipeline pressure is applied from a couple of places:
  // rasterizer: When there are more items as of the time of Consume.
  // animator (via shell): Frame gets produces every vsync.
  // Enqueing here is to account for the following scenario:
  // T = 1
  //  - one item (A) in the pipeline
  //  - rasterizer starts (and merges the threads)
  //  - pipeline consume result says no items to process
  // T = 2
  //  - animator produces (B) to the pipeline
  //  - applies pipeline pressure via platform thread.
  // T = 3
  //   - rasterizes finished (and un-merges the threads)
  //   - |Draw| for B yields as its on the wrong thread.
  // This enqueue ensures that we attempt to consume from the right
  // thread one more time after un-merge.
  if (raster_thread_merger_) {
    if (raster_thread_merger_->DecrementLease() ==
        fml::RasterThreadStatus::kUnmergedNow) {
      return DoDrawResult{
          .status = DoDrawStatus::kEnqueuePipeline,
          .resubmitted_item = std::move(result.resubmitted_item),
      };
    }
  }

  return result;
}

Rasterizer::DoDrawResult Rasterizer::DrawToSurfaces(
    FrameTimingsRecorder& frame_timings_recorder,
    std::vector<std::unique_ptr<LayerTreeTask>> tasks) {
  TRACE_EVENT0("flutter", "Rasterizer::DrawToSurfaces");
  FML_DCHECK(surface_);
  frame_timings_recorder.AssertInState(FrameTimingsRecorder::State::kBuildEnd);

  DoDrawResult result{
      .status = DoDrawStatus::kDone,
  };
  if (surface_->AllowsDrawingWhenGpuDisabled()) {
    result.resubmitted_item =
        DrawToSurfacesUnsafe(frame_timings_recorder, std::move(tasks));
  } else {
    delegate_.GetIsGpuDisabledSyncSwitch()->Execute(
        fml::SyncSwitch::Handlers()
            .SetIfTrue([&] {
              result.status = DoDrawStatus::kGpuUnavailable;
              frame_timings_recorder.RecordRasterStart(fml::TimePoint::Now());
              frame_timings_recorder.RecordRasterEnd();
            })
            .SetIfFalse([&] {
              result.resubmitted_item = DrawToSurfacesUnsafe(
                  frame_timings_recorder, std::move(tasks));
            }));
  }
  frame_timings_recorder.AssertInState(FrameTimingsRecorder::State::kRasterEnd);

  return result;
}

std::unique_ptr<FrameItem> Rasterizer::DrawToSurfacesUnsafe(
    FrameTimingsRecorder& frame_timings_recorder,
    std::vector<std::unique_ptr<LayerTreeTask>> tasks) {
  compositor_context_->ui_time().SetLapTime(
      frame_timings_recorder.GetBuildDuration());

  // First traverse: Filter out discarded trees
  auto task_iter = tasks.begin();
  while (task_iter != tasks.end()) {
    LayerTreeTask& task = **task_iter;
    if (delegate_.ShouldDiscardLayerTree(task.view_id, *task.layer_tree)) {
      EnsureViewRecord(task.view_id).last_draw_status =
          DrawSurfaceStatus::kDiscarded;
      task_iter = tasks.erase(task_iter);
    } else {
      ++task_iter;
    }
  }
  if (tasks.empty()) {
    frame_timings_recorder.RecordRasterStart(fml::TimePoint::Now());
    frame_timings_recorder.RecordRasterEnd();
    return nullptr;
  }

  if (external_view_embedder_) {
    FML_DCHECK(!external_view_embedder_->GetUsedThisFrame());
    external_view_embedder_->SetUsedThisFrame(true);
    external_view_embedder_->BeginFrame(surface_->GetContext(),
                                        raster_thread_merger_);
  }

  std::optional<fml::TimePoint> presentation_time = std::nullopt;
  // TODO (https://github.com/flutter/flutter/issues/105596): this can be in
  // the past and might need to get snapped to future as this frame could
  // have been resubmitted. `presentation_time` on SubmitInfo is not set
  // in this case.
  {
    const auto vsync_target_time = frame_timings_recorder.GetVsyncTargetTime();
    if (vsync_target_time > fml::TimePoint::Now()) {
      presentation_time = vsync_target_time;
    }
  }

  frame_timings_recorder.RecordRasterStart(fml::TimePoint::Now());

  // Second traverse: draw all layer trees.
  std::vector<std::unique_ptr<LayerTreeTask>> resubmitted_tasks;
  for (std::unique_ptr<LayerTreeTask>& task : tasks) {
    int64_t view_id = task->view_id;
    std::unique_ptr<LayerTree> layer_tree = std::move(task->layer_tree);
    float device_pixel_ratio = task->device_pixel_ratio;

    DrawSurfaceStatus status = DrawToSurfaceUnsafe(
        view_id, *layer_tree, device_pixel_ratio, presentation_time);
    FML_DCHECK(status != DrawSurfaceStatus::kDiscarded);

    auto& view_record = EnsureViewRecord(task->view_id);
    view_record.last_draw_status = status;
    if (status == DrawSurfaceStatus::kSuccess) {
      view_record.last_successful_task = std::make_unique<LayerTreeTask>(
          view_id, std::move(layer_tree), device_pixel_ratio);
    } else if (status == DrawSurfaceStatus::kRetry) {
      resubmitted_tasks.push_back(std::make_unique<LayerTreeTask>(
          view_id, std::move(layer_tree), device_pixel_ratio));
    }
  }
  // TODO(dkwingsmt): Pass in raster cache(s) for all views.
  // See https://github.com/flutter/flutter/issues/135530, item 4.
  frame_timings_recorder.RecordRasterEnd(
      NOT_SLIMPELLER(&compositor_context_->raster_cache()));

  FireNextFrameCallbackIfPresent();

#if !SLIMPELLER
  if (surface_->GetContext()) {
    surface_->GetContext()->performDeferredCleanup(kSkiaCleanupExpiration);
  }
#endif  //  !SLIMPELLER

  if (resubmitted_tasks.empty()) {
    return nullptr;
  } else {
    return std::make_unique<FrameItem>(
        std::move(resubmitted_tasks),
        frame_timings_recorder.CloneUntil(
            FrameTimingsRecorder::State::kBuildEnd));
  }
}

/// \see Rasterizer::DrawToSurfaces
DrawSurfaceStatus Rasterizer::DrawToSurfaceUnsafe(
    int64_t view_id,
    flutter::LayerTree& layer_tree,
    float device_pixel_ratio,
    std::optional<fml::TimePoint> presentation_time) {
  FML_DCHECK(surface_);

  DlCanvas* embedder_root_canvas = nullptr;
  if (external_view_embedder_) {
    external_view_embedder_->PrepareFlutterView(layer_tree.frame_size(),
                                                device_pixel_ratio);
    // TODO(dkwingsmt): Add view ID here.
    embedder_root_canvas = external_view_embedder_->GetRootCanvas();
  }

  // On Android, the external view embedder deletes surfaces in `BeginFrame`.
  //
  // Deleting a surface also clears the GL context. Therefore, acquire the
  // frame after calling `BeginFrame` as this operation resets the GL context.
  auto frame = surface_->AcquireFrame(layer_tree.frame_size());
  if (frame == nullptr) {
    return DrawSurfaceStatus::kFailed;
  }

  // If the external view embedder has specified an optional root surface, the
  // root surface transformation is set by the embedder instead of
  // having to apply it here.
  DlMatrix root_surface_transformation =
      embedder_root_canvas ? DlMatrix() : surface_->GetRootTransformation();

  auto root_surface_canvas =
      embedder_root_canvas ? embedder_root_canvas : frame->Canvas();
  auto compositor_frame = compositor_context_->AcquireFrame(
      surface_->GetContext(),         // skia GrContext
      root_surface_canvas,            // root surface canvas
      external_view_embedder_.get(),  // external view embedder
      root_surface_transformation,    // root surface transformation
      true,                           // instrumentation enabled
      frame->framebuffer_info()
          .supports_readback,           // surface supports pixel reads
      raster_thread_merger_,            // thread merger
      surface_->GetAiksContext().get()  // aiks context
  );
  if (compositor_frame) {
    NOT_SLIMPELLER(compositor_context_->raster_cache().BeginFrame());

    std::unique_ptr<FrameDamage> damage;
    // when leaf layer tracing is enabled we wish to repaint the whole frame
    // for accurate performance metrics.
    if (frame->framebuffer_info().supports_partial_repaint) {
      // Disable partial repaint if external_view_embedder_ SubmitFlutterView is
      // involved - ExternalViewEmbedder unconditionally clears the entire
      // surface and also partial repaint with platform view present is
      // something that still need to be figured out.
      bool force_full_repaint =
          external_view_embedder_ &&
          (!raster_thread_merger_ || raster_thread_merger_->IsMerged());

      damage = std::make_unique<FrameDamage>();
      auto existing_damage = frame->framebuffer_info().existing_damage;
      if (existing_damage.has_value() && !force_full_repaint) {
        damage->SetPreviousLayerTree(GetLastLayerTree(view_id));
        damage->AddAdditionalDamage(existing_damage.value());
        damage->SetClipAlignment(
            frame->framebuffer_info().horizontal_clip_alignment,
            frame->framebuffer_info().vertical_clip_alignment);
      }
    }

    bool ignore_raster_cache = true;
    if (surface_->EnableRasterCache()) {
      ignore_raster_cache = false;
    }

    RasterStatus frame_status =
        compositor_frame->Raster(layer_tree,           // layer tree
                                 ignore_raster_cache,  // ignore raster cache
                                 damage.get()          // frame damage
        );
    if (frame_status == RasterStatus::kSkipAndRetry) {
      return DrawSurfaceStatus::kRetry;
    }

    SurfaceFrame::SubmitInfo submit_info;
    submit_info.presentation_time = presentation_time;
    if (damage) {
      submit_info.frame_damage = damage->GetFrameDamage();
      submit_info.buffer_damage = damage->GetBufferDamage();
    }

    frame->set_submit_info(submit_info);

    if (external_view_embedder_ &&
        (!raster_thread_merger_ || raster_thread_merger_->IsMerged())) {
      FML_DCHECK(!frame->IsSubmitted());
      external_view_embedder_->SubmitFlutterView(
          view_id, surface_->GetContext(), surface_->GetAiksContext(),
          std::move(frame));
    } else {
      frame->Submit();
    }

#if !SLIMPELLER
    // Do not update raster cache metrics for kResubmit because that status
    // indicates that the frame was not actually painted.
    if (frame_status != RasterStatus::kResubmit) {
      compositor_context_->raster_cache().EndFrame();
    }
#endif  //  !SLIMPELLER

    if (frame_status == RasterStatus::kResubmit) {
      return DrawSurfaceStatus::kRetry;
    } else {
      FML_CHECK(frame_status == RasterStatus::kSuccess);
      return DrawSurfaceStatus::kSuccess;
    }
  }

  return DrawSurfaceStatus::kFailed;
}

Rasterizer::ViewRecord& Rasterizer::EnsureViewRecord(int64_t view_id) {
  return view_records_[view_id];
}

static sk_sp<SkData> ScreenshotLayerTreeAsPicture(
    flutter::LayerTree* tree,
    flutter::CompositorContext& compositor_context) {
#if SLIMPELLER
  return nullptr;
#else  // SLIMPELLER
  FML_DCHECK(tree != nullptr);
  SkPictureRecorder recorder;
  recorder.beginRecording(
      SkRect::MakeWH(tree->frame_size().width, tree->frame_size().height));

  DlMatrix root_surface_transformation;
  DlSkCanvasAdapter canvas(recorder.getRecordingCanvas());

  // TODO(amirh): figure out how to take a screenshot with embedded UIView.
  // https://github.com/flutter/flutter/issues/23435
  auto frame = compositor_context.AcquireFrame(nullptr, &canvas, nullptr,
                                               root_surface_transformation,
                                               false, true, nullptr, nullptr);
  frame->Raster(*tree, true, nullptr);

#if defined(OS_FUCHSIA)
  SkSerialProcs procs = {0};
  procs.fImageProc = SerializeImageWithoutData;
  procs.fTypefaceProc = SerializeTypefaceWithoutData;
#else
  SkSerialProcs procs = {0};
  procs.fTypefaceProc = SerializeTypefaceWithData;
  procs.fImageProc = [](SkImage* img, void*) -> SkSerialReturnType {
    return SkPngEncoder::Encode(nullptr, img, SkPngEncoder::Options{});
  };
#endif

  return recorder.finishRecordingAsPicture()->serialize(&procs);
#endif  //  SLIMPELLER
}

static void RenderFrameForScreenshot(
    flutter::CompositorContext& compositor_context,
    DlCanvas* canvas,
    flutter::LayerTree* tree,
    GrDirectContext* surface_context,
    const std::shared_ptr<impeller::AiksContext>& aiks_context) {
  // There is no root surface transformation for the screenshot layer. Reset
  // the matrix to identity.
  DlMatrix root_surface_transformation;

  auto frame = compositor_context.AcquireFrame(
      /*gr_context=*/surface_context,
      /*canvas=*/canvas,
      /*view_embedder=*/nullptr,
      /*root_surface_transformation=*/root_surface_transformation,
      /*instrumentation_enabled=*/false,
      /*surface_supports_readback=*/true,
      /*raster_thread_merger=*/nullptr,
      /*aiks_context=*/aiks_context.get());
  canvas->Clear(DlColor::kTransparent());
  frame->Raster(*tree, true, nullptr);
  canvas->Flush();
}

#if IMPELLER_SUPPORTS_RENDERING
Rasterizer::ScreenshotFormat ToScreenshotFormat(impeller::PixelFormat format) {
  switch (format) {
    case impeller::PixelFormat::kUnknown:
    case impeller::PixelFormat::kA8UNormInt:
    case impeller::PixelFormat::kR8UNormInt:
    case impeller::PixelFormat::kR8G8UNormInt:
    case impeller::PixelFormat::kR8G8B8A8UNormIntSRGB:
    case impeller::PixelFormat::kB8G8R8A8UNormIntSRGB:
    case impeller::PixelFormat::kB10G10R10XRSRGB:
    case impeller::PixelFormat::kS8UInt:
    case impeller::PixelFormat::kD24UnormS8Uint:
    case impeller::PixelFormat::kD32FloatS8UInt:
    case impeller::PixelFormat::kR32G32B32A32Float:
    case impeller::PixelFormat::kB10G10R10XR:
    case impeller::PixelFormat::kB10G10R10A10XR:
    case impeller::PixelFormat::kR32Float:
    case impeller::PixelFormat::kBC1RGBAUNormInt:
    case impeller::PixelFormat::kBC1RGBAUNormIntSRGB:
    case impeller::PixelFormat::kBC3RGBAUNormInt:
    case impeller::PixelFormat::kBC3RGBAUNormIntSRGB:
    case impeller::PixelFormat::kBC5RGUNormInt:
    case impeller::PixelFormat::kBC7RGBAUNormInt:
    case impeller::PixelFormat::kBC7RGBAUNormIntSRGB:
    case impeller::PixelFormat::kETC2RGB8UNormInt:
    case impeller::PixelFormat::kETC2RGB8UNormIntSRGB:
    case impeller::PixelFormat::kETC2RGBA8UNormInt:
    case impeller::PixelFormat::kETC2RGBA8UNormIntSRGB:
    case impeller::PixelFormat::kASTC4x4LDR:
    case impeller::PixelFormat::kASTC4x4LDRSRGB:
    case impeller::PixelFormat::kASTC8x8LDR:
    case impeller::PixelFormat::kASTC8x8LDRSRGB:
    case impeller::PixelFormat::kASTC4x4HDR:
    case impeller::PixelFormat::kASTC8x8HDR:
      FML_DCHECK(false);
      return Rasterizer::ScreenshotFormat::kUnknown;
    case impeller::PixelFormat::kR8G8B8A8UNormInt:
      return Rasterizer::ScreenshotFormat::kR8G8B8A8UNormInt;
    case impeller::PixelFormat::kB8G8R8A8UNormInt:
      return Rasterizer::ScreenshotFormat::kB8G8R8A8UNormInt;
    case impeller::PixelFormat::kR16G16B16A16Float:
      return Rasterizer::ScreenshotFormat::kR16G16B16A16Float;
  }
}

static std::pair<sk_sp<SkData>, Rasterizer::ScreenshotFormat>
ScreenshotLayerTreeAsImageImpeller(
    const std::shared_ptr<impeller::AiksContext>& aiks_context,
    flutter::LayerTree* tree,
    flutter::CompositorContext& compositor_context,
    bool compressed) {
  if (compressed) {
    FML_LOG(ERROR) << "Compressed screenshots not supported for Impeller";
    return {nullptr, Rasterizer::ScreenshotFormat::kUnknown};
  }

  DisplayListBuilder builder(DlRect::MakeSize(tree->frame_size()));

  RenderFrameForScreenshot(compositor_context, &builder, tree, nullptr,
                           aiks_context);

  std::shared_ptr<impeller::Texture> texture = impeller::DisplayListToTexture(
      builder.Build(), impeller::ISize(tree->frame_size()), *aiks_context);
  if (!texture) {
    FML_LOG(ERROR) << "Failed to render to texture";
    return {nullptr, Rasterizer::ScreenshotFormat::kUnknown};
  }

  impeller::DeviceBufferDescriptor buffer_desc;
  buffer_desc.storage_mode = impeller::StorageMode::kHostVisible;
  buffer_desc.size =
      texture->GetTextureDescriptor().GetByteSizeOfBaseMipLevel();
  auto impeller_context = aiks_context->GetContext();
  auto buffer =
      impeller_context->GetResourceAllocator()->CreateBuffer(buffer_desc);
  auto command_buffer = impeller_context->CreateCommandBuffer();
  command_buffer->SetLabel("BlitTextureToBuffer Command Buffer");
  auto pass = command_buffer->CreateBlitPass();
  pass->AddCopy(texture, buffer);
  pass->EncodeCommands();
  fml::AutoResetWaitableEvent latch;
  sk_sp<SkData> sk_data;
  auto completion = [buffer, &buffer_desc, &sk_data,
                     &latch](impeller::CommandBuffer::Status status) {
    fml::ScopedCleanupClosure cleanup([&latch]() { latch.Signal(); });
    if (status != impeller::CommandBuffer::Status::kCompleted) {
      FML_LOG(ERROR) << "Failed to complete blit pass.";
      return;
    }
    sk_data = SkData::MakeWithCopy(buffer->OnGetContents(), buffer_desc.size);
  };

  if (!impeller_context->GetCommandQueue()
           ->Submit({command_buffer}, completion)
           .ok()) {
    FML_LOG(ERROR) << "Failed to submit commands.";
  }
  latch.Wait();
  return std::make_pair(
      sk_data, ToScreenshotFormat(texture->GetTextureDescriptor().format));
}
#endif

std::pair<sk_sp<SkData>, Rasterizer::ScreenshotFormat>
Rasterizer::ScreenshotLayerTreeAsImage(
    flutter::LayerTree* tree,
    flutter::CompositorContext& compositor_context,
    bool compressed) {
#if IMPELLER_SUPPORTS_RENDERING
  if (delegate_.GetSettings().enable_impeller) {
    return ScreenshotLayerTreeAsImageImpeller(GetAiksContext(), tree,
                                              compositor_context, compressed);
  }
#endif  // IMPELLER_SUPPORTS_RENDERING

#if SLIMPELLER
  FML_LOG(FATAL) << "Impeller opt-out unavailable.";
  return {nullptr, ScreenshotFormat::kUnknown};
#else   // SLIMPELLER
  GrDirectContext* surface_context = GetGrContext();
  // Attempt to create a snapshot surface depending on whether we have access
  // to a valid GPU rendering context.
  std::unique_ptr<OffscreenSurface> snapshot_surface =
      std::make_unique<OffscreenSurface>(surface_context, tree->frame_size());

  if (!snapshot_surface->IsValid()) {
    FML_LOG(ERROR) << "Screenshot: unable to create snapshot surface";
    return {nullptr, ScreenshotFormat::kUnknown};
  }

  // Draw the current layer tree into the snapshot surface.
  DlCanvas* canvas = snapshot_surface->GetCanvas();

  // snapshot_surface->makeImageSnapshot needs the GL context to be set if the
  // render context is GL. frame->Raster() pops the gl context in platforms
  // that gl context switching are used. (For example, older iOS that uses GL)
  // We reset the GL context using the context switch.
  auto context_switch = surface_->MakeRenderContextCurrent();
  if (!context_switch->GetResult()) {
    FML_LOG(ERROR) << "Screenshot: unable to make image screenshot";
    return {nullptr, ScreenshotFormat::kUnknown};
  }

  RenderFrameForScreenshot(compositor_context, canvas, tree, surface_context,
                           nullptr);

  return std::make_pair(snapshot_surface->GetRasterData(compressed),
                        ScreenshotFormat::kUnknown);
#endif  //  !SLIMPELLER
}

Rasterizer::Screenshot Rasterizer::ScreenshotLastLayerTree(
    Rasterizer::ScreenshotType type,
    bool base64_encode) {
  if (delegate_.GetSettings().enable_impeller &&
      type == ScreenshotType::SkiaPicture) {
    FML_DCHECK(false);
    FML_LOG(ERROR) << "Last layer tree cannot be screenshotted as a "
                      "SkiaPicture when using Impeller.";
    return {};
  }
  // TODO(dkwingsmt): Support screenshotting all last layer trees
  // when the shell protocol supports multi-views.
  // https://github.com/flutter/flutter/issues/135534
  // https://github.com/flutter/flutter/issues/135535
  auto* layer_tree = GetLastLayerTree(kFlutterImplicitViewId);
  if (layer_tree == nullptr) {
    FML_LOG(ERROR) << "Last layer tree was null when screenshotting.";
    return {};
  }

  std::pair<sk_sp<SkData>, ScreenshotFormat> data{nullptr,
                                                  ScreenshotFormat::kUnknown};
  std::string format;

  switch (type) {
    case ScreenshotType::SkiaPicture:
      format = "ScreenshotType::SkiaPicture";
      data.first =
          ScreenshotLayerTreeAsPicture(layer_tree, *compositor_context_);
      break;
    case ScreenshotType::UncompressedImage:
      format = "ScreenshotType::UncompressedImage";
      data =
          ScreenshotLayerTreeAsImage(layer_tree, *compositor_context_, false);
      break;
    case ScreenshotType::CompressedImage:
      format = "ScreenshotType::CompressedImage";
      data = ScreenshotLayerTreeAsImage(layer_tree, *compositor_context_, true);
      break;
    case ScreenshotType::SurfaceData: {
      Surface::SurfaceData surface_data = surface_->GetSurfaceData();
      format = surface_data.pixel_format;
      data.first = surface_data.data;
      break;
    }
  }

  if (data.first == nullptr) {
    FML_LOG(ERROR) << "Screenshot data was null.";
    return {};
  }

  if (base64_encode) {
    size_t b64_size = Base64::EncodedSize(data.first->size());
    auto b64_data = SkData::MakeUninitialized(b64_size);
    Base64::Encode(data.first->data(), data.first->size(),
                   b64_data->writable_data());
    return Rasterizer::Screenshot{b64_data, layer_tree->frame_size(), format,
                                  data.second};
  }

  return Rasterizer::Screenshot{data.first, layer_tree->frame_size(), format,
                                data.second};
}

void Rasterizer::SetNextFrameCallback(const fml::closure& callback) {
  next_frame_callback_ = callback;
}

void Rasterizer::SetExternalViewEmbedder(
    const std::shared_ptr<ExternalViewEmbedder>& view_embedder) {
  external_view_embedder_ = view_embedder;
}

void Rasterizer::SetSnapshotSurfaceProducer(
    std::unique_ptr<SnapshotSurfaceProducer> producer) {
  snapshot_surface_producer_ = std::move(producer);
}

fml::RefPtr<fml::RasterThreadMerger> Rasterizer::GetRasterThreadMerger() {
  return raster_thread_merger_;
}

void Rasterizer::FireNextFrameCallbackIfPresent() {
  if (!next_frame_callback_) {
    return;
  }
  // It is safe for the callback to set a new callback.
  auto callback = next_frame_callback_;
  next_frame_callback_ = nullptr;
  callback();
}

void Rasterizer::SetResourceCacheMaxBytes(size_t max_bytes, bool from_user) {
#if !SLIMPELLER
  user_override_resource_cache_bytes_ |= from_user;

  if (!from_user && user_override_resource_cache_bytes_) {
    // We should not update the setting here if a user has explicitly set a
    // value for this over the flutter/skia channel.
    return;
  }

  max_cache_bytes_ = max_bytes;
  if (!surface_) {
    return;
  }

  GrDirectContext* context = surface_->GetContext();
  if (context) {
    auto context_switch = surface_->MakeRenderContextCurrent();
    if (!context_switch->GetResult()) {
      return;
    }

    context->setResourceCacheLimit(max_bytes);
  }
#endif  //  !SLIMPELLER
}

std::optional<size_t> Rasterizer::GetResourceCacheMaxBytes() const {
#if SLIMPELLER
  return std::nullopt;
#else   // SLIMPELLER
  if (!surface_) {
    return std::nullopt;
  }
  GrDirectContext* context = surface_->GetContext();
  if (context) {
    return context->getResourceCacheLimit();
  }
  return std::nullopt;
#endif  //  SLIMPELLER
}

Rasterizer::Screenshot::Screenshot() {}

Rasterizer::Screenshot::Screenshot(sk_sp<SkData> p_data,
                                   DlISize p_size,
                                   const std::string& p_format,
                                   ScreenshotFormat p_pixel_format)
    : data(std::move(p_data)),
      frame_size(p_size),
      format(p_format),
      pixel_format(p_pixel_format) {}

Rasterizer::Screenshot::Screenshot(const Screenshot& other) = default;

Rasterizer::Screenshot::~Screenshot() = default;

}  // namespace flutter
