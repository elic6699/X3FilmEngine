// lensfun_correction.cpp -- see lensfun_correction.hpp.
#include "lensfun_correction.hpp"

/* The static Lensfun 0.3.4 build in this tree is Windows-only: the official
 * sources are compiled against third_party/lensfun_shim, a Win32 GLib layer.
 * Non-Windows host builds keep the same API but report "not supported", so
 * build-linux.sh (NATIVE_RAW + HOST) still links and generic RAW decoding
 * works without lens correction. */
#ifdef _WIN32

#include <lensfun.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <thread>
#include <vector>

#include "platform.hpp"   // path_utf8

namespace fs = std::filesystem;

namespace spkhost {
namespace {

/* 按行分块并行。lensfun 的三个 Apply* 都是 const、只读标定表，行与行之间
 * 没有共享状态；每一行只写目标缓冲属于自己的一段，所以并行结果与单线程
 * 逐位一致。42MP 全幅单线程几何重映射约 4.8 s，是通用 RAW 全幅最贵的一段。 */
template <typename F>
void parallel_rows(int rows, F&& body) {
    const unsigned hw = std::thread::hardware_concurrency();
    const unsigned threads = std::min<unsigned>(
        hw ? hw : 1, std::min<unsigned>(8, std::max(1, rows / 32)));
    if (threads <= 1) { body(0, rows); return; }
    std::vector<std::thread> pool;
    const int step = (rows + int(threads) - 1) / int(threads);
    for (unsigned t = 0; t < threads; ++t) {
        const int y0 = int(t) * step, y1 = std::min(rows, y0 + step);
        if (y0 >= y1) break;
        pool.emplace_back([&, y0, y1] { body(y0, y1); });
    }
    for (auto& th : pool) th.join();
}

struct DbState {
    std::mutex mu;
    lfDatabase* db = nullptr;
    bool tried = false;
    std::string dir;
};
DbState g_db;

bool ensure_database(const fs::path& resources, std::string& dir)
{
    std::lock_guard<std::mutex> lock(g_db.mu);
    if (g_db.tried) {
        dir = g_db.dir;
        return g_db.db != nullptr;
    }
    g_db.tried = true;

    std::vector<fs::path> candidates;
    if (const char* env = std::getenv("LENSFUN_DB_DIR")) {
        if (*env) candidates.emplace_back(fs::path(env));
    }
    candidates.push_back(resources / "lensfun" / "version_1");
    candidates.push_back(resources / "lensfun" / "db");
    candidates.push_back(resources / "lensfun");

    for (const auto& d : candidates) {
        std::error_code ec;
        if (!fs::is_directory(d, ec)) continue;
        lfDatabase* db = lfDatabase::Create();
        if (db && db->LoadDirectory(path_utf8(d).c_str())) {
            g_db.db = db;
            g_db.dir = path_utf8(d);
            dir = g_db.dir;
            return true;
        }
        if (db) db->Destroy();
    }
    return false;
}

inline float clamp_f(float v, float lo, float hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

/* Bilinear fetch of one channel from a tightly packed RGB float frame.
   Out-of-range coordinates clamp to the border (no black wedges after
   distortion correction; scale is fixed at 1.0 for a stable output size). */
inline float sample_channel(const float* src, int w, int h,
                            float sx, float sy, int channel)
{
    sx = clamp_f(sx, 0.0f, float(w - 1));
    sy = clamp_f(sy, 0.0f, float(h - 1));
    int x0 = int(std::floor(sx)), y0 = int(std::floor(sy));
    int x1 = std::min(x0 + 1, w - 1), y1 = std::min(y0 + 1, h - 1);
    float fx = sx - float(x0), fy = sy - float(y0);
    const float* a = src + (size_t(y0) * w + x0) * 3 + channel;
    const float* b = src + (size_t(y0) * w + x1) * 3 + channel;
    const float* c = src + (size_t(y1) * w + x0) * 3 + channel;
    const float* d = src + (size_t(y1) * w + x1) * 3 + channel;
    float top = *a + (*b - *a) * fx;
    float bot = *c + (*d - *c) * fx;
    return top + (bot - top) * fy;
}

std::string fmt_float(float v)
{
    char b[32];
    std::snprintf(b, sizeof(b), "%.4g", double(v));
    return b;
}

}  // namespace

bool lensfun_database_available(const fs::path& resources)
{
    std::string dir;
    return ensure_database(resources, dir);
}

bool apply_lensfun(FloatImage& frame, const Metadata& meta,
                   const fs::path& resources, bool enabled,
                   LensCorrectionInfo& out)
{
    out = LensCorrectionInfo{};
    out.requested = enabled;
    if (!enabled || frame.rgb.empty() || !frame.width || !frame.height) return true;

    const auto t0 = std::chrono::steady_clock::now();
    auto finish = [&](bool ok) {
        out.ms = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - t0).count();
        return ok;
    };

    std::string dbdir;
    if (!ensure_database(resources, dbdir)) {
        out.reason = "lensfun database not found under resources/lensfun";
        return finish(true);
    }
    if (meta.model.empty() || meta.lens.empty()) {
        out.reason = "missing camera model or lens name in EXIF";
        return finish(true);
    }

    const lfCamera** cams = g_db.db->FindCameras(
        meta.make.empty() ? nullptr : meta.make.c_str(), meta.model.c_str());
    const lfCamera* cam = (cams && cams[0]) ? cams[0] : nullptr;
    float crop = cam ? cam->CropFactor : 1.0f;
    if (crop <= 0.0f) crop = 1.0f;
    if (cam) {
        out.camera = std::string(cam->Maker ? cam->Maker : "") + " " +
                     std::string(cam->Model ? cam->Model : "");
    }

    /* Prefer a mount-aware search (uses the camera's crop/mount), then fall
       back to a loose model-only match -- third-party lenses often carry a
       maker string that differs from the EXIF one. */
    const lfLens** lenses = g_db.db->FindLenses(
        cam, meta.make.empty() ? nullptr : meta.make.c_str(),
        meta.lens.c_str(), LF_SEARCH_LOOSE);
    if (!lenses || !lenses[0]) {
        if (lenses) lf_free(lenses);
        lfLens pattern;
        pattern.SetModel(meta.lens.c_str());
        lenses = g_db.db->FindLenses(&pattern, LF_SEARCH_LOOSE);
    }
    if (!lenses || !lenses[0]) {
        out.reason = "no lensfun profile for this camera/lens";
        if (lenses) lf_free(lenses);
        if (cams) lf_free(cams);
        return finish(true);
    }

    const lfLens* lens = lenses[0];
    out.supported = true;
    out.lens = std::string(lens->Maker ? lens->Maker : "") + " " +
               std::string(lens->Model ? lens->Model : "");

    float focal = meta.focal_mm ? float(*meta.focal_mm) : lens->MinFocal;
    if (focal <= 0.0f) focal = lens->MinFocal > 0 ? lens->MinFocal : 50.0f;
    float aperture = meta.aperture ? float(*meta.aperture) : 0.0f;
    out.focal = fmt_float(focal) + "mm f/" + fmt_float(aperture);

    const int request = LF_MODIFY_DISTORTION | LF_MODIFY_TCA | LF_MODIFY_VIGNETTING;
    lfModifier mod(lens, crop, int(frame.width), int(frame.height));
    int flags = mod.Initialize(lens, LF_PF_F32, focal, aperture, 1000.0f,
                               1.0f, lens->Type, request, false);
    out.flags = flags;

    if (cams) lf_free(cams);
    if (lenses) lf_free(lenses);

    if (flags == 0) {
        out.reason = "profile has no calibration for this focal/aperture";
        return finish(true);
    }

    const int w = int(frame.width), h = int(frame.height);

    /* Stage 1: vignetting, in place, row by row (Lensfun's own examples do
       this to avoid alignment assumptions about a whole-frame buffer). */
    if (flags & LF_MODIFY_VIGNETTING) {
        const int stride = int(size_t(w) * 3 * sizeof(float));
        parallel_rows(h, [&](int y0, int y1) {
            for (int y = y0; y < y1; ++y) {
                mod.ApplyColorModification(frame.rgb.data() + size_t(y) * size_t(w) * 3,
                                           0.0f, float(y), w, 1,
                                           LF_CR_3(RED, GREEN, BLUE), stride);
            }
        });
        out.applied = true;
    }

    /* Stages 2+3 in one pass: ApplySubpixelGeometryDistortion returns three
       (x,y) pairs per target pixel (R, G, B) in source pixel coordinates. */
    std::vector<float> dst(frame.rgb.size());
    const float* src = frame.rgb.data();
    std::atomic<bool> remapped{false};
    parallel_rows(h, [&](int y0, int y1) {
        std::vector<float> coord(size_t(w) * 6);
        for (int y = y0; y < y1; ++y) {
            bool ok = mod.ApplySubpixelGeometryDistortion(0.0f, float(y), w, 1,
                                                          coord.data());
            if (!ok) {
                /* Only geometry, no TCA: fall back to the single-pair routine. */
                std::vector<float> one(size_t(w) * 2);
                if (!mod.ApplyGeometryDistortion(0.0f, float(y), w, 1, one.data()))
                    continue;
                for (int x = 0; x < w; ++x) {
                    float sx = one[size_t(x) * 2], sy = one[size_t(x) * 2 + 1];
                    float* d = dst.data() + (size_t(y) * w + x) * 3;
                    d[0] = sample_channel(src, w, h, sx, sy, 0);
                    d[1] = sample_channel(src, w, h, sx, sy, 1);
                    d[2] = sample_channel(src, w, h, sx, sy, 2);
                }
                remapped = true;
                continue;
            }
            for (int x = 0; x < w; ++x) {
                const float* c = coord.data() + size_t(x) * 6;
                float* d = dst.data() + (size_t(y) * w + x) * 3;
                d[0] = sample_channel(src, w, h, c[0], c[1], 0);
                d[1] = sample_channel(src, w, h, c[2], c[3], 1);
                d[2] = sample_channel(src, w, h, c[4], c[5], 2);
            }
            remapped = true;
        }
    });
    if (remapped) {
        frame.rgb.swap(dst);
        out.applied = true;
    }
    if (!out.applied) out.reason = "no correction callback produced output";
    return finish(true);
}

}  // namespace spkhost

#else  // !_WIN32

namespace spkhost {

bool apply_lensfun(FloatImage&, const Metadata&, const std::filesystem::path&,
                   bool enabled, LensCorrectionInfo& out)
{
    out.requested = enabled;
    out.supported = false;
    out.applied = false;
    out.reason = "lensfun correction is available in the Windows host build only";
    return true;    // not an error: the frame is simply left untouched
}

bool lensfun_database_available(const std::filesystem::path&) { return false; }

}  // namespace spkhost

#endif  // _WIN32
