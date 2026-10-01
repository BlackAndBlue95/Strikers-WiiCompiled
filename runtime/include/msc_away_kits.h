#pragma once

#include <cstdint>
#include <string>
#include <vector>

// Generated away kits for the captains Mario Strikers Charged gives only one kit (Mario, Luigi,
// Waluigi, Wario). Recolours that team's textures, in their CMPR form: the captain's kit, the
// gloves/banners/backgrounds in the captain's ExtraTextures, the sidekicks' team texture and the
// goalie's kit. See msc_away_kits.cpp for how kit areas are found.
namespace MscAwayKits {

enum class Kind : uint8_t {
    Kit,         // captain kit texture: colour rules with hysteresis
    Extra,       // captain extra texture (gloves, banners): colour rules
    Background,  // team-tinted background: every coloured texel, lightness kept
    Sidekick,    // <sidekick>_<captain>: differs from other teams' versions + kit hue
    Goalie,      // <captain>goalie: differs from other teams' goalies + kit hue
};

struct Texture {
    uint32_t hash;            // nlStringLowerHash of the texture name
    Kind kind;
    std::string bundle;       // disc file it is loaded from
    std::string refBundle[2]; // Sidekick/Goalie: the same texture of two other teams
    uint32_t refHash[2];
};

// True for the captains that get a generated away kit.
bool Has(int32_t captain);
// 0xRRGGBB team colour of the generated kit (HUD, captain select tint).
uint32_t TeamColour(int32_t captain);
// Every texture that carries the captain's team colour.
const std::vector<Texture>& Textures(int32_t captain);

// Starts making the captain's away-kit textures from the disc files on a worker thread, so they
// are ready by the time the game loads them. Results are kept for the session.
void Prepare(int32_t captain);

enum class Result { Pending, Ready, Unchanged };
// The away-kit version of `texture`, whose data in memory (home kit) is `data`: Ready with `out`
// filled, Pending while it is being made (from this data if Prepare did not cover it), or
// Unchanged when nothing in it is team-coloured.
Result Get(int32_t captain, const Texture& texture, const uint8_t* data, uint32_t size, uint32_t width,
           uint32_t height, uint32_t levels, std::vector<uint8_t>& out);

// Recolours one texture's CMPR data (all mip levels) in place, on the calling thread. False if
// nothing was changed.
bool Recolour(int32_t captain, const Texture& texture, uint8_t* data, uint32_t width, uint32_t height,
              uint32_t levels);

} // namespace MscAwayKits
