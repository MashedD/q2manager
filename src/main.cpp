#include "raylib.h"

#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wmissing-field-initializers"
#pragma clang diagnostic ignored "-Wenum-compare"
#pragma clang diagnostic ignored "-Wunused-parameter"
#pragma clang diagnostic ignored "-Wunused-but-set-variable"
#elif defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#pragma GCC diagnostic ignored "-Wenum-compare"
#pragma GCC diagnostic ignored "-Wunused-parameter"
#pragma GCC diagnostic ignored "-Wunused-but-set-variable"
#endif

#define RAYGUI_IMPLEMENTATION
#include "raygui.h"

#if defined(__clang__)
#pragma clang diagnostic pop
#elif defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <string>
#include <system_error>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#else
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
#include <limits.h>
extern char **environ;
#endif

namespace fs = std::filesystem;
using json = nlohmann::json;

#ifndef Q2MANAGER_VERSION
#define Q2MANAGER_VERSION "dev"
#endif

constexpr int kMusicSampleRate = 44100;
constexpr float kPi = 3.14159265358979323846f;

struct MusicSynth {
    uint64_t sample = 0;
    float leadPhase = 0.0f;
    float leadPhase2 = 0.0f;
    float arpPhase = 0.0f;
    float bassPhase = 0.0f;
    uint32_t noise = 0x12345678u;
};

static MusicSynth gMusicSynth;

struct Preset {
    std::string name = "New preset";
    std::vector<std::string> packages;
    std::string autoexec;
    std::string extraArgs;
};

struct Settings {
    std::string enginePath;
    std::string quakeDir;
    std::string theme = "cyber";
    int selectedPreset = 0;
    std::vector<Preset> presets;
};

struct ThemePalette {
    const char *id;
    const char *label;
    Color bgTop;
    Color bgBottom;
    Color panel;
    Color panelSoft;
    Color status;
    Color accent;
    Color accent2;
    Color border;
    Color hover;
    Color text;
    Color muted;
    uint32_t guiText;
    uint32_t guiTextPressed;
    uint32_t guiBase;
    uint32_t guiFocus;
    uint32_t guiPressed;
    uint32_t guiBorder;
    uint32_t guiBorderFocus;
    uint32_t guiBorderPressed;
};

struct App;

static const ThemePalette kThemes[] = {
    {
        "cyber", "Cyber",
        Color{18, 15, 25, 255}, Color{4, 9, 14, 255}, Color{10, 12, 18, 205}, Color{23, 21, 34, 255}, Color{5, 8, 12, 230},
        Color{113, 250, 255, 255}, Color{255, 92, 214, 255}, Color{101, 233, 255, 255}, Color{58, 37, 77, 255},
        Color{220, 245, 255, 255}, Color{180, 220, 230, 255},
        0xffd8f7ff, 0xffffffff, 0x30243dff, 0x241831ff, 0x161020ff, 0x65e9ffff, 0xff55d8ff, 0x70f4ffff,
    },
    {
        "matrix", "Matrix",
        Color{1, 12, 3, 255}, Color{0, 0, 0, 255}, Color{0, 18, 7, 220}, Color{2, 32, 10, 255}, Color{0, 10, 3, 235},
        Color{76, 255, 96, 255}, Color{0, 180, 70, 255}, Color{40, 240, 80, 255}, Color{12, 70, 24, 255},
        Color{205, 255, 210, 255}, Color{120, 210, 135, 255},
        0xffcfffda, 0xffffffff, 0x08250fff, 0x0b3a16ff, 0x031807ff, 0x28f050ff, 0x4cff60ff, 0xaaffb4ff,
    },
};

static const ThemePalette &Theme(const App &app);

enum class BrowserTarget { None, Engine, QuakeDir, Package, Autoexec };
enum class TextField { None, Engine, PresetName, ExtraArgs };
enum class PendingAction { None, SwitchPreset, NewPreset, ClonePreset, DeletePreset, MovePresetUp, MovePresetDown, Quit };

struct Browser {
    bool open = false;
    BrowserTarget target = BrowserTarget::None;
    fs::path current;
    std::vector<fs::directory_entry> entries;
    int scroll = 0;
    std::string error;
};

struct App {
    Settings settings;
    Browser browser;
    int selectedPreset = 0;
    int selectedPackage = -1;
    int presetScroll = 0;
    int packageScroll = 0;
    std::vector<std::string> availablePackages;
    std::string scannedPakStore;
    TextField activeText = TextField::None;
    std::array<char, 256> engineBuf{};
    std::array<char, 128> nameBuf{};
    std::array<char, 512> argsBuf{};
    std::string status = "Set engine and Quake dir, create preset, launch.";
    bool quitRequested = false;
    bool confirmUnsavedOpen = false;
    PendingAction pendingAction = PendingAction::None;
    int pendingPresetIndex = 0;
    Font font{};
    bool customFont = false;
    Font monoFont{};
    bool customMonoFont = false;
    AudioStream music{};
    bool audioReady = false;
    bool musicPlaying = false;
};

static const ThemePalette &Theme(const App &app) {
    for (const auto &theme : kThemes) {
        if (app.settings.theme == theme.id) return theme;
    }
    return kThemes[0];
}

static float Square(float phase) {
    return std::sin(phase) >= 0.0f ? 1.0f : -1.0f;
}

static float Saw(float phase) {
    return 2.0f * (phase / (2.0f * kPi)) - 1.0f;
}

static float Noise() {
    gMusicSynth.noise = gMusicSynth.noise * 1664525u + 1013904223u;
    return static_cast<float>((gMusicSynth.noise >> 16) & 0xffffu) / 32768.0f - 1.0f;
}

static float Decay(uint64_t pos, uint64_t len) {
    if (pos >= len) return 0.0f;
    const float t = static_cast<float>(pos) / static_cast<float>(len);
    return (1.0f - t) * (1.0f - t);
}

static void MusicCallback(void *bufferData, unsigned int frames) {
    auto *out = static_cast<int16_t *>(bufferData);
    constexpr uint64_t stepSamples = kMusicSampleRate / 10; // 150-ish BPM sixteenth notes.
    constexpr std::array<float, 32> lead = {
        440.0f, 554.37f, 659.25f, 880.0f, 783.99f, 659.25f, 554.37f, 493.88f,
        523.25f, 659.25f, 783.99f, 1046.5f, 987.77f, 783.99f, 659.25f, 587.33f,
        493.88f, 622.25f, 739.99f, 987.77f, 880.0f, 739.99f, 622.25f, 554.37f,
        392.0f, 493.88f, 587.33f, 783.99f, 739.99f, 587.33f, 493.88f, 440.0f,
    };
    constexpr std::array<float, 32> arp = {
        880.0f, 0.0f, 1108.73f, 1318.51f, 0.0f, 1108.73f, 880.0f, 659.25f,
        1046.5f, 0.0f, 1318.51f, 1567.98f, 0.0f, 1318.51f, 1046.5f, 783.99f,
        987.77f, 0.0f, 1244.51f, 1479.98f, 0.0f, 1244.51f, 987.77f, 739.99f,
        783.99f, 0.0f, 987.77f, 1174.66f, 0.0f, 987.77f, 783.99f, 587.33f,
    };
    constexpr std::array<float, 16> bass = {
        110.0f, 110.0f, 164.81f, 110.0f,
        130.81f, 130.81f, 196.0f, 130.81f,
        123.47f, 123.47f, 185.0f, 123.47f,
        98.0f, 98.0f, 146.83f, 98.0f,
    };

    for (unsigned int i = 0; i < frames; ++i) {
        const uint64_t step = (gMusicSynth.sample / stepSamples) % lead.size();
        const uint64_t local = gMusicSynth.sample % stepSamples;
        const uint64_t beat = (gMusicSynth.sample / (stepSamples * 4)) % 8;
        const float phraseLift = ((gMusicSynth.sample / (stepSamples * 32)) % 2) ? 1.005f : 0.995f;
        const float vibrato = 1.0f + 0.006f * std::sin(2.0f * kPi * 5.0f * static_cast<float>(gMusicSynth.sample) / kMusicSampleRate);
        const float leadFreq = lead[static_cast<size_t>(step)] * phraseLift * vibrato;
        const float arpFreq = arp[static_cast<size_t>(step)] * phraseLift;
        const float bassFreq = bass[static_cast<size_t>((gMusicSynth.sample / (stepSamples * 2)) % bass.size())];
        const float leadGate = Decay(local, stepSamples);
        const float bassGate = Decay(gMusicSynth.sample % (stepSamples * 2), stepSamples * 2);

        gMusicSynth.leadPhase += 2.0f * kPi * leadFreq / kMusicSampleRate;
        gMusicSynth.leadPhase2 += 2.0f * kPi * (leadFreq * 1.006f) / kMusicSampleRate;
        gMusicSynth.arpPhase += 2.0f * kPi * arpFreq / kMusicSampleRate;
        gMusicSynth.bassPhase += 2.0f * kPi * bassFreq / kMusicSampleRate;
        if (gMusicSynth.leadPhase > 2.0f * kPi) gMusicSynth.leadPhase -= 2.0f * kPi;
        if (gMusicSynth.leadPhase2 > 2.0f * kPi) gMusicSynth.leadPhase2 -= 2.0f * kPi;
        if (gMusicSynth.arpPhase > 2.0f * kPi) gMusicSynth.arpPhase -= 2.0f * kPi;
        if (gMusicSynth.bassPhase > 2.0f * kPi) gMusicSynth.bassPhase -= 2.0f * kPi;

        const uint64_t beatLocal = gMusicSynth.sample % (stepSamples * 4);
        const float kick = (beatLocal < kMusicSampleRate / 20) ? Decay(beatLocal, kMusicSampleRate / 20) * std::sin(gMusicSynth.bassPhase * 0.5f + 20.0f * Decay(beatLocal, kMusicSampleRate / 20)) : 0.0f;
        const bool snareHit = (beat == 2 || beat == 6) && beatLocal < kMusicSampleRate / 18;
        const float snare = snareHit ? Noise() * Decay(beatLocal, kMusicSampleRate / 18) : 0.0f;
        const float hat = (local < kMusicSampleRate / 80) ? Noise() * Decay(local, kMusicSampleRate / 80) : 0.0f;
        const float duck = 1.0f - 0.35f * Decay(beatLocal, kMusicSampleRate / 12);

        const float leadMix = (0.11f * Square(gMusicSynth.leadPhase) + 0.05f * Saw(gMusicSynth.leadPhase2)) * leadGate * duck;
        const float arpMix = (arpFreq > 0.0f ? 0.05f * Square(gMusicSynth.arpPhase) * Decay(local, stepSamples / 2) : 0.0f) * duck;
        const float bassMix = 0.13f * Square(gMusicSynth.bassPhase) * bassGate;
        const float drumMix = 0.20f * kick + 0.08f * snare + 0.035f * hat;
        const float sample = leadMix + arpMix + bassMix + drumMix;
        const auto pcm = static_cast<int16_t>(std::clamp(sample, -0.9f, 0.9f) * 32767.0f);
        out[i * 2] = pcm;
        out[i * 2 + 1] = pcm;
        ++gMusicSynth.sample;
    }
}

static fs::path ExecutableDir() {
#ifdef _WIN32
    std::array<char, MAX_PATH> path{};
    const DWORD len = GetModuleFileNameA(nullptr, path.data(), static_cast<DWORD>(path.size()));
    if (len > 0) return fs::path(std::string(path.data(), len)).parent_path();
#else
    std::array<char, PATH_MAX> path{};
    const ssize_t len = readlink("/proc/self/exe", path.data(), path.size() - 1);
    if (len > 0) return fs::path(std::string(path.data(), static_cast<size_t>(len))).parent_path();
#endif
    return fs::current_path();
}

static fs::path ConfigPath() { return ExecutableDir() / "q2manager.json"; }

static fs::path ExistingAbsolutePath(const fs::path &path);

static bool IsParentRelativeEscape(const fs::path &path) {
    for (const auto &part : path) {
        if (part == "..") return true;
    }
    return false;
}

static fs::path ResolveGamePath(const std::string &value) {
    fs::path path(value);
    if (path.empty()) return path;
    if (path.is_relative()) path = ExecutableDir() / path;
    return ExistingAbsolutePath(path);
}

static std::string PortableGamePath(const std::string &value) {
    if (value.empty()) return {};
    std::error_code ec;
    const fs::path base = ExistingAbsolutePath(ExecutableDir());
    const fs::path path = ExistingAbsolutePath(value);
    const fs::path rel = fs::relative(path, base, ec);
    if (!ec && !rel.empty() && !IsParentRelativeEscape(rel)) return rel.generic_string();
    return path.string();
}

static fs::path PakStoreDir(const std::string &quakeDir) {
    return fs::path(quakeDir) / ".q2manager" / "paks";
}

static fs::path ExistingAbsolutePath(const fs::path &path) {
    std::error_code ec;
    fs::path full = fs::weakly_canonical(fs::absolute(path), ec);
    return ec ? fs::absolute(path) : full;
}

static void CopyToBuf(const std::string &value, char *buf, size_t size) {
    std::fill(buf, buf + size, '\0');
    value.copy(buf, size - 1);
}

static bool HasExt(const fs::path &path, const std::vector<std::string> &exts) {
    std::string ext = path.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return std::find(exts.begin(), exts.end(), ext) != exts.end();
}

static bool IsInsideDir(const fs::path &path, const fs::path &dir) {
    const fs::path fullPath = ExistingAbsolutePath(path);
    const fs::path fullDir = ExistingAbsolutePath(dir);
    auto pathIt = fullPath.begin();
    for (auto dirIt = fullDir.begin(); dirIt != fullDir.end(); ++dirIt, ++pathIt) {
        if (pathIt == fullPath.end() || *pathIt != *dirIt) return false;
    }
    return true;
}

static std::string PathKey(const fs::path &path) {
    return ExistingAbsolutePath(path).generic_string();
}

static std::string ResolveGamePathString(const std::string &value) {
    return ResolveGamePath(value).generic_string();
}

static std::string DisplayPakName(const fs::path &path) {
    std::string filename = path.filename().string();
    if (filename.rfind("q2dl-", 0) == 0) filename.erase(0, 5);
    if (filename.size() >= 4 && filename.substr(filename.size() - 4) == ".pkz") filename.erase(filename.size() - 4);
    return filename;
}

static void ScanPakStore(App &app) {
    app.availablePackages.clear();
    const fs::path pakStore = PakStoreDir(app.settings.quakeDir);
    app.scannedPakStore = ExistingAbsolutePath(pakStore).string();
    std::error_code ec;
    fs::create_directories(pakStore, ec);
    if (ec) { app.status = "Create pak store failed: " + ec.message(); return; }
    for (const auto &entry : fs::recursive_directory_iterator(pakStore, ec)) {
        if (entry.is_regular_file(ec) && HasExt(entry.path(), {".pak", ".pkz"})) {
            app.availablePackages.push_back(PathKey(entry.path()));
        }
    }
    if (ec) { app.status = "Scan pak store failed: " + ec.message(); return; }
    std::sort(app.availablePackages.begin(), app.availablePackages.end(), [](const std::string &a, const std::string &b) {
        return fs::path(a).filename().string() < fs::path(b).filename().string();
    });
    app.packageScroll = 0;
}

static void EnsurePreset(App &app) {
    if (app.settings.presets.empty()) app.settings.presets.push_back({"Vanilla", {}, {}, {}});
    app.selectedPreset = std::clamp(app.selectedPreset, 0, static_cast<int>(app.settings.presets.size()) - 1);
    app.settings.selectedPreset = app.selectedPreset;
}

static Preset &CurrentPreset(App &app) {
    EnsurePreset(app);
    return app.settings.presets[static_cast<size_t>(app.selectedPreset)];
}

static void SaveSettings(const App &app);

static void SyncBuffers(App &app) {
    EnsurePreset(app);
    CopyToBuf(app.settings.enginePath, app.engineBuf.data(), app.engineBuf.size());
    CopyToBuf(CurrentPreset(app).name, app.nameBuf.data(), app.nameBuf.size());
    CopyToBuf(CurrentPreset(app).extraArgs, app.argsBuf.data(), app.argsBuf.size());
}

static bool PresetHasBufferedChanges(App &app) {
    Preset &preset = CurrentPreset(app);
    return preset.name != app.nameBuf.data() || preset.extraArgs != app.argsBuf.data();
}

static void CommitCurrentPresetBuffers(App &app) {
    app.settings.enginePath = app.engineBuf.data();
    app.settings.quakeDir = ExecutableDir().string();
    Preset &preset = CurrentPreset(app);
    preset.name = app.nameBuf.data();
    preset.extraArgs = app.argsBuf.data();
    app.settings.selectedPreset = app.selectedPreset;
    SaveSettings(app);
}

static void DiscardCurrentPresetBuffers(App &app) {
    SyncBuffers(app);
}

static void LoadReadableFont(App &app) {
    const std::array<fs::path, 8> candidates = {
        "/usr/share/fonts/TTF/DejaVuSans.ttf",
        "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
        "/usr/share/fonts/liberation/LiberationSans-Regular.ttf",
        "/usr/share/fonts/liberation2/LiberationSans-Regular.ttf",
        "/usr/share/fonts/noto/NotoSans-Regular.ttf",
        "C:/Windows/Fonts/segoeui.ttf",
        "C:/Windows/Fonts/arial.ttf",
        "C:/Windows/Fonts/calibri.ttf",
    };

    for (const auto &path : candidates) {
        if (!fs::is_regular_file(path)) continue;
        Font font = LoadFontEx(path.string().c_str(), 24, nullptr, 0);
        if (font.texture.id == 0) continue;
        app.font = font;
        app.customFont = true;
        SetTextureFilter(app.font.texture, TEXTURE_FILTER_BILINEAR);
        GuiSetFont(app.font);
        return;
    }
    app.font = GetFontDefault();
}

static void LoadMonospaceFont(App &app) {
    const std::array<fs::path, 9> candidates = {
        "/usr/share/fonts/TTF/DejaVuSansMono.ttf",
        "/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf",
        "/usr/share/fonts/liberation/LiberationMono-Regular.ttf",
        "/usr/share/fonts/liberation2/LiberationMono-Regular.ttf",
        "/usr/share/fonts/noto/NotoSansMono-Regular.ttf",
        "C:/Windows/Fonts/consola.ttf",
        "C:/Windows/Fonts/cour.ttf",
        "C:/Windows/Fonts/lucon.ttf",
        "C:/Windows/Fonts/cascadiamono.ttf",
    };

    for (const auto &path : candidates) {
        if (!fs::is_regular_file(path)) continue;
        Font font = LoadFontEx(path.string().c_str(), 24, nullptr, 0);
        if (font.texture.id == 0) continue;
        app.monoFont = font;
        app.customMonoFont = true;
        SetTextureFilter(app.monoFont.texture, TEXTURE_FILTER_BILINEAR);
        return;
    }
    app.monoFont = app.customFont ? app.font : GetFontDefault();
}

static void InitMusic(App &app) {
    InitAudioDevice();
    if (!IsAudioDeviceReady()) {
        app.status = "Audio device unavailable";
        return;
    }
    app.music = LoadAudioStream(kMusicSampleRate, 16, 2);
    SetAudioStreamCallback(app.music, MusicCallback);
    SetAudioStreamVolume(app.music, 0.45f);
    app.audioReady = true;
}

static void ToggleMusic(App &app) {
    if (!app.audioReady) {
        app.status = "Audio device unavailable";
        return;
    }
    if (app.musicPlaying) {
        PauseAudioStream(app.music);
        app.musicPlaying = false;
    } else {
        if (IsAudioStreamPlaying(app.music)) ResumeAudioStream(app.music);
        else PlayAudioStream(app.music);
        app.musicPlaying = true;
    }
}

static void DrawAppText(const App &app, const std::string &text, int x, int y, float size, Color color) {
    DrawTextEx(app.customFont ? app.font : GetFontDefault(), text.c_str(), {static_cast<float>(x), static_cast<float>(y)}, size, 1.0f, color);
}

static void DrawMonoText(const App &app, const std::string &text, int x, int y, float size, Color color) {
    DrawTextEx(app.customMonoFont ? app.monoFont : (app.customFont ? app.font : GetFontDefault()), text.c_str(), {static_cast<float>(x), static_cast<float>(y)}, size, 1.0f, color);
}

static float TextWidth(const App &app, const std::string &text, float size) {
    return MeasureTextEx(app.customFont ? app.font : GetFontDefault(), text.c_str(), size, 1.0f).x;
}

static std::string FitMiddleText(const App &app, const std::string &text, float maxWidth, float size) {
    if (TextWidth(app, text, size) <= maxWidth) return text;
    if (TextWidth(app, "...", size) > maxWidth) return "";

    size_t left = std::min<size_t>(text.size(), 8);
    size_t right = std::min<size_t>(text.size() - left, 8);
    std::string best = text.substr(0, left) + "..." + text.substr(text.size() - right);

    while (left + right + 3 < text.size()) {
        std::string candidate;
        if (left <= right) {
            candidate = text.substr(0, left + 1) + "..." + text.substr(text.size() - right);
            if (TextWidth(app, candidate, size) <= maxWidth) {
                ++left;
                best = std::move(candidate);
                continue;
            }
        }
        candidate = text.substr(0, left) + "..." + text.substr(text.size() - right - 1);
        if (right + 1 <= text.size() - left && TextWidth(app, candidate, size) <= maxWidth) {
            ++right;
            best = std::move(candidate);
            continue;
        }
        break;
    }

    while (TextWidth(app, best, size) > maxWidth && (left > 1 || right > 1)) {
        if (left >= right && left > 1) --left;
        else if (right > 1) --right;
        best = text.substr(0, left) + "..." + text.substr(text.size() - right);
    }
    return best;
}

static void LoadSettings(App &app) {
    std::ifstream in(ConfigPath());
    if (!in) {
        app.settings.presets.push_back({"Vanilla", {}, {}, {}});
        SyncBuffers(app);
        return;
    }

    json j;
    try {
        in >> j;
        app.settings.enginePath = j.value("engine", "");
        app.settings.theme = j.value("theme", "cyber");
        app.settings.selectedPreset = j.value("selected_preset", 0);
        for (const auto &p : j.value("presets", json::array())) {
            Preset preset;
            preset.name = p.value("name", "Preset");
            preset.autoexec = p.value("autoexec", "");
            if (!preset.autoexec.empty()) preset.autoexec = ResolveGamePathString(preset.autoexec);
            preset.extraArgs = p.value("extra_args", "");
            preset.packages = p.value("packages", std::vector<std::string>{});
            for (auto &pkg : preset.packages) pkg = ResolveGamePathString(pkg);
            app.settings.presets.push_back(std::move(preset));
        }
        app.status = "Loaded q2manager.json";
    } catch (const std::exception &e) {
        app.status = std::string("Config load failed: ") + e.what();
    }
    app.selectedPreset = app.settings.selectedPreset;
    EnsurePreset(app);
    SyncBuffers(app);
}

static void SaveSettings(const App &app) {
    json presets = json::array();
    for (const auto &p : app.settings.presets) {
        std::vector<std::string> packages;
        packages.reserve(p.packages.size());
        for (const auto &pkg : p.packages) packages.push_back(PortableGamePath(pkg));
        presets.push_back({
            {"name", p.name},
            {"packages", packages},
            {"autoexec", PortableGamePath(p.autoexec)},
            {"extra_args", p.extraArgs},
        });
    }
    json j = {
        {"engine", app.settings.enginePath},
        {"theme", app.settings.theme},
        {"selected_preset", app.selectedPreset},
        {"presets", presets},
    };
    std::ofstream out(ConfigPath());
    out << j.dump(2) << '\n';
}

static void RefreshBrowser(Browser &browser) {
    browser.entries.clear();
    browser.error.clear();
    std::error_code ec;
    if (browser.current.empty()) browser.current = fs::current_path(ec);
    for (const auto &entry : fs::directory_iterator(browser.current, ec)) browser.entries.push_back(entry);
    if (ec) browser.error = ec.message();
    std::sort(browser.entries.begin(), browser.entries.end(), [](const auto &a, const auto &b) {
        if (a.is_directory() != b.is_directory()) return a.is_directory() > b.is_directory();
        return a.path().filename().string() < b.path().filename().string();
    });
    browser.scroll = 0;
}

static void OpenBrowser(App &app, BrowserTarget target, const fs::path &start) {
    app.browser.open = true;
    app.browser.target = target;
    if (target == BrowserTarget::Package) {
        std::error_code ec;
        fs::create_directories(PakStoreDir(app.settings.quakeDir), ec);
        if (ec) app.status = "Create pak store failed: " + ec.message();
    }
    app.browser.current = fs::is_directory(start) ? start : start.parent_path();
    if (app.browser.current.empty()) app.browser.current = fs::current_path();
    RefreshBrowser(app.browser);
}

static bool AcceptsTarget(BrowserTarget target, const fs::path &path) {
    if (target == BrowserTarget::QuakeDir) return fs::is_directory(path);
    if (target == BrowserTarget::Package) return fs::is_regular_file(path) && HasExt(path, {".pak", ".pkz"});
    if (target == BrowserTarget::Autoexec) return fs::is_regular_file(path) && HasExt(path, {".cfg"});
    if (target == BrowserTarget::Engine) return fs::is_regular_file(path);
    return false;
}

static void SelectBrowserPath(App &app, const fs::path &path) {
    if (!AcceptsTarget(app.browser.target, path)) return;
    if (app.browser.target == BrowserTarget::Package && !IsInsideDir(path, PakStoreDir(app.settings.quakeDir))) {
        app.status = "Packages must be in " + PakStoreDir(app.settings.quakeDir).string();
        return;
    }
    Preset &preset = CurrentPreset(app);
    const std::string value = PathKey(path);
    switch (app.browser.target) {
        case BrowserTarget::Engine: app.settings.enginePath = value; break;
        case BrowserTarget::QuakeDir: app.settings.quakeDir = value; break;
        case BrowserTarget::Package: preset.packages.push_back(value); app.selectedPackage = static_cast<int>(preset.packages.size()) - 1; break;
        case BrowserTarget::Autoexec: preset.autoexec = value; break;
        default: break;
    }
    app.browser.open = false;
    SyncBuffers(app);
    SaveSettings(app);
}

static bool RecreateSymlink(const fs::path &target, const fs::path &link, std::string &error) {
    std::error_code ec;
    fs::remove(link, ec);
    ec.clear();
    const fs::path absoluteTarget = ExistingAbsolutePath(target);
    const fs::path absoluteLinkParent = ExistingAbsolutePath(link.parent_path());
    fs::path linkTarget = fs::relative(absoluteTarget, absoluteLinkParent, ec);
    if (ec) {
        ec.clear();
        linkTarget = absoluteTarget;
    }
    fs::create_symlink(linkTarget, link, ec);
    if (ec) {
        error = "Symlink failed: " + link.string() + " -> " + linkTarget.string() + ": " + ec.message();
        return false;
    }
    return true;
}

static std::string DateStamp() {
    const auto now = std::chrono::system_clock::now();
    const std::time_t t = std::chrono::system_clock::to_time_t(now);
    std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    std::ostringstream out;
    out << std::put_time(&tm, "%Y-%m-%d");
    return out.str();
}

static bool BackupExistingAutoexec(const fs::path &autoexec, const fs::path &marker, std::string &error) {
    std::error_code ec;
    if (!fs::exists(autoexec, ec)) return true;
    if (fs::exists(marker, ec) && fs::is_symlink(autoexec, ec)) {
        fs::remove(autoexec, ec);
        if (ec) { error = "Remove old autoexec symlink failed: " + ec.message(); return false; }
        return true;
    }

    fs::path backup = autoexec;
    backup += "." + DateStamp();
    for (int i = 1; fs::exists(backup, ec); ++i) {
        backup = autoexec;
        backup += "." + DateStamp() + "." + std::to_string(i);
    }
    fs::rename(autoexec, backup, ec);
    if (ec) { error = "Backup autoexec failed: " + ec.message(); return false; }
    return true;
}

static std::vector<std::string> SplitArgs(const std::string &text) {
    std::vector<std::string> args;
    std::string cur;
    bool quoted = false;
    for (char c : text) {
        if (c == '"') { quoted = !quoted; continue; }
        if (std::isspace(static_cast<unsigned char>(c)) && !quoted) {
            if (!cur.empty()) { args.push_back(cur); cur.clear(); }
        } else {
            cur.push_back(c);
        }
    }
    if (!cur.empty()) args.push_back(cur);
    return args;
}

static bool PrepareBaseq2Links(const App &app, const Preset &preset, fs::path &baseq2Dir, std::string &error) {
    const fs::path quakeDir = ExistingAbsolutePath(app.settings.quakeDir);
    if (!fs::is_directory(quakeDir)) { error = "Quake dir invalid"; return false; }
    baseq2Dir = quakeDir / "baseq2";
    const fs::path pakStore = quakeDir / ".q2manager" / "paks";

    std::error_code ec;
    if (!fs::is_directory(baseq2Dir)) { error = "baseq2 dir missing: " + baseq2Dir.string(); return false; }
    fs::create_directories(pakStore, ec);
    if (ec) { error = "Create pak store failed: " + ec.message(); return false; }

    for (const auto &entry : fs::directory_iterator(baseq2Dir, ec)) {
        if (entry.path().filename().string().rfind("q2m_", 0) == 0) {
            fs::remove(entry.path(), ec);
            if (ec) { error = "Clean baseq2 links failed: " + ec.message(); return false; }
        }
    }

    for (size_t i = 0; i < preset.packages.size(); ++i) {
        const auto &pkg = preset.packages[i];
        fs::path source = ExistingAbsolutePath(pkg);
        if (!fs::is_regular_file(source)) { error = "Package missing: " + pkg; return false; }
        if (!IsInsideDir(source, pakStore)) { error = "Package outside pak store: " + pkg; return false; }
        std::ostringstream name;
        name << "q2m_";
        if (i < 10) name << '0';
        name << i << '_' << source.filename().string();
        if (!RecreateSymlink(source, baseq2Dir / name.str(), error)) return false;
    }
    if (!preset.autoexec.empty()) {
        fs::path source = ExistingAbsolutePath(preset.autoexec);
        if (!fs::is_regular_file(source)) { error = "Autoexec missing: " + preset.autoexec; return false; }
        const fs::path link = baseq2Dir / "autoexec.cfg";
        const fs::path marker = baseq2Dir / ".q2manager_autoexec";
        if (!BackupExistingAutoexec(link, marker, error)) return false;
        if (!RecreateSymlink(source, link, error)) return false;
        std::ofstream(marker) << source.string() << '\n';
    } else {
        const fs::path link = baseq2Dir / "autoexec.cfg";
        const fs::path marker = baseq2Dir / ".q2manager_autoexec";
        if (fs::exists(marker, ec) && fs::is_symlink(link, ec)) {
            fs::remove(link, ec);
            if (ec) { error = "Remove managed autoexec failed: " + ec.message(); return false; }
            fs::remove(marker, ec);
        }
    }
    return true;
}

#ifdef _WIN32
static std::string Quote(const std::string &s) {
    if (s.find_first_of(" \t\"") == std::string::npos) return s;
    std::string out = "\"";
    for (char c : s) out += (c == '"') ? "\\\"" : std::string(1, c);
    out += "\"";
    return out;
}
#endif

static bool LaunchProcess(const std::vector<std::string> &args, const fs::path &cwd, std::string &error) {
    if (args.empty()) { error = "No engine path"; return false; }
#ifdef _WIN32
    std::ostringstream cmd;
    for (size_t i = 0; i < args.size(); ++i) {
        if (i) cmd << ' ';
        cmd << Quote(args[i]);
    }
    std::string cmdStr = cmd.str();
    STARTUPINFOA si{};
    PROCESS_INFORMATION pi{};
    si.cb = sizeof(si);
    BOOL ok = CreateProcessA(nullptr, cmdStr.data(), nullptr, nullptr, FALSE, 0, nullptr,
                             cwd.string().c_str(), &si, &pi);
    if (!ok) { error = "CreateProcess failed: " + std::to_string(GetLastError()); return false; }
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    return true;
#else
    std::vector<char *> argv;
    argv.reserve(args.size() + 1);
    for (const auto &arg : args) argv.push_back(const_cast<char *>(arg.c_str()));
    argv.push_back(nullptr);
    pid_t pid = 0;
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawnattr_t attr;
    posix_spawnattr_init(&attr);
    const fs::path old = fs::current_path();
    std::error_code ec;
    fs::current_path(cwd, ec);
    if (ec) { error = "chdir failed: " + ec.message(); return false; }
    const int rc = posix_spawn(&pid, args[0].c_str(), &actions, &attr, argv.data(), environ);
    fs::current_path(old, ec);
    posix_spawn_file_actions_destroy(&actions);
    posix_spawnattr_destroy(&attr);
    if (rc != 0) { error = "posix_spawn failed: " + std::to_string(rc); return false; }
    return true;
#endif
}

static void LaunchPreset(App &app) {
    app.settings.enginePath = app.engineBuf.data();
    app.settings.quakeDir = ExecutableDir().string();
    Preset &preset = CurrentPreset(app);
    preset.name = app.nameBuf.data();
    preset.extraArgs = app.argsBuf.data();
    SaveSettings(app);

    fs::path baseq2Dir;
    std::string error;
    if (!PrepareBaseq2Links(app, preset, baseq2Dir, error)) { app.status = error; return; }

    std::vector<std::string> args = {app.settings.enginePath, "+set", "basedir", app.settings.quakeDir, "+set", "game", "baseq2"};
    if (!preset.autoexec.empty()) {
        args.push_back("+exec");
        args.push_back("autoexec.cfg");
    }
    auto extra = SplitArgs(preset.extraArgs);
    args.insert(args.end(), extra.begin(), extra.end());

    if (!fs::is_regular_file(app.settings.enginePath)) { app.status = "Engine path invalid"; return; }
    if (!LaunchProcess(args, app.settings.quakeDir, error)) { app.status = error; return; }
    app.status = "Launched " + preset.name + " using " + baseq2Dir.string();
}

static void ExecutePendingAction(App &app) {
    constexpr int visiblePresets = 7;
    switch (app.pendingAction) {
        case PendingAction::SwitchPreset:
            app.selectedPreset = std::clamp(app.pendingPresetIndex, 0, static_cast<int>(app.settings.presets.size()) - 1);
            app.settings.selectedPreset = app.selectedPreset;
            app.selectedPackage = -1;
            SyncBuffers(app);
            SaveSettings(app);
            break;
        case PendingAction::NewPreset:
            app.settings.presets.push_back({"Preset " + std::to_string(app.settings.presets.size() + 1), {}, {}, {}});
            app.selectedPreset = static_cast<int>(app.settings.presets.size()) - 1;
            app.settings.selectedPreset = app.selectedPreset;
            app.presetScroll = std::max(0, static_cast<int>(app.settings.presets.size()) - visiblePresets);
            app.selectedPackage = -1;
            SyncBuffers(app);
            SaveSettings(app);
            break;
        case PendingAction::ClonePreset: {
            Preset clone = CurrentPreset(app);
            clone.name += " copy";
            app.settings.presets.push_back(std::move(clone));
            app.selectedPreset = static_cast<int>(app.settings.presets.size()) - 1;
            app.settings.selectedPreset = app.selectedPreset;
            app.presetScroll = std::max(0, static_cast<int>(app.settings.presets.size()) - visiblePresets);
            app.selectedPackage = -1;
            SyncBuffers(app);
            SaveSettings(app);
            break;
        }
        case PendingAction::DeletePreset:
            if (app.settings.presets.size() > 1) {
                app.settings.presets.erase(app.settings.presets.begin() + app.selectedPreset);
                app.selectedPreset = 0;
                app.settings.selectedPreset = 0;
                app.presetScroll = 0;
                app.selectedPackage = -1;
                SyncBuffers(app);
                SaveSettings(app);
            }
            break;
        case PendingAction::MovePresetUp:
            if (app.selectedPreset > 0) {
                std::swap(app.settings.presets[app.selectedPreset], app.settings.presets[app.selectedPreset - 1]);
                --app.selectedPreset;
                app.settings.selectedPreset = app.selectedPreset;
                if (app.selectedPreset < app.presetScroll) app.presetScroll = app.selectedPreset;
                SyncBuffers(app);
                SaveSettings(app);
            }
            break;
        case PendingAction::MovePresetDown:
            if (app.selectedPreset + 1 < static_cast<int>(app.settings.presets.size())) {
                std::swap(app.settings.presets[app.selectedPreset], app.settings.presets[app.selectedPreset + 1]);
                ++app.selectedPreset;
                app.settings.selectedPreset = app.selectedPreset;
                if (app.selectedPreset >= app.presetScroll + visiblePresets) app.presetScroll = app.selectedPreset - visiblePresets + 1;
                SyncBuffers(app);
                SaveSettings(app);
            }
            break;
        case PendingAction::Quit:
            app.quitRequested = true;
            break;
        case PendingAction::None:
            break;
    }
    app.pendingAction = PendingAction::None;
    app.pendingPresetIndex = 0;
}

static void RequestAction(App &app, PendingAction action, int presetIndex = 0) {
    app.pendingAction = action;
    app.pendingPresetIndex = presetIndex;
    if (PresetHasBufferedChanges(app)) {
        app.confirmUnsavedOpen = true;
        return;
    }
    ExecutePendingAction(app);
}

static void DrawUnsavedModal(App &app) {
    if (!app.confirmUnsavedOpen) return;
    const auto &theme = Theme(app);
    DrawRectangle(0, 0, GetScreenWidth(), GetScreenHeight(), Color{0, 0, 0, 150});
    Rectangle box = {210, 192, 380, 152};
    DrawRectangleRec(box, theme.panelSoft);
    DrawRectangleLinesEx(box, 2, theme.accent);
    DrawAppText(app, "Unsaved preset changes", 238, 216, 20, theme.accent);
    DrawAppText(app, "Save changes before continuing?", 238, 248, 15, theme.text);

    if (GuiButton({238, 292, 92, 30}, "Save")) {
        CommitCurrentPresetBuffers(app);
        app.confirmUnsavedOpen = false;
        ExecutePendingAction(app);
    }
    if (GuiButton({354, 292, 92, 30}, "Discard")) {
        DiscardCurrentPresetBuffers(app);
        app.confirmUnsavedOpen = false;
        ExecutePendingAction(app);
    }
    if (GuiButton({470, 292, 92, 30}, "Cancel")) {
        app.confirmUnsavedOpen = false;
        app.pendingAction = PendingAction::None;
        app.pendingPresetIndex = 0;
    }
}

static void DrawBackground(const App &app) {
    const auto &theme = Theme(app);
    ClearBackground(theme.bgTop);
    DrawRectangleGradientV(0, 0, GetScreenWidth(), GetScreenHeight(), theme.bgTop, theme.bgBottom);
    for (int y = 0; y < GetScreenHeight(); y += 6) DrawRectangle(0, y, GetScreenWidth(), 1, Color{255, 255, 255, 14});
    for (int i = 0; i < 10; ++i) {
        Color ring = theme.accent;
        ring.a = static_cast<unsigned char>(24 - i);
        DrawCircleLines(620 + i * 12, 82 + i * 9, 76 + i * 13, ring);
    }
    DrawAppText(app, "Q2 MOD MANAGER", 28, 20, 30, theme.accent);
    DrawAppText(app, "q2pro preset symlink launcher", 31, 58, 15, theme.accent2);
    DrawAppText(app, std::string("version ") + Q2MANAGER_VERSION, 31, 74, 12, theme.muted);
}

static void ApplyStyle(const App &app) {
    const auto &theme = Theme(app);
    GuiSetStyle(DEFAULT, TEXT_SIZE, 16);
    GuiSetStyle(DEFAULT, TEXT_COLOR_NORMAL, static_cast<int>(theme.guiText));
    GuiSetStyle(DEFAULT, TEXT_COLOR_FOCUSED, static_cast<int>(theme.guiTextPressed));
    GuiSetStyle(DEFAULT, TEXT_COLOR_PRESSED, static_cast<int>(theme.guiTextPressed));
    GuiSetStyle(DEFAULT, BASE_COLOR_NORMAL, static_cast<int>(theme.guiBase));
    GuiSetStyle(DEFAULT, BASE_COLOR_FOCUSED, static_cast<int>(theme.guiFocus));
    GuiSetStyle(DEFAULT, BASE_COLOR_PRESSED, static_cast<int>(theme.guiPressed));
    GuiSetStyle(DEFAULT, BORDER_COLOR_NORMAL, static_cast<int>(theme.guiBorder));
    GuiSetStyle(DEFAULT, BORDER_COLOR_FOCUSED, static_cast<int>(theme.guiBorderFocus));
    GuiSetStyle(DEFAULT, BORDER_COLOR_PRESSED, static_cast<int>(theme.guiBorderPressed));
}

static void DrawBrowser(App &app) {
    const auto &theme = Theme(app);
    Browser &b = app.browser;
    DrawRectangle(54, 72, 692, 430, theme.status);
    DrawRectangleLinesEx({54, 72, 692, 430}, 2, theme.accent);
    DrawAppText(app, b.current.string(), 72, 88, 14, theme.text);
    if (GuiButton({650, 84, 72, 24}, "Close")) b.open = false;
    if (GuiButton({72, 118, 72, 24}, "Up")) {
        if (b.current.has_parent_path()) b.current = b.current.parent_path();
        RefreshBrowser(b);
    }
    if (b.target == BrowserTarget::QuakeDir && GuiButton({154, 118, 130, 24}, "Use This Dir")) SelectBrowserPath(app, b.current);
    if (!b.error.empty()) DrawAppText(app, b.error, 300, 122, 12, RED);

    Rectangle list = {72, 154, 650, 316};
    DrawRectangleRec(list, theme.bgTop);
    const int rowH = 24;
    const int visible = static_cast<int>(list.height) / rowH;
    if (!app.confirmUnsavedOpen) b.scroll += static_cast<int>(-GetMouseWheelMove());
    b.scroll = std::clamp(b.scroll, 0, std::max(0, static_cast<int>(b.entries.size()) - visible));
    for (int i = 0; i < visible && b.scroll + i < static_cast<int>(b.entries.size()); ++i) {
        const auto &entry = b.entries[static_cast<size_t>(b.scroll + i)];
        Rectangle row = {list.x, list.y + i * rowH, list.width, static_cast<float>(rowH)};
        const bool hover = CheckCollisionPointRec(GetMousePosition(), row);
        if (hover) DrawRectangleRec(row, theme.hover);
        const std::string label = std::string(entry.is_directory() ? "[DIR] " : "      ") + entry.path().filename().string();
        DrawAppText(app, label, static_cast<int>(row.x + 8), static_cast<int>(row.y + 4), 15,
                    AcceptsTarget(b.target, entry.path()) || entry.is_directory() ? theme.text : theme.muted);
        if (!app.confirmUnsavedOpen && hover && IsMouseButtonPressed(MOUSE_LEFT_BUTTON)) {
            if (entry.is_directory()) { b.current = entry.path(); RefreshBrowser(b); }
            else SelectBrowserPath(app, entry.path());
        }
    }
}

static void DrawTextInput(App &app, Rectangle bounds, char *text, int textSize, TextField field) {
    if (GuiTextBox(bounds, text, textSize, app.activeText == field)) {
        app.activeText = (app.activeText == field) ? TextField::None : field;
    }
}

static void DrawPackageToggleList(App &app, Preset &preset) {
    const auto &theme = Theme(app);
    const fs::path pakStore = PakStoreDir(app.settings.quakeDir);
    const std::string currentStore = ExistingAbsolutePath(pakStore).string();
    if (app.scannedPakStore != currentStore) ScanPakStore(app);

    DrawAppText(app, "PACKAGES", 276, 184, 15, theme.accent);
    if (GuiButton({574, 180, 74, 22}, "Refresh")) ScanPakStore(app);

    Rectangle list = {276, 204, 372, 202};
    DrawRectangleRec(list, theme.bgTop);
    const int rowH = 24;
    const int visible = static_cast<int>(list.height) / rowH;
    std::unordered_map<std::string, int> activeIndexByPath;
    activeIndexByPath.reserve(preset.packages.size());
    for (int i = 0; i < static_cast<int>(preset.packages.size()); ++i) {
        activeIndexByPath.emplace(preset.packages[static_cast<size_t>(i)], i);
    }
    std::unordered_set<std::string> availablePaths;
    availablePaths.reserve(app.availablePackages.size());
    for (const auto &pkg : app.availablePackages) availablePaths.insert(pkg);

    std::vector<std::string> missingPackages;
    for (const auto &pkg : preset.packages) {
        if (!availablePaths.contains(pkg)) {
            missingPackages.push_back(pkg);
        }
    }
    const int missingCount = static_cast<int>(missingPackages.size());
    const int totalRows = static_cast<int>(app.availablePackages.size()) + missingCount;
    if (!app.confirmUnsavedOpen && CheckCollisionPointRec(GetMousePosition(), list)) {
        app.packageScroll += static_cast<int>(-GetMouseWheelMove());
    }
    app.packageScroll = std::clamp(app.packageScroll, 0, std::max(0, totalRows - visible));

    const int maxScroll = std::max(0, totalRows - visible);
    Rectangle scrollTrack = {276, 410, 372, 12};
    DrawRectangleRec(scrollTrack, theme.panelSoft);
    DrawRectangleLinesEx(scrollTrack, 1, theme.border);
    const float thumbWidth = maxScroll > 0 ? std::max(42.0f, scrollTrack.width * static_cast<float>(visible) / static_cast<float>(totalRows)) : scrollTrack.width;
    const float thumbX = maxScroll > 0 ? scrollTrack.x + (scrollTrack.width - thumbWidth) * static_cast<float>(app.packageScroll) / static_cast<float>(maxScroll) : scrollTrack.x;
    Rectangle scrollThumb = {thumbX, scrollTrack.y + 2, thumbWidth, scrollTrack.height - 4};
    DrawRectangleRec(scrollThumb, theme.accent2);
    if (!app.confirmUnsavedOpen && maxScroll > 0 && CheckCollisionPointRec(GetMousePosition(), scrollTrack) && IsMouseButtonDown(MOUSE_LEFT_BUTTON)) {
        const float t = std::clamp((GetMouseX() - scrollTrack.x - thumbWidth * 0.5f) / (scrollTrack.width - thumbWidth), 0.0f, 1.0f);
        app.packageScroll = static_cast<int>(std::round(t * static_cast<float>(maxScroll)));
    }

    int drawn = 0;
    int rowIndex = 0;
    auto drawRow = [&](const std::string &path, bool missing) {
        if (rowIndex++ < app.packageScroll || drawn >= visible) return;
        const auto activeIt = activeIndexByPath.find(path);
        const int activeIndex = activeIt == activeIndexByPath.end() ? -1 : activeIt->second;
        Rectangle r = {list.x + 8, list.y + 8 + static_cast<float>(drawn * rowH), list.width - 16, 22};
        const bool hover = CheckCollisionPointRec(GetMousePosition(), r);
        if (hover) DrawRectangleRec(r, theme.hover);
        if (app.selectedPackage == activeIndex && activeIndex >= 0) DrawRectangleLinesEx(r, 1, theme.accent2);

        std::ostringstream label;
        if (activeIndex >= 0) {
            if (activeIndex + 1 < 10) label << '0';
            label << activeIndex + 1 << " [x] ";
        } else {
            label << "-- [ ] ";
        }
        label << (missing ? "missing: " : "") << DisplayPakName(path);
        DrawMonoText(app, label.str(), static_cast<int>(r.x + 6), static_cast<int>(r.y + 3), 14,
                     missing ? Color{255, 115, 115, 255} : theme.text);

        if (!app.confirmUnsavedOpen && hover && IsMouseButtonPressed(MOUSE_RIGHT_BUTTON) && activeIndex >= 0) {
            app.selectedPackage = activeIndex;
        }
        if (!app.confirmUnsavedOpen && hover && IsMouseButtonPressed(MOUSE_LEFT_BUTTON)) {
            if (activeIndex >= 0) {
                preset.packages.erase(preset.packages.begin() + activeIndex);
                app.selectedPackage = -1;
            } else if (!missing) {
                preset.packages.push_back(path);
                app.selectedPackage = static_cast<int>(preset.packages.size()) - 1;
            }
            SaveSettings(app);
        }
        ++drawn;
    };

    for (const auto &pkg : app.availablePackages) drawRow(pkg, false);
    for (const auto &pkg : missingPackages) drawRow(pkg, true);

    if (GuiButton({660, 204, 38, 24}, "Up") && app.selectedPackage > 0) {
        std::swap(preset.packages[app.selectedPackage], preset.packages[app.selectedPackage - 1]);
        --app.selectedPackage;
        SaveSettings(app);
    }
    if (GuiButton({706, 204, 38, 24}, "Dn") && app.selectedPackage >= 0 && app.selectedPackage + 1 < static_cast<int>(preset.packages.size())) {
        std::swap(preset.packages[app.selectedPackage], preset.packages[app.selectedPackage + 1]);
        ++app.selectedPackage;
        SaveSettings(app);
    }
    DrawAppText(app, "left: toggle", 660, 238, 12, theme.muted);
    DrawAppText(app, "right: select", 660, 256, 12, theme.muted);
}

static void DrawMain(App &app) {
    const auto &theme = Theme(app);
    DrawBackground(app);
    if (GuiButton({744, 18, 34, 28}, "X")) RequestAction(app, PendingAction::Quit);
    if (GuiButton({676, 18, 58, 28}, app.musicPlaying ? "Pause" : "Play")) ToggleMusic(app);
    if (GuiButton({506, 18, 78, 28}, "Cyber")) {
        app.settings.theme = "cyber";
        ApplyStyle(app);
        SaveSettings(app);
    }
    if (GuiButton({590, 18, 78, 28}, "Matrix")) {
        app.settings.theme = "matrix";
        ApplyStyle(app);
        SaveSettings(app);
    }
    DrawRectangleLinesEx(app.settings.theme == "cyber" ? Rectangle{506, 18, 78, 28} : Rectangle{590, 18, 78, 28}, 2, theme.accent);
    DrawRectangle(24, 88, 752, 420, theme.panel);
    DrawRectangleLinesEx({24, 88, 752, 420}, 2, theme.accent2);

    GuiLabel({42, 108, 80, 22}, "Engine");
    DrawTextInput(app, {118, 106, 520, 26}, app.engineBuf.data(), app.engineBuf.size(), TextField::Engine);
    if (GuiButton({648, 106, 96, 26}, "Browse")) OpenBrowser(app, BrowserTarget::Engine, app.engineBuf.data());

    DrawRectangle(42, 142, 214, 286, theme.panelSoft);
    DrawAppText(app, "PRESETS", 54, 152, 15, theme.accent);
    const Rectangle presetList = {54, 176, 190, 204};
    const int presetRowH = 26;
    const int visiblePresets = static_cast<int>(presetList.height) / presetRowH;
    const int presetCount = static_cast<int>(app.settings.presets.size());
    const int maxPresetScroll = std::max(0, presetCount - visiblePresets);
    if (!app.confirmUnsavedOpen && CheckCollisionPointRec(GetMousePosition(), presetList)) {
        app.presetScroll += static_cast<int>(-GetMouseWheelMove());
    }
    app.presetScroll = std::clamp(app.presetScroll, 0, maxPresetScroll);
    for (int row = 0; row < visiblePresets && app.presetScroll + row < presetCount; ++row) {
        const int i = app.presetScroll + row;
        Rectangle r = {54, static_cast<float>(176 + row * presetRowH), 190, 23};
        const bool selected = i == app.selectedPreset;
        const int oldBase = GuiGetStyle(DEFAULT, BASE_COLOR_NORMAL);
        const int oldText = GuiGetStyle(DEFAULT, TEXT_COLOR_NORMAL);
        const int oldBorder = GuiGetStyle(DEFAULT, BORDER_COLOR_NORMAL);
        if (selected) {
            GuiSetStyle(DEFAULT, BASE_COLOR_NORMAL, ColorToInt(theme.accent2));
            GuiSetStyle(DEFAULT, TEXT_COLOR_NORMAL, ColorToInt(theme.bgBottom));
            GuiSetStyle(DEFAULT, BORDER_COLOR_NORMAL, ColorToInt(theme.accent));
        }
        const std::string fullName = app.settings.presets[static_cast<size_t>(i)].name;
        const std::string label = FitMiddleText(app, fullName, r.width - 18.0f, 16.0f);
        if (!app.confirmUnsavedOpen && CheckCollisionPointRec(GetMousePosition(), r) && label != fullName) {
            app.status = fullName;
        }
        if (GuiButton(r, label.c_str())) {
            RequestAction(app, PendingAction::SwitchPreset, i);
        }
        if (selected) {
            GuiSetStyle(DEFAULT, BASE_COLOR_NORMAL, oldBase);
            GuiSetStyle(DEFAULT, TEXT_COLOR_NORMAL, oldText);
            GuiSetStyle(DEFAULT, BORDER_COLOR_NORMAL, oldBorder);
        }
    }
    Rectangle presetTrack = {54, 374, 190, 10};
    DrawRectangleRec(presetTrack, theme.bgTop);
    DrawRectangleLinesEx(presetTrack, 1, theme.border);
    const float presetThumbWidth = maxPresetScroll > 0 ? std::max(38.0f, presetTrack.width * static_cast<float>(visiblePresets) / static_cast<float>(presetCount)) : presetTrack.width;
    const float presetThumbX = maxPresetScroll > 0 ? presetTrack.x + (presetTrack.width - presetThumbWidth) * static_cast<float>(app.presetScroll) / static_cast<float>(maxPresetScroll) : presetTrack.x;
    Rectangle presetThumb = {presetThumbX, presetTrack.y + 2, presetThumbWidth, presetTrack.height - 4};
    DrawRectangleRec(presetThumb, theme.accent2);
    if (!app.confirmUnsavedOpen && maxPresetScroll > 0 && CheckCollisionPointRec(GetMousePosition(), presetTrack) && IsMouseButtonDown(MOUSE_LEFT_BUTTON)) {
        const float t = std::clamp((GetMouseX() - presetTrack.x - presetThumbWidth * 0.5f) / (presetTrack.width - presetThumbWidth), 0.0f, 1.0f);
        app.presetScroll = static_cast<int>(std::round(t * static_cast<float>(maxPresetScroll)));
    }
    if (GuiButton({54, 390, 58, 24}, "New")) {
        RequestAction(app, PendingAction::NewPreset);
    }
    if (GuiButton({120, 390, 58, 24}, "Clone")) {
        RequestAction(app, PendingAction::ClonePreset);
    }
    if (GuiButton({186, 390, 58, 24}, "Del") && app.settings.presets.size() > 1) {
        RequestAction(app, PendingAction::DeletePreset);
    }
    if (GuiButton({120, 420, 58, 24}, "Up") && app.selectedPreset > 0) {
        RequestAction(app, PendingAction::MovePresetUp);
    }
    if (GuiButton({186, 420, 58, 24}, "Dn") && app.selectedPreset + 1 < static_cast<int>(app.settings.presets.size())) {
        RequestAction(app, PendingAction::MovePresetDown);
    }

    Preset &preset = CurrentPreset(app);
    GuiLabel({276, 146, 90, 22}, "Name");
    DrawTextInput(app, {340, 144, 240, 26}, app.nameBuf.data(), app.nameBuf.size(), TextField::PresetName);
    if (GuiButton({594, 144, 82, 26}, "Save")) {
        app.settings.enginePath = app.engineBuf.data();
        app.settings.quakeDir = ExecutableDir().string();
        preset.name = app.nameBuf.data();
        preset.extraArgs = app.argsBuf.data();
        SaveSettings(app);
        app.status = "Saved q2manager.json";
    }

    DrawPackageToggleList(app, preset);

    GuiLabel({276, 426, 80, 22}, "Autoexec");
    const std::string cfg = preset.autoexec.empty() ? "(none)" : fs::path(preset.autoexec).filename().string();
    DrawAppText(app, cfg, 354, 430, 14, theme.text);
    if (GuiButton({660, 422, 84, 24}, "Choose")) OpenBrowser(app, BrowserTarget::Autoexec, app.settings.quakeDir);

    GuiLabel({276, 462, 80, 22}, "Extra Args");
    DrawTextInput(app, {354, 460, 290, 26}, app.argsBuf.data(), app.argsBuf.size(), TextField::ExtraArgs);
    if (GuiButton({660, 460, 84, 32}, "Launch")) LaunchPreset(app);

    DrawRectangle(24, 520, 752, 28, theme.status);
    DrawAppText(app, app.status, 34, 527, 14, theme.text);
}

int main() {
    App app;
    app.settings.quakeDir = ExecutableDir().string();
    LoadSettings(app);
    app.settings.quakeDir = ExecutableDir().string();
    SetConfigFlags(FLAG_WINDOW_RESIZABLE | FLAG_VSYNC_HINT);
    const std::string windowTitle = std::string("q2manager ") + Q2MANAGER_VERSION;
    InitWindow(800, 560, windowTitle.c_str());
    SetTargetFPS(60);
    InitMusic(app);
    LoadReadableFont(app);
    LoadMonospaceFont(app);
    ApplyStyle(app);
    while (!app.quitRequested) {
        if (WindowShouldClose()) RequestAction(app, PendingAction::Quit);
        BeginDrawing();
        if (app.confirmUnsavedOpen) GuiLock();
        DrawMain(app);
        if (app.confirmUnsavedOpen) GuiUnlock();
        if (app.confirmUnsavedOpen) GuiLock();
        if (app.browser.open) DrawBrowser(app);
        if (app.confirmUnsavedOpen) GuiUnlock();
        DrawUnsavedModal(app);
        EndDrawing();
    }
    app.settings.enginePath = app.engineBuf.data();
    app.settings.quakeDir = ExecutableDir().string();
    if (!app.settings.presets.empty()) {
        CurrentPreset(app).name = app.nameBuf.data();
        CurrentPreset(app).extraArgs = app.argsBuf.data();
    }
    SaveSettings(app);
    if (app.audioReady) {
        UnloadAudioStream(app.music);
        CloseAudioDevice();
    }
    if (app.customMonoFont) UnloadFont(app.monoFont);
    if (app.customFont) UnloadFont(app.font);
    CloseWindow();
}
