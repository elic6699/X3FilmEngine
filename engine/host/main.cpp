// spektralab-host -- the engine as a child process (desktop/HOST-PROTOCOL.md).
//
// stdin/stdout carry length-prefixed frames (u32 header_len, u32 payload_len,
// JSON header, binary payload); stderr is a free-form log. A reader thread
// owns stdin: it answers `ping`, `cancel` and `shutdown` itself and queues
// everything else for one worker thread, which owns the engine. So requests
// run in order, a render can be cancelled while it runs, and a host that is
// busy still answers a liveness check.
//
// The engine is reached only through its C ABI (`spk_engine.h`), exactly as
// the macOS app reaches it -- the host is a client of the engine, not a
// second implementation of any of it.

#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <system_error>
#include <optional>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "image_io.hpp"
#include "generic_develop.hpp"
#include "luma_finish.hpp"
#include "json.hpp"
#include "lensfun_correction.hpp"
#include "output.hpp"
#include "platform.hpp"
#include "spektrafilm/spk_engine.h"

#ifndef SPEKTRALAB_HOST_VERSION
#define SPEKTRALAB_HOST_VERSION "0.1.0"
#endif

using spk::Json;
namespace fs = std::filesystem;
using namespace spkhost;

namespace {

constexpr int kProtocol = 1;
constexpr uint32_t kMaxHeader = 64u << 20;

using Clock = std::chrono::steady_clock;
double ms_since(Clock::time_point t) {
    return std::chrono::duration<double, std::milli>(Clock::now() - t).count();
}

// --- logging -------------------------------------------------------------------

int g_log_level = 1;   // 0 error, 1 info, 2 debug
std::mutex g_log_mutex;
void log_line(int level, const std::string& text) {
    if (level > g_log_level) return;
    std::lock_guard<std::mutex> lock(g_log_mutex);
    static const char* names[] = {"error", "info", "debug"};
    std::fprintf(stderr, "spektralab-host %s: %s\n", names[level], text.c_str());
    std::fflush(stderr);
}

// --- framing -------------------------------------------------------------------

std::mutex g_out_mutex;

void write_frame(const Json& header, const void* payload = nullptr, size_t payload_len = 0) {
    const std::string text = header.dump();
    uint8_t prefix[8];
    const uint32_t h = uint32_t(text.size()), p = uint32_t(payload_len);
    for (int i = 0; i < 4; ++i) { prefix[i] = uint8_t(h >> (8 * i)); prefix[4 + i] = uint8_t(p >> (8 * i)); }
    std::lock_guard<std::mutex> lock(g_out_mutex);
    std::fwrite(prefix, 1, 8, stdout);
    std::fwrite(text.data(), 1, text.size(), stdout);
    if (payload_len) std::fwrite(payload, 1, payload_len, stdout);
    std::fflush(stdout);
}

bool read_exact(void* dst, size_t n) {
    uint8_t* p = static_cast<uint8_t*>(dst);
    while (n) {
        const size_t got = std::fread(p, 1, n, stdin);
        if (got == 0) return false;
        p += got;
        n -= got;
    }
    return true;
}

struct Request {
    uint32_t id = 0;
    std::string method;
    Json params = Json::object();
    std::vector<uint8_t> payload;
};

struct HostError {
    std::string code, message;
};

Json number(double v) { return Json(v); }
Json str(const std::string& s) { return Json(s); }

void respond_ok(uint32_t id, Json result, const void* payload = nullptr, size_t n = 0) {
    Json h = Json::object();
    h.set("id", number(id));
    h.set("ok", Json(true));
    h.set("result", std::move(result));
    write_frame(h, payload, n);
}

void respond_error(uint32_t id, const std::string& code, const std::string& message) {
    Json e = Json::object();
    e.set("code", str(code));
    e.set("message", str(message));
    Json h = Json::object();
    h.set("id", number(id));
    h.set("ok", Json(false));
    h.set("error", std::move(e));
    write_frame(h);
    log_line(2, "request " + std::to_string(id) + " failed: " + code + ": " + message);
}

void emit_event(Json event) { write_frame(event); }

// --- state ---------------------------------------------------------------------

struct Session {
    std::string id;
    spk_session* handle = nullptr;
    std::string path;
    uint32_t width = 0, height = 0;
    Metadata metadata;
    std::string output_space = "ProPhoto RGB";
};

struct State {
    fs::path resources;
    spk_engine* engine = nullptr;
    std::string engine_error;
    std::map<std::string, std::shared_ptr<Session>> sessions;
    std::mutex sessions_mutex;   // the reader thread reads `sessions` for cancel
    uint64_t next_session = 1;
    std::map<std::string, OutputTransform> transforms;

    // cancellation
    std::mutex cancel_mutex;
    std::string running_session, running_progress;
    std::set<std::pair<std::string, std::string>> cancelled;   // (session, progress_id) not yet started
} g;

std::shared_ptr<Session> find_session(const Json& params) {
    const std::string id = params.at("session").as_string();
    std::lock_guard<std::mutex> lock(g.sessions_mutex);
    auto it = g.sessions.find(id);
    if (it == g.sessions.end()) throw HostError{"not_found", "no session '" + id + "'"};
    return it->second;
}

[[noreturn]] void engine_fail(spk_status status, const std::string& what) {
    const std::string message = what + ": " + spk_last_error(g.engine);
    if (status == SPK_ERR_CANCELLED) throw HostError{"cancelled", message};
    if (status == SPK_ERR_UNSUPPORTED) throw HostError{"unsupported", message};
    if (status == SPK_ERR_INVALID_ARG || status == SPK_ERR_USER) throw HostError{"bad_request", message};
    if (status == SPK_ERR_IO) throw HostError{"io_error", message};
    throw HostError{"engine_error", message};
}

void need_engine() {
    if (!g.engine) throw HostError{"engine_error", "the engine did not start: " + g.engine_error};
}

Json parse_json(const char* text) {
    Json out;
    std::string error;
    if (!text || !Json::parse(text, out, error)) throw HostError{"internal", "engine returned unparsable JSON: " + error};
    return out;
}

Json take_json(char* text) {
    Json out;
    std::string error;
    const bool ok = text && Json::parse(text, out, error);
    if (text) spk_string_free(text);
    if (!ok) throw HostError{"internal", "engine returned unparsable JSON: " + error};
    return out;
}

const std::string& require_string(const Json& params, const char* key) {
    const Json& v = params.at(key);
    if (!v.is_string() || v.as_string().empty()) throw HostError{"bad_request", std::string("missing string '") + key + "'"};
    return v.as_string();
}

std::string optional_string(const Json& params, const char* key, const std::string& fallback = {}) {
    const Json& v = params.at(key);
    return v.is_string() ? v.as_string() : fallback;
}

std::string iso_datetime(const std::string& exif) {
    // "YYYY:MM:DD HH:MM:SS" -> "YYYY-MM-DDTHH:MM:SS"
    if (exif.size() >= 19 && exif[4] == ':' && exif[7] == ':') {
        std::string s = exif.substr(0, 19);
        s[4] = '-'; s[7] = '-'; s[10] = 'T';
        return s;
    }
    return exif;
}

Json metadata_json(const Metadata& m) {
    Json j = Json::object();
    if (!m.make.empty()) j.set("make", str(m.make));
    if (!m.model.empty()) j.set("model", str(m.model));
    if (!m.lens.empty()) j.set("lens", str(m.lens));
    if (m.iso) j.set("iso", number(*m.iso));
    if (m.shutter_s) j.set("shutter_s", number(*m.shutter_s));
    if (m.aperture) j.set("aperture", number(*m.aperture));
    if (m.focal_mm) j.set("focal_mm", number(*m.focal_mm));
    if (!m.datetime_original.empty()) j.set("datetime_original", str(iso_datetime(m.datetime_original)));
    j.set("orientation", number(m.orientation));
    if (m.as_shot_temperature_k && m.as_shot_tint) {
        Json a = Json::object();
        a.set("temperature_k", number(std::round(*m.as_shot_temperature_k)));
        a.set("tint", number(std::round(*m.as_shot_tint * 10) / 10));
        j.set("as_shot", std::move(a));
    }
    return j;
}

Exif exif_of(const Metadata& m) {
    Exif e;
    e.make = m.make;
    e.model = m.model;
    e.lens = m.lens;
    e.datetime_original = m.datetime_original;
    e.iso = m.iso.value_or(0);
    e.shutter_s = m.shutter_s.value_or(0);
    e.aperture = m.aperture.value_or(0);
    e.focal_mm = m.focal_mm.value_or(0);
    return e;
}

std::string output_space_of(const Json& params) {
    const Json& nested = params.path("io.output_color_space");
    if (nested.is_string()) return nested.as_string();
    const Json& flat = params.at("output_color_space");
    if (flat.is_string()) return flat.as_string();
    return "ProPhoto RGB";
}

const OutputTransform& transform(const std::string& src, const std::string& dst) {
    const std::string key = src + "->" + dst;
    auto it = g.transforms.find(key);
    if (it != g.transforms.end()) return it->second;
    OutputTransform t;
    std::string error;
    if (!t.fetch(g.engine, src, dst, error)) throw HostError{"engine_error", "output transform " + key + ": " + error};
    return g.transforms.emplace(key, std::move(t)).first->second;
}

// Holds a spk_result and frees it.
struct Result {
    spk_result r{};
    ~Result() { if (r.texture) spk_result_free(&r); }
};

// Engine pixels -> the requested wire format. Fills `image` and `payload`.
void package_pixels(const spk_result& r, const std::string& source_space, const Json& params, Json& image,
                    std::vector<uint8_t>& payload) {
    const std::string format = optional_string(params, "format", "rgba8");
    std::string display = optional_string(params, "display");
    if (format != "rgba8" && format != "rgba16") throw HostError{"bad_request", "format must be rgba8 or rgba16"};
    if (format == "rgba8" && display.empty()) display = "srgb";
    const uint32_t w = r.width, h = r.height, stride = r.row_stride_px ? r.row_stride_px : r.width;
    image = Json::object();
    image.set("width", number(w));
    image.set("height", number(h));
    image.set("format", str(format));
    if (format == "rgba16" && display.empty()) {
        payload.resize(size_t(w) * h * 8);
        for (uint32_t y = 0; y < h; ++y) {
            const uint16_t* row = r.rgba16 + size_t(y) * stride * 4;
            uint8_t* out = &payload[size_t(y) * w * 8];
            for (size_t i = 0; i < size_t(w) * 4; ++i) { out[2 * i] = uint8_t(row[i]); out[2 * i + 1] = uint8_t(row[i] >> 8); }
        }
        image.set("color_space", str(source_space));
        image.set("row_bytes", number(double(w) * 8));
        return;
    }
    const std::string target = engine_space_name(display);
    if (target.empty()) throw HostError{"bad_request", "display must be srgb, display-p3 or prophoto"};
    const OutputTransform& t = transform(source_space, target);
    std::vector<float> lin;
    t.to_linear(r.rgba16, w, h, stride, lin);
    if (format == "rgba8") {
        t.encode_rgba8(lin, payload);
        image.set("row_bytes", number(double(w) * 4));
    } else {
        std::vector<uint16_t> px;
        t.encode_rgba16(lin, px);
        payload.resize(px.size() * 2);
        for (size_t i = 0; i < px.size(); ++i) { payload[2 * i] = uint8_t(px[i]); payload[2 * i + 1] = uint8_t(px[i] >> 8); }
        image.set("row_bytes", number(double(w) * 8));
    }
    image.set("color_space", str(target));
}

void progress_event(const Session& s, const std::string& progress_id, double fraction, const char* stage) {
    if (progress_id.empty()) return;
    Json e = Json::object();
    e.set("event", str("progress"));
    e.set("session", str(s.id));
    e.set("progress_id", str(progress_id));
    e.set("fraction", number(fraction));
    e.set("stage", str(stage));
    emit_event(std::move(e));
}

// Marks the session/progress pair as the one in flight; throws if it was
// cancelled while queued.
struct Running {
    Running(const std::string& session, const std::string& progress) {
        std::lock_guard<std::mutex> lock(g.cancel_mutex);
        if (!progress.empty() && g.cancelled.erase({session, progress}))
            throw HostError{"cancelled", "cancelled before it started"};
        g.running_session = session;
        g.running_progress = progress;
    }
    ~Running() {
        std::lock_guard<std::mutex> lock(g.cancel_mutex);
        g.running_session.clear();
        g.running_progress.clear();
    }
};

// --- methods -------------------------------------------------------------------

Json m_hello(const Request&) {
    Json r = Json::object();
    r.set("protocol", number(kProtocol));
    r.set("host_version", str(SPEKTRALAB_HOST_VERSION));
    r.set("build_info", str(spk_build_info()));
    r.set("resources_dir", str(path_utf8(g.resources)));
    Json backend = Json::object();
    backend.set("api", str("vulkan"));
    if (!g.engine) {
        backend.set("available", Json(false));
        backend.set("error", str(g.engine_error));
        r.set("backend", std::move(backend));
        r.set("capabilities", Json());
        return r;
    }
    Json caps = parse_json(spk_capabilities(g.engine));
    const Json& b = caps.at("backend");
    backend.set("available", Json(true));
    backend.set("device_name", str(b.at("gpu").as_string()));
    backend.set("math_mode", str(b.at("math_mode").as_string()));
    backend.set("render_core", str(b.at("render_core").as_string()));
    r.set("backend", std::move(backend));
    r.set("capabilities", std::move(caps));
    Json methods = Json::array();
    for (const char* m : {"hello", "ping", "shutdown", "params_schema", "print_lut_catalog", "memory_report",
                          "probe", "thumbnail", "decode_raw_frame", "open", "open_pixels", "redecode", "close", "set_params", "get_params", "solve", "render",
                          "scene_latitude", "overscan_geometry", "preview_stock_lut", "cancel", "progress",
                          "export_image", "export_cube", "export_di", "write_image"})
        methods.push(str(m));
    r.set("methods", std::move(methods));
    return r;
}

Json m_params_schema(const Request&) { need_engine(); return parse_json(spk_params_schema(g.engine)); }
Json m_print_lut_catalog(const Request&) { need_engine(); return parse_json(spk_print_lut_catalog(g.engine)); }
Json m_memory_report(const Request&) { need_engine(); return parse_json(spk_memory_report(g.engine)); }

Json m_probe(const Request& q) {
    Probe p;
    std::string error;
    const fs::path path = utf8_path(require_string(q.params, "path"));
    if (!probe_file(path, p, error)) {
        std::error_code ec;
        throw HostError{fs::exists(path, ec) ? "decode_failed" : "not_found", error};
    }
    Json r = Json::object();
    r.set("kind", str(kind_name(p.kind)));
    r.set("width", number(p.width));
    r.set("height", number(p.height));
    r.set("metadata", metadata_json(p.metadata));
    return r;
}

Json m_thumbnail(const Request& q, std::vector<uint8_t>& payload) {
    const fs::path path = utf8_path(require_string(q.params, "path"));
    const uint32_t long_edge = uint32_t(std::max(1, q.params.at("long_edge").as_int(256)));
    Rgba8 t;
    std::string source, error;
    std::error_code ec;
    if (!fs::exists(path, ec)) throw HostError{"not_found", "not found: " + path_utf8(path)};
    if (!thumbnail(path, long_edge, t, source, error)) throw HostError{"decode_failed", error};
    Json image = Json::object();
    image.set("width", number(t.width));
    image.set("height", number(t.height));
    image.set("format", str("rgba8"));
    image.set("color_space", str("sRGB"));
    image.set("row_bytes", number(double(t.width) * 4));
    Json r = Json::object();
    r.set("image", std::move(image));
    r.set("source", str(source));
    r.set("orientation_applied", Json(true));
    payload = std::move(t.rgba);
    return r;
}

DecodeOptions decode_options(const Json& decode) {
    DecodeOptions options;
    if (decode.is_null()) return options;
    if (!decode.is_object()) throw HostError{"bad_request", "decode must be an object"};
    options.raw_mode = optional_string(decode, "raw_mode", "compatible16");
    if (options.raw_mode != "compatible16" && options.raw_mode != "headroom")
        throw HostError{"bad_request", "decode.raw_mode must be compatible16 or headroom"};
    for (const auto& kv : decode.fields()) {
        if (kv.first == "raw_mode") continue;
        if (kv.first == "white_balance") {
            const Json& wb = kv.second;
            const std::string mode = optional_string(wb, "mode", "as_shot");
            if (mode == "as_shot") continue;
            if (mode != "custom") throw HostError{"bad_request", "white_balance.mode must be as_shot or custom"};
            const double t = wb.at("temperature_k").as_double(0);
            if (!(t >= 2000 && t <= 50000)) throw HostError{"bad_request", "white_balance.temperature_k must be 2000..50000"};
            const double tint = wb.at("tint").as_double(0);
            if (!(tint >= -150 && tint <= 150)) throw HostError{"bad_request", "white_balance.tint must be -150..150"};
            options.temperature_k = t;
            options.tint = tint;
        } else if (kv.first == "lens_correction") {
            // Lensfun replaces Core Image's vendor lens profiles on Windows.
            // Applied to the decoded linear frame after LibRaw; see
            // lensfun_correction.cpp. Missing database/profile is a no-op,
            // reported in the decode_raw_frame result.
            options.lens_correction = kv.second.as_bool(false);
        } else {
            throw HostError{"unsupported", "decode." + kv.first + " is not supported by this host"};
        }
    }
    return options;
}

struct Decoded {
    FloatImage frame;
    Probe probe;
    double decode_ms = 0;
};

Decoded decode_frame(const fs::path& path, const DecodeOptions& options) {
    std::error_code ec;
    if (!fs::exists(path, ec)) throw HostError{"not_found", "not found: " + path_utf8(path)};
    const auto t0 = Clock::now();
    Decoded d;
    std::string error;
    if (!decode_linear_prophoto(path, options, d.frame, d.probe, error)) {
        const std::string prefix = kUnsupportedPrefix;
        if (error.rfind(prefix, 0) == 0) throw HostError{"unsupported", error.substr(prefix.size())};
        throw HostError{"decode_failed", error};
    }
    d.decode_ms = ms_since(t0);
    return d;
}

spk_session* engine_open(const FloatImage& frame, const std::string& delta, Json& opened, double& open_ms) {
    const auto t1 = Clock::now();
    spk_image image{frame.rgb.data(), frame.width, frame.height, 3};
    char* reply = nullptr;
    spk_session* handle = spk_open(g.engine, &image, delta.empty() ? nullptr : delta.c_str(), &reply);
    if (!handle) {
        if (reply) spk_string_free(reply);
        throw HostError{"engine_error", std::string("open: ") + spk_last_error(g.engine)};
    }
    opened = take_json(reply);
    open_ms = ms_since(t1);
    return handle;
}

Json open_reply(const Session& s, const Decoded& d, const Json& opened, double open_ms) {
    Json r = Json::object();
    r.set("session", str(s.id));
    r.set("width", number(d.frame.width));
    r.set("height", number(d.frame.height));
    r.set("kind", str(kind_name(d.probe.kind)));
    r.set("metadata", metadata_json(d.probe.metadata));
    r.set("params", opened.at("params"));
    r.set("detected_input", opened.at("detected_input"));
    r.set("output_color_space", str(s.output_space));
    Json timings = Json::object();
    timings.set("decode", number(d.decode_ms));
    timings.set("open", number(open_ms));
    r.set("timings_ms", std::move(timings));
    return r;
}

Json m_open(const Request& q) {
    need_engine();
    const fs::path path = utf8_path(require_string(q.params, "path"));
    const DecodeOptions options = decode_options(q.params.at("decode"));
    Decoded d = decode_frame(path, options);
    std::string delta;
    const Json& params = q.params.at("params");
    if (params.is_object()) delta = params.dump();
    Json opened;
    double open_ms = 0;
    spk_session* handle = engine_open(d.frame, delta, opened, open_ms);

    auto s = std::make_shared<Session>();
    s->handle = handle;
    s->path = path_utf8(path);
    s->width = d.frame.width;
    s->height = d.frame.height;
    s->metadata = d.probe.metadata;
    s->output_space = output_space_of(opened.at("params"));
    {
        std::lock_guard<std::mutex> lock(g.sessions_mutex);
        s->id = "s" + std::to_string(g.next_session++);
        g.sessions[s->id] = s;
    }
    log_line(1, "open " + s->id + " " + s->path + " " + std::to_string(d.frame.width) + "x" +
                    std::to_string(d.frame.height) + " decode " + std::to_string(int(d.decode_ms)) + " ms, open " +
                    std::to_string(int(open_ms)) + " ms");
    return open_reply(*s, d, opened, open_ms);
}

/* X3Lumina Windows shell, 2026-11-08: the app already converted its
 * display-referred RGB to linear ProPhoto 16-bit and holds it in memory.
 * Accept it directly rather than round-tripping through a TIFF on disk; this
 * is the macOS product host's `open_pixels` extension to HOST-PROTOCOL.
 * `space=linear_prophoto` means the samples are already linear (no ROMM TRC),
 * exactly what engine_open()/spk_open() expect in FloatImage. */
Json m_open_pixels(const Request& q) {
    need_engine();
    const int width  = q.params.at("width").as_int(0);
    const int height = q.params.at("height").as_int(0);
    const std::string format = optional_string(q.params, "format", "rgb16le");
    const std::string space  = optional_string(q.params, "space", "linear_prophoto");
    if (width <= 0 || height <= 0)
        throw HostError{"bad_request", "width and height are required"};
    /* X3Lumina Windows shell, 2026-11-10: rgb32f = linear ProPhoto IEEE-754
     * float (LE), 12 B/px. The scene-first shell renders scene-linear floats
     * that legitimately exceed 1.0; the 16-bit wire quantised/clamped that
     * headroom away. FloatImage is float internally anyway, so rgb32f is a
     * straight memcpy — no conversion, no clamp. rgb16le stays for older
     * shells. */
    if (format != "rgb16le" && format != "rgb32f")
        throw HostError{"unsupported", "open_pixels supports only rgb16le / rgb32f (linear_prophoto)"};
    if (space != "linear_prophoto")
        throw HostError{"unsupported", "open_pixels supports only linear_prophoto"};
    const bool f32 = (format == "rgb32f");
    const size_t need = size_t(width) * size_t(height) * 3 * (f32 ? 4 : 2);
    if (q.payload.size() != need)
        throw HostError{"bad_request",
                        "payload must be width*height*3*" + std::string(f32 ? "4" : "2") +
                            " bytes of " + format};

    Decoded d;
    d.frame.width  = uint32_t(width);
    d.frame.height = uint32_t(height);
    d.frame.rgb.resize(size_t(width) * size_t(height) * 3);
    if (f32) {
        std::memcpy(d.frame.rgb.data(), q.payload.data(), need);
    } else {
        const uint8_t* p = q.payload.data();
        for (size_t i = 0; i < d.frame.rgb.size(); ++i) {
            const uint32_t v = uint32_t(p[2 * i]) | (uint32_t(p[2 * i + 1]) << 8);
            d.frame.rgb[i] = float(v) / 65535.0f;
        }
    }
    d.probe.width  = d.frame.width;
    d.probe.height = d.frame.height;

    std::string delta;
    const Json& initial = q.params.at("params");
    if (initial.is_object()) delta = initial.dump();
    Json opened;
    double open_ms = 0;
    spk_session* handle = engine_open(d.frame, delta, opened, open_ms);

    auto s = std::make_shared<Session>();
    s->handle = handle;
    s->path = "<pixels>";
    s->width  = d.frame.width;
    s->height = d.frame.height;
    s->output_space = output_space_of(opened.at("params"));
    {
        std::lock_guard<std::mutex> lock(g.sessions_mutex);
        s->id = "s" + std::to_string(g.next_session++);
        g.sessions[s->id] = s;
    }
    log_line(1, "open_pixels " + s->id + " " + std::to_string(width) + "x" +
                    std::to_string(height) + " open " + std::to_string(int(open_ms)) + " ms");
    return open_reply(*s, d, opened, open_ms);
}

// R2: decode the session's file again with other decode settings, keeping its
// id and its params. The engine session is replaced (a new source means a new
// negative); on any failure the old one is untouched.
Json m_redecode(const Request& q) {
    auto s = find_session(q.params);
    const DecodeOptions options = decode_options(q.params.at("decode"));
    Decoded d = decode_frame(utf8_path(s->path), options);
    char* current = nullptr;
    const spk_status st = spk_get_params(s->handle, &current);
    if (st != SPK_OK) engine_fail(st, "get_params");
    Json params = take_json(current);
    Json opened;
    double open_ms = 0;
    spk_session* handle = engine_open(d.frame, params.dump(), opened, open_ms);
    spk_session* old = nullptr;
    {
        std::lock_guard<std::mutex> lock(g.sessions_mutex);
        old = s->handle;
        s->handle = handle;
    }
    spk_session_release(old);
    s->width = d.frame.width;
    s->height = d.frame.height;
    s->metadata = d.probe.metadata;
    s->output_space = output_space_of(opened.at("params"));
    return open_reply(*s, d, opened, open_ms);
}

Json m_close(const Request& q) {
    std::shared_ptr<Session> s;
    {
        std::lock_guard<std::mutex> lock(g.sessions_mutex);
        const std::string id = q.params.at("session").as_string();
        auto it = g.sessions.find(id);
        if (it == g.sessions.end()) throw HostError{"not_found", "no session '" + id + "'"};
        s = it->second;
        g.sessions.erase(it);
    }
    spk_session_release(s->handle);
    s->handle = nullptr;
    return Json::object();
}

Json m_set_params(const Request& q) {
    auto s = find_session(q.params);
    const Json& delta = q.params.at("delta");
    if (!delta.is_object()) throw HostError{"bad_request", "delta must be an object"};
    char* reply = nullptr;
    const spk_status st = spk_set_params(s->handle, delta.dump().c_str(), &reply);
    if (st != SPK_OK) { if (reply) spk_string_free(reply); engine_fail(st, "set_params"); }
    Json out = take_json(reply);
    s->output_space = output_space_of(out.at("params"));
    Json r = Json::object();
    r.set("params", out.at("params"));
    if (out.has("invalidated")) r.set("invalidated", out.at("invalidated"));
    return r;
}

Json m_get_params(const Request& q) {
    auto s = find_session(q.params);
    char* reply = nullptr;
    const spk_status st = spk_get_params(s->handle, &reply);
    if (st != SPK_OK) engine_fail(st, "get_params");
    Json r = Json::object();
    r.set("params", take_json(reply));
    return r;
}

Json m_solve(const Request& q) {
    auto s = find_session(q.params);
    const std::string target = optional_string(q.params, "target", "both");
    char* reply = nullptr;
    const spk_status st = spk_solve(s->handle, target.c_str(), &reply);
    if (st != SPK_OK) engine_fail(st, "solve");
    return take_json(reply);
}

std::string tier_of(const Json& params, const char* fallback) {
    const std::string tier = optional_string(params, "tier", fallback);
    if (tier != "live" && tier != "preview" && tier != "full") throw HostError{"bad_request", "tier must be live, preview or full"};
    return tier;
}

Json m_render(const Request& q, std::vector<uint8_t>& payload) {
    auto s = find_session(q.params);
    const std::string tier = tier_of(q.params, "preview");
    const bool want_reprint = q.params.at("reprint").as_bool(false);
    const std::string progress_id = optional_string(q.params, "progress_id");
    Running running(s->id, progress_id);
    progress_event(*s, progress_id, 0.0, "render");
    const auto t0 = Clock::now();
    Result result;
    bool reprinted = false;
    spk_status st = SPK_ERR_USER;
    if (want_reprint) {
        st = spk_reprint(s->handle, tier.c_str(), &result.r);
        reprinted = st == SPK_OK;
        if (st == SPK_ERR_CANCELLED) engine_fail(st, "reprint");
    }
    if (!reprinted) {
        result.r = spk_result{};
        st = spk_render(s->handle, tier.c_str(), &result.r);
        if (st != SPK_OK) engine_fail(st, "render");
    }
    const double render_ms = ms_since(t0);
    progress_event(*s, progress_id, 0.9, "transform");
    const auto t1 = Clock::now();
    Json image;
    package_pixels(result.r, s->output_space, q.params, image, payload);
    Json r = Json::object();
    r.set("image", std::move(image));
    r.set("tier", str(tier));
    r.set("reprinted", Json(reprinted));
    r.set("negative_was_cached", Json(result.r.negative_was_cached != 0));
    r.set("engine_progress_id", str(result.r.progress_id));
    Json timings = Json::object();
    timings.set("engine", number(result.r.elapsed_ms));
    timings.set("render", number(render_ms));
    timings.set("transform", number(ms_since(t1)));
    r.set("timings_ms", std::move(timings));
    progress_event(*s, progress_id, 1.0, "done");
    return r;
}

Json m_scene_latitude(const Request& q) {
    auto s = find_session(q.params);
    const Json& request = q.params.at("request");
    const std::string text = request.is_object() ? request.dump() : std::string();
    char* reply = nullptr;
    const spk_status st = spk_scene_latitude(s->handle, text.empty() ? nullptr : text.c_str(), &reply);
    if (st != SPK_OK) engine_fail(st, "scene_latitude");
    return take_json(reply);
}

Json m_overscan_geometry(const Request& q) {
    auto s = find_session(q.params);
    char* reply = nullptr;
    const spk_status st = spk_overscan_geometry(s->handle, &reply);
    if (st != SPK_OK) engine_fail(st, "overscan_geometry");
    return take_json(reply);
}

std::string lut_output_space(const std::string& stock) {
    Json catalog = parse_json(spk_print_lut_catalog(g.engine));
    const Json& entry = catalog.at(stock);
    if (!entry.is_object()) throw HostError{"not_found", "no print LUT for '" + stock + "'"};
    const std::string space = entry.at("output_color_space").as_string();
    return space.empty() ? "Display P3" : space;
}

Json m_preview_stock_lut(const Request& q, std::vector<uint8_t>& payload) {
    auto s = find_session(q.params);
    const std::string stock = require_string(q.params, "print_stock");
    const std::string tier = tier_of(q.params, "preview");
    const std::string space = lut_output_space(stock);
    Result result;
    char* reply = nullptr;
    const spk_status st = spk_preview_stock_lut(s->handle, stock.c_str(), tier.c_str(), &result.r, &reply);
    if (st != SPK_OK) { if (reply) spk_string_free(reply); engine_fail(st, "preview_stock_lut"); }
    Json info = take_json(reply);
    Json image;
    package_pixels(result.r, space, q.params, image, payload);
    Json r = Json::object();
    r.set("image", std::move(image));
    r.set("lut", std::move(info));
    return r;
}

Json m_progress(const Request& q) {
    auto s = find_session(q.params);
    const std::string id = optional_string(q.params, "engine_progress_id");
    char* reply = nullptr;
    const spk_status st = spk_progress(s->handle, id.empty() ? nullptr : id.c_str(), &reply);
    if (st != SPK_OK) engine_fail(st, "progress");
    return take_json(reply);
}

// --- export ---------------------------------------------------------------------

struct Encoded {
    std::vector<uint8_t> bytes;
};

// Linear target RGB -> file bytes in `format`.
void encode_file(const OutputTransform& t, std::vector<float>& lin, uint32_t w, uint32_t h,
                 const std::string& format, int quality, const std::vector<uint8_t>& icc, std::vector<uint8_t>& out,
                 const Exif* exif) {
    std::string error;
    if (format == "tiff16") {
        std::vector<uint16_t> rgba;
        t.encode_rgba16(lin, rgba);
        std::vector<uint16_t> rgb(size_t(w) * h * 3);
        for (size_t p = 0; p < size_t(w) * h; ++p) for (int c = 0; c < 3; ++c) rgb[p * 3 + c] = rgba[p * 4 + c];
        if (!encode_tiff(rgb.data(), w, h, 16, icc, out, error, exif)) throw HostError{"io_error", error};
        return;
    }
    std::vector<uint8_t> rgba;
    t.encode_rgba8(lin, rgba);
    std::vector<uint8_t> rgb(size_t(w) * h * 3);
    for (size_t p = 0; p < size_t(w) * h; ++p) for (int c = 0; c < 3; ++c) rgb[p * 3 + c] = rgba[p * 4 + c];
    bool ok = false;
    if (format == "tiff8") ok = encode_tiff(rgb.data(), w, h, 8, icc, out, error, exif);
    else if (format == "png") ok = encode_png8(rgb.data(), w, h, icc, out, error, exif);
    else if (format == "jpeg") ok = encode_jpeg(rgb.data(), w, h, quality, icc, out, error, exif);
    else throw HostError{"bad_request", "format must be tiff16, tiff8, jpeg or png"};
    if (!ok) throw HostError{"io_error", error};
}

void check_format(const std::string& format) {
    if (format != "tiff16" && format != "tiff8" && format != "jpeg" && format != "png")
        throw HostError{"bad_request", "format must be tiff16, tiff8, jpeg or png"};
}

void shrink_to(std::vector<float>& lin, uint32_t& w, uint32_t& h, uint32_t long_edge) {
    const uint32_t l = std::max(w, h);
    if (!long_edge || l <= long_edge) return;
    const double scale = double(long_edge) / l;
    const uint32_t dw = std::max(1u, uint32_t(std::lround(w * scale)));
    const uint32_t dh = std::max(1u, uint32_t(std::lround(h * scale)));
    std::vector<float> out;
    resize_area(lin.data(), w, h, 3, dw, dh, out);
    lin.swap(out);
    w = dw; h = dh;
}

/* Linear ProPhoto (LibRaw output_color=4) -> sRGB-encoded RGB24 / RGBA8.
 * The 3x3 is the numerical inverse of the C core's kSRGBToProPhoto and is
 * byte-for-byte the matrix the macOS GenericRAW.decode uses
 * (GenericRAW.swift:244-248). Gain stays 1.0, matching the neutral chain.
 * A 64K-entry TRC LUT keeps the full-frame conversion cheap. */
static void linear_prophoto_to_srgb8(const std::vector<float>& lin,
                                     uint32_t w, uint32_t h,
                                     std::vector<uint8_t>& rgb8,
                                     std::vector<uint8_t>* rgba8)
{
    static const double m[9] = {
         2.0341864, -0.7273632, -0.3068354,
        -0.2286284,  1.2315863, -0.0029359,
        -0.0085190, -0.1532727,  1.1616103
    };
    static const std::vector<uint8_t> trc = [] {
        std::vector<uint8_t> t(65536);
        for (int i = 0; i < 65536; ++i) {
            double v = double(i) / 65535.0;
            double e = v <= 0.0031308 ? 12.92 * v
                                      : 1.055 * std::pow(v, 1.0 / 2.4) - 0.055;
            t[i] = uint8_t(std::lround(std::min(1.0, std::max(0.0, e)) * 255.0));
        }
        return t;
    }();
    const size_t n = std::min(lin.size() / 3, size_t(w) * h);
    rgb8.assign(n * 3, 0);
    if (rgba8) rgba8->assign(n * 4, 255);
    /* 逐行并行：每个像素只读 lin、只写自己那一格，运算顺序与单线程完全相同，
     * 所以输出逐位一致。42MP 全幅这一遍约 0.7 s，四核可压到 0.2 s 以内。 */
    uint8_t* out3 = rgb8.data();
    uint8_t* out4 = rgba8 ? rgba8->data() : nullptr;
    if (!w || !h) return;
    spkhost::graw::parallel_rows(h, [&](uint32_t y0, uint32_t y1) {
        for (uint32_t y = y0; y < y1; ++y) {
            const size_t rowEnd = std::min(n, (size_t(y) + 1) * w);
            for (size_t i = size_t(y) * w; i < rowEnd; ++i) {
                const double r = lin[i * 3], g = lin[i * 3 + 1], b = lin[i * 3 + 2];
                for (int c = 0; c < 3; ++c) {
                    double v = m[c * 3] * r + m[c * 3 + 1] * g + m[c * 3 + 2] * b;
                    v = std::min(1.0, std::max(0.0, v));
                    uint8_t q = trc[uint16_t(v * 65535.0 + 0.5)];
                    out3[i * 3 + c] = q;
                    if (out4) out4[i * 4 + c] = q;
                }
            }
        }
    });
}

/* ─── 通用 RAW 影调阶段（与 X3F 同一套数学，live.js 的 transform() 是引擎真值）──
 * 曝光 = 线性光域乘 2^ev（ProPhoto 线性域，sRGB 编码前）；
 * 对比度 = logit 域陡度 2^(c/2)；饱和度 = Rec.601 亮度拉开色差（u=1+0.5s）；
 * 阴影/高光 = Rec.709 分区幂曲线（exp2(-2v)，高光压暗侧只有 1/4 强度）。
 * WB 已在 decode（custom_wb）施加，这里不再重复。零值 = 中性直通。 */
struct ToneP { double exposure = 0, contrast = 0, saturation = 0, shadow = 0, highlight = 0; };
static bool toneIsIdentity(const ToneP& t) {
    return t.exposure == 0 && t.contrast == 0 && t.saturation == 0 &&
           t.shadow == 0 && t.highlight == 0;
}

/* 线性 ProPhoto（3ch，可越界）→ sRGB 浮点 0..1（与 linear_prophoto_to_srgb8
 * 的矩阵 + sRGB OETF 同公式；零回归的「全 0 影调」路径仍走原函数，不进这里）。 */
static void linear_prophoto_to_srgb_float(const std::vector<float>& lin, uint32_t w, uint32_t h,
                                          std::vector<float>& srgb) {
    static const double m[9] = {
         2.0341864, -0.7273632, -0.3068354,
        -0.2286284,  1.2315863, -0.0029359,
        -0.0085190, -0.1532727,  1.1616103
    };
    const size_t n = std::min(lin.size() / 3, size_t(w) * h);
    srgb.assign(n * 3, 0.0f);
    if (!w || !h) return;
    spkhost::graw::parallel_rows(h, [&](uint32_t y0, uint32_t y1) {
        for (uint32_t y = y0; y < y1; ++y) {
            const size_t rowEnd = std::min(n, (size_t(y) + 1) * w);
            for (size_t i = size_t(y) * w; i < rowEnd; ++i) {
                const double r = lin[i * 3], g = lin[i * 3 + 1], b = lin[i * 3 + 2];
                for (int c = 0; c < 3; ++c) {
                    double v = m[c * 3] * r + m[c * 3 + 1] * g + m[c * 3 + 2] * b;
                    v = std::min(1.0, std::max(0.0, v));
                    double e = v <= 0.0031308 ? 12.92 * v
                                              : 1.055 * std::pow(v, 1.0 / 2.4) - 0.055;
                    srgb[i * 3 + c] = float(std::min(1.0, std::max(0.0, e)));
                }
            }
        }
    });
}

/* 线性 ProPhoto → **线性 sRGB**（只做矩阵，不做 sRGB OETF）。Fill Light 必须
 * 在这一域施加：X3FCore build_fill_gain 的输入 lin 就是场景线性 sRGB（亮度
 * 权重 0.25/0.65/0.10 也按它标定），换域会让提亮量整体走样。 */
static void linear_prophoto_to_linear_srgb(const std::vector<float>& lin, uint32_t w, uint32_t h,
                                            std::vector<float>& out) {
    static const double m[9] = {
         2.0341864, -0.7273632, -0.3068354,
        -0.2286284,  1.2315863, -0.0029359,
        -0.0085190, -0.1532727,  1.1616103
    };
    const size_t n = std::min(lin.size() / 3, size_t(w) * h);
    out.assign(n * 3, 0.0f);
    if (!w || !h) return;
    spkhost::graw::parallel_rows(h, [&](uint32_t y0, uint32_t y1) {
        for (uint32_t y = y0; y < y1; ++y) {
            const size_t rowEnd = std::min(n, (size_t(y) + 1) * w);
            for (size_t i = size_t(y) * w; i < rowEnd; ++i) {
                const double r = lin[i * 3], g = lin[i * 3 + 1], b = lin[i * 3 + 2];
                for (int c = 0; c < 3; ++c) {
                    double v = m[c * 3] * r + m[c * 3 + 1] * g + m[c * 3 + 2] * b;
                    out[i * 3 + c] = float(v > 0.0 ? v : 0.0);
                }
            }
        }
    });
}

/* 线性 sRGB → sRGB 编码值。与 linear_prophoto_to_srgb_float 的 OETF 同式，
 * 但**不把线性值先夹到 1**：Fill Light 正档会把暗部整体抬过 1，硬夹会让高光
 * 失去肩部形状（原版 X3FCore 一路用 double 带着余量走）。输出允许 >1，
 * 最终量化时再夹 0..1。 */
static void srgb_encode_float(std::vector<float>& v, uint32_t w, uint32_t h) {
    const size_t n = std::min(v.size() / 3, size_t(w) * h);
    if (!w || !h) return;
    spkhost::graw::parallel_rows(h, [&](uint32_t y0, uint32_t y1) {
        for (uint32_t y = y0; y < y1; ++y) {
            const size_t rowEnd = std::min(n, (size_t(y) + 1) * w);
            for (size_t i = size_t(y) * w * 3; i < rowEnd * 3; ++i) {
                double x = double(v[i]);
                if (x < 0.0) x = 0.0;
                const double e = x <= 0.0031308 ? 12.92 * x
                                                : 1.055 * std::pow(x, 1.0 / 2.4) - 0.055;
                v[i] = float(e);
            }
        }
    });
}

/* 把 sRGB 浮点 0..1 量化成 8-bit（rgb8 / 可选 rgba8，alpha=255）。 */
static void srgb_float_to_bytes(const std::vector<float>& srgb, uint32_t w, uint32_t h,
                                std::vector<uint8_t>& rgb8, std::vector<uint8_t>* rgba8) {
    const size_t n = std::min(srgb.size() / 3, size_t(w) * h);
    rgb8.assign(n * 3, 0);
    if (rgba8) rgba8->assign(n * 4, 255);
    if (!w || !h) return;
    uint8_t* out3 = rgb8.data();
    uint8_t* out4 = rgba8 ? rgba8->data() : nullptr;
    spkhost::graw::parallel_rows(h, [&](uint32_t y0, uint32_t y1) {
        for (uint32_t y = y0; y < y1; ++y) {
            const size_t rowEnd = std::min(n, (size_t(y) + 1) * w);
            for (size_t i = size_t(y) * w; i < rowEnd; ++i) {
                for (int c = 0; c < 3; ++c) {
                    double v = double(srgb[i * 3 + c]);
                    int q = int(std::lround(std::min(1.0, std::max(0.0, v)) * 255.0));
                    out3[i * 3 + c] = uint8_t(q);
                    if (out4) out4[i * 4 + c] = uint8_t(q);
                }
            }
        }
    });
}

/* 在 sRGB 显示域施加对比度 / 饱和度 / 阴影 / 高光（exposure 已作为线性增益在
 * 调用方先乘好）。就地修改 srgb（0..1 浮点）。 */
static void apply_tone_srgb(std::vector<float>& s, uint32_t w, uint32_t h, const ToneP& t) {
    const double k   = std::pow(2.0, 0.5 * t.contrast);    // 对比度 logit 陡度
    const double u   = 1.0 + 0.5 * t.saturation;           // 饱和度
    const double eSh = std::pow(2.0, -2.0 * t.shadow);     // 阴影分区幂
    double eHl = -t.highlight;                             // 高光：正 = 压暗（SPP 负向）
    if (eHl < 0) eHl *= 0.25;                             // 压暗侧只有 1/4 强度
    eHl = std::pow(2.0, -2.0 * eHl);
    const size_t n = std::min(s.size() / 3, size_t(w) * h);
    if (!w || !h) return;
    /* 逐行并行：每个像素只读自己那三个通道，输出写回原地，行间无共享状态。 */
    spkhost::graw::parallel_rows(h, [&](uint32_t y0, uint32_t y1) {
    for (uint32_t y = y0; y < y1; ++y) {
        const size_t rowEnd = std::min(n, (size_t(y) + 1) * w);
    for (size_t i = size_t(y) * w; i < rowEnd; ++i) {
        double r = s[i * 3], g = s[i * 3 + 1], b = s[i * 3 + 2];
        /* 1) 对比度：logit 域斜率乘子，端点 0/1 不动点 */
        if (t.contrast != 0) {
            for (int c = 0; c < 3; ++c) {
                double v = s[i * 3 + c];
                if (v > 0.0 && v < 1.0) {
                    double ln = std::log(v / (1.0 - v));
                    s[i * 3 + c] = float(1.0 / (1.0 + std::exp(-k * ln)));
                }
            }
        }
        /* 2) 饱和度：Rec.601 亮度为中心拉开色差（保亮度 → 可叠加） */
        if (t.saturation != 0) {
            double y6 = 0.299 * r + 0.587 * g + 0.114 * b;
            s[i * 3]     = float(y6 + (s[i * 3]     - y6) * u);
            s[i * 3 + 1] = float(y6 + (s[i * 3 + 1] - y6) * u);
            s[i * 3 + 2] = float(y6 + (s[i * 3 + 2] - y6) * u);
            r = s[i * 3]; g = s[i * 3 + 1]; b = s[i * 3 + 2];
        }
        /* 3) 阴影/高光：Rec.709 显示亮度分区幂曲线，按 Y'/Y 同比缩放 RGB
         *    （只在 0..25% / 75%..100% 段内动，中间调不洗）。 */
        if (t.shadow != 0 || t.highlight != 0) {
            double y7 = 0.2126 * r + 0.7152 * g + 0.0722 * b;
            double y2 = y7;
            if (t.shadow != 0 && y7 > 0.0 && y7 < 0.25)
                y2 = 0.25 * std::pow(y7 / 0.25, eSh);
            if (t.highlight != 0 && y7 > 0.75 && y7 < 1.0)
                y2 = 0.75 + 0.25 * std::pow((y7 - 0.75) / 0.25, eHl);
            if (y2 != y7 && y7 > 1e-9) {
                double sc = y2 / y7;
                s[i * 3]     *= float(sc);
                s[i * 3 + 1] *= float(sc);
                s[i * 3 + 2] *= float(sc);
            }
        }
        for (int c = 0; c < 3; ++c) {
            double v = double(s[i * 3 + c]);
            s[i * 3 + c] = v < 0 ? 0.0f : (v > 1 ? 1.0f : s[i * 3 + c]);
        }
    }
    }
    });
}

/* Generic RAW frame export (LibRaw only, no engine session).
 *
 * This is the Windows counterpart of macOS host's decode_raw_frame: the shell
 * owns the neutral display chain, so the host returns a linear ProPhoto
 * float32 frame rather than opening a SpektraLab session. X3F/X3I stay on the
 * X3FCore path in the shell and are explicitly rejected here.
 *
 * params:
 *   path            source file (UTF-8)
 *   decode          {raw_mode, lens_correction}
 *   long_edge       downsample before correction (0 = full size)
 *   output          "rgb_f32le" (default) | "rgba8" | "jpeg" | "tiff16" | "dng"
 *   quality         JPEG quality when output=jpeg (default 92)
 *   develop         {exposure, contrast, saturation, shadow, highlight,
 *                    wb_*, curve[257], split[6], hsl{h,s,l}, mono, sharpness}
 *                   —— 全中性时整段旁路，输出与 3.5.49 逐位一致。
 *   tiff16 / dng 由外壳的「TIFF 16 位」「DNG（真实 RAW）」两档调用；DNG 写的是
 *   未套观感的线性 ProPhoto 16 位 LinearRaw，观感参数不进文件。
 * result.format / color_space follow `output`; payload is the corresponding
 * byte stream. `jpeg` encodes inside the host, so a 100 MP export never
 * pushes a 1 GB float frame across the pipe.
 */
/* 通用 RAW 解码帧缓存（3.5.50）：LibRaw 全尺寸 AHD 解码是整条链最贵的一段，
 * 以前每发一次 decode_raw_frame 都从磁盘重解一遍，拖一根滑杆就付一次几秒~
 * 几十秒的全解码。这里按「源文件 + 解码选项 + 目标长边」缓存已经缩图、已经
 * lensfun 矫正的线性 ProPhoto 帧；develop 变化只重跑显示域调整 + 编码。
 * 上限：单帧 ≤ 512 MB 且最多 2 帧（预览降采样帧很小；全尺寸 100 MP 不缓存，
 * 导出每张本来也只解一次）。host 是单 worker 串行，shared_ptr 保证读期间不失效。 */
struct GrawFrameCache {
    std::string key;
    Decoded d;
    LensCorrectionInfo lc;
    size_t bytes = 0;
};
static std::mutex g_grawCacheMu;
static std::vector<std::shared_ptr<GrawFrameCache>> g_grawCache;   /* 最近优先 */

/* Fill Light 基座（A/B/C 三张 map）单槽缓存：只依赖未做曝光/白平衡的线性帧，
 * 与 F 无关。拖 Fill Light 滑杆时只剩查表 + 采样，不重跑金字塔 ——
 * 对应 X3FCore 的 g_fl_slot_a/b/c（2026-10-22 的「apply 中间结果缓存」）。 */
static std::mutex g_fillBaseMu;
static std::shared_ptr<spkhost::graw::FillBase> g_fillBase;
static std::string g_fillBaseKey;

static std::string graw_cache_key(const fs::path& path, const DecodeOptions& o,
                                  uint32_t long_edge) {
    std::error_code ec;
    const uint64_t size = uint64_t(fs::file_size(path, ec));
    const uint64_t mtime = ec ? 0 : uint64_t(
        fs::last_write_time(path, ec).time_since_epoch().count());
    char b[256];
    std::snprintf(b, sizeof(b), "|%llu|%llu|%s|%.4f|%.4f|%d|%u",
                  (unsigned long long)size, (unsigned long long)mtime,
                  o.raw_mode.c_str(),
                  o.temperature_k ? *o.temperature_k : 0.0,
                  o.tint ? *o.tint : 0.0,
                  o.lens_correction ? 1 : 0, long_edge);
    return path_utf8(path) + b;
}

Json m_decode_raw_frame(const Request& q, std::vector<uint8_t>& payload) {
    const fs::path path = utf8_path(require_string(q.params, "path"));
    /* long_edge is a TOP-LEVEL key: decode_options() rejects unknown keys
       inside `decode`, exactly as the macOS host does. */
    const uint32_t long_edge = uint32_t(std::max(0, q.params.at("long_edge").as_int(0)));
    const DecodeOptions options = decode_options(q.params.at("decode"));
    const std::string output = optional_string(q.params, "output", "rgb_f32le");
    if (output != "rgb_f32le" && output != "rgba8" && output != "jpeg" &&
        output != "tiff16" && output != "dng")
        throw HostError{"bad_request", "output must be rgb_f32le, rgba8, jpeg, tiff16 or dng"};
    const int quality = std::min(100, std::max(1, q.params.at("quality").as_int(92)));

    /* SPK_TIMING=1：把这一帧的时间拆成 decode / convert / encode 打到 stderr。
     * 只影响日志，不影响任何输出字节。LibRaw 解码是大头，量出来才知道往哪投。 */
    const bool timing = std::getenv("SPK_TIMING") != nullptr;
    const auto tEnter = Clock::now();
    auto tStage = Clock::now();
    std::string tLine;
    auto mark = [&](const char* name) {
        if (!timing) return;
        const double ms = ms_since(tStage);
        tStage = Clock::now();
        char b[96];
        std::snprintf(b, sizeof(b), "%s=%.0fms ", name, ms);
        tLine += b;
    };

    /* 通用 RAW 影调阶段参数（与 X3F 同一套数学）。缺 develop 对象时全 0 = 中性直通。 */
    ToneP tone;
    if (q.params.has("develop")) {
        const Json dev = q.params.at("develop");
        tone.exposure   = dev.at("exposure").as_double(0.0);
        tone.contrast   = dev.at("contrast").as_double(0.0);
        tone.saturation = dev.at("saturation").as_double(0.0);
        tone.shadow     = dev.at("shadow").as_double(0.0);
        tone.highlight  = dev.at("highlight").as_double(0.0);
    }
    /* 3.5.50 扩展：白平衡 RGB / 色调曲线 / 分色 HSL / 色彩分离 / 黑白 / 锐度。
     * 全部为中性时整段旁路，零滑杆与 3.5.49 逐位一致。 */
    const spkhost::graw::Develop dev = spkhost::graw::parse(q.params.at("develop"));

    /* 明度层 finish（3.5.52）：与 X3F 侧 X3L_YLayerFinishRGB24 同一套算法，只作用在
     * 8-bit 出图（jpeg / rgba8）上 —— 编码在引擎里，这一道必须插在编码之前才能保住
     * EXIF / ICC 一次成型。develop 里没这几个键 / yfinish=0 时整段跳过（零回归）。 */
    spkhost::graw::LumaFinish fin;
    {
        const Json dv = q.params.at("develop");
        fin.on          = dv.at("yfinish").as_double(0.0) > 0.5;
        fin.sharpen     = dv.at("ysharpen").as_double(0.0);
        fin.grain_size  = dv.at("grainsize").as_double(3.0);
        fin.grain_rough = dv.at("grainrough").as_double(0.0);
        fin.strength    = dv.at("ystrength").as_double(100.0);
        fin.scale       = dv.at("finish_scale").as_double(1.0);
    }

    const std::string ext = [&] {
        std::string e = path_utf8(path.extension());
        for (char& c : e) c = char(std::tolower(static_cast<unsigned char>(c)));
        return e;
    }();
    if (ext == ".x3f" || ext == ".x3i")
        throw HostError{"unsupported", "decode_raw_frame is for camera RAW, not X3F/X3I"};

    /* ── 解码（带缓存）──────────────────────────────────────────────── */
    const std::string cacheKey = graw_cache_key(path, options, long_edge);
    std::shared_ptr<GrawFrameCache> used;
    bool cacheHit = false;
    {
        std::lock_guard<std::mutex> lock(g_grawCacheMu);
        for (const auto& c : g_grawCache)
            if (c->key == cacheKey) { used = c; cacheHit = true; break; }
    }
    if (!used) {
        auto fresh = std::make_shared<GrawFrameCache>();
        fresh->key = cacheKey;
        fresh->d = decode_frame(path, options);   // LibRaw, no need_engine()
        if (fresh->d.probe.kind != FileKind::Raw)
            throw HostError{"unsupported", "decode_raw_frame only accepts camera RAW files"};
        /* Shrink BEFORE lens correction: a preview of a 100 MP frame then costs
           ~20 MB of payload and a few ms of correction instead of ~1 GB. */
        shrink_to(fresh->d.frame.rgb, fresh->d.frame.width, fresh->d.frame.height, long_edge);
        apply_lensfun(fresh->d.frame, fresh->d.probe.metadata, g.resources,
                      options.lens_correction, fresh->lc);
        fresh->bytes = fresh->d.frame.rgb.size() * sizeof(float);
        used = fresh;
        if (fresh->bytes <= (size_t(512) << 20)) {
            std::lock_guard<std::mutex> lock(g_grawCacheMu);
            g_grawCache.insert(g_grawCache.begin(), fresh);
            if (g_grawCache.size() > 2) g_grawCache.resize(2);
        }
    }
    const Decoded& d = used->d;
    mark("decode");
    const LensCorrectionInfo& lc = used->lc;
    if (timing)
        log_line(1, std::string("  lens opt=") + (options.lens_correction ? "1" : "0") +
                        " req=" + (lc.requested ? "1" : "0") +
                        " applied=" + (lc.applied ? "1" : "0") +
                        " supported=" + (lc.supported ? "1" : "0"));
    const uint32_t w = d.frame.width, h = d.frame.height;
    const size_t pixels = size_t(w) * h;
    if (d.frame.rgb.size() < pixels * 3)
        throw HostError{"engine_error", "decoded frame size mismatch"};
    const bool neutral = toneIsIdentity(tone) && spkhost::graw::identity(dev);

    std::string format, color_space;
    double row_bytes = 0;
    auto clamp_unit = [](double v) { return v < 0 ? 0.0 : (v > 1 ? 1.0 : v); };
    std::vector<uint8_t> icc;
    std::string error;
    if (!icc_for(engine_space_name("sRGB"), g.resources, icc, error))
        throw HostError{"io_error", error};

    if (output == "dng") {
        /* 真实 RAW：写的是 LibRaw 解出的线性 ProPhoto，观感参数一概不进文件
         * （与 X3F 的 DNG 同口径）。AsShotNeutral=1:1:1、ColorMatrix1 描述
         * ProPhoto，读 DNG 的软件自行接手显影。 */
        std::vector<uint16_t> lin16(pixels * 3);
        {
            const float* src = d.frame.rgb.data();
            spkhost::graw::parallel_rows(h, [&](uint32_t y0, uint32_t y1) {
                const size_t from = size_t(y0) * w * 3, to = size_t(y1) * w * 3;
                for (size_t i = from; i < to; ++i)
                    lin16[i] = uint16_t(std::lround(clamp_unit(src[i]) * 65535.0));
            });
        }
        mark("convert");
        const Exif exif = exif_of(d.probe.metadata);
        const std::string camera = d.probe.metadata.make.empty()
            ? d.probe.metadata.model
            : d.probe.metadata.make + " " + d.probe.metadata.model;
        if (!encode_dng(lin16.data(), w, h, camera, &exif, payload, error))
            throw HostError{"io_error", error};
        mark("encode");
        format = "dng";
        color_space = "linear_prophoto";
    } else if (output == "rgb_f32le") {
        /* Legacy/debug path: the shell wants the raw linear ProPhoto float
           frame. It must not be used for full-res previews of large RAWs. */
        format = "rgb_f32le";
        color_space = "linear_prophoto";
        row_bytes = double(w) * 12;
        payload.resize(pixels * 3 * 4);
        std::memcpy(payload.data(), d.frame.rgb.data(), pixels * 3 * 4);
    } else {
        std::vector<uint8_t> rgb8, rgba8;
        std::vector<float> srgb;
        if (neutral && output != "tiff16") {
            /* 零滑杆 = 3.5.49 的原路径，逐位一致（零回归铁律）。 */
            if (output == "jpeg") {
                linear_prophoto_to_srgb8(d.frame.rgb, w, h, rgb8, nullptr);
            } else {
                linear_prophoto_to_srgb8(d.frame.rgb, w, h, rgb8, &rgba8);
                /* 明度层 finish 在 RGBA 上跑（预览 / 细节帧）：只动 RGB，alpha 不动。 */
                if (fin.active())
                    spkhost::graw::apply_luma_finish(rgba8.data(), int(w), int(h), 4, fin);
                format = "rgba8";
                color_space = "sRGB";
                row_bytes = double(w) * 4;
                payload = std::move(rgba8);
            }
            mark("convert");
        } else {
            /* 曝光为线性增益（ProPhoto 线性域，sRGB 编码前），WB 的 RGB 三通道
             * 强度也在这一域；随后转 sRGB 浮点再施加影调 / 曲线 / HSL / 色彩
             * 分离 / 黑白 / 锐度。custom_wb 已在 decode 阶段完成。 */
            std::vector<float> lin = d.frame.rgb;
            /* Fill Light 的增益图必须在**未做曝光/白平衡**的线性帧上算：
             * X3FCore 是 build_fill_gain(src8) 之后才在 apply_row 里乘 WB/曝光
             * 增益，所以曝光滑杆不会连带改变填充光的提亮量。 */
            std::vector<float> grawGain;
            if (dev.fill_light != 0.0) {
                /* 基座只依赖未增益的线性帧；取到后立刻出锁，多线程导出只读共享。 */
                std::shared_ptr<spkhost::graw::FillBase> fb;
                {
                    std::lock_guard<std::mutex> gfl(g_fillBaseMu);
                    if (g_fillBaseKey != cacheKey || !g_fillBase || !g_fillBase->valid()) {
                        auto nb = std::make_shared<spkhost::graw::FillBase>();
                        std::vector<float> base;
                        linear_prophoto_to_linear_srgb(d.frame.rgb, w, h, base);
                        if (spkhost::graw::fill_base_build(base.data(), int(w), int(h), *nb)) {
                            g_fillBase = nb;
                            g_fillBaseKey = cacheKey;
                        } else {
                            g_fillBase.reset();
                            g_fillBaseKey.clear();
                        }
                    }
                    fb = g_fillBase;
                }
                if (fb && fb->valid())
                    grawGain = spkhost::graw::fill_gain_from_base(*fb, int(w), int(h),
                                                                  dev.fill_light);
            }
            if (tone.exposure != 0.0) {
                const double g = std::pow(2.0, tone.exposure);
                for (float& v : lin) v = float(v * g);
            }
            spkhost::graw::apply_wb_gain(lin, dev);
            if (!grawGain.empty()) {
                /* 线性 ProPhoto → 线性 sRGB（只做矩阵）→ 乘增益 → sRGB 编码。 */
                std::vector<float> linsrgb;
                linear_prophoto_to_linear_srgb(lin, w, h, linsrgb);
                lin.clear();
                lin.shrink_to_fit();
                spkhost::graw::apply_fill_gain(linsrgb, w, h, grawGain);
                srgb_encode_float(linsrgb, w, h);
                srgb = std::move(linsrgb);
            } else {
                linear_prophoto_to_srgb_float(lin, w, h, srgb);
                lin.clear();
                lin.shrink_to_fit();     /* 100 MP 全幅时别白占 1.2 GB */
            }
            if (!toneIsIdentity(tone)) apply_tone_srgb(srgb, w, h, tone);
            if (!spkhost::graw::identity(dev)) spkhost::graw::apply_display(srgb, w, h, dev);
            mark("convert");
            if (output == "tiff16") {
                std::vector<uint16_t> rgb16(pixels * 3);
                spkhost::graw::parallel_rows(h, [&](uint32_t y0, uint32_t y1) {
                    const size_t from = size_t(y0) * w * 3, to = size_t(y1) * w * 3;
                    for (size_t i = from; i < to; ++i)
                        rgb16[i] = uint16_t(std::lround(clamp_unit(srgb[i]) * 65535.0));
                });
                mark("quantize");
                const Exif exif = exif_of(d.probe.metadata);
                if (!encode_tiff(rgb16.data(), w, h, 16, icc, payload, error, &exif))
                    throw HostError{"io_error", error};
                mark("encode");
                format = "tiff16";
                color_space = "sRGB";
            } else {
                srgb_float_to_bytes(srgb, w, h, rgb8,
                                    output == "jpeg" ? nullptr : &rgba8);
                mark("quantize");
                if (output != "jpeg") {
                    if (fin.active())
                        spkhost::graw::apply_luma_finish(rgba8.data(), int(w), int(h), 4, fin);
                    format = "rgba8";
                    color_space = "sRGB";
                    row_bytes = double(w) * 4;
                    payload = std::move(rgba8);
                }
            }
        }
        if (output == "jpeg") {
            /* 明度层 finish：编码前最后一刀（与外壳 X3F 路径同位置 —— 画幅裁切
             * 之后、JPEG 编码之前）。中性 / 非中性两条路都汇到这里。 */
            if (fin.active()) {
                spkhost::graw::apply_luma_finish(rgb8.data(), int(w), int(h), 3, fin);
                mark("luma");
            }
            const Exif exif = exif_of(d.probe.metadata);
            if (!encode_jpeg(rgb8.data(), w, h, quality, icc, payload, error, &exif))
                throw HostError{"io_error", error};
            mark("encode");
            format = "jpeg";
            color_space = "sRGB";
        }
    }
    if (timing)
        log_line(1, "decode_raw_frame " + std::to_string(w) + "x" + std::to_string(h) +
                        " " + output + ": " + tLine +
                        "libraw=" + std::to_string(int(d.decode_ms)) +
                        "ms lensfun=" + std::to_string(int(lc.ms)) +
                        "ms total=" + std::to_string(int(ms_since(tEnter))) + "ms");

    Json r = Json::object();
    r.set("width", number(w));
    r.set("height", number(h));
    r.set("format", str(format));
    r.set("color_space", str(color_space));
    if (row_bytes > 0) r.set("row_bytes", number(row_bytes));
    r.set("bytes", number(double(payload.size())));
    r.set("orientation_applied", Json(true));
    r.set("kind", str(kind_name(d.probe.kind)));
    r.set("metadata", metadata_json(d.probe.metadata));
    r.set("decode_ms", number(d.decode_ms));
    r.set("decode_cached", Json(cacheHit));   /* 3.5.50：命中解码帧缓存 = 没重跑 LibRaw */
    r.set("long_edge_applied", Json(long_edge > 0));

    Json lens = Json::object();
    lens.set("available", Json(lensfun_database_available(g.resources)));
    lens.set("requested", Json(lc.requested));
    lens.set("supported", Json(lc.supported));
    lens.set("applied", Json(lc.applied));
    lens.set("flags", number(lc.flags));
    lens.set("ms", number(lc.ms));
    if (!lc.camera.empty()) lens.set("camera", str(lc.camera));
    if (!lc.lens.empty())   lens.set("lens", str(lc.lens));
    if (!lc.focal.empty())  lens.set("focal", str(lc.focal));
    if (!lc.reason.empty()) lens.set("reason", str(lc.reason));
    r.set("lens_correction", std::move(lens));
    return r;
}

Json written(const fs::path& path, uint32_t w, uint32_t h, size_t bytes, const std::string& space) {
    Json r = Json::object();
    r.set("path", str(path_utf8(path)));
    r.set("width", number(w));
    r.set("height", number(h));
    r.set("bytes", number(double(bytes)));
    if (!space.empty()) r.set("color_space", str(space));
    return r;
}

Json m_export_image(const Request& q) {
    auto s = find_session(q.params);
    const fs::path path = utf8_path(require_string(q.params, "path"));
    const std::string format = optional_string(q.params, "format", "tiff16");
    check_format(format);
    const std::string space = engine_space_name(optional_string(q.params, "color_space", "sRGB"));
    if (space.empty()) throw HostError{"bad_request", "color_space must be sRGB, display-p3 or prophoto"};
    const int quality = q.params.at("quality").as_int(92);
    const uint32_t long_edge = uint32_t(std::max(0, q.params.at("long_edge").as_int(0)));
    const bool overwrite = q.params.at("overwrite").as_bool(false);
    std::error_code ec;
    if (!overwrite && fs::exists(path, ec)) throw HostError{"io_error", "destination exists: " + path_utf8(path)};
    const std::string progress_id = optional_string(q.params, "progress_id");
    Running running(s->id, progress_id);
    progress_event(*s, progress_id, 0.0, "render");
    Result result;
    const spk_status st = spk_render(s->handle, "full", &result.r);
    if (st != SPK_OK) engine_fail(st, "render full");
    progress_event(*s, progress_id, 0.7, "encode");
    const OutputTransform& t = transform(s->output_space, space);
    std::vector<float> lin;
    uint32_t w = result.r.width, h = result.r.height;
    t.to_linear(result.r.rgba16, w, h, result.r.row_stride_px ? result.r.row_stride_px : w, lin);
    shrink_to(lin, w, h, long_edge);
    std::vector<uint8_t> icc, bytes;
    std::string error;
    if (!icc_for(space, g.resources, icc, error)) throw HostError{"io_error", error};
    const Exif exif = exif_of(s->metadata);
    encode_file(t, lin, w, h, format, quality, icc, bytes, &exif);
    if (!write_file_atomic(path, bytes, overwrite, error)) throw HostError{"io_error", error};
    progress_event(*s, progress_id, 1.0, "done");
    return written(path, w, h, bytes.size(), space);
}

// R1: the app renders, applies geometry and Layer 2, and hands the host the
// finished pixels (rgba16 LE, top row first, encoded in `color_space`).
Json m_write_image(const Request& q) {
    const fs::path path = utf8_path(require_string(q.params, "path"));
    const std::string format = optional_string(q.params, "format", "tiff16");
    check_format(format);
    const std::string space = engine_space_name(require_string(q.params, "color_space"));
    if (space.empty()) throw HostError{"bad_request", "color_space must be sRGB, display-p3 or prophoto"};
    const int w = q.params.at("width").as_int(0), h = q.params.at("height").as_int(0);
    if (w <= 0 || h <= 0) throw HostError{"bad_request", "width and height are required"};
    if (q.payload.size() != size_t(w) * size_t(h) * 8)
        throw HostError{"bad_request", "payload must be width*height*8 bytes of rgba16"};
    const bool overwrite = q.params.at("overwrite").as_bool(false);
    const int quality = q.params.at("quality").as_int(92);
    std::vector<uint8_t> icc, bytes;
    std::string error;
    if (!icc_for(space, g.resources, icc, error)) throw HostError{"io_error", error};
    // R1: the shooting data of the photograph the pixels came from.
    std::optional<Exif> exif;
    const std::string source = optional_string(q.params, "source_path");
    if (!source.empty()) {
        Probe probe;
        std::string why;
        if (probe_file(utf8_path(source), probe, why)) exif = exif_of(probe.metadata);
        else log_line(1, "write_image: no EXIF from " + source + ": " + why);
    }
    const Exif* ex = exif ? &*exif : nullptr;
    const size_t pixels = size_t(w) * h;
    auto sample = [&](size_t i) { return uint16_t(q.payload[2 * i] | (q.payload[2 * i + 1] << 8)); };
    bool ok;
    if (format == "tiff16") {
        std::vector<uint16_t> rgb(pixels * 3);
        for (size_t p = 0; p < pixels; ++p) for (int c = 0; c < 3; ++c) rgb[p * 3 + c] = sample(p * 4 + c);
        ok = encode_tiff(rgb.data(), uint32_t(w), uint32_t(h), 16, icc, bytes, error, ex);
    } else {
        std::vector<uint8_t> rgb(pixels * 3);
        for (size_t p = 0; p < pixels; ++p)
            for (int c = 0; c < 3; ++c) rgb[p * 3 + c] = uint8_t((uint32_t(sample(p * 4 + c)) * 255 + 32767) / 65535);
        if (format == "tiff8") ok = encode_tiff(rgb.data(), uint32_t(w), uint32_t(h), 8, icc, bytes, error, ex);
        else if (format == "png") ok = encode_png8(rgb.data(), uint32_t(w), uint32_t(h), icc, bytes, error, ex);
        else ok = encode_jpeg(rgb.data(), uint32_t(w), uint32_t(h), quality, icc, bytes, error, ex);
    }
    if (!ok) throw HostError{"io_error", error};
    if (!write_file_atomic(path, bytes, overwrite, error)) throw HostError{"io_error", error};
    Json r = written(path, uint32_t(w), uint32_t(h), bytes.size(), space);
    r.set("exif_copied", Json(ex != nullptr));
    return r;
}

Json m_export_cube(const Request& q) {
    need_engine();
    const std::string stock = require_string(q.params, "print_stock");
    const fs::path path = utf8_path(require_string(q.params, "path"));
    const float* table = nullptr;
    uint32_t size = 0;
    const spk_status st = spk_print_lut_table(g.engine, stock.c_str(), &table, &size);
    if (st != SPK_OK) engine_fail(st, "print_lut_table");
    const std::string text = cube_text(table, size, "SpektraLab print LUT " + stock);
    std::string error;
    if (!write_file_atomic(path, std::vector<uint8_t>(text.begin(), text.end()), q.params.at("overwrite").as_bool(false), error))
        throw HostError{"io_error", error};
    Json r = Json::object();
    r.set("path", str(path_utf8(path)));
    r.set("size", number(size));
    return r;
}

Json m_export_di(const Request& q) {
    auto s = find_session(q.params);
    const fs::path path = utf8_path(require_string(q.params, "path"));
    const std::string stock = optional_string(q.params, "print_stock");
    Result result;
    char* reply = nullptr;
    const spk_status st = spk_export_di(s->handle, stock.empty() ? nullptr : stock.c_str(), &result.r, &reply);
    if (st != SPK_OK) { if (reply) spk_string_free(reply); engine_fail(st, "export_di"); }
    Json info = take_json(reply);
    const uint32_t w = result.r.width, h = result.r.height, stride = result.r.row_stride_px ? result.r.row_stride_px : w;
    std::vector<uint16_t> rgb(size_t(w) * h * 3);
    for (uint32_t y = 0; y < h; ++y)
        for (uint32_t x = 0; x < w; ++x)
            for (int c = 0; c < 3; ++c) rgb[(size_t(y) * w + x) * 3 + c] = result.r.rgba16[(size_t(y) * stride + x) * 4 + c];
    std::vector<uint8_t> bytes;
    std::string error;
    // Normalised negative density, not a colour space: no ICC profile.
    if (!encode_tiff(rgb.data(), w, h, 16, {}, bytes, error)) throw HostError{"io_error", error};
    if (!write_file_atomic(path, bytes, q.params.at("overwrite").as_bool(false), error)) throw HostError{"io_error", error};
    Json r = written(path, w, h, bytes.size(), "");
    r.set("di", std::move(info));
    return r;
}

void dispatch(const Request& q) {
    std::vector<uint8_t> payload;
    Json result;
    const std::string& m = q.method;
    if (m == "hello") result = m_hello(q);
    else if (m == "params_schema") result = m_params_schema(q);
    else if (m == "print_lut_catalog") result = m_print_lut_catalog(q);
    else if (m == "memory_report") result = m_memory_report(q);
    else if (m == "probe") result = m_probe(q);
    else if (m == "thumbnail") result = m_thumbnail(q, payload);
    else if (m == "decode_raw_frame") result = m_decode_raw_frame(q, payload);
    else if (m == "open") result = m_open(q);
    else if (m == "open_pixels") result = m_open_pixels(q);
    else if (m == "close") result = m_close(q);
    else if (m == "redecode") result = m_redecode(q);
    else if (m == "set_params") result = m_set_params(q);
    else if (m == "get_params") result = m_get_params(q);
    else if (m == "solve") result = m_solve(q);
    else if (m == "render") result = m_render(q, payload);
    else if (m == "scene_latitude") result = m_scene_latitude(q);
    else if (m == "overscan_geometry") result = m_overscan_geometry(q);
    else if (m == "preview_stock_lut") result = m_preview_stock_lut(q, payload);
    else if (m == "progress") result = m_progress(q);
    else if (m == "export_image") result = m_export_image(q);
    else if (m == "write_image") result = m_write_image(q);
    else if (m == "export_cube") result = m_export_cube(q);
    else if (m == "export_di") result = m_export_di(q);
    else throw HostError{"unsupported", "unknown method '" + m + "'"};
    respond_ok(q.id, std::move(result), payload.data(), payload.size());
}

// --- threads ---------------------------------------------------------------------

std::mutex g_queue_mutex;
std::condition_variable g_queue_cv;
std::deque<Request> g_queue;
bool g_queue_closed = false;

void worker() {
    for (;;) {
        Request q;
        {
            std::unique_lock<std::mutex> lock(g_queue_mutex);
            g_queue_cv.wait(lock, [] { return g_queue_closed || !g_queue.empty(); });
            if (g_queue.empty()) return;
            q = std::move(g_queue.front());
            g_queue.pop_front();
        }
        const auto t0 = Clock::now();
        try {
            dispatch(q);
        } catch (const HostError& e) {
            respond_error(q.id, e.code, e.message);
        } catch (const std::bad_alloc&) {
            respond_error(q.id, "internal", "out of memory");
        } catch (const std::system_error& e) {
            // FrameMatter Windows port: surface the method and error category.
            // A cancel racing a render can hit a non-recursive mutex twice on
            // the same thread; report it as a cancellation-like outcome.
            respond_error(q.id, "internal", std::string("system_error in ") + q.method +
                          ": " + e.what());
        } catch (const std::exception& e) {
            respond_error(q.id, "internal", e.what());
        }
        log_line(2, q.method + " #" + std::to_string(q.id) + " " + std::to_string(int(ms_since(t0))) + " ms");
    }
}

void handle_cancel(const Request& q) {
    const std::string session = q.params.at("session").as_string();
    const std::string progress = q.params.at("progress_id").as_string();
    bool running = false;
    {
        std::lock_guard<std::mutex> lock(g.cancel_mutex);
        running = g.running_session == session && (progress.empty() || g.running_progress == progress);
        if (!running && !progress.empty()) g.cancelled.insert({session, progress});
    }
    if (running) {
        std::lock_guard<std::mutex> lock(g.sessions_mutex);
        auto it = g.sessions.find(session);
        if (it != g.sessions.end() && it->second->handle) spk_cancel(it->second->handle, nullptr);
    }
    Json r = Json::object();
    r.set("was_running", Json(running));
    respond_ok(q.id, std::move(r));
}

[[noreturn]] void quit(int code) {
    std::fflush(stdout);
    std::fflush(stderr);
    // The worker may be inside a render; the GPU driver and the OS reclaim
    // everything. Destroying the engine under a live render would race it.
    std::_Exit(code);
}

void reader() {
    for (;;) {
        uint8_t prefix[8];
        if (!read_exact(prefix, 8)) { log_line(1, "stdin closed; exiting"); quit(0); }
        uint32_t h = 0, p = 0;
        for (int i = 0; i < 4; ++i) { h |= uint32_t(prefix[i]) << (8 * i); p |= uint32_t(prefix[4 + i]) << (8 * i); }
        if (h > kMaxHeader) { log_line(0, "header length " + std::to_string(h) + " exceeds bound; exiting"); quit(2); }
        std::string text(h, '\0');
        Request q;
        q.payload.resize(p);
        if (!read_exact(text.data(), h) || (p && !read_exact(q.payload.data(), p))) {
            log_line(0, "stdin closed inside a frame; exiting");
            quit(2);
        }
        Json header;
        std::string error;
        if (!Json::parse(text, header, error) || !header.is_object()) {
            log_line(0, "unparsable request header: " + error);
            continue;
        }
        q.id = uint32_t(header.at("id").as_double(0));
        q.method = header.at("method").as_string();
        if (header.at("params").is_object()) q.params = header.at("params");
        if (q.method.empty()) { respond_error(q.id, "bad_request", "missing method"); continue; }
        if (q.method == "ping") { respond_ok(q.id, Json::object()); continue; }
        if (q.method == "cancel") {
            try { handle_cancel(q); } catch (const std::exception& e) { respond_error(q.id, "internal", e.what()); }
            continue;
        }
        if (q.method == "shutdown") {
            respond_ok(q.id, Json::object());
            log_line(1, "shutdown requested");
            quit(0);
        }
        {
            std::lock_guard<std::mutex> lock(g_queue_mutex);
            g_queue.push_back(std::move(q));
        }
        g_queue_cv.notify_one();
    }
}

void usage() {
    std::fprintf(stderr,
                 "usage: spektralab-host --resources <engine-resources-dir> [--device <index>] "
                 "[--log-level error|info|debug]\n"
                 "Speaks desktop/HOST-PROTOCOL.md on stdin/stdout.\n");
}

}  // namespace

int main(int argc, char** argv) {
    set_binary_stdio();
    const std::vector<std::string> args = utf8_args(argc, argv);
    std::string resources;
    for (size_t i = 1; i < args.size(); ++i) {
        const std::string& a = args[i];
        const bool has_value = i + 1 < args.size();
        if (a == "--resources" && has_value) resources = args[++i];
        else if (a == "--device" && has_value) set_env("SPEKTRAFILM_VULKAN_DEVICE", args[++i]);
        else if (a == "--log-level" && has_value) {
            const std::string& v = args[++i];
            g_log_level = v == "error" ? 0 : v == "debug" ? 2 : 1;
        } else if (a == "--version") {
            std::printf("spektralab-host %s (%s)\n", SPEKTRALAB_HOST_VERSION, spk_build_info());
            return 0;
        } else if (a == "--help" || a == "-h") {
            usage();
            return 0;
        } else {
            usage();
            return 2;
        }
    }
    // Default: beside the executable, as build/host-<os>-x64/ stages it.
    if (resources.empty()) resources = path_utf8(executable_dir(argv[0]) / "engine");
    g.resources = utf8_path(resources);
    const auto t0 = Clock::now();
    g.engine = spk_engine_create(resources.c_str(), nullptr);
    if (!g.engine) {
        g.engine_error = spk_last_error(nullptr);
        log_line(0, "engine did not start: " + g.engine_error);
    } else {
        Json caps = parse_json(spk_capabilities(g.engine));
        log_line(1, "engine up in " + std::to_string(int(ms_since(t0))) + " ms on " +
                        caps.at("backend").at("gpu").as_string() + " (" +
                        caps.at("backend").at("math_mode").as_string() + ")");
    }
    std::thread work(worker);
    reader();   // never returns: exits through quit()
    work.join();
    return 0;
}
