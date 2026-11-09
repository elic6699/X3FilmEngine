// SPDX-FileCopyrightText: 2026 LittleSixNine
// SPDX-License-Identifier: GPL-3.0-or-later
// Applies only to FrameMatter contributions; upstream notices are retained. See COPYRIGHT.txt.

// Explicit boundaries for effects added upstream after the first Windows port.
// Included only by Windows call sites; macOS retains its upstream behavior.
#pragma once
#include "params.hpp"

namespace spk {

/* FrameMatter Windows port, 2026-11-08: overscan / date imprint / aging
 * textures / wide light leaks now have real Vulkan kernels and a portable
 * edge rasteriser. Only Cineon DI, scene-latitude mapping and contrast mask
 * remain refused. The two trailing flags stay in the signature because the
 * contrast-mask stubs in windows_unported.cpp still call the 5-arg form. */
inline const char* windows_unported_error(bool di, bool latitude, bool mask,
                                         bool overscan, bool date) {
    (void)overscan; (void)date;
    if (di) return "digital intermediate is not implemented by the Windows Vulkan backend yet";
    if (latitude) return "scene latitude mapping is not implemented by the Windows Vulkan backend yet";
    if (mask) return "contrast mask is not implemented by the Windows Vulkan backend yet";
    return nullptr;
}

inline const char* windows_unported_error(const Params& p) {
    return windows_unported_error(p.io.digital_intermediate, p.camera.scene_latitude.active,
                                  p.print_render.contrast_mask.active, false, false);
}

// Validate an already schema-checked delta before it mutates the session.
inline const char* windows_unported_delta_error(const Json& delta) {
    return windows_unported_error(delta.at("digital_intermediate").as_bool(),
                                  delta.at("scene_latitude_active").as_bool(),
                                  delta.at("contrast_mask_active").as_bool(),
                                  false, false);
}

}  // namespace spk
