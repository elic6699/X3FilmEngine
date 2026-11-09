// SPDX-FileCopyrightText: 2026 LittleSixNine
// SPDX-License-Identifier: GPL-3.0-or-later
// Applies only to FrameMatter contributions; upstream notices are retained. See COPYRIGHT.txt.

// Test-only probe of the shipping grain stage, without print/scan transforms.
// Build against engine/build/*.o after engine/build.sh dylib.
#include "pipeline/pipeline.hpp"
#include <cstdio>
#include <cstdlib>
#include <memory>

int main(int argc, char** argv) {
    if (argc != 7) {
        std::fprintf(stderr, "usage: grain_probe resources out.f32 area blur layered density\n");
        return 2;
    }
    using namespace spk;
    const std::string res = argv[1];
    std::string error;
    auto check = [&](bool ok) {
        if (!ok) { std::fprintf(stderr, "%s\n", error.c_str()); std::exit(1); }
    };
    Blob blob;
    Colour colour;
    check(blob.open(res + "/spektrafilm_constants.bin", error));
    check(colour.init(blob, error));
    std::unique_ptr<gpu::Gpu> gpu(gpu::Gpu::create_metal(nullptr, res + "/spektrafilm.metallib", error));
    check(bool(gpu));
    Params p;
    check(init_params(res, "kodak_portra_400", "kodak_portra_endura", Json::object(), p, error));
    Json delta = Json::object();
    delta.set("grain_particle_area_um2", Json(std::atof(argv[3])));
    delta.set("grain_blur", Json(std::atof(argv[4])));
    delta.set("grain_sublayers_active", Json(std::atoi(argv[5]) != 0));
    std::string param;
    check(validate_delta(delta, error, param));
    apply_delta(p, delta);
    SetupCache cache;
    Pipeline pipeline(gpu.get(), &colour, &blob, &cache);
    check(pipeline.build(p, error));
    // 35 mm / 20,000 pixels = 1.75 µm, held equal in the upstream probe.
    pipeline.set_source_long_edge(20000, 20000);
    std::vector<float> values(512 * 512 * 3, float(std::atof(argv[6])));
    Image density;
    density.h = density.w = 512;
    density.buf = gpu->upload(values.data(), values.size() * sizeof(float), error);
    check(bool(density.buf));
    Pipeline::Chain in, out;
    in.cur = density;
    check(pipeline.film_grain(in, out, error));
    check(gpu->read(out.cur.buf.get(), 0, values.data(), values.size() * sizeof(float), error));
    auto file = std::fopen(argv[2], "wb");
    if (!file) return 1;
    const bool written = std::fwrite(values.data(), sizeof(float), values.size(), file) == values.size();
    const bool closed = std::fclose(file) == 0;
    return written && closed ? 0 : 1;
}
