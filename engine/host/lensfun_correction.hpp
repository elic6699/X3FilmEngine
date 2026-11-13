// lensfun_correction.hpp -- Lensfun-backed camera/lens correction for the
// generic RAW (LibRaw) decode path.
//
// The host decodes to a linear ProPhoto float frame. Lensfun only needs the
// EXIF camera/lens/focal/aperture plus that frame: stage 1 fixes vignetting
// in place, stages 2+3 remap geometry and lateral CA via a bilinear resample.
// Nothing here touches the X3F path or the SpektraLab engine session.
#pragma once

#include <filesystem>
#include <string>

#include "image_io.hpp"

namespace spkhost {

struct LensCorrectionInfo {
    bool requested = false;   // decode.lens_correction was set
    bool supported = false;   // camera + lens profile found in the database
    bool applied = false;     // pixels were actually modified
    int  flags = 0;           // LF_MODIFY_* bits really enabled
    double ms = 0.0;
    std::string camera;
    std::string lens;
    std::string focal;
    std::string reason;
};

// Returns true on success. A missing database / profile is NOT an error: it
// reports through `out.reason` and leaves the frame untouched (zero regression
// for X3F and for RAW files with no Lensfun profile). Only real failures
// return false.
bool apply_lensfun(FloatImage& frame, const Metadata& meta,
                   const std::filesystem::path& resources, bool enabled,
                   LensCorrectionInfo& out);

// Database availability, for hello/capability reporting without doing work.
bool lensfun_database_available(const std::filesystem::path& resources);

}  // namespace spkhost
