#pragma once

#include <string>
#include <string_view>

#include <pl/Config.hpp>

namespace clange_me {

// Settings loosely mirror the Java BetterHitreg mod (sound muffle/sharpen, toggles).
struct ModConfig {
    int version = 1;
    bool enabled = true;
    bool hitSound = true;
    bool hitFlash = true;
    int hitVolume = 100;    // 0-100
    int muffleAmount = 0;   // 0-100
    int sharpenAmount = 0;  // 0-100
    // Log-only helper: records sounds played right after you hit something
    // into sound_samples.txt (used to learn the game's sound call).
    bool soundSampler = true;
    // Where to find GameMode::attack in libminecraftpe.so (1.26.62).
    // Fill ONE of these. Signature = byte pattern with ?? wildcards.
    // Rva = hex offset from the module base, e.g. "0x1234567".
    std::string attackSignature = "";
    std::string attackRva = "";
};

nlohmann::json makeDefaultConfigJson();
nlohmann::json makeConfigSchemaJson();

} // namespace clange_me

template <> struct pl::config::Schema<clange_me::ModConfig> {
    static constexpr std::string_view title = "Better Hitreg Config";
    static constexpr std::string_view description = {};

    static constexpr FieldSchema field(std::string_view name) {
        if (name == "version")
            return {.title = "Version", .readOnly = true};
        if (name == "enabled")
            return {.title = "Enabled", .description = "Turns the mod on or off."};
        if (name == "hitSound")
            return {.title = "Instant hit sound", .description = "Play hit feedback immediately on swing."};
        if (name == "hitFlash")
            return {.title = "Instant hit flash", .description = "Show damage flash immediately on swing."};
        if (name == "hitVolume")
            return {.title = "Hit volume", .description = "0-100."};
        if (name == "muffleAmount")
            return {.title = "Muffle amount", .description = "0-100."};
        if (name == "sharpenAmount")
            return {.title = "Sharpen amount", .description = "0-100."};
        if (name == "soundSampler")
            return {.title = "Sound sampler", .description = "Write sound_samples.txt (log only, no gameplay change)."};
        if (name == "attackSignature")
            return {.title = "Attack signature", .description = "Byte pattern for the attack function."};
        if (name == "attackRva")
            return {.title = "Attack RVA", .description = "Hex offset of the attack function, e.g. 0x1234567."};
        return {};
    }
};
