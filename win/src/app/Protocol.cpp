#include "app/Protocol.h"

#include "base/Log.h"

#include <cmath>
#include <cstdio>
#include <fstream>
#include <functional>

namespace vela::app {
using nlohmann::json;
using namespace vela::core;

namespace {
template <typename E>
struct EnumNames { E value; const char* name; };

constexpr EnumNames<PacingMode> kPacing[] = {{PacingMode::Even, "even"}, {PacingMode::LowLatency, "lowLatency"}};
constexpr EnumNames<HdrMode> kHdrMode[] = {{HdrMode::Auto, "auto"}, {HdrMode::Off, "off"}};
constexpr EnumNames<AdaptSpeed> kSpeed[] = {{AdaptSpeed::Slow, "slow"}, {AdaptSpeed::Normal, "normal"}, {AdaptSpeed::Fast, "fast"}};
constexpr EnumNames<ContentRate> kContent[] = {{ContentRate::Auto, "auto"}, {ContentRate::Fps30, "30"},
                                                {ContentRate::Fps60, "60"}, {ContentRate::Fps120, "120"}};
constexpr EnumNames<SyncMode> kSync[] = {{SyncMode::Vrr, "vrr"}, {SyncMode::VSync, "vsync"}, {SyncMode::Tearing, "tearing"}};

template <typename E, size_t N>
const char* nameOf(const EnumNames<E> (&table)[N], E v) {
    for (const auto& e : table) if (e.value == v) return e.name;
    return table[0].name;
}
template <typename E, size_t N>
bool parse(const EnumNames<E> (&table)[N], const json& j, E& out) {
    if (!j.is_string()) return false;
    for (const auto& e : table) if (j.get<std::string>() == e.name) { out = e.value; return true; }
    return false;
}

std::string modeKey(const CaptureSettings& c) {
    if (!c.width || !c.height) return "auto";
    return std::to_string(c.width) + "x" + std::to_string(c.height) + "@" + std::to_string(int(std::lround(c.fps))) +
           (c.preferHdr ? ":p010" : ":nv12");
}
bool parseModeKey(const std::string& key, CaptureSettings& c) {
    if (key == "auto") { c.width = c.height = 0; c.fps = 0; c.preferHdr = true; return true; }
    unsigned w = 0, h = 0, f = 0;
    char fmt[8]{};
    if (std::sscanf(key.c_str(), "%ux%u@%u:%7s", &w, &h, &f, fmt) != 4) return false;
    c.width = w;
    c.height = h;
    c.fps = f;
    c.preferHdr = std::string(fmt) == "p010";
    return true;
}

// One table drives get (settingsToJson) and set (applySetting).
struct Field {
    const char* path;
    std::function<json(const Settings&)> get;
    std::function<bool(Settings&, const json&)> set;
};
template <typename T>
bool assign(const json& j, T& out) {
    if constexpr (std::is_same_v<T, bool>) { if (!j.is_boolean()) return false; }
    else if (!j.is_number()) return false;
    out = j.get<T>();
    return true;
}
#define VELA_FIELD(path, member) {path, [](const Settings& s) { return json(s.member); }, [](Settings& s, const json& j) { return assign(j, s.member); }}
#define VELA_ENUM(path, member, table) {path, [](const Settings& s) { return json(nameOf(table, s.member)); }, [](Settings& s, const json& j) { return parse(table, j, s.member); }}

const std::vector<Field>& fields() {
    static const std::vector<Field> f = {
        VELA_FIELD("frameGen.enabled", frameGen.enabled),
        VELA_FIELD("frameGen.multiplier", frameGen.multiplier),
        VELA_ENUM("frameGen.contentRate", frameGen.contentRate, kContent),
        VELA_ENUM("frameGen.pacing", frameGen.pacing, kPacing),
        VELA_FIELD("frameGen.compression", frameGen.compression),
        VELA_FIELD("frameGen.reflex", frameGen.reflex),
        VELA_FIELD("frameGen.performanceMode", frameGen.performanceMode),
        VELA_FIELD("latency.hud", latency.hud),
        VELA_FIELD("latency.hudCorner", latency.hudCorner),
        VELA_FIELD("latency.captureHardwareMs", latency.captureHardwareMs),
        VELA_FIELD("latency.displayProcessingMs", latency.displayProcessingMs),
        VELA_FIELD("superRes.enabled", superRes.enabled),
        VELA_FIELD("superRes.quality", superRes.quality),
        VELA_FIELD("superRes.sharpness", superRes.sharpness),
        VELA_ENUM("hdr.mode", hdr.mode, kHdrMode),
        VELA_FIELD("hdr.rtxHdr", hdr.rtxHdr),
        VELA_FIELD("hdr.peakNits", hdr.peakNits),
        VELA_FIELD("hdr.contrast", hdr.contrast),
        VELA_FIELD("hdr.saturation", hdr.saturation),
        VELA_FIELD("hdr.middleGray", hdr.middleGray),
        VELA_FIELD("color.enabled", color.enabled),
        VELA_FIELD("color.strength", color.strength),
        VELA_FIELD("color.levels", color.levels),
        VELA_FIELD("color.whiteBalance", color.whiteBalance),
        VELA_FIELD("color.vibrance", color.vibrance),
        VELA_ENUM("color.speed", color.speed, kSpeed),
        {"capture.device", [](const Settings& s) { return json(narrow(s.capture.devicePath)); },
         [](Settings& s, const json& j) { if (!j.is_string()) return false; s.capture.devicePath = widen(j.get<std::string>()); return true; }},
        {"capture.mode", [](const Settings& s) { return json(modeKey(s.capture)); },
         [](Settings& s, const json& j) { return j.is_string() && parseModeKey(j.get<std::string>(), s.capture); }},
        VELA_FIELD("capture.audio", capture.audio),
        VELA_FIELD("capture.volume", capture.volume),
        VELA_FIELD("capture.muted", capture.muted),
        VELA_ENUM("display.sync", display.sync, kSync),
        VELA_FIELD("display.fullscreen", display.fullscreen),
        {"display.monitor", [](const Settings& s) { return json(narrow(s.display.monitor)); },
         [](Settings& s, const json& j) { if (!j.is_string()) return false; s.display.monitor = widen(j.get<std::string>()); return true; }},
    };
    return f;
}
#undef VELA_FIELD
#undef VELA_ENUM

json summary(const RollingStats::Summary& s) { return {{"p50", s.p50}, {"p95", s.p95}, {"p99", s.p99}}; }
} // namespace

json settingsToJson(const Settings& settings) {
    json out = json::object();
    for (const auto& f : fields()) {
        const std::string path = f.path;
        const auto dot = path.find('.');
        out[path.substr(0, dot)][path.substr(dot + 1)] = f.get(settings);
    }
    return out;
}

bool applySetting(Settings& settings, const std::string& path, const json& value) {
    for (const auto& f : fields())
        if (path == f.path) {
            const bool ok = f.set(settings, value);
            settings.clamp();
            return ok;
        }
    return false;
}

Settings settingsFromJson(const json& j) {
    Settings s;
    for (const auto& f : fields()) {
        const std::string path = f.path;
        const auto dot = path.find('.');
        const auto group = path.substr(0, dot), key = path.substr(dot + 1);
        if (j.contains(group) && j[group].contains(key)) f.set(s, j[group][key]);
    }
    s.clamp();
    return s;
}

std::string stateMessage(const engine::EngineState& st) {
    json caps = {
        {"gpu", st.gpuName}, {"vramMB", st.caps.vramMB}, {"fgMaxMultiplier", st.caps.fgMaxMultiplier},
        {"videoSr", st.caps.videoSr}, {"dlssSr", st.caps.dlssSr}, {"trueHdr", st.caps.trueHdr},
        {"opticalFlow", st.caps.opticalFlow}, {"hags", st.caps.hags},
        {"dlssgVersion", st.caps.fgMaxMultiplier >= 2 ? "nvngx_dlssg.dll" : "—"}};
    json plan = {
        {"outputWidth", st.plan.outputWidth}, {"outputHeight", st.plan.outputHeight},
        {"superRes", srPathKey(st.plan.superRes)}, {"hdr", hdrPathKey(st.plan.hdr)},
        {"multipliers", st.plan.multipliers}, {"multiplier", st.plan.multiplier},
        {"contentFps", st.plan.contentFps}, {"outputFps", st.plan.outputFps}, {"frameGenBlocked", st.plan.frameGenBlocked},
        {"vramEstimateMB", st.plan.vramEstimateMB}, {"notes", st.plan.notes}};
    json source = {
        {"signal", st.source.signal}, {"width", st.source.width}, {"height", st.source.height}, {"fps", st.source.fps},
        {"hdr", st.source.hdr}, {"format", st.source.format},
        {"colorspace", st.source.hdr ? "HDR10 · BT.2020 PQ" : "SDR · BT.709"},
        {"maxCll", st.source.maxCll}, {"maxFall", st.source.maxFall}};
    json display = {
        {"width", st.display.width}, {"height", st.display.height}, {"refreshHz", st.display.refreshHz},
        {"hdr", st.display.hdr}, {"vrr", st.display.vrr}, {"peakNits", std::lround(st.displayPeakNits)}};
    json devices = json::array();
    for (const auto& d : st.devices) devices.push_back({{"path", narrow(d.link)}, {"name", narrow(d.name)}});
    json modes = json::array({{{"key", "auto"}, {"label", "自动（按信号）"}}});
    std::vector<std::string> seen;
    for (const auto& m : st.modes) {
        const auto key = m.key();
        if (std::find(seen.begin(), seen.end(), key) != seen.end()) continue;
        seen.push_back(key);
        modes.push_back({{"key", key}, {"label", std::to_string(m.width) + "×" + std::to_string(m.height) + " · " +
            std::to_string(int(std::lround(m.fps()))) + " Hz · " + (m.format == capture::PixelFormat::P010 ? "P010 HDR" : "NV12")}});
    }
    json system = json::array();
    static const char* kState[] = {"ok", "warn", "bad"};
    for (const auto& c : st.system) system.push_back({{"name", c.name}, {"state", kState[std::clamp(c.state, 0, 2)]}, {"detail", c.detail}});
    return json{{"type", "state"}, {"settings", settingsToJson(st.settings)}, {"caps", caps}, {"plan", plan},
                {"source", source}, {"display", display}, {"devices", devices}, {"modes", modes}, {"system", system}}.dump();
}

std::string telemetryMessage(const engine::EngineTelemetry& t) {
    const auto& l = t.latency;
    json stages = json::object();
    for (size_t i = 0; i < l.stages.size(); ++i) {
        const auto key = gpuStageKey(static_cast<GpuStage>(i));
        if (l.stages[i].p50 >= 0) stages[key] = {{"p50", l.stages[i].p50}, {"p95", l.stages[i].p95}};
        else stages[key] = nullptr;
    }
    json intervals = json::array();
    for (const double v : t.intervals) intervals.push_back(std::round(v * 100) / 100);
    const auto& c = t.color;
    json body = {
        {"signalFps", l.signalFps}, {"sourceFps", l.sourceFps}, {"outputFps", l.outputFps},
        {"contentFps", t.contentFps}, {"detectedFps", t.detectedFps}, {"repeats", t.repeats},
        {"pipeline", summary(l.pipeline)}, {"captureCopy", summary(l.captureCopy)}, {"gpuTotal", summary(l.gpuTotal)},
        {"presentInterval", summary(l.presentInterval)}, {"stages", stages}, {"scanoutMs", l.scanoutMs},
        {"endToEndMs", l.endToEndMs}, {"fgAddedMs", l.fgAddedMs}, {"calibrated", l.calibrated},
        {"sourceFrames", l.sourceFrames}, {"droppedSource", l.droppedSource}, {"presented", l.presented},
        {"presentedGenerated", l.presentedGenerated}, {"droppedGenerated", l.droppedGenerated},
        {"intervals", intervals},
        {"color", {{"blackPoint", c.blackPoint}, {"whitePoint", c.whitePoint}, {"gamma", c.gamma},
                   {"gainR", c.gainR}, {"gainG", c.gainG}, {"gainB", c.gainB}, {"saturation", c.saturation}}}};
    return json{{"type", "telemetry"}, {"t", body}}.dump();
}

Settings loadSettings() {
    std::ifstream f(appDataDirectory() / L"settings.json");
    if (!f) return Settings{};
    const json j = json::parse(f, nullptr, false);
    if (j.is_discarded()) {
        log::warn("settings", "settings.json unreadable; using defaults");
        return Settings{};
    }
    return settingsFromJson(j);
}

void saveSettings(const Settings& settings) {
    std::error_code ec;
    std::filesystem::create_directories(appDataDirectory(), ec);
    const auto path = appDataDirectory() / L"settings.json";
    const auto temp = appDataDirectory() / L"settings.json.tmp";
    {
        std::ofstream f(temp, std::ios::trunc);
        f << settingsToJson(settings).dump(2);
    }
    std::filesystem::rename(temp, path, ec);   // replace atomically
}

} // namespace vela::app
