// output.hpp -- leaving the working space, and writing files.
//
// The engine renders encoded ProPhoto RGB (the session's resolved
// `io.output_color_space`). A display or a file wants another space, and the
// conversion is the one the macOS canvas runs (`Canvas/Shaders.metal`
// `outputTransform`, RFC-018 §5.4): the source curve off, the engine's
// CAT02-adapted matrix, CAM16-UCS gamut compression into the target, the
// target curve on. Every number comes from `spk_output_transform`; this file
// only does the per-pixel arithmetic, in float32 as the shader does.
#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include "spektrafilm/spk_engine.h"

namespace spkhost {

// "srgb" / "sRGB" -> "sRGB", "display-p3" -> "Display P3",
// "prophoto" -> "ProPhoto RGB". Empty on an unknown name.
std::string engine_space_name(const std::string& wire);

class OutputTransform {
public:
    bool fetch(spk_engine* engine, const std::string& src, const std::string& dst, std::string& error);
    const std::string& target() const { return dst_; }
    bool identity() const { return identity_; }
    // Encoded source RGBA16 (row stride in pixels) -> linear target RGB float.
    void to_linear(const uint16_t* rgba16, uint32_t width, uint32_t height, uint32_t stride_px,
                   std::vector<float>& rgb) const;
    // Linear target RGB -> encoded target, then quantised.
    void encode_rgba8(const std::vector<float>& rgb, std::vector<uint8_t>& rgba) const;
    void encode_rgba16(const std::vector<float>& rgb, std::vector<uint16_t>& rgba) const;
    uint32_t target_mode() const { return dst_mode_; }

private:
    std::string src_, dst_;
    bool identity_ = false;
    uint32_t src_mode_ = 0, dst_mode_ = 0;
    std::array<float, 9> matrix_{};
    bool cam16_ = false, lightness_ = false;
    std::array<float, 9> m2x_{}, m2r_{};
    std::array<float, 22> k_{};
    uint32_t nl_ = 0, nh_ = 0;
    const float* cmax_ = nullptr;
};

float cctf_decode_mode(float v, uint32_t mode);
float cctf_encode_mode(float v, uint32_t mode);

// --- files -----------------------------------------------------------------

// An ICC profile describing `space` ("sRGB" uses the bundled resource).
bool icc_for(const std::string& engine_space, const std::filesystem::path& resources,
             std::vector<uint8_t>& icc, std::string& error);

// The shooting data a written file carries (EXIF): what the frame came from,
// never what it was before the host turned it upright (Orientation is 1).
struct Exif {
    std::string make, model, lens, datetime_original;   // EXIF "YYYY:MM:DD HH:MM:SS"
    double iso = 0, shutter_s = 0, aperture = 0, focal_mm = 0;   // 0 = absent
};

// Interleaved 3-channel encoded samples. `exif` may be null.
bool encode_tiff(const void* rgb, uint32_t width, uint32_t height, uint32_t bits,
                 const std::vector<uint8_t>& icc, std::vector<uint8_t>& out, std::string& error,
                 const Exif* exif = nullptr);
// 通用 RAW 的 DNG 出口：把 LibRaw 解出的**线性 ProPhoto 16 位 RGB**按 DNG
// 的 LinearRaw（PhotometricInterpretation=34892）封装。DNG 是「真实 RAW」容器，
// 观感参数不写进文件（与 X3F 的 DNG 同口径），只带 AsShotNeutral / ColorMatrix1 /
// WhiteLevel 这类解 RAW 必需的标定。Lightroom 等按 DNG 规范读 LinearRaw 的软件
// 可以直接打开；个别只认 Bayer CFA 的工具会退回显示内嵌预览。
bool encode_dng(const uint16_t* rgb, uint32_t width, uint32_t height,
                const std::string& camera_model, const Exif* exif,
                std::vector<uint8_t>& out, std::string& error);

bool encode_png8(const uint8_t* rgb, uint32_t width, uint32_t height, const std::vector<uint8_t>& icc,
                 std::vector<uint8_t>& out, std::string& error, const Exif* exif = nullptr);
bool encode_jpeg(const uint8_t* rgb, uint32_t width, uint32_t height, int quality,
                 const std::vector<uint8_t>& icc, std::vector<uint8_t>& out, std::string& error,
                 const Exif* exif = nullptr);
// A complete little-endian TIFF-structured EXIF block (JPEG APP1 after
// "Exif\0\0", PNG eXIf).
std::vector<uint8_t> exif_block(const Exif& exif, uint32_t width, uint32_t height);
// A .cube from spk_print_lut_table's (S,S,S,3) [r][g][b] table, domain 0..1.
std::string cube_text(const float* table, uint32_t size, const std::string& title);

}  // namespace spkhost
