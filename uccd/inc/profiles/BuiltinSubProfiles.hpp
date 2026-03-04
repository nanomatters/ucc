/*
 * Built-in (non-editable) sub-profile constants.
 *
 * GPU built-in profiles are dynamically generated at runtime from hardware
 * state (see rebuildBuiltinGpuProfiles()).  Only the ID and display name
 * are defined here.
 *
 * Keyboard built-in profiles are fully hardcoded — the JSON payload below
 * represents the factory-default per-key RGB layout.
 */

#pragma once

#include <string>

// ---------------------------------------------------------------------------
// GPU built-in sub-profile
// ---------------------------------------------------------------------------
inline constexpr const char *BUILTIN_GPU_PROFILE_ID   = "gpu-default-builtin";
inline constexpr const char *BUILTIN_GPU_PROFILE_NAME = "Default [Built-in]";

// ---------------------------------------------------------------------------
// Keyboard built-in sub-profile
// ---------------------------------------------------------------------------
inline constexpr const char *BUILTIN_KEYBOARD_PROFILE_ID   = "keyboard-default-builtin";
inline constexpr const char *BUILTIN_KEYBOARD_PROFILE_NAME = "Default [Built-in]";

// 126-zone per-key RGB layout at brightness 50.
// clang-format off
inline const std::string BUILTIN_KEYBOARD_PROFILE_JSON = R"json(
{
  "brightness": 50,
  "name": "Default [Built-in]",
  "states": [
    {"blue": 25, "brightness": 50, "green": 102, "mode": 0, "red": 255},
    {"blue": 255, "brightness": 50, "green": 204, "mode": 0, "red": 138},
    {"blue": 25, "brightness": 50, "green": 102, "mode": 0, "red": 255},
    {"blue": 25, "brightness": 50, "green": 102, "mode": 0, "red": 255},
    {"blue": 25, "brightness": 50, "green": 102, "mode": 0, "red": 255},
    {"blue": 255, "brightness": 50, "green": 204, "mode": 0, "red": 138},
    {"blue": 255, "brightness": 50, "green": 204, "mode": 0, "red": 138},
    {"blue": 255, "brightness": 50, "green": 204, "mode": 0, "red": 138},
    {"blue": 255, "brightness": 50, "green": 204, "mode": 0, "red": 138},
    {"blue": 255, "brightness": 50, "green": 204, "mode": 0, "red": 138},
    {"blue": 25, "brightness": 50, "green": 102, "mode": 0, "red": 255},
    {"blue": 255, "brightness": 50, "green": 204, "mode": 0, "red": 138},
    {"blue": 25, "brightness": 50, "green": 102, "mode": 0, "red": 255},
    {"blue": 255, "brightness": 50, "green": 204, "mode": 0, "red": 138},
    {"blue": 255, "brightness": 50, "green": 204, "mode": 0, "red": 138},
    {"blue": 255, "brightness": 50, "green": 204, "mode": 0, "red": 138},
    {"blue": 255, "brightness": 50, "green": 204, "mode": 0, "red": 138},
    {"blue": 255, "brightness": 50, "green": 204, "mode": 0, "red": 138},
    {"blue": 255, "brightness": 50, "green": 204, "mode": 0, "red": 138},
    {"blue": 255, "brightness": 50, "green": 204, "mode": 0, "red": 138},
    {"blue": 255, "brightness": 50, "green": 204, "mode": 0, "red": 138},
    {"blue": 255, "brightness": 50, "green": 204, "mode": 0, "red": 138},
    {"blue": 25, "brightness": 50, "green": 102, "mode": 0, "red": 255},
    {"blue": 255, "brightness": 50, "green": 204, "mode": 0, "red": 138},
    {"blue": 255, "brightness": 50, "green": 204, "mode": 0, "red": 138},
    {"blue": 255, "brightness": 50, "green": 204, "mode": 0, "red": 138},
    {"blue": 255, "brightness": 50, "green": 204, "mode": 0, "red": 138},
    {"blue": 255, "brightness": 50, "green": 204, "mode": 0, "red": 138},
    {"blue": 255, "brightness": 50, "green": 204, "mode": 0, "red": 138},
    {"blue": 255, "brightness": 50, "green": 204, "mode": 0, "red": 138},
    {"blue": 255, "brightness": 50, "green": 204, "mode": 0, "red": 138},
    {"blue": 255, "brightness": 50, "green": 204, "mode": 0, "red": 138},
    {"blue": 255, "brightness": 50, "green": 204, "mode": 0, "red": 138},
    {"blue": 255, "brightness": 50, "green": 204, "mode": 0, "red": 138},
    {"blue": 255, "brightness": 50, "green": 204, "mode": 0, "red": 138},
    {"blue": 25, "brightness": 50, "green": 102, "mode": 0, "red": 255},
    {"blue": 255, "brightness": 50, "green": 204, "mode": 0, "red": 138},
    {"blue": 255, "brightness": 50, "green": 204, "mode": 0, "red": 138},
    {"blue": 255, "brightness": 50, "green": 204, "mode": 0, "red": 138},
    {"blue": 25, "brightness": 50, "green": 102, "mode": 0, "red": 255},
    {"blue": 255, "brightness": 50, "green": 204, "mode": 0, "red": 138},
    {"blue": 255, "brightness": 50, "green": 204, "mode": 0, "red": 138},
    {"blue": 25, "brightness": 50, "green": 102, "mode": 0, "red": 255},
    {"blue": 255, "brightness": 50, "green": 204, "mode": 0, "red": 138},
    {"blue": 0, "brightness": 50, "green": 255, "mode": 0, "red": 68},
    {"blue": 0, "brightness": 50, "green": 255, "mode": 0, "red": 68},
    {"blue": 0, "brightness": 50, "green": 255, "mode": 0, "red": 68},
    {"blue": 255, "brightness": 50, "green": 204, "mode": 0, "red": 138},
    {"blue": 255, "brightness": 50, "green": 204, "mode": 0, "red": 138},
    {"blue": 255, "brightness": 50, "green": 204, "mode": 0, "red": 138},
    {"blue": 255, "brightness": 50, "green": 204, "mode": 0, "red": 138},
    {"blue": 255, "brightness": 50, "green": 204, "mode": 0, "red": 138},
    {"blue": 255, "brightness": 50, "green": 204, "mode": 0, "red": 138},
    {"blue": 255, "brightness": 50, "green": 204, "mode": 0, "red": 138},
    {"blue": 255, "brightness": 50, "green": 204, "mode": 0, "red": 138},
    {"blue": 255, "brightness": 50, "green": 204, "mode": 0, "red": 138},
    {"blue": 255, "brightness": 50, "green": 204, "mode": 0, "red": 138},
    {"blue": 255, "brightness": 50, "green": 204, "mode": 0, "red": 138},
    {"blue": 255, "brightness": 50, "green": 204, "mode": 0, "red": 138},
    {"blue": 255, "brightness": 50, "green": 204, "mode": 0, "red": 138},
    {"blue": 255, "brightness": 50, "green": 204, "mode": 0, "red": 138},
    {"blue": 255, "brightness": 50, "green": 204, "mode": 0, "red": 138},
    {"blue": 255, "brightness": 50, "green": 204, "mode": 0, "red": 138},
    {"blue": 25, "brightness": 50, "green": 102, "mode": 0, "red": 255},
    {"blue": 255, "brightness": 50, "green": 204, "mode": 0, "red": 138},
    {"blue": 255, "brightness": 50, "green": 204, "mode": 0, "red": 138},
    {"blue": 0, "brightness": 50, "green": 255, "mode": 0, "red": 68},
    {"blue": 255, "brightness": 50, "green": 204, "mode": 0, "red": 138},
    {"blue": 255, "brightness": 50, "green": 204, "mode": 0, "red": 138},
    {"blue": 255, "brightness": 50, "green": 204, "mode": 0, "red": 138},
    {"blue": 255, "brightness": 50, "green": 204, "mode": 0, "red": 138},
    {"blue": 255, "brightness": 50, "green": 204, "mode": 0, "red": 138},
    {"blue": 255, "brightness": 50, "green": 204, "mode": 0, "red": 138},
    {"blue": 255, "brightness": 50, "green": 204, "mode": 0, "red": 138},
    {"blue": 255, "brightness": 50, "green": 204, "mode": 0, "red": 138},
    {"blue": 255, "brightness": 50, "green": 204, "mode": 0, "red": 138},
    {"blue": 255, "brightness": 50, "green": 204, "mode": 0, "red": 138},
    {"blue": 25, "brightness": 50, "green": 102, "mode": 0, "red": 255},
    {"blue": 255, "brightness": 50, "green": 204, "mode": 0, "red": 138},
    {"blue": 255, "brightness": 50, "green": 204, "mode": 0, "red": 138},
    {"blue": 255, "brightness": 50, "green": 204, "mode": 0, "red": 138},
    {"blue": 255, "brightness": 50, "green": 204, "mode": 0, "red": 138},
    {"blue": 255, "brightness": 50, "green": 204, "mode": 0, "red": 138},
    {"blue": 255, "brightness": 50, "green": 204, "mode": 0, "red": 138},
    {"blue": 255, "brightness": 50, "green": 204, "mode": 0, "red": 138},
    {"blue": 255, "brightness": 50, "green": 204, "mode": 0, "red": 138},
    {"blue": 255, "brightness": 50, "green": 204, "mode": 0, "red": 138},
    {"blue": 255, "brightness": 50, "green": 204, "mode": 0, "red": 138},
    {"blue": 255, "brightness": 50, "green": 204, "mode": 0, "red": 138},
    {"blue": 255, "brightness": 50, "green": 204, "mode": 0, "red": 138},
    {"blue": 255, "brightness": 50, "green": 204, "mode": 0, "red": 138},
    {"blue": 255, "brightness": 50, "green": 204, "mode": 0, "red": 138},
    {"blue": 255, "brightness": 50, "green": 204, "mode": 0, "red": 138},
    {"blue": 255, "brightness": 50, "green": 204, "mode": 0, "red": 138},
    {"blue": 255, "brightness": 50, "green": 204, "mode": 0, "red": 138},
    {"blue": 255, "brightness": 50, "green": 204, "mode": 0, "red": 138},
    {"blue": 255, "brightness": 50, "green": 204, "mode": 0, "red": 138},
    {"blue": 255, "brightness": 50, "green": 204, "mode": 0, "red": 138},
    {"blue": 25, "brightness": 50, "green": 102, "mode": 0, "red": 255},
    {"blue": 25, "brightness": 50, "green": 102, "mode": 0, "red": 255},
    {"blue": 255, "brightness": 50, "green": 204, "mode": 0, "red": 138},
    {"blue": 255, "brightness": 50, "green": 204, "mode": 0, "red": 138},
    {"blue": 255, "brightness": 50, "green": 204, "mode": 0, "red": 138},
    {"blue": 255, "brightness": 50, "green": 204, "mode": 0, "red": 138},
    {"blue": 255, "brightness": 50, "green": 204, "mode": 0, "red": 138},
    {"blue": 25, "brightness": 50, "green": 102, "mode": 0, "red": 255},
    {"blue": 25, "brightness": 50, "green": 102, "mode": 0, "red": 255},
    {"blue": 25, "brightness": 50, "green": 102, "mode": 0, "red": 255},
    {"blue": 25, "brightness": 50, "green": 102, "mode": 0, "red": 255},
    {"blue": 25, "brightness": 50, "green": 102, "mode": 0, "red": 255},
    {"blue": 25, "brightness": 50, "green": 102, "mode": 0, "red": 255},
    {"blue": 25, "brightness": 50, "green": 102, "mode": 0, "red": 255},
    {"blue": 25, "brightness": 50, "green": 102, "mode": 0, "red": 255},
    {"blue": 25, "brightness": 50, "green": 102, "mode": 0, "red": 255},
    {"blue": 25, "brightness": 50, "green": 102, "mode": 0, "red": 255},
    {"blue": 25, "brightness": 50, "green": 102, "mode": 0, "red": 255},
    {"blue": 25, "brightness": 50, "green": 102, "mode": 0, "red": 255},
    {"blue": 25, "brightness": 50, "green": 102, "mode": 0, "red": 255},
    {"blue": 25, "brightness": 50, "green": 102, "mode": 0, "red": 255},
    {"blue": 25, "brightness": 50, "green": 102, "mode": 0, "red": 255},
    {"blue": 25, "brightness": 50, "green": 102, "mode": 0, "red": 255},
    {"blue": 25, "brightness": 50, "green": 102, "mode": 0, "red": 255},
    {"blue": 25, "brightness": 50, "green": 102, "mode": 0, "red": 255},
    {"blue": 25, "brightness": 50, "green": 102, "mode": 0, "red": 255},
    {"blue": 25, "brightness": 50, "green": 102, "mode": 0, "red": 255},
    {"blue": 255, "brightness": 50, "green": 204, "mode": 0, "red": 138}
  ]
}
)json";
// clang-format on
