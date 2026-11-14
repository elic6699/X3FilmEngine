// generic_develop.hpp -- 通用相机 RAW（LibRaw 直通链）的显示域调整。
//
// 3.5.50：通用 RAW 的右栏与 X3F 共用同一批参数键，但 X3FCore 的观感链
// 吃的是 Foveon 线性帧，不能跨进程套过来。这里把「纯点式 + 两个空间
// 算子」的那部分按同一套数学实现，供 decode_raw_frame 调用：
//
//   Fill Light（X3FCore build_fill_gain 原样移植，**场景线性 sRGB 域**，空间算子）
//   → sRGB 编码
//   → 色调曲线（X3FCore x3f_tone_curve_build 生成的 257 点 LUT，逐位一致）
//   → 分色 HSL（8 通道色相/饱和度/明亮度，色相线性插值）
//   → 色彩分离（暗部/亮部保亮度注入色差）
//   → 黑白（通道混合 + 保亮度色调增益）
//   → 锐度（3x3 USM）
//
// 白平衡 RGB 三通道强度 / 归一化色温色调作用在**线性 ProPhoto 域**（曝光之后、
// sRGB 编码之前），见 apply_wb_gain()。曝光/对比度/饱和度/阴影/高光仍由
// main.cpp 的 ToneP + apply_tone_srgb() 负责，本文件不重复施加。
//
// header-only：不进 CMake 源文件表，MSVC C++17 / GCC 通用。全中性时 identity()
// 为 true，调用方直接旁路 —— 零滑杆路径逐位不变（零回归）。
#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include "json.hpp"

namespace spkhost {
namespace graw {

struct Develop {
    /* 线性域白平衡（X3F 的 p_wb_temp/p_wb_tint 是 -1..1 归一化偏移，
     * p_wb_red/green/blue 是 0..2 线性乘子，1 = 不动）。 */
    double wb_temp = 0, wb_tint = 0, wb_r = 1, wb_g = 1, wb_b = 1;

    /* 色调曲线：显示域 257 点 0..1 LUT，空 = 四滑杆全 0 = 不施加。 */
    std::vector<float> curve;

    /* 色彩分离：sh_hue, sh_sat, hl_hue, hl_sat, balance, blend(0..100) */
    std::array<double, 6> split{0, 0, 0, 0, 0, 50};
    /* 分色 HSL：8 通道 × (色相 ±180°, 饱和度 ±100, 明亮度 ±100) */
    std::array<double, 8> hsl_h{}, hsl_s{}, hsl_l{};
    /* 黑白：p_mono + 三通道混合比 + 每通道透过率（100 = 不调整） */
    bool mono = false;
    std::array<double, 3> mix{100.0 / 3, 100.0 / 3, 100.0 / 3};
    std::array<double, 3> tone{100, 100, 100};
    /* 锐度 0..1（3x3 USM 强度） */
    double sharpness = 0;
    /* Fill Light −2..+2。apply_fill_light() 在**场景线性 sRGB** 上施加，
     * 是 x3f_render.c build_fill_gain() 的逐行移植。 */
    double fill_light = 0;
};

inline void read_array(const spk::Json& j, double* out, size_t n, double fallback = 0) {
    if (!j.is_array()) return;
    const auto& a = j.items();
    for (size_t i = 0; i < n; ++i)
        out[i] = i < a.size() ? a[i].as_double(fallback) : fallback;
}

inline Develop parse(const spk::Json& j) {
    Develop d;
    if (!j.is_object()) return d;
    d.wb_temp = j.at("wb_temp").as_double(0);
    d.wb_tint = j.at("wb_tint").as_double(0);
    d.wb_r = j.at("wb_r").as_double(1);
    d.wb_g = j.at("wb_g").as_double(1);
    d.wb_b = j.at("wb_b").as_double(1);
    const spk::Json& cv = j.at("curve");
    if (cv.is_array()) {
        d.curve.reserve(cv.items().size());
        for (const auto& x : cv.items()) d.curve.push_back(float(x.as_double(0)));
    }
    read_array(j.at("split"), d.split.data(), d.split.size());
    d.split[5] = j.at("split").is_array() && j.at("split").items().size() > 5
                     ? j.at("split").items()[5].as_double(50) : 50;
    const spk::Json& hsl = j.at("hsl");
    if (hsl.is_object()) {
        read_array(hsl.at("h"), d.hsl_h.data(), 8);
        read_array(hsl.at("s"), d.hsl_s.data(), 8);
        read_array(hsl.at("l"), d.hsl_l.data(), 8);
    }
    const spk::Json& mo = j.at("mono");
    if (mo.is_object()) {
        d.mono = mo.at("on").as_double(0) > 0.5;
        read_array(mo.at("mix"), d.mix.data(), 3, 100.0 / 3);
        read_array(mo.at("tone"), d.tone.data(), 3, 100);
    }
    d.sharpness = j.at("sharpness").as_double(0);
    d.fill_light = j.at("fill_light").as_double(0);
    return d;
}

inline bool identity(const Develop& d) {
    if (d.wb_temp != 0 || d.wb_tint != 0 || d.wb_r != 1 || d.wb_g != 1 || d.wb_b != 1) return false;
    if (!d.curve.empty()) {
        /* 四滑杆全 0 时 X3FCore 生成恒等表；逐点比对，防旧调用方传恒等表。 */
        const size_t n = d.curve.size();
        for (size_t i = 0; i < n; ++i)
            if (std::fabs(d.curve[i] - double(i) / double(n - 1)) > 1e-5) return false;
    }
    if (d.split[1] != 0 || d.split[3] != 0) return false;
    for (int i = 0; i < 8; ++i)
        if (d.hsl_h[i] != 0 || d.hsl_s[i] != 0 || d.hsl_l[i] != 0) return false;
    if (d.mono || d.sharpness != 0 || d.fill_light != 0) return false;
    return true;
}

/* 与 live.js 的 wbGain() / X3FCore wb_gain_calc 同一公式。作用在线性光域。 */
inline std::array<double, 3> wb_gain(const Develop& d) {
    const double t = d.wb_temp, i = d.wb_tint;
    return {
        std::max(0.1, 1 + 0.30 * t + 0.12 * i) * std::max(0.01, d.wb_r),
        std::max(0.1, 1 - 0.15 * i) * std::max(0.01, d.wb_g),
        std::max(0.1, 1 - 0.30 * t + 0.12 * i) * std::max(0.01, d.wb_b)
    };
}

inline void apply_wb_gain(std::vector<float>& lin, const Develop& d) {
    if (d.wb_temp == 0 && d.wb_tint == 0 && d.wb_r == 1 && d.wb_g == 1 && d.wb_b == 1) return;
    const auto g = wb_gain(d);
    for (size_t p = 0; p + 2 < lin.size(); p += 3) {
        lin[p]     = float(lin[p]     * g[0]);
        lin[p + 1] = float(lin[p + 1] * g[1]);
        lin[p + 2] = float(lin[p + 2] * g[2]);
    }
}

// --- 小规模色彩 helper -------------------------------------------------------

inline void rgb_to_hsl(double r, double g, double b, double& h, double& s, double& l) {
    const double mx = std::max(r, std::max(g, b)), mn = std::min(r, std::min(g, b));
    const double c = mx - mn;
    l = (mx + mn) * 0.5;
    if (c <= 1e-9) { h = 0; s = 0; return; }
    s = c / (1.0 - std::fabs(2.0 * l - 1.0) + 1e-9);
    if (mx == r) h = std::fmod((g - b) / c, 6.0);
    else if (mx == g) h = (b - r) / c + 2.0;
    else h = (r - g) / c + 4.0;
    h *= 60.0;
    if (h < 0) h += 360.0;
}

inline double hue_to_rgb(double p, double q, double t) {
    if (t < 0) t += 1;
    if (t > 1) t -= 1;
    if (t < 1.0 / 6) return p + (q - p) * 6 * t;
    if (t < 0.5) return q;
    if (t < 2.0 / 3) return p + (q - p) * (2.0 / 3 - t) * 6;
    return p;
}

inline void hsl_to_rgb(double h, double s, double l, double& r, double& g, double& b) {
    if (s <= 1e-9) { r = g = b = l; return; }
    const double q = l < 0.5 ? l * (1 + s) : l + s - l * s;
    const double p = 2 * l - q;
    const double hk = h / 360.0;
    r = hue_to_rgb(p, q, hk + 1.0 / 3);
    g = hue_to_rgb(p, q, hk);
    b = hue_to_rgb(p, q, hk - 1.0 / 3);
}

/* 按行分块并行（std::thread，核数封顶 8）。LibRaw 解码是单线程的，但显示域
 * 这几段是纯数据并行的大循环，多核能实打实省下时间。 */
template <class F>
inline void parallel_rows(uint32_t rows, F&& body) {
    const unsigned hw = std::thread::hardware_concurrency();
    const unsigned threads = std::min<unsigned>(hw ? hw : 1, std::min<unsigned>(8, std::max<uint32_t>(1, rows / 32)));
    if (threads <= 1) { body(0u, rows); return; }
    std::vector<std::thread> pool;
    const uint32_t step = (rows + threads - 1) / threads;
    for (unsigned t = 0; t < threads; ++t) {
        const uint32_t y0 = t * step, y1 = std::min<uint32_t>(rows, y0 + step);
        if (y0 >= y1) break;
        pool.emplace_back([&, y0, y1] { body(y0, y1); });
    }
    for (auto& th : pool) th.join();
}

/* ══ Fill Light：X3FCore build_fill_gain() 的原样移植 ═════════════════════
 *
 * 对照源：outputs/X3FCore/Sources/CX3FRaw/x3f_render.c
 *   1795 apply_base_tone()      基础曲线（41 锚点，log2 −16…0，步长 0.5）
 *   7851 fill_gain_lut()        增益表 lut[n+32768] = exp(k·tanh(s·n))
 *   7916 fl_down2()             2×2 盒式降采样
 *   7955 fl_up2()               双线性上采样（像素中心对齐）
 *   7987 fl_pyramid()           多尺度低通（权重 0.9·0.65^i，meanall/lmean 归一）
 *   8107 build_fill_gain()      主算子（非 6.9 / TONEDIN 默认分支）
 *
 * 常数、钳位、取整顺序逐条照搬，**没有自创的近似**：
 *   A = 1024·ln(min(262143, base_tone(Y,0.5)·vscale) + 30)，vscale = 5300·dm^0.75（300…8000）
 *   B = fl_pyramid(A)，V = clamp(floor((A−B)²/1024), 0, 32767)，V = fl_pyramid(V)
 *   C = sqrt(V·1024 + 512²)，k0 = (meanA + 6761.44)/2 + 8，scale = 32·sqrt(meanC)
 *   map = 0.75·(k0−A) + zw·scale·(A−B)/C，zw 含 HLZRED=0.95 的亮部回拉（F>0）
 *   gain = lut[trunc(map) + 32768]，再双线性放大到全幅
 *   乘在**场景线性 sRGB** 三通道，>1 时按 SATLIFT=0.015 反向补回色差
 *
 * 只删掉了标定用的环境变量开关（X3F_FL_*）与 6.9 定点分支——它们默认值就是
 * 这里写死的常量，默认路径不受影响。工作分辨率固定 2712 长边（CANON），
 * 保证预览小图与全幅导出的提亮量一致（原版为此专门改过）。
 */

inline double fl_base_tone(double lin, double hl_keep) {
    static const double kCurve[41] = {
        0.000009, 0.000013, 0.000018, 0.000025,
        0.000035, 0.000050, 0.000071, 0.000100,
        0.000142, 0.000201, 0.000284, 0.000402,
        0.000568, 0.000803, 0.001136, 0.001624,
        0.001788, 0.002502, 0.003282, 0.004635,
        0.007449, 0.013139, 0.026769, 0.057818,
        0.092199, 0.163640, 0.256734, 0.347419,
        0.437330, 0.538847, 0.650903, 0.769143,
        0.986740, 0.990624, 0.993370, 0.995312,
        0.996685, 0.997656, 0.998343, 0.998828,
        0.999171
    };
    if (lin <= 0.0) return 0.0;
    const double e = (std::log2(lin) + 16.0) / 0.5;
    if (e <= 0.0) return 0.590 * lin;
    if (e >= 40.0) return kCurve[40];
    const int i = int(e);
    const double f = e - i;
    double v = kCurve[i] + (kCurve[i + 1] - kCurve[i]) * f;
    if (hl_keep > 0.0) {
        const int kTop = 8;
        const int i0 = 40 - kTop;
        if (i >= i0) {
            double t = (i - i0 + f) / double(kTop);
            if (t < 0.0) t = 0.0;
            if (t > 1.0) t = 1.0;
            const double lo = kCurve[i0] * (1.0 - hl_keep) + 0.90 * hl_keep;
            v = lo + (1.0 - lo) * t;
        }
    }
    return v;
}

/* 2×2 盒式降采样，越界钳到边缘像素（对应 fl_down2） */
inline void fl_down2(const std::vector<float>& s, int sw, int sh,
                     std::vector<float>& d, int dw, int dh, double* mean_out) {
    d.assign(size_t(dw) * dh, 0.0f);
    double acc = 0.0;
    for (int y = 0; y < dh; ++y) {
        int y0 = 2 * y, y1 = y0 + 1;
        if (y1 >= sh) y1 = sh - 1;
        for (int x = 0; x < dw; ++x) {
            int x0 = 2 * x, x1 = x0 + 1;
            if (x1 >= sw) x1 = sw - 1;
            const double v = 0.25 * (double(s[size_t(y0) * sw + x0]) + double(s[size_t(y0) * sw + x1]) +
                                     double(s[size_t(y1) * sw + x0]) + double(s[size_t(y1) * sw + x1]));
            d[size_t(y) * dw + x] = float(v);
            acc += v;
        }
    }
    if (mean_out) *mean_out = (double(dw) * dh > 0.0) ? acc / (double(dw) * dh) : 0.0;
}

/* 2× 上采样（双线性、像素中心对齐，对应 fl_up2） */
inline void fl_up2(const std::vector<float>& s, int sw, int sh,
                   std::vector<float>& d, int dw, int dh) {
    d.assign(size_t(dw) * dh, 0.0f);
    if (sw < 2 || sh < 2) {                       /* 退化：直接复制 */
        std::fill(d.begin(), d.end(), s.empty() ? 0.0f : s[0]);
        return;
    }
    for (int y = 0; y < dh; ++y) {
        const double fy = ((double)y + 0.5) * sh / dh - 0.5;
        int y0 = int(std::floor(fy));
        double ty = fy - y0;
        if (y0 < 0) { y0 = 0; ty = 0.0; }
        if (y0 > sh - 2) { y0 = sh - 2; if (ty > 1.0) ty = 1.0; }
        for (int x = 0; x < dw; ++x) {
            const double fx = ((double)x + 0.5) * sw / dw - 0.5;
            int x0 = int(std::floor(fx));
            double tx = fx - x0;
            if (x0 < 0) { x0 = 0; tx = 0.0; }
            if (x0 > sw - 2) { x0 = sw - 2; if (tx > 1.0) tx = 1.0; }
            const double a = s[size_t(y0) * sw + x0];
            const double b = s[size_t(y0) * sw + x0 + 1];
            const double c = s[size_t(y0 + 1) * sw + x0];
            const double e = s[size_t(y0 + 1) * sw + x0 + 1];
            d[size_t(y) * dw + x] = float((a + (b - a) * tx) * (1.0 - ty) +
                                          (c + (e - c) * tx) * ty);
        }
    }
}

/* 多尺度低通（对应 fl_pyramid；out 可与 src 同址，逐元素写回是安全的） */
inline bool fl_pyramid(const std::vector<float>& src, int w, int h, std::vector<float>& out) {
    if (w < 2 || h < 2) return false;
    const size_t npix = size_t(w) * h;
    if (src.size() < npix) return false;

    std::vector<std::vector<float>> lev;
    lev.reserve(32);                     /* 必须预留：下面存的是 &lev.back() */
    int lw[32], lh[32], m = 0;
    double lmean[32] = {0};
    {
        int cw = w, ch = h;
        const std::vector<float>* pin = &src;
        while (m < 32) {
            const int dw = (cw + 1) / 2, dh = (ch + 1) / 2;
            lev.emplace_back();
            fl_down2(*pin, cw, ch, lev.back(), dw, dh, &lmean[m]);
            lw[m] = dw; lh[m] = dh; m++;
            if (!(cw >= 3 && dh > 2)) break;
            pin = &lev.back();
            cw = dw; ch = dh;
        }
    }

    double meanall = 0.0;
    for (size_t j = 0; j < npix; ++j) meanall += src[j];
    meanall /= double(npix);

    std::vector<float> up;
    const std::vector<float>* cur = &lev[m - 1];
    for (int i = 0; i < m; ++i) {
        const int base = m - 1 - i;
        const double wgt = 0.9 * std::pow(0.65, double(i));
        if (base > 0) {
            const int dw = lw[base - 1], dh = lh[base - 1];
            fl_up2(*cur, lw[base], lh[base], up, dw, dh);
            std::vector<float>& dstl = lev[base - 1];
            const double mn = lmean[base - 1] > 1e-9 ? meanall / lmean[base - 1] : 1.0;
            for (size_t j = 0; j < dstl.size(); ++j)
                dstl[j] = float((1.0 - wgt) * up[j] + wgt * mn * double(dstl[j]));
            cur = &dstl;
        } else {
            fl_up2(*cur, lw[0], lh[0], up, w, h);
            /* 注意：调用方可能是同址调用（fl_pyramid(V,...,V)），所以先算到
             * 临时缓冲再换出来 —— 直接 assign 会把源数据清掉。 */
            std::vector<float> res(npix);
            for (size_t j = 0; j < npix; ++j)
                res[j] = float((1.0 - wgt) * up[j] + wgt * double(src[j]));
            out.swap(res);
        }
    }
    return true;
}

/* 与 F 无关的基座：A（对数亮度）、B（低通）、C（局部标准差）+ 锚点/尺度。
 * X3FCore 的 apply 缓存缓存的正是这三张图（g_fl_slot_a/b/c），拖 Fill Light
 * 时只重做「查表 + 双线性放大」，不必重跑金字塔。 */
struct FillBase {
    int gw = 0, gh = 0;
    std::vector<float> A, B, C;
    double k0 = 0.0, scale = 0.0;
    bool valid() const { return gw >= 4 && gh >= 4 && A.size() == size_t(gw) * gh; }
};

/* 建基座。lin = 场景线性 sRGB（3 通道交错，可 >1）。 */
inline bool fill_base_build(const float* lin, int w, int h, FillBase& base) {
    base = FillBase();
    if (lin == nullptr || w < 4 || h < 4) return false;

    const double zred    = 0.95;    /* X3F_FL_HLZRED  */
    const double logadd  = 30.0;    /* X3F_FL_LOGADD  */
    const double cfloor  = 512.0;   /* X3F_FL_CFLOOR  */
    const double canon   = 2712.0;  /* X3F_FL_CANON：固定工作分辨率 */

    /* 工作分辨率：canon 长边 + sd 整除量化（与 build_fill_gain 同算法） */
    const double amax = double(w > h ? w : h);
    int cw, ch, sd;
    {
        const double k = canon / amax;
        cw = int(double(w) * k + 0.5);
        ch = int(double(h) * k + 0.5);
        sd = int(amax / canon + 0.5);
        if (sd < 1) sd = 1;
        if (w / sd < 8 || h / sd < 8) sd = 1;
        if (cw < 4) cw = 4;
        if (ch < 4) ch = 4;
    }
    const int sw = (w + sd - 1) / sd, sh = (h + sd - 1) / sd;
    const int gw = cw, gh = ch;
    if (gw < 4 || gh < 4) return false;
    const size_t np = size_t(gw) * gh;
    (void)zred;

    base.gw = gw;
    base.gh = gh;
    base.A.assign(np, 0.0f);
    base.B.assign(np, 0.0f);
    base.C.assign(np, 0.0f);
    std::vector<float>& A = base.A;
    std::vector<float>& B = base.B;
    std::vector<float>& C = base.C;

    /* 1a) 半分辨率线性亮度（0.25/0.65/0.10，sd×sd 盒式均值） */
    {
        std::vector<float> S(size_t(sw) * sh, 0.0f);
        for (int y = 0; y < sh; ++y) {
            const int y0 = y * sd;
            const int y1 = std::min(h, y0 + sd);
            for (int x = 0; x < sw; ++x) {
                const int x0 = x * sd;
                const int x1 = std::min(w, x0 + sd);
                double s = 0.0;
                int cnt = 0;
                for (int yy = y0; yy < y1; ++yy) {
                    const float* p = lin + (size_t(yy) * w + x0) * 3;
                    for (int xx = x0; xx < x1; ++xx, p += 3)
                        s += 0.25 * p[0] + 0.65 * p[1] + 0.10 * p[2];
                    cnt += (x1 - x0);
                }
                S[size_t(y) * sw + x] = float(cnt ? s / cnt : 0.0);
            }
        }
        if (sw == cw && sh == ch) {
            std::copy(S.begin(), S.begin() + long(np), A.begin());
        } else {
            std::vector<float> up;
            fl_up2(S, sw, sh, up, cw, ch);
            A = up;
        }
    }

    /* 1b) TONEDIN：先把亮度折到显示域（基础曲线 hl_keep=0.5），再由均亮定 vscale */
    double dm = 0.0;
    for (size_t i = 0; i < np; ++i) {
        A[i] = float(fl_base_tone(A[i] > 0.0f ? double(A[i]) : 0.0, 0.5));
        dm += A[i];
    }
    dm = np ? dm / double(np) : 1e-4;
    if (!(dm > 1e-9)) dm = 1e-9;
    double vscale = 5300.0 * std::pow(dm, 0.75);
    if (vscale < 300.0) vscale = 300.0;
    if (vscale > 8000.0) vscale = 8000.0;

    /* 1c) 亮度 → 对数压缩 */
    for (size_t i = 0; i < np; ++i) {
        double v = double(A[i]) * vscale;
        if (!(v > 0.0)) v = 0.0;
        if (v > 262143.0) v = 262143.0;
        A[i] = float(1024.0 * std::log(v + logadd));
    }

    /* 2) B = 低通(A)（lmean=0 → 金字塔） */
    if (!fl_pyramid(A, gw, gh, B)) return false;
    double meanA = 0.0;
    for (size_t i = 0; i < np; ++i) meanA += A[i];
    meanA /= double(np);

    /* 3) 局部方差 */
    std::vector<float> V(np, 0.0f);
    for (size_t i = 0; i < np; ++i) {
        const double d = double(A[i]) - double(B[i]);
        double v = std::floor(d * d / 1024.0);
        if (v < 0.0) v = 0.0;
        if (v > 32767.0) v = 32767.0;
        V[i] = float(v);
    }
    if (!fl_pyramid(V, gw, gh, V)) return false;

    /* 4) 方差 → 标准差（含 512 底噪） */
    double meanC = 0.0;
    for (size_t i = 0; i < np; ++i) {
        const double v = V[i] > 0.0f ? double(V[i]) : 0.0;
        C[i] = float(std::sqrt(v * 1024.0 + cfloor * cfloor));
        meanC += C[i];
    }
    meanC /= double(np);
    base.scale = 32.0 * std::sqrt(meanC);
    base.k0 = (meanA + 6761.44) / 2.0 + 8.0;
    return true;
}

/* 由基座 + F 生成全幅增益图（查表 + 双线性放大，与 build_fill_gain 第 5 步同）。 */
inline std::vector<float> fill_gain_from_base(const FillBase& base, int w, int h, double F) {
    std::vector<float> gain;
    if (!base.valid() || w < 4 || h < 4) return gain;

    const double zred    = 0.95;
    const double mapgain = 0.75;
    const double zgain   = 1.0;
    const int gw = base.gw, gh = base.gh;
    const std::vector<float>& A = base.A;
    const std::vector<float>& B = base.B;
    const std::vector<float>& C = base.C;
    const double k0 = base.k0, scale = base.scale;

    std::vector<float> lut(65536);
    {
        const double kk = 1.33 * F;
        const double ss = 1.0 / (1024.0 * std::pow(std::fabs(kk) + 0.0625, 0.25));
        for (int n = -32768; n < 32768; ++n)
            lut[size_t(n + 32768)] = float(std::exp(kk * std::tanh(ss * double(n))));
    }

    gain.assign(size_t(w) * h, 1.0f);
    const double sx = double(gw) / double(w), sy = double(gh) / double(h);
    const double fs = F < 2.0 ? F * 0.5 : 1.0;
    parallel_rows(uint32_t(h), [&](uint32_t yy0, uint32_t yy1) {
        for (uint32_t y = yy0; y < yy1; ++y) {
            const double fy = (double(y) + 0.5) * sy - 0.5;
            int y0 = int(std::floor(fy));
            double ty = fy - y0;
            if (y0 < 0) { y0 = 0; ty = 0.0; }
            if (y0 > gh - 2) { y0 = gh - 2; if (ty > 1.0) ty = 1.0; }
            for (uint32_t x = 0; x < uint32_t(w); ++x) {
                const double fx = (double(x) + 0.5) * sx - 0.5;
                int x0 = int(std::floor(fx));
                double tx = fx - x0;
                if (x0 < 0) { x0 = 0; tx = 0.0; }
                if (x0 > gw - 2) { x0 = gw - 2; if (tx > 1.0) tx = 1.0; }
                const size_t j0 = size_t(y0) * gw + x0, j1 = j0 + gw;
                const double a = (double(A[j0]) * (1 - tx) + double(A[j0 + 1]) * tx) * (1 - ty) +
                                 (double(A[j1]) * (1 - tx) + double(A[j1 + 1]) * tx) * ty;
                const double b = (double(B[j0]) * (1 - tx) + double(B[j0 + 1]) * tx) * (1 - ty) +
                                 (double(B[j1]) * (1 - tx) + double(B[j1 + 1]) * tx) * ty;
                const double c = (double(C[j0]) * (1 - tx) + double(C[j0 + 1]) * tx) * (1 - ty) +
                                 (double(C[j1]) * (1 - tx) + double(C[j1 + 1]) * tx) * ty;

                double zw = zgain;
                if (zred > 0.0 && F > 0.0) {
                    /* 亮部局部项回拉：起点 k0、宽 1200（X3F_FL_HLSTART/HLWIDTH 默认） */
                    double t = (a - k0) / 1200.0;
                    if (t < 0.0) t = 0.0;
                    if (t > 1.0) t = 1.0;
                    t = t * t * (3.0 - 2.0 * t);
                    zw *= 1.0 - zred * fs * t;
                }
                double m = mapgain * (k0 - a) + zw * scale * (a - b) / (c > 1e-6 ? c : 1e-6);
                if (!(m == m)) m = 0.0;               /* NaN → 不改变 */
                int i0 = int(m);                      /* 截断（fcvtzs） */
                if (i0 < -32768) i0 = -32768;
                if (i0 > 32767) i0 = 32767;
                gain[size_t(y) * w + x] = lut[size_t(i0 + 32768)];
            }
        }
    });
    return gain;
}

/* 一步到位：建基座 + 出图（不看缓存的调用方用这个）。 */
inline std::vector<float> fill_gain_map(const float* lin, int w, int h, double F) {
    std::vector<float> gain;
    if (F == 0.0) return gain;
    FillBase base;
    if (!fill_base_build(lin, w, h, base)) return gain;
    return fill_gain_from_base(base, w, h, F);
}

/* 把已算好的增益图乘到线性 sRGB 上：三通道同乘，>1 时按 SATLIFT 补色差。
 * 与 X3FCore apply_row() 的「WB/曝光增益 → 填充光」顺序一致。
 * 增益图本身要**在未曝光/未白平衡的线性帧上算**（原版 build_fill_gain(src8)），
 * 否则曝光滑杆会连带改变填充光的提亮量。 */
inline void apply_fill_gain(std::vector<float>& lin, uint32_t w, uint32_t h,
                            const std::vector<float>& gain) {
    if (w < 4 || h < 4) return;
    if (gain.size() != size_t(w) * h || lin.size() < size_t(w) * h * 3) return;
    const double alpha = 0.015;   /* X3F_FL_SATLIFT 默认 */
    parallel_rows(h, [&](uint32_t y0, uint32_t y1) {
        for (uint32_t y = y0; y < y1; ++y) {
            for (uint32_t x = 0; x < w; ++x) {
                const size_t i = (size_t(y) * w + x) * 3;
                const double fg = double(gain[size_t(y) * w + x]);
                double lv[3] = { double(lin[i]) * fg, double(lin[i + 1]) * fg, double(lin[i + 2]) * fg };
                if (fg > 1.0) {
                    const double y7 = 0.2126 * lv[0] + 0.7152 * lv[1] + 0.0722 * lv[2];
                    const double kk = 1.0 + alpha * (fg - 1.0);
                    for (int c = 0; c < 3; ++c) lv[c] = y7 + (lv[c] - y7) * kk;
                }
                for (int c = 0; c < 3; ++c)
                    lin[i + c] = float(lv[c] > 0.0 ? lv[c] : 0.0);
            }
        }
    });
}

/* 一步到位版本（算图 + 施加，同一张图）。 */
inline void apply_fill_light(std::vector<float>& lin, uint32_t w, uint32_t h, double F) {
    if (F == 0 || w < 4 || h < 4 || lin.size() < size_t(w) * h * 3) return;
    const std::vector<float> gain = fill_gain_map(lin.data(), int(w), int(h), F);
    apply_fill_gain(lin, w, h, gain);
}

/* sRGB 浮点（可 >1，未夹断）上施加 曲线 → HSL → 色彩分离 → 黑白 → 锐度。
 * 顺序与 X3F 显示域链同口径；全中性时调用方应直接旁路。 */
inline void apply_display(std::vector<float>& s, uint32_t w, uint32_t h, const Develop& d) {
    if (!w || !h || s.size() < size_t(w) * h * 3) return;

    bool hsl_on = false;
    for (int i = 0; i < 8; ++i)
        if (d.hsl_h[i] || d.hsl_s[i] || d.hsl_l[i]) { hsl_on = true; break; }
    const bool split_on = d.split[1] != 0 || d.split[3] != 0;
    const bool curve_on = !d.curve.empty();
    const double splitCut = 0.5 + d.split[4] / 200.0;
    const double splitWidth = std::max(0.02, d.split[5] / 100.0);

    auto clamp01 = [](double v) { return v < 0 ? 0.0 : (v > 1 ? 1.0 : v); };

    parallel_rows(h, [&](uint32_t y0, uint32_t y1) {
        for (uint32_t y = y0; y < y1; ++y) {
            for (uint32_t x = 0; x < w; ++x) {
                const size_t i = (size_t(y) * w + x) * 3;
                double r = s[i], g = s[i + 1], b = s[i + 2];

                /* 1) 色调曲线（显示域逐通道 LUT，线性插值） */
                if (curve_on) {
                    const size_t n = d.curve.size();
                    auto lut = [&](double v) {
                        const double t = clamp01(v) * (n - 1);
                        const size_t k = size_t(t);
                        const double f = t - k;
                        return double(d.curve[k]) * (1 - f) +
                               double(d.curve[std::min(k + 1, n - 1)]) * f;
                    };
                    r = lut(r); g = lut(g); b = lut(b);
                }

                /* 2) 分色 HSL：色相按 45° 段线性插值到相邻两通道 */
                if (hsl_on) {
                    double hh, ss, ll;
                    rgb_to_hsl(r, g, b, hh, ss, ll);
                    const double pos = std::fmod(hh + 360.0, 360.0) / 45.0;
                    const int c0 = int(pos) % 8, c1 = (c0 + 1) % 8;
                    const double f = pos - std::floor(pos);
                    const double dh = d.hsl_h[c0] * (1 - f) + d.hsl_h[c1] * f;
                    const double ds = (d.hsl_s[c0] * (1 - f) + d.hsl_s[c1] * f) / 100.0;
                    const double dl = (d.hsl_l[c0] * (1 - f) + d.hsl_l[c1] * f) / 100.0;
                    if (ss > 1e-6 && (dh != 0 || ds != 0 || dl != 0)) {
                        hsl_to_rgb(std::fmod(hh + dh + 360.0, 360.0),
                                   clamp01(ss * (1 + ds)), clamp01(ll * (1 + dl)), r, g, b);
                    }
                }

                /* 3) 色彩分离：保亮度的暗部/亮部色差注入 */
                if (split_on) {
                    const double lum = 0.299 * r + 0.587 * g + 0.114 * b;
                    double wSh = (splitCut + splitWidth - lum) / (2 * splitWidth);
                    wSh = clamp01(wSh);
                    const double wHl = 1.0 - wSh;
                    for (int z = 0; z < 2; ++z) {
                        const double strength = (z ? d.split[3] : d.split[1]) / 100.0 *
                                                (z ? wHl : wSh) * 0.35;
                        if (strength == 0) continue;
                        const double hue = z ? d.split[2] : d.split[0];
                        double tr, tg, tb;
                        hsl_to_rgb(std::fmod(hue + 360.0, 360.0), 1.0, 0.5, tr, tg, tb);
                        const double mean = (tr + tg + tb) / 3.0;
                        r += (tr - mean) * strength;
                        g += (tg - mean) * strength;
                        b += (tb - mean) * strength;
                    }
                }

                /* 4) 黑白：归一化加权混合 + 保亮度色调增益 */
                if (d.mono) {
                    const double sum = std::max(1e-6, d.mix[0] + d.mix[1] + d.mix[2]);
                    const double gray = (d.mix[0] * r + d.mix[1] * g + d.mix[2] * b) / sum;
                    const double den = std::max(1e-6,
                        0.30 * d.tone[0] + 0.59 * d.tone[1] + 0.11 * d.tone[2]);
                    r = gray * d.tone[0] / den;
                    g = gray * d.tone[1] / den;
                    b = gray * d.tone[2] / den;
                }

                s[i]     = float(clamp01(r));
                s[i + 1] = float(clamp01(g));
                s[i + 2] = float(clamp01(b));
            }
        }
    });

    /* 5) 锐度：3x3 USM（先算 4 邻均值，再按 amount 加回高频）。单独一遍是因为
     * 空间算子要读邻域，不能与上一段逐像素循环就地混在一起。 */
    if (d.sharpness > 0) {
        std::vector<float> src = s;
        const double amount = std::min(1.0, d.sharpness) * 0.9;
        parallel_rows(h, [&](uint32_t y0, uint32_t y1) {
            for (uint32_t y = y0; y < y1; ++y) {
                for (uint32_t x = 0; x < w; ++x) {
                    const size_t i = (size_t(y) * w + x) * 3;
                    for (int c = 0; c < 3; ++c) {
                        const uint32_t xl = x ? x - 1 : 0, xr = std::min(x + 1, w - 1);
                        const uint32_t yu = y ? y - 1 : 0, yd = std::min(y + 1, h - 1);
                        const double blur = 0.25 * (
                            src[(size_t(y) * w + xl) * 3 + c] + src[(size_t(y) * w + xr) * 3 + c] +
                            src[(size_t(yu) * w + x) * 3 + c] + src[(size_t(yd) * w + x) * 3 + c]);
                        s[i + c] = float(clamp01(src[i + c] + amount * (src[i + c] - blur)));
                    }
                }
            }
        });
    }
}

}  // namespace graw
}  // namespace spkhost
