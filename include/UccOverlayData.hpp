// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>

/// Shared-memory layout written by uccd daemon and read by ucc-fps-layer.
/// Name: "/ucc-overlay"  (opened via shm_open).
/// The daemon creates / updates it; the layer only reads.

struct UccOverlayData
{
  /// Monotonically increasing sequence number; the layer can skip re-rendering
  /// the pixel buffer when this hasn't changed since the last blit.
  uint32_t sequence;

  /// True while an Auto-OC or Auto-Undervolt scan is in progress.
  uint8_t  active;

  /// 0 = Auto-OC (core/vram offset search), 1 = Auto-Undervolt (freq-cap search).
  uint8_t  mode;

  /// Phase of the current scan (maps to AutoOCPhase / UVPhase enums).
  /// 0=Idle  1=Baseline  2=Searching  3=OffsetSearching(UV)  4=Validating  5=Done
  uint8_t  phase;

  uint8_t  _pad0;

  // ── Progress ──────────────────────────────────────────────────────────────
  int32_t  iteration;        ///< Current step (1-based)
  int32_t  maxIterations;    ///< Estimated total steps

  // ── Clocks / offsets under test ───────────────────────────────────────────
  int32_t  currentOffsetMHz; ///< Core offset (AutoOC) or freq cap (UV) being tested
  int32_t  bestStableMHz;    ///< Best stable value found so far

  // ── Telemetry ─────────────────────────────────────────────────────────────
  int32_t  gpuClockMHz;
  int32_t  vramClockMHz;
  int32_t  tempC;
  int32_t  gpuUtilPct;
  int32_t  powerDrawW;
  double   fps;

  // ── Human-readable status ─────────────────────────────────────────────────
  char     message[128];

  // ── Last test result ──────────────────────────────────────────────────────
  /// 0=Stable  1=Unstable  2=ThermalLimit  3=Aborted
  uint8_t  lastResult;

  uint8_t  _pad1[7];
};

static_assert( sizeof(UccOverlayData) <= 4096, "UccOverlayData must fit in one page" );

inline constexpr const char *kUccOverlayShmName = "/ucc-overlay";
