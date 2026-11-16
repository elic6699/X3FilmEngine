#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace spk::io::detail {

// darktable "inpaint opposed" 高光重建（RGB 域移植，算法来自 X3Lumina-studio
// 对 darktable iop/hlreconstruct/opposed.c 的改写）。
//
// 与被移植版本的两点结构差异，是本工程的输入条件决定的：
//  1) 重建在**色彩矩阵之前**做。输入是 LibRaw 已应用白平衡的相机 RGB，此时
//     三通道还是真实的传感器对向关系；矩阵之后（ProPhoto）通道已经不是物理
//     的 R/G/B，"另两个通道的几何均值"失去意义，clip 判据也不再是 1.0。
//  2) 输出走 float、不做 CLIP。compatible16 的 uint16 通路会把重建值（常常
//     > 白点）截回 65535，等于白算；这里复刻 headroom 的浮点出口，只把被裁
//     通道换成一个可超过 1 的估计值。

struct ReconstructResult {
    bool any_clipped = false;
    std::uint64_t clipped_pixels = 0;
    // 每个通道的全局色度偏差（裁剪样本与对向通道估计之差），>=30 个样本才生效。
    std::array<double, 3> chrominance{};
    std::array<std::uint64_t, 3> chroma_samples{};
};

// darktable 的裁剪判据取 0.987 而不是 1.0：ADC 满量程附近已经进入非线性，
// 且 shot noise 会让真实饱和像素落在 1.0 之下一点。
inline constexpr float kHilightMagic = 0.987f;

// 把 LibRaw 的相机平面（w*h*4 的 ushort，WB 已应用、未 flip、未矩阵）转成
// 已 flip、已套矩阵的 float RGB。strength == 0 时与 convert_camera_rgb_headroom
// 走同一段代码，逐位一致。
//   clips[c]          该通道的饱和点，归一化到 1.0 = 白点（compatible16 三通道相同，
//                     headroom 按 pre_mul 逐通道缩放）。
//   exposure_restore  headroom 的白平衡曝光还原系数；compatible16 传 1.0。
bool convert_camera_rgb_opposed(const std::uint16_t* camera, std::size_t camera_pixels,
                                std::uint32_t width, std::uint32_t height, int flip,
                                const std::array<float, 12>& matrix, double exposure_restore,
                                const std::array<float, 3>& clips, double strength,
                                std::vector<float>& rgb, std::uint32_t& output_width,
                                std::uint32_t& output_height, ReconstructResult* result,
                                std::string& error);

}  // namespace spk::io::detail
