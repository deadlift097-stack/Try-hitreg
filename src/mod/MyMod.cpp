#include "mod/MyMod.h"

#include <atomic>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>

#include <link.h>

#include <pl/Mod.hpp>
#include <pl/ModMenu.hpp>
#include <pl/memory/Signature.hpp>

namespace clange_me {

namespace {

constexpr const char *kModuleId = "better_hitreg.main";

// Change this when the build changes so old crash markers stop applying.
constexpr const char *kGuardTag = "abi2-2026-11-08";

// Minecraft Bedrock 1.26.62.1 (libminecraftpe.so, arm64). Found through the
// GameMode vtable: slot 16 (attack) forwards to this shared sender with the
// 3rd argument set to 1; slot 15 (interact) has its own function.
// Offsets are version specific, so the first two instructions are checked
// before hooking (see targetLooksValid).
constexpr uintptr_t kRvaAttackSender = 0x1009d290;
constexpr uint32_t kExpectWord0 = 0xd10743ff; // sub sp, sp, #464
constexpr uint32_t kExpectWord1 = 0xa9187bfd; // stp x29, x30, [sp, #384]

// SoundEngine::play-style function (1.26.62.1): 2nd entry of the SoundEngine
// class table. Takes (this, std::string const& name, Vec3 const& pos, two
// more 64-bit values, float volume, float pitch) and returns a 64-bit handle.
// Only used for logging for now.
constexpr uintptr_t kRvaSoundPlay = 0xbce0498;
constexpr uint32_t kSoundWord0 = 0xd10343ff; // sub sp, sp, #208
constexpr uint32_t kSoundWord1 = 0x6d0823e9; // stp d9, d8, [sp, #128]

// Live settings: changed by the menu / config file, read by the hook.
std::atomic<bool> gEnabled{true};
std::atomic<bool> gHitSound{true};
std::atomic<bool> gHitFlash{true};
std::atomic<int> gHitVolume{100};
std::atomic<int> gMuffle{0};
std::atomic<int> gSharpen{0};

// Pass-through detour using 8 integer registers so it stays safe even if the
// real function takes fewer arguments.
using U = uint64_t;
using Fn8 = U (*)(U, U, U, U, U, U, U, U);
Fn8 gOriginal = nullptr;

std::atomic<int> gAttackFirstCalls{0};
std::atomic<int> gSoundFirstCalls{0};

U attackDetour(U a0, U a1, U a2, U a3, U a4, U a5, U a6, U a7) {
    const bool first = gAttackFirstCalls.fetch_add(1) < 3;
    if (first)
        ClangeMeMod::instance().guardSet("attack_hook.pending");
    if (gEnabled.load()) {
        // a0 = GameMode*, a1 = target Actor*, a2 = flag (1 on the attack path),
        // a3 = hit position pointer.
        ClangeMeMod::instance().onAttackCall((a2 & 1U) != 0);
        // Instant hit feedback (sound / flash) belongs here once the matching
        // game functions are located; it would use gHitSound, gHitFlash,
        // gHitVolume, gMuffle and gSharpen.
    }
    const U result = gOriginal(a0, a1, a2, a3, a4, a5, a6, a7);
    if (first)
        ClangeMeMod::instance().guardClear("attack_hook.pending");
    return result;
}

using SoundPlayFn = U (*)(void *, const void *, const float *, U, U, float, float);
SoundPlayFn gSoundOriginal = nullptr;

U soundDetour(void *self, const void *name, const float *pos, U a3, U a4, float volume, float pitch) {
    const bool first = gSoundFirstCalls.fetch_add(1) < 3;
    if (first)
        ClangeMeMod::instance().guardSet("sound_hook.pending");
    ClangeMeMod::instance().onSoundCall(self, name, pos, a3, a4, volume, pitch);
    const U result = gSoundOriginal(self, name, pos, a3, a4, volume, pitch);
    if (first)
        ClangeMeMod::instance().guardClear("sound_hook.pending");
    return result;
}

// Reads a libc++ std::string (short or long form) without owning it.
std::string readLibcxxString(const void *p) {
    if (!p)
        return {};
    const auto *b = static_cast<const unsigned char *>(p);
    if (b[0] & 1U) {
        size_t len = *reinterpret_cast<const size_t *>(b + 8);
        const char *data = *reinterpret_cast<const char *const *>(b + 16);
        if (!data || len > 200)
            return "<long>";
        return std::string(data, len);
    }
    size_t len = b[0] >> 1;
    if (len > 22)
        return "<bad>";
    return std::string(reinterpret_cast<const char *>(b + 1), len);
}

int64_t nowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

uintptr_t moduleBase(const char *name) {
    struct Ctx {
        const char *name;
        uintptr_t base;
    } ctx{name, 0};
    dl_iterate_phdr(
        [](dl_phdr_info *info, size_t, void *data) -> int {
            auto *c = static_cast<Ctx *>(data);
            if (info->dlpi_name && std::string_view(info->dlpi_name).find(c->name) != std::string_view::npos) {
                c->base = info->dlpi_addr;
                return 1;
            }
            return 0;
        },
        &ctx);
    return ctx.base;
}

void applyConfig(const ModConfig &c) {
    gEnabled = c.enabled;
    gHitSound = c.hitSound;
    gHitFlash = c.hitFlash;
    gHitVolume = c.hitVolume;
    gMuffle = c.muffleAmount;
    gSharpen = c.sharpenAmount;
}

void onModuleToggle(std::string_view, bool enabled) { gEnabled = enabled; }

bool parseBool(std::string_view v) { return v == "true" || v == "1" || v == "on"; }

int parseInt(std::string_view v, int fallback) {
    try {
        return std::stoi(std::string(v));
    } catch (...) {
        return fallback;
    }
}

// Called by the launcher when a menu entry changes (moduleId, key, value).
void handleConfigChanged(std::string_view, std::string_view key, std::string_view value) {
    if (key == "hitSound")
        gHitSound = parseBool(value);
    else if (key == "hitFlash")
        gHitFlash = parseBool(value);
    else if (key == "hitVolume")
        gHitVolume = parseInt(value, gHitVolume.load());
    else if (key == "muffleAmount")
        gMuffle = parseInt(value, gMuffle.load());
    else if (key == "sharpenAmount")
        gSharpen = parseInt(value, gSharpen.load());
}

// Attach the change callback only if this SDK's builder accepts one with this
// shape; otherwise leave the builder untouched.
template <class B> decltype(auto) withConfigCallback(B &&b) {
    if constexpr (requires { std::forward<B>(b).onConfigChanged(&handleConfigChanged); })
        return std::forward<B>(b).onConfigChanged(&handleConfigChanged);
    else
        return std::forward<B>(b);
}

} // namespace

uintptr_t ClangeMeMod::resolveAttackTarget() {
    auto &log = getSelf().getLogger();
    if (!mConfig.attackSignature.empty()) {
        uintptr_t addr = pl::memory::resolveSignature(mConfig.attackSignature, "libminecraftpe.so");
        if (addr == 0)
            log.warn("attackSignature did not match in libminecraftpe.so");
        return addr;
    }
    uintptr_t base = moduleBase("libminecraftpe.so");
    if (base == 0) {
        log.warn("Could not find libminecraftpe.so base address");
        return 0;
    }
    uintptr_t rva = kRvaAttackSender;
    if (!mConfig.attackRva.empty())
        rva = static_cast<uintptr_t>(std::strtoull(mConfig.attackRva.c_str(), nullptr, 16));
    return base + rva;
}

bool ClangeMeMod::targetLooksValid(uintptr_t addr) {
    // Only enforced for the built-in offset. A user-supplied signature or RVA
    // is trusted as given.
    if (!mConfig.attackSignature.empty() || !mConfig.attackRva.empty())
        return true;
    const auto *w = reinterpret_cast<const uint32_t *>(addr);
    return w[0] == kExpectWord0 && w[1] == kExpectWord1;
}

void ClangeMeMod::reloadConfigIfDue() {
    auto now = std::chrono::steady_clock::now();
    if (now - mLastReload < std::chrono::seconds(2))
        return;
    mLastReload = now;
    if (mConfigFile && mConfigFile->load()) {
        mConfig = mConfigFile->value();
        applyConfig(mConfig);
    }
}

void ClangeMeMod::writeCalibrationFile(bool force) {
    auto now = std::chrono::steady_clock::now();
    if (!force && now - mLastWrite < std::chrono::seconds(1))
        return;
    mLastWrite = now;
    std::error_code ec;
    std::ofstream out(getSelf().getDataDir() / "hit_calibration.txt", std::ios::trunc);
    if (!out)
        return;
    out << "attack_calls=" << mAttackCalls.load() << "\n"
        << "other_calls=" << mOtherCalls.load() << "\n"
        << "hook_rva=0x" << std::hex << kRvaAttackSender << "\n";
}

void ClangeMeMod::onSoundCall(void *self, const void *name, const float *pos, uint64_t a3,
                              uint64_t a4, float volume, float pitch) {
    // Only sounds within 1.5 s after an attack call, at most 80 lines.
    if (nowMs() - mLastAttackMs.load() > 1500 || mSoundSamples.load() >= 80)
        return;
    ++mSoundSamples;
    std::ofstream out(getSelf().getDataDir() / "sound_samples.txt", std::ios::app);
    if (!out)
        return;
    out << "name=" << readLibcxxString(name) << " engine=0x" << std::hex
        << reinterpret_cast<uintptr_t>(self) << " a3=0x" << a3 << " a4=0x" << a4 << std::dec
        << " vol=" << volume << " pitch=" << pitch;
    if (pos)
        out << " pos=" << pos[0] << "," << pos[1] << "," << pos[2];
    out << " ms_since_attack=" << (nowMs() - mLastAttackMs.load()) << "\n";
}

void ClangeMeMod::onAttackCall(bool isAttack) {
    if (isAttack) {
        mLastAttackMs = nowMs();
        ++mAttackCalls;
    } else {
        ++mOtherCalls;
    }
    reloadConfigIfDue();
    writeCalibrationFile(false);
}

bool ClangeMeMod::guardTripped(const char *name) {
    std::ifstream in(getSelf().getDataDir() / name);
    if (!in)
        return false;
    std::string content;
    std::getline(in, content);
    return content == kGuardTag;
}

void ClangeMeMod::guardSet(const char *name) {
    std::ofstream out(getSelf().getDataDir() / name, std::ios::trunc);
    if (out)
        out << kGuardTag << "\n";
}

void ClangeMeMod::guardClear(const char *name) {
    std::error_code ec;
    std::filesystem::remove(getSelf().getDataDir() / name, ec);
}

ClangeMeMod &ClangeMeMod::instance() {
    static ClangeMeMod instance;
    return instance;
}

ClangeMeMod::ClangeMeMod() : mSelf(*ll::mod::NativeMod::current()) {}

bool ClangeMeMod::load() {
    auto &self = getSelf();
    self.getLogger().debug("Loading...");

    std::error_code ec;
    std::filesystem::create_directories(self.getDataDir(), ec);
    if (ec) {
        self.getLogger().error("Failed to create data directory {}: {}", self.getDataDir().string(),
                               ec.message());
        return false;
    }

    std::filesystem::create_directories(self.getConfigDir(), ec);
    if (ec) {
        self.getLogger().error("Failed to create config directory {}: {}",
                               self.getConfigDir().string(), ec.message());
        return false;
    }

    mConfigFile.emplace();
    if (!mConfigFile->load()) {
        self.getLogger().warn("Failed to load typed config");
        return false;
    }
    mConfig = mConfigFile->value();
    applyConfig(mConfig);

    self.getLogger().info("Loaded {} from {}", self.getName(), self.getModDir().string());
    return true;
}

bool ClangeMeMod::enable() {
    auto &self = getSelf();
    self.getLogger().debug("Enabling...");

    // ---- In-game menu ----
    if (guardTripped("menu.pending")) {
        self.getLogger().warn("The menu crashed the game last time; skipping it. Delete menu.pending in the data folder to retry.");
    } else {
        guardSet("menu.pending");

        const std::string hitVolume = std::to_string(mConfig.hitVolume);
        const std::string muffle = std::to_string(mConfig.muffleAmount);
        const std::string sharpen = std::to_string(mConfig.sharpenAmount);

        const bool menuOk =
            withConfigCallback(
                pl::modmenu::ModuleBuilder(kModuleId, "Better Hitreg")
                    .modId(self.getId())
                    .description("Instant hit feedback. Tune it here.")
                    .defaultEnabled(mConfig.enabled)
                    .onToggle(onModuleToggle)
                    .config("hitSound", "Instant hit sound (in development)",
                            pl::modmenu::ConfigType::Toggle, mConfig.hitSound ? "true" : "false")
                    .config("hitFlash", "Instant hit flash (in development)",
                            pl::modmenu::ConfigType::Toggle, mConfig.hitFlash ? "true" : "false")
                    .config("hitVolume", "Hit volume", pl::modmenu::ConfigType::SliderInt,
                            hitVolume.c_str(), "0", "100")
                    .config("muffleAmount", "Muffle amount", pl::modmenu::ConfigType::SliderInt,
                            muffle.c_str(), "0", "100")
                    .config("sharpenAmount", "Sharpen amount", pl::modmenu::ConfigType::SliderInt,
                            sharpen.c_str(), "0", "100"))
                .registerModule();

        guardClear("menu.pending");
        if (!menuOk)
            self.getLogger().warn("Could not register the Better Hitreg menu module");
    }

    // ---- Hook ----
    if (guardTripped("attack_hook.pending")) {
        self.getLogger().warn("The attack hook crashed the game last time; skipping it. Delete attack_hook.pending to retry.");
        return true;
    }
    uintptr_t target = resolveAttackTarget();
    if (target == 0)
        return true; // already logged; the mod stays idle

    if (!targetLooksValid(target)) {
        self.getLogger().warn("Bytes at the attack target do not match 1.26.62.1; hook skipped to avoid a crash.");
        return true;
    }

    mAttackHook = pl::memory::HookHandle(reinterpret_cast<void *>(target),
                                         reinterpret_cast<void *>(&attackDetour),
                                         reinterpret_cast<void **>(&gOriginal),
                                         pl::memory::HookPriority::Normal);
    if (!mAttackHook.installed()) {
        self.getLogger().error("Failed to install attack hook");
        return true;
    }
    self.getLogger().info("Attack hook installed at 0x{:x}", target);
    writeCalibrationFile(true);

    // Log-only sound sampler (pass-through).
    if (mConfig.soundSampler && !guardTripped("sound_hook.pending")) {
        uintptr_t base = moduleBase("libminecraftpe.so");
        if (base != 0) {
            const auto *w = reinterpret_cast<const uint32_t *>(base + kRvaSoundPlay);
            if (w[0] == kSoundWord0 && w[1] == kSoundWord1) {
                mSoundHook = pl::memory::HookHandle(reinterpret_cast<void *>(base + kRvaSoundPlay),
                                                    reinterpret_cast<void *>(&soundDetour),
                                                    reinterpret_cast<void **>(&gSoundOriginal),
                                                    pl::memory::HookPriority::Normal);
                if (mSoundHook.installed())
                    self.getLogger().info("Sound sampler installed");
            } else {
                self.getLogger().warn("Sound sampler skipped: bytes do not match 1.26.62.1");
            }
        }
    }
    return true;
}

bool ClangeMeMod::disable() {
    getSelf().getLogger().debug("Disabling...");
    mAttackHook.reset();
    mSoundHook.reset();
    pl::modmenu::unregisterModule(kModuleId);
    return true;
}

bool ClangeMeMod::unload() {
    getSelf().getLogger().debug("Unloading...");
    mConfigFile.reset();
    return true;
}

} // namespace clange_me
