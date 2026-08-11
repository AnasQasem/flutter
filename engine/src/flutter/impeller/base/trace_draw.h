// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef FLUTTER_IMPELLER_BASE_TRACE_DRAW_H_
#define FLUTTER_IMPELLER_BASE_TRACE_DRAW_H_

#include "flutter/fml/trace_event.h"

/// `IMPELLER_TRACE_DRAW` marks the PER-DRAW trace points — the ones that fire
/// once per draw call rather than once per frame.
///
/// Why this exists: those trace points are how the 120 fps work found its
/// answers (see `DL::Replay` / `Canvas::DrawPath` in the perf handoff), but
/// their volume is extreme. A Quran page measured **11497 `Canvas::DrawPath`
/// slices across 31 raster frames — ~371 per frame** — plus ~78
/// `Canvas::DrawTextFrame` and 250 `RenderPass::EncodeCommands`. Every
/// `TRACE_EVENT0` is a begin AND an end, so that is ~1400 timeline operations
/// per raster frame.
///
/// Two costs follow, and they are easy to miss:
///
/// 1. **While a tracer is attached** — i.e. during every DevTools measurement —
///    each one runs the full handler: allowlist query plus a timeline write.
///    That lands on the raster thread, inside `DL::Replay`, and therefore
///    inflates the very raster times being measured. It is also why the VM's
///    timeline ring buffer fills in under a second of swiping.
///
/// 2. **Even with no tracer attached**, `TraceEvent0` reads the clock as an
///    ARGUMENT (`gTimelineMicrosSource.load()()` in `fml/trace_event.cc`), so
///    the read happens before the `if (handler)` fast path can skip anything.
///    ~1400 clock reads per raster frame are paid regardless.
///
/// And on Android none of this compiles out in release:
/// `FLUTTER_TIMELINE_ENABLED` is gated on
/// `FLUTTER_RELEASE && ... && !defined(FML_OS_ANDROID)` (`fml/trace_event.h`),
/// so `FML_OS_ANDROID` keeps the timeline enabled in shipped release builds.
///
/// Default is ON, so existing behaviour and every attribution workflow in the
/// handoff are unchanged. Build with `--args="impeller_trace_per_draw=false"`
/// to get a measurement free of this observer effect; comparing the two answers
/// "how much of the raster time is the measurement itself?".
///
/// Only genuinely per-draw sites belong here. Per-frame events
/// (`DL::Replay`, `DL::FirstPass`, `DL::FinishRecording`) and cache-MISS-only
/// events (`TessellateCacheMiss`, `ExtractColorGlyph`, `BuildMergedBlobPath`)
/// are low volume and must stay unconditional — they are what makes a cold
/// frame legible.
#ifdef IMPELLER_TRACE_PER_DRAW
#define IMPELLER_TRACE_DRAW(name) TRACE_EVENT0("impeller", name)
#else
#define IMPELLER_TRACE_DRAW(name) \
  do {                            \
  } while (0)
#endif  // IMPELLER_TRACE_PER_DRAW

#endif  // FLUTTER_IMPELLER_BASE_TRACE_DRAW_H_
