//  darktable "inpaint opposed" 高光重建（RGB 域移植）。
//
//  算法来源：X3Lumina-studio（alcedo_studio/src/decoders/processor/operators/gpu/
//  opencl_shader/highlight_reconstruct.cl 与 cuda_highlight_reconstruct.cu），
//  后者又是 darktable src/iop/hlreconstruct/opposed.c 的改写。这里保留其判据与
//  统计方式，只把数据域换成 LibRaw 的相机 RGB（色彩矩阵之前）。
//
//  原始算法 / darktable 的假设：三色滤光片对同一场景亮度响应可以互相预测，
//  因此一个被裁通道可以用"另两个通道的几何均值"（refavg）估计；再叠一个只从
//  裁剪边界外圈（未裁、且亮于 0.2*clip）统计出来的全局色度偏差，抵消白平衡
//  与光照带来的系统性偏移。
//
//  相对 darktable 原版保留的四处收紧（来自 studio 版本）：
//   - 硬阈值换成 0.95..clip 的 smoothstep 渐入，噪声不会在高光边缘抖出麻点；
//   - refavg 只累加未裁样本，整通道全裁时才退回全部样本，平顶不会被算进均值；
//   - 色度采样下界 0.2*clip，避免边界阴影侧的负离群值；
//   - 样本数门槛 30（去马赛克后的数据比 CFA 域更稀疏）。

#include "highlight_reconstruct.hpp"

#include <algorithm>
#include <cmath>
#include <thread>
#include <utility>

namespace spk::io::detail {
namespace {

constexpr float kChromaRingLo = 0.2f;
constexpr float kSoftClipLo = 0.95f;
constexpr double kMinChromaSamples = 30.0;
constexpr int kDilateRadius = 3;
constexpr std::size_t kParallelMinPixels = std::size_t(2) * 1000 * 1000;
constexpr int kMaxThreads = 8;
constexpr float kInvWhite = 1.0f / 65535.0f;

// mask 位：1=R 2=G 4=B
constexpr std::uint8_t kMaskR = 1;
constexpr std::uint8_t kMaskG = 2;
constexpr std::uint8_t kMaskB = 4;

inline float cube(const float value) { return value * value * value; }

// 传感器的读出是有限、非负的电子数；负的黑电平残差与 NaN/Inf 归 0，单个坏
// 样本既不能变成暗离群值，也不能污染邻域统计。
inline float sanitize(const float value) {
    return std::isfinite(value) ? std::max(value, 0.0f) : 0.0f;
}

inline float soft_clip_weight(const float value, const float clip) {
    const float lo = kSoftClipLo * clip;
    const float span = clip - lo;
    if (!(span > 0.0f)) return value >= clip ? 1.0f : 0.0f;
    const float t = std::min(1.0f, std::max(0.0f, (value - lo) / span));
    return t * t * (3.0f - 2.0f * t);
}

constexpr std::uint8_t channel_bit(const int channel) {
    return channel == 0 ? kMaskR : (channel == 1 ? kMaskG : kMaskB);
}

// 直接从 LibRaw 的四通道 ushort 平面读（不展开 float 副本：100 MP 的 float
// 平面就是 1.2 GB，而这里每像素只多一次乘法）。
struct CameraPlane {
    const std::uint16_t* data = nullptr;
    std::uint32_t width = 0;

    void get(const std::uint32_t x, const std::uint32_t y, float out[3]) const {
        const std::uint16_t* pixel = data + (std::size_t(y) * width + x) * 4;
        out[0] = float(pixel[0]) * kInvWhite;
        out[1] = float(pixel[1]) * kInvWhite;
        out[2] = float(pixel[2]) * kInvWhite;
    }
};

template <typename Fn>
void parallel_rows(const std::uint32_t height, const std::size_t pixels, Fn&& fn) {
    const unsigned hardware = std::thread::hardware_concurrency();
    int workers = static_cast<int>(std::min<unsigned>(hardware ? hardware : 1u, kMaxThreads));
    if (pixels < kParallelMinPixels || height < 64) workers = 1;
    if (workers <= 1) {
        fn(std::uint32_t(0), height, 0);
        return;
    }
    const std::uint32_t chunk = (height + std::uint32_t(workers) - 1) / std::uint32_t(workers);
    std::vector<std::thread> threads;
    threads.reserve(std::size_t(workers));
    for (int i = 0; i < workers; ++i) {
        const std::uint32_t begin = std::min(height, std::uint32_t(i) * chunk);
        const std::uint32_t end = std::min(height, begin + chunk);
        if (begin >= end) continue;
        threads.emplace_back([&fn, begin, end, i] { fn(begin, end, i); });
    }
    for (auto& thread : threads) thread.join();
}

// 3x3 邻域的对向通道估计：先按通道剔除被裁样本求均值，再在立方根域取"另两个
// 通道的平均"，最后三次方还原。立方根域平均等于对向通道的几何均值，贴合高光
// 衰减的非线性。
inline void refavg_at(const CameraPlane& plane, const std::uint32_t height, const int row,
                      const int col, const std::array<float, 3>& clips, float out[3]) {
    float valid_sum[3] = {0.0f, 0.0f, 0.0f};
    float valid_cnt[3] = {0.0f, 0.0f, 0.0f};
    float all_sum[3] = {0.0f, 0.0f, 0.0f};
    float all_cnt[3] = {0.0f, 0.0f, 0.0f};

    const int y0 = std::max(0, row - 1);
    const int y1 = std::min(int(height) - 1, row + 1);
    const int x0 = std::max(0, col - 1);
    const int x1 = std::min(int(plane.width) - 1, col + 1);

    for (int y = y0; y <= y1; ++y) {
        for (int x = x0; x <= x1; ++x) {
            float pixel[3];
            plane.get(std::uint32_t(x), std::uint32_t(y), pixel);
            for (int c = 0; c < 3; ++c) {
                const float value = sanitize(pixel[c]);
                all_sum[c] += value;
                all_cnt[c] += 1.0f;
                if (value < clips[std::size_t(c)]) {
                    valid_sum[c] += value;
                    valid_cnt[c] += 1.0f;
                }
            }
        }
    }

    float mean[3];
    for (int c = 0; c < 3; ++c) {
        const float m = valid_cnt[c] > 0.0f ? valid_sum[c] / valid_cnt[c]
                        : all_cnt[c] > 0.0f ? all_sum[c] / all_cnt[c]
                                            : 0.0f;
        mean[c] = std::cbrt(m);
    }
    out[0] = cube(0.5f * (mean[1] + mean[2]));
    out[1] = cube(0.5f * (mean[0] + mean[2]));
    out[2] = cube(0.5f * (mean[0] + mean[1]));
}

// 源坐标 -> 目标坐标。与 raw_headroom.cpp 的正向映射严格互逆。
inline void target_coord(const int flip, const std::uint32_t width, const std::uint32_t height,
                         const std::uint32_t x, const std::uint32_t y, std::uint32_t& out_x,
                         std::uint32_t& out_y) {
    const std::uint32_t rows = (flip & 2) ? height - 1 - y : y;
    const std::uint32_t cols = (flip & 1) ? width - 1 - x : x;
    if (flip & 4) {
        out_x = rows;
        out_y = cols;
    } else {
        out_x = cols;
        out_y = rows;
    }
}

}  // namespace

bool convert_camera_rgb_opposed(const std::uint16_t* camera, const std::size_t camera_pixels,
                                const std::uint32_t width, const std::uint32_t height,
                                const int flip, const std::array<float, 12>& matrix,
                                const double exposure_restore, const std::array<float, 3>& clips,
                                const double strength, std::vector<float>& rgb,
                                std::uint32_t& output_width, std::uint32_t& output_height,
                                ReconstructResult* result, std::string& error) {
    error.clear();
    if (!camera || width == 0 || height == 0) {
        error = "RAW reconstruct: empty camera plane";
        return false;
    }
    const std::size_t pixels = std::size_t(width) * height;
    if (camera_pixels < pixels) {
        error = "RAW reconstruct: camera plane is smaller than its declared bounds";
        return false;
    }
    if (!std::isfinite(exposure_restore) || exposure_restore <= 0.0) {
        error = "RAW reconstruct: invalid white-balance exposure restoration";
        return false;
    }
    if (!(strength > 0.0)) {
        error = "RAW reconstruct: strength must be positive on this path";
        return false;
    }
    for (int c = 0; c < 3; ++c) {
        if (!std::isfinite(matrix[std::size_t(c) * 4]) ||
            !std::isfinite(matrix[std::size_t(c) * 4 + 1]) ||
            !std::isfinite(matrix[std::size_t(c) * 4 + 2])) {
            error = "RAW reconstruct: non-finite colour matrix";
            return false;
        }
        if (!std::isfinite(clips[std::size_t(c)]) || clips[std::size_t(c)] <= 0.0f) {
            error = "RAW reconstruct: invalid per-channel clip level";
            return false;
        }
    }

    const std::uint32_t out_width = (flip & 4) ? height : width;
    const std::uint32_t out_height = (flip & 4) ? width : height;
    const float mix = float(std::min(1.0, std::max(0.0, strength)));
    const CameraPlane plane{camera, width};

    // ---- 1. 逐通道裁剪 mask ------------------------------------------------
    std::vector<std::uint8_t> mask(pixels, 0);
    std::uint64_t clipped_pixels = 0;
    for (std::uint32_t y = 0; y < height; ++y) {
        std::uint8_t* line = mask.data() + std::size_t(y) * width;
        for (std::uint32_t x = 0; x < width; ++x) {
            float pixel[3];
            plane.get(x, y, pixel);
            std::uint8_t bits = 0;
            if (pixel[0] >= clips[0]) bits |= kMaskR;
            if (pixel[1] >= clips[1]) bits |= kMaskG;
            if (pixel[2] >= clips[2]) bits |= kMaskB;
            line[x] = bits;
            if (bits) ++clipped_pixels;
        }
    }

    ReconstructResult stats;
    stats.any_clipped = clipped_pixels != 0;
    stats.clipped_pixels = clipped_pixels;

    // 没有裁剪（或只有零星裁剪）：只做颜色转换，重建整段旁路。空图不必付
    // mask 膨胀与邻域统计的代价。
    if (!stats.any_clipped) {
        rgb.assign(std::size_t(out_width) * out_height * 3, 0.0f);
        for (std::uint32_t y = 0; y < height; ++y) {
            for (std::uint32_t x = 0; x < width; ++x) {
                float pixel[3];
                plane.get(x, y, pixel);
                std::uint32_t ox = 0, oy = 0;
                target_coord(flip, width, height, x, y, ox, oy);
                float* destination = rgb.data() + (std::size_t(oy) * out_width + ox) * 3;
                for (int c = 0; c < 3; ++c) {
                    const float* coefficients = matrix.data() + std::size_t(c) * 4;
                    const float value = coefficients[0] * pixel[0] + coefficients[1] * pixel[1] +
                                        coefficients[2] * pixel[2];
                    destination[c] = float(double(value) * exposure_restore);
                }
            }
        }
        if (result) *result = stats;
        output_width = out_width;
        output_height = out_height;
        return true;
    }

    // ---- 2. 膨胀（分离式，半径 3）------------------------------------------
    // 统计与重建只关心"裁剪区外一圈"，逐像素 7x7 直接扫要 49 次读；横竖两遍
    // 各 7 次比较，代价降到 1/3.5。
    std::vector<std::uint8_t> dilated(pixels, 0);
    {
        std::vector<std::uint8_t> horizontal(pixels, 0);
        parallel_rows(height, pixels, [&](std::uint32_t row_begin, std::uint32_t row_end, int) {
            for (std::uint32_t y = row_begin; y < row_end; ++y) {
                const std::uint8_t* in = mask.data() + std::size_t(y) * width;
                std::uint8_t* out = horizontal.data() + std::size_t(y) * width;
                for (std::uint32_t x = 0; x < width; ++x) {
                    const std::uint32_t lo =
                        x >= std::uint32_t(kDilateRadius) ? x - kDilateRadius : 0;
                    const std::uint32_t hi = std::min(width - 1, x + std::uint32_t(kDilateRadius));
                    std::uint8_t bits = 0;
                    for (std::uint32_t k = lo; k <= hi; ++k) bits |= in[k];
                    out[x] = bits;
                }
            }
        });
        parallel_rows(height, pixels, [&](std::uint32_t row_begin, std::uint32_t row_end, int) {
            for (std::uint32_t y = row_begin; y < row_end; ++y) {
                const std::uint32_t lo =
                    y >= std::uint32_t(kDilateRadius) ? y - kDilateRadius : 0;
                const std::uint32_t hi = std::min(height - 1, y + std::uint32_t(kDilateRadius));
                std::uint8_t* out = dilated.data() + std::size_t(y) * width;
                for (std::uint32_t x = 0; x < width; ++x) {
                    std::uint8_t bits = 0;
                    for (std::uint32_t k = lo; k <= hi; ++k)
                        bits |= horizontal[std::size_t(k) * width + x];
                    out[x] = bits;
                }
            }
        });
    }

    // ---- 3. 全局色度偏差 ---------------------------------------------------
    struct LocalStats {
        double sums[3] = {0.0, 0.0, 0.0};
        std::uint64_t counts[3] = {0, 0, 0};
    };
    std::vector<LocalStats> locals(std::size_t(kMaxThreads) + 1);
    parallel_rows(height, pixels, [&](std::uint32_t row_begin, std::uint32_t row_end, int id) {
        LocalStats& local = locals[std::size_t(std::min(id, kMaxThreads))];
        for (std::uint32_t y = row_begin; y < row_end; ++y) {
            const std::uint8_t* ring = dilated.data() + std::size_t(y) * width;
            for (std::uint32_t x = 0; x < width; ++x) {
                const std::uint8_t bits = ring[x];
                if (!bits) continue;
                float pixel[3];
                plane.get(x, y, pixel);
                bool use[3];
                bool any = false;
                for (int c = 0; c < 3; ++c) {
                    const float clip = clips[std::size_t(c)];
                    use[c] = (bits & channel_bit(c)) != 0 && pixel[c] > kChromaRingLo * clip &&
                             pixel[c] < clip;
                    any = any || use[c];
                }
                if (!any) continue;
                float ref[3];
                refavg_at(plane, height, int(y), int(x), clips, ref);
                for (int c = 0; c < 3; ++c) {
                    if (!use[c]) continue;
                    local.sums[c] += double(pixel[c]) - double(ref[c]);
                    local.counts[c] += 1;
                }
            }
        }
    });
    for (const LocalStats& local : locals) {
        for (int c = 0; c < 3; ++c) {
            stats.chrominance[std::size_t(c)] += local.sums[c];
            stats.chroma_samples[std::size_t(c)] += local.counts[c];
        }
    }
    float chrominance[3] = {0.0f, 0.0f, 0.0f};
    for (int c = 0; c < 3; ++c) {
        const double count = double(stats.chroma_samples[std::size_t(c)]);
        chrominance[c] =
            count > kMinChromaSamples ? float(stats.chrominance[std::size_t(c)] / count) : 0.0f;
    }

    // ---- 4. 重建 + 颜色矩阵 + 方向 -----------------------------------------
    rgb.assign(std::size_t(out_width) * out_height * 3, 0.0f);
    parallel_rows(height, pixels, [&](std::uint32_t row_begin, std::uint32_t row_end, int) {
        for (std::uint32_t y = row_begin; y < row_end; ++y) {
            for (std::uint32_t x = 0; x < width; ++x) {
                float pixel[3];
                plane.get(x, y, pixel);
                bool active = false;
                for (int c = 0; c < 3; ++c) {
                    pixel[c] = sanitize(pixel[c]);
                    active = active || soft_clip_weight(pixel[c], clips[std::size_t(c)]) > 0.0f;
                }
                // 未进入渐入区（含全部未裁像素）严格保留原值：重建值常低于原来
                // 的平顶 1.0，对未裁像素套用会把正常高光压暗。
                float reconstructed[3] = {pixel[0], pixel[1], pixel[2]};
                if (active) {
                    float ref[3];
                    refavg_at(plane, height, int(y), int(x), clips, ref);
                    for (int c = 0; c < 3; ++c) {
                        const float weight =
                            soft_clip_weight(pixel[c], clips[std::size_t(c)]) * mix;
                        if (weight > 0.0f) {
                            const float estimate = std::max(pixel[c], ref[c] + chrominance[c]);
                            reconstructed[c] = pixel[c] + weight * (estimate - pixel[c]);
                        }
                    }
                }
                std::uint32_t ox = 0, oy = 0;
                target_coord(flip, width, height, x, y, ox, oy);
                float* destination = rgb.data() + (std::size_t(oy) * out_width + ox) * 3;
                for (int c = 0; c < 3; ++c) {
                    const float* coefficients = matrix.data() + std::size_t(c) * 4;
                    const float value = coefficients[0] * reconstructed[0] +
                                        coefficients[1] * reconstructed[1] +
                                        coefficients[2] * reconstructed[2];
                    destination[c] = float(double(value) * exposure_restore);
                }
            }
        }
    });

    if (result) *result = stats;
    output_width = out_width;
    output_height = out_height;
    return true;
}

}  // namespace spk::io::detail
