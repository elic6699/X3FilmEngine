// 明度层 finish（通用相机 RAW 的 LibRaw 直通链）
//
// 与外壳 X3L_YLayerFinishRGB24（src/export.cpp，照 macOS RawRender.finishYPlane
// 逐行移植）同一套算法：把 8-bit 出图按 JPEG 口径的 YCbCr 分开，只对 Y（黑白
// 明度）层做锐化 / 颗粒，色度原样合回 —— 提细节不发彩。
//
// 为什么放在引擎而不是外壳：通用 RAW 的 JPEG 是在引擎里编码的（EXIF / ICC 都在
// 这一步写），外壳拿到的只是编码后的字节流。要让这道工序和导出的 EXIF / 编码
// 一次成型，只能插在编码之前。
//
// 只在 8-bit 输出（jpeg / rgba8）上跑；tiff16 是给后期的母版，dng 是线性 RAW，
// 与 X3F 侧「只有 JPEG 档吃 finish」同一口径。全关时调用方整段跳过（零回归）。

#ifndef SPKHOST_LUMA_FINISH_HPP
#define SPKHOST_LUMA_FINISH_HPP

#include <cmath>
#include <cstdint>
#include <vector>

namespace spkhost {
namespace graw {

/* 明度层 finish 参数（develop 对象里的同名键；缺键 = 关）。 */
struct LumaFinish {
    bool   on = false;
    double sharpen = 0.0;      // 0..200（%）
    double grain_size = 3.0;   // 1..8（px）
    double grain_rough = 0.0;  // 0..100（%）
    double strength = 100.0;   // 0..100（明度层占比）
    double scale = 1.0;        // 预览缩放：锐化半径 / 颗粒间距按它缩（导出恒 1）
    bool active() const {
        return on && (sharpen > 0.0 || grain_rough > 0.0);
    }
};

/* 盒式低通（平面），边缘夹住 —— 与 Mac boxBlurPlanar / 外壳 YBoxBlurPlanar 同一实现。 */
inline void box_blur_planar(const std::vector<float>& src, int w, int h, int radius,
                            std::vector<float>* dst) {
    const float denom = float(2 * radius + 1);
    std::vector<float> tmp(size_t(w) * size_t(h));
    for (int y = 0; y < h; y++) {
        const float* row = &src[size_t(y) * size_t(w)];
        float* trow = &tmp[size_t(y) * size_t(w)];
        float sum = 0;
        for (int dx = -radius; dx <= radius; dx++)
            sum += row[dx < 0 ? 0 : (dx > w - 1 ? w - 1 : dx)];
        for (int x = 0; x < w; x++) {
            trow[x] = sum / denom;
            int a = x + radius + 1; if (a < 0) a = 0; if (a > w - 1) a = w - 1;
            int b = x - radius;     if (b < 0) b = 0; if (b > w - 1) b = w - 1;
            sum += row[a] - row[b];
        }
    }
    dst->assign(size_t(w) * size_t(h), 0.0f);
    for (int x = 0; x < w; x++) {
        float sum = 0;
        for (int dy = -radius; dy <= radius; dy++) {
            int yy = dy < 0 ? 0 : (dy > h - 1 ? h - 1 : dy);
            sum += tmp[size_t(yy) * size_t(w) + x];
        }
        for (int y = 0; y < h; y++) {
            (*dst)[size_t(y) * size_t(w) + x] = sum / denom;
            int a = y + radius + 1; if (a < 0) a = 0; if (a > h - 1) a = h - 1;
            int b = y - radius;     if (b < 0) b = 0; if (b > h - 1) b = h - 1;
            sum += tmp[size_t(a) * size_t(w) + x] - tmp[size_t(b) * size_t(w) + x];
        }
    }
}

/* YCbCr → RGB 写回一个像素（色度取自原像素 —— 「只动黑白层」的关键）。 */
inline void put_ycbcr(float yy, float r0, float g0, float b0, uint8_t* px) {
    float cb = -0.168736f * r0 - 0.331264f * g0 + 0.5f * b0 + 128.0f;
    float cr = 0.5f * r0 - 0.418688f * g0 - 0.081312f * b0 + 128.0f;
    float rr = yy + 1.402f * (cr - 128.0f);
    float gg = yy - 0.344136f * (cb - 128.0f) - 0.714136f * (cr - 128.0f);
    float bb = yy + 1.772f * (cb - 128.0f);
    rr = rr < 0 ? 0 : (rr > 255 ? 255 : rr);
    gg = gg < 0 ? 0 : (gg > 255 ? 255 : gg);
    bb = bb < 0 ? 0 : (bb > 255 ? 255 : bb);
    px[0] = uint8_t(rr + 0.5f);
    px[1] = uint8_t(gg + 0.5f);
    px[2] = uint8_t(bb + 0.5f);
}

/* 就地对 8-bit 像素做明度层 finish。
 * channels = 3（RGB24 / JPEG 编码前）或 4（RGBA8 预览，alpha 原样保留）。
 * 非 3/4 通道直接返回（DNG / tiff16 不走这里）。 */
inline void apply_luma_finish(uint8_t* px, int w, int h, int channels,
                              const LumaFinish& f) {
    if (!px || w <= 0 || h <= 0) return;
    if (channels != 3 && channels != 4) return;
    if (!f.active()) return;
    const size_t n = size_t(w) * size_t(h);
    const size_t stride = size_t(channels);
    const float s = float(std::min(std::max(f.strength, 0.0), 100.0) / 100.0);

    /* JPEG 口径的 YCbCr（与 SPP Rec.601 同族），Y 先抽出来 */
    std::vector<float> y(n);
    for (size_t i = 0; i < n; i++) {
        const uint8_t* p = px + i * stride;
        y[i] = 0.299f * p[0] + 0.587f * p[1] + 0.114f * p[2];
    }
    std::vector<float> y0;
    if (s < 1.0f) y0 = y;

    /* 锐化：SPP 黑白加性核（Y += 0.25·D·Δ，D = Y − blur(Y)），盒式低通 ×2。 */
    if (f.sharpen > 0) {
        int r = int(2.0 * std::max(0.25, std::min(f.scale, 4.0)) + 0.5);
        if (r < 1) r = 1;
        std::vector<float> b1, b2;
        box_blur_planar(y, w, h, r, &b1);
        box_blur_planar(b1, w, h, r, &b2);
        const float delta = float(std::min(std::max(f.sharpen, 0.0), 300.0) / 100.0 * 12.0);
        for (size_t i = 0; i < n; i++) {
            float v = y[i] + 0.25f * (y[i] - b2[i]) * delta;
            if (v < 0) v = 0; else if (v > 255) v = 255;
            y[i] = v;
        }
    }

    /* 颗粒：粗网格值噪声（平滑插值 → 团块大小 = 网格间距），只加在明度上。
     * 固定种子 0：同参数逐位可复现（与 Mac / 外壳同一张噪声网）。 */
    if (f.grain_rough > 0) {
        const double cell = std::max(1.0, f.grain_size * std::max(0.25, std::min(f.scale, 4.0)));
        const float amp = float(std::min(std::max(f.grain_rough, 0.0), 100.0) / 100.0) * 20.0f;
        const int nx = int(w / cell) + 2;
        const int ny = int(h / cell) + 2;
        std::vector<float> lat(size_t(nx) * size_t(ny));
        const uint64_t seed = 0;
        for (int j = 0; j < ny; j++) {
            for (int ii = 0; ii < nx; ii++) {
                uint64_t hh = seed * 0x9E3779B97F4A7C15ULL
                    + uint64_t(int64_t(ii)) * 0xC2B2AE3D27D4EB4FULL
                    + uint64_t(int64_t(j)) * 0x165667B19E3779F9ULL;
                hh ^= hh >> 31; hh = hh * 0x9E3779B97F4A7C15ULL; hh ^= hh >> 27;
                lat[size_t(j) * size_t(nx) + size_t(ii)] = float(hh % 4096) / 2047.5f - 1.0f;
            }
        }
        for (int yy = 0; yy < h; yy++) {
            const double gy = double(yy) / cell;
            int j0 = int(gy); if (j0 < 0) j0 = 0; if (j0 > ny - 2) j0 = ny - 2;
            const float fy = float(gy - double(j0));
            const float sy = fy * fy * (3.0f - 2.0f * fy);
            const size_t rowA = size_t(j0) * size_t(nx), rowB = size_t(j0 + 1) * size_t(nx);
            for (int xx = 0; xx < w; xx++) {
                const double gx = double(xx) / cell;
                int i0 = int(gx); if (i0 < 0) i0 = 0; if (i0 > nx - 2) i0 = nx - 2;
                const float fx = float(gx - double(i0));
                const float sx = fx * fx * (3.0f - 2.0f * fx);
                const float a = lat[rowA + size_t(i0)] +
                                (lat[rowA + size_t(i0 + 1)] - lat[rowA + size_t(i0)]) * sx;
                const float b = lat[rowB + size_t(i0)] +
                                (lat[rowB + size_t(i0 + 1)] - lat[rowB + size_t(i0)]) * sx;
                const float nn = a + (b - a) * sy;
                float v = y[size_t(yy) * size_t(w) + size_t(xx)] + amp * nn;
                if (v < 0) v = 0; else if (v > 255) v = 255;
                y[size_t(yy) * size_t(w) + size_t(xx)] = v;
            }
        }
    }

    /* 明度层强度混合（放最后：对合成结果整体混，与外壳 / Mac 同序） */
    if (!y0.empty()) {
        for (size_t i = 0; i < n; i++) y[i] = y0[i] + (y[i] - y0[i]) * s;
    }

    /* 原色度 Cb/Cr 原样合回 */
    for (size_t i = 0; i < n; i++) {
        uint8_t* p = px + i * stride;
        put_ycbcr(y[i], float(p[0]), float(p[1]), float(p[2]), p);
    }
}

}  // namespace graw
}  // namespace spkhost

#endif  // SPKHOST_LUMA_FINISH_HPP
