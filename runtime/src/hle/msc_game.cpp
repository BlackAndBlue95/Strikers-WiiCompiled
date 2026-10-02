// Mario Strikers Charged (R4QE01) game-specific HLE.
#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <map>
#include <set>
#include <string>
#include <vector>
#include "abi_bridge.h"
#include "hle_stubs.h"
#include "msc_away_kits.h"
#include "runtime_config.h"
#include "runtime_log.h"
#include "wiimote_hid.h"
#include <SDL3/SDL_timer.h>
#include "guest_interrupt_context.h"
#include "hle/msc_guest.h"
#include "mods/mod_plugins.h"
#include "mods/mod_catalogs.h"
#include "mods/mod_registry.h"
#include "mods/variants.h"
#include "ppc_runtime.h"

bool DVDPathExistsForRuntime(const char* dvdPath);  // storage/dvd.cpp
extern "C" void OS__Report_803B5BE4(CpuContext* ctx);
extern "C" void GxNotifyGuestRamDmaWrite(uint32_t addr, uint32_t size);

// nlPrintf(const char*, ...) is compiled to a no-op sink in retail; route it to
// OSReport so engine diagnostics (allocator panics, asserts) reach the log.
extern "C" void MSC_nlPrintf_80009B34(CpuContext* ctx)
{
    OS__Report_803B5BE4(ctx);
}

PPC_NATIVE_OVERRIDE_VOID(80009B34, MSC_nlPrintf_80009B34, (CpuContext* ctx), (ctx));

#include "wii_remote_input.h"
#include "memory.h"

// WPADSetConnectCallback(chan, cb) -> previous cb. Strikers Charged's PlatPadManager only polls
// a channel after its connect callback reports WPAD_ERR_OK, and MKW never registers one, so the
// runtime had no implementation. Store it and report a controller that is already present, as
// the SDK does for a remote paired before the game starts; MSC_PollRemoteConnections reports the
// ones that connect or disconnect later.
namespace {
uint32_t s_connectCallbacks[4]{};
bool s_reportedConnected[4]{};
constexpr int32_t kWpadErrOk = 0, kWpadErrNoController = -1;

void ReportConnection(CpuContext* ctx, uint32_t chan, bool connected)
{
    s_reportedConnected[chan] = connected;
    const uint32_t cb = s_connectCallbacks[chan];
    if (cb == 0) return;
    const uint32_t savedLr = ctx->lr;
    ctx->gpr[3] = chan;
    ctx->gpr[4] = static_cast<uint32_t>(connected ? kWpadErrOk : kWpadErrNoController);
    InvokeIndirectCpu(cb, ctx);
    ctx->lr = savedLr;
}
} // namespace

extern "C" void MSC_WPADSetConnectCallback_803CD0DC(CpuContext* ctx)
{
    const uint32_t chan = ctx->gpr[3];
    const uint32_t cb = ctx->gpr[4];
    if (chan >= 4) {
        ctx->gpr[3] = 0;
        return;
    }
    const uint32_t previous = s_connectCallbacks[chan];
    s_connectCallbacks[chan] = cb;
    s_reportedConnected[chan] = false;
    if (cb != 0 && WiiRemoteInput::IsRemoteChannel(chan)) {
        ReportConnection(ctx, chan, true);
    }
    ctx->gpr[3] = previous;
}

PPC_NATIVE_OVERRIDE_VOID(803CD0DC, MSC_WPADSetConnectCallback_803CD0DC, (CpuContext* ctx), (ctx));

// Controllers that appear after the game registered its callbacks (or SDL enumerates late at
// boot) are announced like a remote pairing mid-game, and ones that go away like a disconnect.
// Called from the per-frame KPADRead; callbacks run on a private register file, as alarms do.
void MSC_PollRemoteConnections()
{
    static uint64_t s_lastPollMs = 0;
    const uint64_t now = SDL_GetTicks();
    if (now - s_lastPollMs < 250) return;
    s_lastPollMs = now;
    for (uint32_t chan = 0; chan < 4; ++chan) {
        if (s_connectCallbacks[chan] == 0) continue;
        const bool present = WiiRemoteInput::IsRemoteChannel(chan);
        if (present == s_reportedConnected[chan]) continue;
        GuestInterruptCallbackContext interrupt;
        ReportConnection(interrupt.get(), chan, present);
    }
}

// UpdatePlatPad: the game only polls channels whose `connected` flag its WPAD connect callback set,
// so a controller that appears after boot has to be announced from here, which runs every frame
// whether or not anything is connected. (Announcing it from KPADRead alone never fired when the only
// controller arrived late, e.g. a Wii Remote finishing its HID setup after the title screen loaded:
// nothing polled, so nothing called KPADRead.) Then the original loop: UpdateChannel per connected
// channel (PlatPadManager::connected[] at +0x2F0).
// Gameplay extras (F10 > Tweaks), applied once per frame from UpdatePlatPad.
namespace FrameMods {
constexpr uint32_t kUnlockAll = 0x806E0F98u;          // gUnlockAll: the game's unlock-everything override
constexpr uint32_t kGameInfoManager = 0x806E0F54u;    // GameInfoManager singleton pointer
constexpr uint32_t kUseCurGameSettings = 0x27C;       // GetCurrentSettings() returns the per-match copy
constexpr uint32_t kCurGameLimitType = 0x04 + 0x04;   // mCurGameGameplayOptions.GameLimitType (1 = goals)
constexpr uint32_t kCurGameGoalLimit = 0x04 + 0x0C;   // mCurGameGameplayOptions.GoalLimit
constexpr uint32_t kGamePtr = 0x806E0C94u;            // g_pGame (non-null during a match)
constexpr uint32_t kTeams = 0x806E0DF8u;              // g_pTeams[2]
constexpr uint32_t kTeamScore = 0x04;                 // cTeam::m_nScore

void UnlockEverything() {
    static bool s_applied = false;
    const bool want = RuntimeConfigFile::ModUnlockEverything();
    if (want) {
        Memory::Write8(kUnlockAll, 1);
        s_applied = true;
    } else if (s_applied) {
        Memory::Write8(kUnlockAll, 0);
        s_applied = false;
    }
}

// Win by 2: every win check reads GetCurrentSettings()->GoalLimit, which for a local match is the
// per-match copy (mUseCurGameSettings), never the saved options. Tied one short of the target, the
// target becomes score + 2; the original is put back between matches.
void WinByTwo() {
    static int32_t s_original = -1;
    static uint32_t s_info = 0;
    const auto restore = [&] {
        if (s_original >= 0 && s_info != 0) Memory::Write32(s_info + kCurGameGoalLimit, static_cast<uint32_t>(s_original));
        s_original = -1;
        s_info = 0;
    };
    const uint32_t info = Memory::Read32(kGameInfoManager);
    const uint32_t game = Memory::Read32(kGamePtr);
    if (!RuntimeConfigFile::ModWinByTwo() || info == 0 || game == 0 || Memory::Read8(info + kUseCurGameSettings) == 0 ||
        Memory::Read32(info + kCurGameLimitType) != 1) {
        restore();
        return;
    }
    const uint32_t home = Memory::Read32(kTeams), away = Memory::Read32(kTeams + 4);
    if (home == 0 || away == 0) return;
    const int32_t s0 = static_cast<int32_t>(Memory::Read32(home + kTeamScore));
    const int32_t s1 = static_cast<int32_t>(Memory::Read32(away + kTeamScore));
    if (s_original < 0 || s_info != info || (s0 == 0 && s1 == 0)) {
        restore();
        s_info = info;
        s_original = static_cast<int32_t>(Memory::Read32(info + kCurGameGoalLimit));
    }
    int32_t limit = static_cast<int32_t>(Memory::Read32(info + kCurGameGoalLimit));
    if (s0 == s1 && s0 >= s_original - 1 && limit < s0 + 2) {
        limit = s0 + 2;
        Memory::Write32(info + kCurGameGoalLimit, static_cast<uint32_t>(limit));
    }
}

// NK bug: action 0x21 (a fielder going through the opposing goalie, cFielder::fn_8004ED64) turns off
// that goalie's ball collision and the ball's player/goalie collision; only the action's normal exit
// turns them back on. Cut short (Boo deking into his own Kritter, Dry Bones teleporting behind the
// goal), they stay off and every shot passes through Kritter. When a fielder leaves 0x21 by any
// route, do what the exit does; and since nothing else ever disables the ball's flags, keep them on
// whenever no fielder is in 0x21. (The goalie's own flag is also cleared briefly by goalie actions,
// so it is only restored on that transition.)
constexpr uint32_t kBallPtr = 0x806E0BC0u;        // g_pBall
constexpr uint32_t kBallPhysics = 0xE8;           // cBall::m_pPhysicsBall
constexpr uint32_t kBallCanCollidePlayer = 0x80;  // PhysicsAIBall::mbCanCollidePlayer / +0x81 goalie
constexpr uint32_t kTeamPlayers = 0xA4;           // cTeam::m_pPlayers[5], goalie last
constexpr uint32_t kFielderAction = 0x430;        // cFielder::m_eActionState
constexpr uint32_t kCharPhysics = 0x20;           // cCharacter::m_pPhysicsCharacter
constexpr uint32_t kPhysFlags = 0x98;             // PhysicsCharacter collision bitfield
constexpr uint32_t kCanCollideWithBall = 0x40000000u;
constexpr uint32_t kActionThroughGoalie = 0x21;

void NkFix() {
    static bool s_inAction[2][4] = {};
    const uint32_t game = Memory::Read32(kGamePtr);
    if (!RuntimeConfigFile::ModNkFix() || game == 0) {
        std::memset(s_inAction, 0, sizeof(s_inAction));
        return;
    }
    const uint32_t teams[2] = {Memory::Read32(kTeams), Memory::Read32(kTeams + 4)};
    const uint32_t ball = Memory::Read32(kBallPtr);
    if (teams[0] == 0 || teams[1] == 0 || ball == 0) return;
    const uint32_t ballPhys = Memory::Read32(ball + kBallPhysics);
    const auto restoreBall = [&] {
        if (ballPhys == 0) return;
        Memory::Write8(ballPhys + kBallCanCollidePlayer, 1);
        Memory::Write8(ballPhys + kBallCanCollidePlayer + 1, 1);
    };
    bool anyInAction = false;
    for (int t = 0; t < 2; ++t) {
        for (int j = 0; j < 4; ++j) {
            const uint32_t fielder = Memory::Read32(teams[t] + kTeamPlayers + j * 4);
            const bool in = fielder != 0 && Memory::Read32(fielder + kFielderAction) == kActionThroughGoalie;
            if (s_inAction[t][j] && !in) {
                const uint32_t goalie = Memory::Read32(teams[1 - t] + kTeamPlayers + 4 * 4);
                const uint32_t phys = goalie != 0 ? Memory::Read32(goalie + kCharPhysics) : 0;
                if (phys != 0) Memory::Write32(phys + kPhysFlags, Memory::Read32(phys + kPhysFlags) | kCanCollideWithBall);
                restoreBall();
            }
            s_inAction[t][j] = in;
            anyInAction = anyInAction || in;
        }
    }
    if (!anyInAction) restoreBall();
}

// All stadiums fast-paced: the pitch surface comes from the stadium's TerrainTweaks (ini/Terrain/*.ini,
// reached through gGameTweaks.mTerrainTweaks); during a match its four values are set to
// DryTerrain.ini's, the fast surface. Each TweakFloatBinding keeps its value pointer at +0x0C.
constexpr uint32_t kGameTweaks = 0x8056CF08u;     // gGameTweaks
constexpr uint32_t kTerrainTweaks = 0x04;         // GameTweaks::mTerrainTweaks
void FastStadiums() {
    if (!RuntimeConfigFile::ModFastStadiums() || Memory::Read32(kGamePtr) == 0) return;
    const uint32_t terrain = Memory::Read32(kGameTweaks + kTerrainTweaks);
    if (terrain == 0) return;
    static constexpr float kDry[4] = {0.65f, 0.0f, 0.2f, 0.6f}; // Speed, Slipperyness, Friction, Bounce
    for (uint32_t i = 0; i < 4; ++i) {
        const uint32_t value = Memory::Read32(terrain + 0x04 + i * 0x10 + 0x0C);
        if (value != 0) Memory::WriteFloat32(value, kDry[i]);
    }
}

// Shot counter: the match summary's Mega Strike row shows a team's PlayerStats +0x18 / +0x16. The
// game tallies every shot by charge (cFielder shot: STATS_00 under 0.4 = white, STATS_01 = yellow,
// STATS_02 from 0.8 = red/orange) in +0x00/+0x02/+0x04; with the mod those fill the Mega Strike
// fields of the team totals both summary screens copy from (StatsTracker current and cumulative).
constexpr uint32_t kStatsTracker = 0x806E0F58u;   // nlSingleton<StatsTracker>::s_pInstance
constexpr uint32_t kCumulativeTeamStats = 0x04;   // TeamStats* [2]
constexpr uint32_t kCurrentTeamStats = 0x0C;      // TeamStats [2], 0x70 each
constexpr uint32_t kTeamTotals = 0x1C;            // TeamStats::mPlayerTotalStats
void ShotCounter() {
    if (!RuntimeConfigFile::ModShotCounter()) return;
    const uint32_t tracker = Memory::Read32(kStatsTracker);
    if (tracker == 0) return;
    const auto fill = [](uint32_t team) {
        if (team == 0) return;
        const uint32_t stats = team + kTeamTotals;
        const uint16_t white = Memory::Read16(stats + 0x00), yellow = Memory::Read16(stats + 0x02);
        Memory::Write16(stats + 0x16, static_cast<uint16_t>(white + yellow));
        Memory::Write16(stats + 0x18, Memory::Read16(stats + 0x04));
    };
    for (uint32_t side = 0; side < 2; ++side) {
        fill(tracker + kCurrentTeamStats + side * 0x70);
        fill(Memory::Read32(tracker + kCumulativeTeamStats + side * 4));
    }
}

// Blue Peach: kits clash when two captains' CharacterInfo colour masks overlap, and the higher
// colour rank switches to its _alt kit (GetAlternateCaptain / NeedsAlternateColour). Peach's mask is
// pink only (0x04), so she stays pink against red teams. With the mod her mask also has red (0x01)
// and her rank is the highest, so against red captains she alone switches, to blue.
constexpr uint32_t kCharacterInfo = 0x80505944u;  // sCharacterInfo[], 0x5C per entry
constexpr uint32_t kPeachInfo = kCharacterInfo + 5 * 0x5C;
constexpr uint32_t kColourMask = 0x4C, kColourRank = 0x50, kPrimaryColour = 0x54, kAlternateColour = 0x58;
void BluePeach() {
    static bool s_applied = false;
    const bool want = RuntimeConfigFile::ModBluePeach();
    if (want == s_applied) return;
    Memory::Write32(kPeachInfo + kColourMask, want ? 0x05u : 0x04u);
    Memory::Write32(kPeachInfo + kColourRank, want ? 240u : 5u);
    s_applied = want;
}

// Kit choice: on captain select, X switches the home team between its home and away kit and Y the
// away team (Wii Remote: - and 2). The game decides kits from the two captains' CharacterInfo colour masks and
// ranks (overlapping masks: the higher rank takes its _alt kit, for the captain select panels via
// ApplyCaptainColours, the match textures and the HUD), so the chosen outcome is produced by setting
// those for the two captains; only one side can be in its away kit. The choice holds until the
// captains change.
constexpr uint32_t kSceneManager = 0x806E1838u;            // GameSceneManager: depth +0x04, handlers +0x88
constexpr uint32_t kChooseCaptainsVtable = 0x8051D168u;    // ChooseCaptainsSceneV2
constexpr uint32_t kSceneCaptainIds = 0x40, kSceneBothConfirmed = 0x4D;
constexpr uint32_t kSceneCaptainPanels = 0xD7C, kCaptainPanelSize = 0x2AC;  // FECharacterPDAComponent[2]
constexpr uint32_t kApplyCaptainColours = 0x801E0B8Cu;     // FECharacterPDAComponent::ApplyCaptainColours(int, int)
enum class KitOutcome { Auto, BothHome, HomeAway, AwayAway };  // which side wears its away kit

struct KitState {
    KitOutcome outcome = KitOutcome::Auto;
    int32_t captains[2] = {-1, -1};
    int customSide = -1;  // side wearing a generated away kit (captain without one of its own)
    bool saved = false;
    uint32_t savedMask[12] = {}, savedRank[12] = {}, savedPrimary[12] = {};
    uint32_t prevButtons[4] = {};
};
KitState g_kit;

void RestoreCaptainColours() {
    if (!g_kit.saved) return;
    for (uint32_t i = 0; i < 12; ++i) {
        Memory::Write32(kCharacterInfo + i * 0x5C + kColourMask, g_kit.savedMask[i]);
        Memory::Write32(kCharacterInfo + i * 0x5C + kColourRank, g_kit.savedRank[i]);
        Memory::Write32(kCharacterInfo + i * 0x5C + kPrimaryColour, g_kit.savedPrimary[i]);
    }
    g_kit.saved = false;
}

// Generated away kits for the captains the game gives only one (Mario, Luigi, Waluigi, Wario; their
// mAlternateColour is 0 and no _alt art exists). The game keeps both teams in their home kit (so it
// never looks for _alt files); the team colour is changed for the HUD and captain select, and the
// team's textures are swapped in memory for recoloured versions while chosen (MscAwayKits makes them
// on a worker thread from the disc files): the captain's kit and ExtraTextures, the sidekicks' team
// texture (<sidekick>_<captain>) and the goalie's kit.
constexpr uint32_t kTextureManager = 0x806E1F08u;   // gTextureManager
constexpr uint32_t kGetTextureIndex = 0x802CE1B8u;  // glTextureManager::GetTextureIndex(hash)
struct RecolouredTexture {
    uint32_t addr, size;
    std::vector<uint8_t> original;
    uint64_t recolouredSum;
};
std::vector<RecolouredTexture> g_recoloured;
int32_t g_recolouredCaptain = -1;

uint64_t Checksum(const uint8_t* data, size_t size) {
    uint64_t h = 1469598103934665603ull;
    for (size_t i = 0; i < size; i += 61) h = (h ^ data[i]) * 1099511628211ull;
    return h;
}

void RestoreRecolouredTextures() {
    for (RecolouredTexture& t : g_recoloured) {
        uint8_t* host = Memory::GetPointer(t.addr, t.size);
        if (host != nullptr && Checksum(host, t.size) == t.recolouredSum) {  // still ours, not reloaded
            std::memcpy(host, t.original.data(), t.size);
            GxNotifyGuestRamDmaWrite(t.addr, t.size);
        }
    }
    g_recoloured.clear();
    g_recolouredCaptain = -1;
}

void EnsureTextureRecoloured(int32_t captain, const MscAwayKits::Texture& texture) {
    const uint32_t mgr = Memory::Read32(kTextureManager);
    if (mgr == 0) return;
    GuestInterruptCallbackContext call;
    call.get()->gpr[3] = mgr;
    call.get()->gpr[4] = texture.hash;
    InvokeIndirectCpu(kGetTextureIndex, call.get());
    const uint32_t index = call.get()->gpr[3];
    if (index >= 0xFFFF) return;  // not loaded
    const uint32_t tex = Memory::Read32(Memory::Read32(mgr + 0x04) + index * 4);  // PlatTexture*
    if (tex == 0 || Memory::Read32(tex + 0x08) != 2 /* GXTex_CMPR */) return;
    const uint32_t data = Memory::Read32(tex + 0x14);  // m_SwizzledData
    const uint32_t w = Memory::Read16(tex), h = Memory::Read16(tex + 2), levels = std::max(1u, uint32_t(Memory::Read8(tex + 4)));
    if (data == 0 || w == 0 || h == 0) return;
    uint32_t size = 0;
    for (uint32_t l = 0; l < levels; ++l) {
        const uint32_t lw = std::max(1u, w >> l), lh = std::max(1u, h >> l);
        size += ((lw + 7) / 8) * ((lh + 7) / 8) * 32;
    }
    uint8_t* host = Memory::GetPointer(data, size);
    if (host == nullptr) return;
    for (const RecolouredTexture& t : g_recoloured) {
        if (t.addr == data && Checksum(host, size) == t.recolouredSum) return;  // already done
    }
    std::vector<uint8_t> recoloured;
    if (MscAwayKits::Get(captain, texture, host, size, w, h, levels, recoloured) != MscAwayKits::Result::Ready ||
        recoloured.size() != size) {
        return;  // still being made, or nothing to change
    }
    std::erase_if(g_recoloured, [&](const RecolouredTexture& t) { return t.addr == data; });  // reloaded
    RecolouredTexture t{data, size, std::vector<uint8_t>(host, host + size), 0};
    std::memcpy(host, recoloured.data(), size);
    t.recolouredSum = Checksum(host, size);
    GxNotifyGuestRamDmaWrite(data, size);
    g_recoloured.push_back(std::move(t));
}

void UpdateCustomKitTextures() {
    int32_t captain = -1;
    if (g_kit.customSide >= 0 && MscAwayKits::Has(g_kit.captains[g_kit.customSide])) captain = g_kit.captains[g_kit.customSide];
    if (captain != g_recolouredCaptain) {
        RestoreRecolouredTextures();
        if (captain >= 0) MscAwayKits::Prepare(captain);
    }
    if (captain < 0) return;
    g_recolouredCaptain = captain;
    for (const MscAwayKits::Texture& texture : MscAwayKits::Textures(captain)) EnsureTextureRecoloured(captain, texture);
}

void ApplyKitOutcome() {
    RestoreCaptainColours();
    g_kit.customSide = -1;
    const int32_t c0 = g_kit.captains[0], c1 = g_kit.captains[1];
    if (g_kit.outcome == KitOutcome::Auto || c0 < 0 || c1 < 0 || c0 > 11 || c1 > 11 || c0 == c1) return;
    for (uint32_t i = 0; i < 12; ++i) {
        g_kit.savedMask[i] = Memory::Read32(kCharacterInfo + i * 0x5C + kColourMask);
        g_kit.savedRank[i] = Memory::Read32(kCharacterInfo + i * 0x5C + kColourRank);
        g_kit.savedPrimary[i] = Memory::Read32(kCharacterInfo + i * 0x5C + kPrimaryColour);
    }
    g_kit.saved = true;
    const uint32_t e0 = kCharacterInfo + static_cast<uint32_t>(c0) * 0x5C, e1 = kCharacterInfo + static_cast<uint32_t>(c1) * 0x5C;
    // An away side whose captain has no away kit of its own wears a generated one: the game keeps
    // both teams at home and only the team colour (and, in the match, the textures) change.
    if (g_kit.outcome != KitOutcome::BothHome) {
        const int side = g_kit.outcome == KitOutcome::HomeAway ? 0 : 1;
        const int32_t captain = side == 0 ? c0 : c1;
        if (Memory::Read32(kCharacterInfo + static_cast<uint32_t>(captain) * 0x5C + kAlternateColour) == 0) {
            if (MscAwayKits::Has(captain)) {
                Memory::Write32(e0 + kColourMask, 0x4000);
                Memory::Write32(e1 + kColourMask, 0x8000);
                Memory::Write32(kCharacterInfo + static_cast<uint32_t>(captain) * 0x5C + kPrimaryColour, MscAwayKits::TeamColour(captain));
                g_kit.customSide = side;
                return;
            }
        }
    }
    if (g_kit.outcome == KitOutcome::BothHome) {
        Memory::Write32(e0 + kColourMask, 0x4000);  // disjoint: nobody switches
        Memory::Write32(e1 + kColourMask, 0x8000);
    } else {
        const bool homeAway = g_kit.outcome == KitOutcome::HomeAway;
        Memory::Write32(e0 + kColourMask, 0x4000);  // overlapping: the higher rank switches
        Memory::Write32(e1 + kColourMask, 0x4000);
        Memory::Write32(e0 + kColourRank, homeAway ? 250u : 1u);
        Memory::Write32(e1 + kColourRank, homeAway ? 1u : 250u);
    }
}

// Does `side` currently wear its away kit (NeedsAlternateColour with the live table)?
bool SideInAwayKit(int side, int32_t c0, int32_t c1) {
    if (g_kit.customSide == side) return true;
    const uint32_t me = kCharacterInfo + static_cast<uint32_t>(side == 0 ? c0 : c1) * 0x5C;
    const uint32_t them = kCharacterInfo + static_cast<uint32_t>(side == 0 ? c1 : c0) * 0x5C;
    return (Memory::Read32(me + kColourMask) & Memory::Read32(them + kColourMask)) != 0 &&
           static_cast<int32_t>(Memory::Read32(me + kColourRank)) > static_cast<int32_t>(Memory::Read32(them + kColourRank));
}

// Which kit button a pad holds: bit 0 = home team's (X / Wii Remote -), bit 1 = away team's (Y / 2).
// Sides, not players: the game clears its side-to-pad mapping once captains are confirmed, and one
// player often picks both teams.
uint32_t KitButtonsDown(uint32_t chan) {
    if (WiimoteHid::Sample hid; WiimoteHid::Read(chan, hid)) {
        return ((hid.hold & 0x1000) ? 1u : 0u) | ((hid.hold & 0x0100) ? 2u : 0u);
    }
    PADStatus pad{};
    if (!MscEmulatedRemote::ReadGameCubePad(chan, pad)) return 0;
    return ((pad.button & (PAD_BUTTON_X | PAD_TRIGGER_L)) ? 1u : 0u) | ((pad.button & (PAD_BUTTON_Y | PAD_TRIGGER_R)) ? 2u : 0u);
}

namespace PartnerGrid {
bool PartnerTeam(uint32_t scene, int side);
void CycleColourway(uint32_t scene, int side);
}

void KitChoice() {
    if (!RuntimeConfigFile::ModKitChoice()) {
        if (g_kit.outcome != KitOutcome::Auto) {
            g_kit.outcome = KitOutcome::Auto;
            g_kit.customSide = -1;
            RestoreCaptainColours();
        }
        UpdateCustomKitTextures();
        return;
    }
    const uint32_t mgr = Memory::Read32(kSceneManager);
    const uint32_t depth = mgr ? Memory::Read32(mgr + 0x04) : 0;
    const uint32_t scene = depth > 0 && depth <= 32 ? Memory::Read32(mgr + 0x88 + (depth - 1) * 4) : 0;
    uint32_t down[4] = {}, pressed = 0;
    for (uint32_t chan = 0; chan < 4; ++chan) {
        down[chan] = KitButtonsDown(chan);
        pressed |= down[chan] & ~g_kit.prevButtons[chan];
    }
    if (scene != 0 && Memory::Read32(scene) == kChooseCaptainsVtable) {
        const int32_t c0 = static_cast<int32_t>(Memory::Read32(scene + kSceneCaptainIds));
        const int32_t c1 = static_cast<int32_t>(Memory::Read32(scene + kSceneCaptainIds + 4));
        if (c0 != g_kit.captains[0] || c1 != g_kit.captains[1]) {
            // New matchup: back to the game's own choice.
            g_kit.captains[0] = c0;
            g_kit.captains[1] = c1;
            g_kit.outcome = KitOutcome::Auto;
            g_kit.customSide = -1;
            RestoreCaptainColours();
        }
        const bool ready = Memory::Read8(scene + kSceneBothConfirmed) != 0 && c0 >= 0 && c1 >= 0 && c0 <= 11 && c1 <= 11 && c0 != c1;
        for (int side = 0; side < 2 && ready; ++side) {
            if (!(pressed & (1u << side))) continue;
            if (PartnerGrid::PartnerTeam(scene, side)) {  // a partner team: wear the next captain's colours
                PartnerGrid::CycleColourway(scene, side);
                continue;
            }
            // Toggle this side: into its away kit (the other side goes home), or back home. Mario,
            // Luigi, Waluigi and Wario have no away kit (mAlternateColour 0): forcing the game's own
            // switch would load art that does not exist, so they get a generated one (MscAwayKits).
            const int32_t captain = side == 0 ? c0 : c1;
            const bool hasAwayKit = Memory::Read32(kCharacterInfo + static_cast<uint32_t>(captain) * 0x5C + kAlternateColour) != 0;
            const bool inAway = SideInAwayKit(side, c0, c1);
            if (!inAway && !hasAwayKit && !MscAwayKits::Has(captain)) continue;
            g_kit.outcome = inAway ? KitOutcome::BothHome : (side == 0 ? KitOutcome::HomeAway : KitOutcome::AwayAway);
            ApplyKitOutcome();
            GuestInterruptCallbackContext call;
            for (uint32_t panel = 0; panel < 2; ++panel) {
                call.get()->gpr[3] = scene + kSceneCaptainPanels + panel * kCaptainPanelSize;
                call.get()->gpr[4] = static_cast<uint32_t>(panel == 0 ? c0 : c1);
                call.get()->gpr[5] = static_cast<uint32_t>(panel == 0 ? c1 : c0);
                InvokeIndirectCpu(kApplyCaptainColours, call.get());
            }
        }
    }
    for (uint32_t chan = 0; chan < 4; ++chan) g_kit.prevButtons[chan] = down[chan];
    UpdateCustomKitTextures();
}

// Captain-only teams: a player's voice slot is the team captain's (1 home, 5 away) for captains and
// that plus the player's ID for sidekicks (fn_800957E4), so captains filling sidekick slots spoke with
// the team captain's voice. Their own voice is loaded into slot + ID (CharacterLoader override), so
// they are pointed there.
void CaptainVoices() {
    if (Memory::Read32(kGamePtr) == 0) return;
    if (!RuntimeConfigFile::ModAllCaptains() && !Mods::Variants::FixedTeamCaptain(0) && !Mods::Variants::FixedTeamCaptain(1)) return;
    constexpr uint32_t kPlayerId = 0x1E4, kSoundSlot = 0x318;  // cPlayer
    for (uint32_t side = 0; side < 2; ++side) {
        const uint32_t team = Memory::Read32(kTeams + side * 4);
        if (team == 0) continue;
        const uint32_t base = Memory::Read32(team) == 0 ? 1u : 5u;  // cTeam::m_nSide
        for (uint32_t i = 0; i < 5; ++i) {
            const uint32_t player = Memory::Read32(team + kTeamPlayers + i * 4);
            if (player == 0) continue;
            const uint32_t id = Memory::Read32(player + kPlayerId);
            if (id >= 1 && id <= 3 && Memory::Read32(player + kSoundSlot) == base) Memory::Write32(player + kSoundSlot, base + id);
        }
    }
}

// Captain-only teams, partner select: teammates are picked from captain select's 12-captain grid, or
// from partner select's own row of partners (- and + switch between the two), so a team can mix
// both. Confirming captains keeps captain select on the scene stack under partner select (the
// BaseGameSceneManager::Pop override below; coming back from choose sides goes through captain select
// again), its grid coming up under partner select's teammate boards while a slot is being chosen. Choosing a board slot and then a
// character puts them in the slot ([mods] captain_teammates_home/away: character indices, captains
// 0-11 and partners 12-19, which the CharacterLoader overrides use). The boards show the picks
// (FECaptainComponent overrides below). Leaving partner select either way closes both screens.
namespace PartnerGrid {
constexpr uint32_t kChooseSidekicksVtable = 0x8051D584u;  // ChooseSidekicksSceneV2
// ChooseCaptainsSceneV2
constexpr uint32_t kCaptainsSceneType = 0x20, kCaptainsInputSuppressed = 0x4E, kCaptainTextures = 0x50,
                   kCaptainButtons = 0xB0, kCaptainInstances = 0x12D4, kCaptainsLayer = 0x1320, kCaptainsPdas = 0x1324,
                   kCaptainsState = 0x1380;
// ChooseSidekicksSceneV2
constexpr uint32_t kSidePads = 0x20, kSelectedSlots = 0x30, kTeams = 0x40, kSidekicksShown = 0x4C, kBoards = 0x50,
                   kBoardSize = 0x28, kSidekickButtons = 0xE8, kSlotButtons = 0x688, kPdas = 0x1354, kPdaSize = 0x2AC,
                   kSlotInstances = 0x18CC, kGreenArrows = 0x190C, kSidekicksLayer = 0x1914;
// FECaptainComponent (a team's board)
constexpr uint32_t kBoardPositions = 0x08, kBoardSide = 0x10, kBoardCaptain = 0x14, kBoardSidekicks = 0x18;
// FEPointerButton
constexpr uint32_t kPointerButtonSize = 0xB4, kPointerEvents = 0x3C, kPointerDisabled = 0x80, kPointerMinX = 0x84,
                   kPointerMaxX = 0x88, kPointerMaxY = 0x8C, kPointerMinY = 0x90, kPointerRotation = 0x94,
                   kPointerPivot = 0x98, kPointerStates = 0xA0;
constexpr uint32_t kHandlerPresentation = 0x14, kHandlerScene = 0x18, kSceneState = 0x74;  // BaseSceneHandler, FEScene
using namespace MscGuest::FE;  // TL offsets and the FEFinder lookups
constexpr uint32_t kCharacterName = 0x04, kCharacterSidekickId = 0x18, kCharacterUnlockable = 0x24,
                   kCharacterOverall = 0x48;                            // CharacterInfo
constexpr uint32_t kPointerPositions = 0x80578460u;  // gFEPointerPositions[4] (nlVector2)
constexpr uint32_t kFEInput = 0x806E2038u;           // g_pFEInput
constexpr uint32_t kCupManager = 0x806E0F90u, kPendingCupTeam = 0x8A28;  // g_pCupManager
constexpr uint32_t kGameMode = 0x11C, kGameInfos = 0x80, kRulesTable = 0x298, kRulesSize = 0xC;  // GameInfoManager
constexpr uint32_t kSetDisplayMode = 0x801E0280u;      // FECharacterPDAComponent::SetDisplayMode(int)
constexpr uint32_t kSetRecycleState = 0x801DABACu;     // FECaptainComponent::SetRecycleState(int, int)
constexpr uint32_t kReloadSidekicks = 0x801DCCECu;     // FECaptainComponent::ReloadSidekicks()
constexpr uint32_t kResetRules = 0x800FDD24u;          // GameInfoManager::ResetRules(int)
constexpr uint32_t kCaptainAcceptSound = 0x801CC090u;  // FECharacterSound::GetCaptainAcceptSound(eTeamID)
constexpr uint32_t kSidekickAcceptSound = 0x801CC0A4u; // FECharacterSound::GetSidekickAcceptSound(eSidekickID)
constexpr uint32_t kPlayAudioEvent = 0x801CBCA0u;      // FEAudio::PlayAnimAudioEvent(ulong, const void*, void*, bool)
constexpr uint32_t kPlayHoverFeedback = 0x802195B4u;   // FEPointerButton::PlayHoverFeedback(int)
constexpr uint32_t kJustPressed = 0x802FAE94u;         // FEInput::JustPressed(pad, button, remap, outPad)
constexpr uint32_t kSidekickCharacter = 0x800FBDDCu;   // GetCharacterIndexFromSidekick(int)
constexpr uint32_t kNlRandom = 0x802B6594u, kDefaultSeed = 0x806DF248u;  // nlRandom(n, &nlDefaultSeed)
constexpr uint32_t kUnlockedFns[3] = {0x8010FF0Cu, 0x8010FFA8u, 0x80110044u};  // Is{BowserJr,DiddyKong,Petey}Unlocked
constexpr uint32_t kButtonSelect = 0x1E, kButtonMinus = 0x31, kButtonPlus = 0x30;  // FE buttons: A, -, + (L, R)
constexpr int kGridCaptains[12] = {0, 5, 3, 6, 4, 7, 1, 8, 2, 9, 10, 11};  // grid button -> captain (lbl_8051CE60)
constexpr int kRowSidekicks[8] = {1, 0, 5, 4, 3, 2, 6, 7};                 // partner button -> eSidekickID (lbl_8051D198)
constexpr int kFirstSidekickCharacter = 12, kLastCharacter = 19;  // CharacterInfo: captains 0-11, partners 12-19

uint32_t g_captains = 0;  // the captain select scene kept under partner select
bool g_gridShown = false;
bool g_showPartners = false;  // the partner row is up instead of the captain grid
bool g_rowShown = false;      // the row is up: while a slot is being chosen
bool g_locked[12] = {};
std::array<int, 3> g_picks[2] = {{-1, -1, -1}, {-1, -1, -1}};  // per board slot: character, -1 = the team's captain
int g_hover[4] = {-1, -1, -1, -1};                            // grid button under each pad's pointer while choosing
int g_preview[2] = {-1, -1};                                  // captain hovered for a board's selected slot
int g_spot[2] = {-1, -1};  // the partner leading each team (captain select's partner mode), -1 = the captain

using MscGuest::Call;
using MscGuest::CString;
using MscGuest::LowerHash;

uint32_t CharacterInfo(uint32_t index) { return MscGuest::CharacterInfo(index); }  // GetCharacterInfo
bool IsSidekick(int character) { return character >= kFirstSidekickCharacter && character <= kLastCharacter; }

// Partner select while it is on top of the kept captain select, else 0.
uint32_t PartnerScene() {
    if (g_captains == 0) return 0;
    const uint32_t mgr = Memory::Read32(kSceneManager);
    const uint32_t depth = mgr ? Memory::Read32(mgr + 0x04) : 0;
    if (depth < 2 || depth > 32) return 0;
    const uint32_t top = Memory::Read32(mgr + 0x88 + (depth - 1) * 4);
    if (Memory::Read32(top) != kChooseSidekicksVtable || Memory::Read32(mgr + 0x88 + (depth - 2) * 4) != g_captains) return 0;
    return top;
}
// Partner select on top of the scene stack, in either mode (with or without captain select kept
// under it), else 0.
uint32_t TopPartnerScene() {
    const uint32_t mgr = Memory::Read32(kSceneManager);
    const uint32_t depth = mgr ? Memory::Read32(mgr + 0x04) : 0;
    if (depth < 1 || depth > 32) return 0;
    const uint32_t top = Memory::Read32(mgr + 0x88 + (depth - 1) * 4);
    return top != 0 && Memory::Read32(top) == kChooseSidekicksVtable ? top : 0;
}
// Which of partner select's boards (FECaptainComponent) `board` is, or -1.
int BoardSide(uint32_t board) {
    const uint32_t scene = PartnerScene();
    if (scene == 0 || board < scene + kBoards || board >= scene + kBoards + 2 * kBoardSize) return -1;
    return static_cast<int>((board - scene - kBoards) / kBoardSize);
}
int SlotCharacter(uint32_t board, int side, int slot) {  // what a board slot shows
    const uint32_t scene = PartnerScene();
    if (scene != 0 && g_preview[side] >= 0 && static_cast<int32_t>(Memory::Read32(scene + kSelectedSlots + side * 4)) == slot)
        return g_preview[side];
    const int pick = g_picks[side][slot];
    if (pick >= 0) return pick;
    return g_spot[side] >= 0 ? g_spot[side] : static_cast<int32_t>(Memory::Read32(board + kBoardCaptain));  // the team's leader
}

void SavePicks(int side) { RuntimeConfigFile::SetModCaptainTeammates(side, g_picks[side]); }

void LeavePartnerMode(uint32_t scene);

void KeepScene(uint32_t captains) {
    g_captains = captains;
    g_gridShown = false;
    g_showPartners = false;
    g_rowShown = false;
    for (int side = 0; side < 2; ++side) {
        g_picks[side] = RuntimeConfigFile::ModCaptainTeammates(side);
        g_spot[side] = RuntimeConfigFile::ModCaptainSpot(side);
        g_preview[side] = -1;
    }
    for (int& hover : g_hover) hover = -1;
}

bool Offline() {
    const uint32_t info = Memory::Read32(kGameInfoManager);
    return info != 0 && Memory::Read8(info + 0x120) == 0;  // !mIsOnlineMode
}

// Pop override: true when this pop is the one closing captain select to open partner select
// (Push(SCENE_CHOOSE_SIDEKICKS, ..., popfirst)), which is then skipped and the scene kept.
bool KeepCaptainSelect(uint32_t top, uint32_t lr) {
    constexpr uint32_t kPushBegin = 0x801C4E44u, kPushEnd = 0x801C5ED0u;  // BaseGameSceneManager::Push
    if (!RuntimeConfigFile::ModAllCaptains() || top == 0 || lr < kPushBegin || lr >= kPushEnd) return false;
    if (Memory::Read32(top) != kChooseCaptainsVtable || Memory::Read32(top + kCaptainsState) != 2) return false;
    if (Memory::Read32(top + kCaptainsSceneType) != 0 || !Offline()) return false;  // Striker Cup, online
    Memory::Write32(top + kCaptainsState, 1);  // done: input stays suppressed, no second push
    LeavePartnerMode(top);
    KeepScene(top);
    return true;
}

// Choose sides going back (LeaveScene override below): captain select, pushed coming back (captains
// confirmed) with its input suppressed and already done, so once loaded it goes straight on to partner
// select, which is kept on top of it as when going forward. Pushing both at once would load the two
// screens together, which never finishes (both wait on shared resources, FEScene state 5).
uint32_t g_forwarding = 0;

uint32_t PushForwardingCaptainSelect(CpuContext* ctx, uint32_t mgr, uint32_t push) {
    const uint32_t captains = Call(ctx, push, {mgr, 2, 2, 0});  // SCENE_CHOOSE_CAPTAINS_DOMINATION, SCREEN_BACK
    if (captains == 0) return 0;
    Memory::Write32(captains + kCaptainsState, 2);
    Memory::Write8(captains + kCaptainsInputSuppressed, 1);
    g_forwarding = captains;
    return captains;
}

bool ContainsPoint(uint32_t region, float x, float y) {  // FEPointerRegion::ContainsPoint
    const float rotation = Memory::ReadFloat32(region + kPointerRotation);
    if (rotation != 0.0f) {
        const float px = Memory::ReadFloat32(region + kPointerPivot), py = Memory::ReadFloat32(region + kPointerPivot + 4);
        const float lx = x - px, ly = y - py, c = std::cos(rotation), s = std::sin(rotation);
        x = lx * c + ly * s + px;
        y = lx * -s + ly * c + py;
    }
    return x >= Memory::ReadFloat32(region + kPointerMinX) && x <= Memory::ReadFloat32(region + kPointerMaxX) &&
           y >= Memory::ReadFloat32(region + kPointerMinY) && y <= Memory::ReadFloat32(region + kPointerMaxY);
}

bool HoveredByOther(int button, int pad) {
    for (int i = 0; i < 4; ++i)
        if (i != pad && g_hover[i] == button) return true;
    return false;
}

void ClearHover(CpuContext* ctx, int pad) {
    const int old = g_hover[pad];
    g_hover[pad] = -1;
    if (old >= 0 && !HoveredByOther(old, pad))
        SetActiveSlide(ctx, Memory::Read32(g_captains + kCaptainInstances + static_cast<uint32_t>(old) * 4), "off");
}

// Captain select's grid buttons all lit (it greys out the two team captains, who can be teammates
// too), locked captains keep their static.
void RefreshGrid() {
    for (uint32_t i = 0; i < 12; ++i) {
        char group[8], image[32];
        std::snprintf(group, sizeof(group), "%u", i);
        std::snprintf(image, sizeof(image), "%02u_dummy_texture", i);
        const char* const groupPath[] = {group};
        const uint32_t groupInstance = FindInSlide(ActiveSlide(Memory::Read32(g_captains + kCaptainInstances + i * 4)), groupPath);
        if (groupInstance == 0) continue;
        const char* const noisePath[] = {"noise"};
        const char* const imagePath[] = {image};
        const uint32_t noise = FindIn(groupInstance, noisePath);
        const uint32_t texture = Memory::Read32(g_captains + kCaptainTextures + i * 8 + 4);  // mCaptainTextures[i][1]
        if (noise) Memory::Write8(noise + kInstanceVisible, g_locked[i] ? 1 : 0);
        if (const uint32_t img = FindIn(groupInstance, imagePath); img && texture && !g_locked[i])
            Memory::Write32(img + kImageTexture, texture);
    }
}

// The bookkeeping of ChooseSidekicksSceneV2::OnSidekickPointerPress once a slot is filled: the slot
// and its buttons back to normal, the board out of choosing, no side choosing.
void FinishSlot(CpuContext* ctx, uint32_t scene, int side, int32_t slot) {
    SetActiveSlide(ctx, Memory::Read32(scene + kSlotInstances + static_cast<uint32_t>(side * 3 + slot) * 4), "off");
    for (uint32_t i = 0; i < 3; ++i)
        for (uint32_t p = 0; p < 4; ++p)
            Memory::Write32(scene + kSlotButtons + (side * 3 + i) * kPointerButtonSize + kPointerStates + p * 4, 0);
    Call(ctx, kSetDisplayMode, {scene + kPdas + side * kPdaSize, 1});
    if (const uint32_t arrow = Memory::Read32(scene + kGreenArrows + side * 4)) Memory::Write8(arrow + kInstanceVisible, 0);
    Call(ctx, kSetRecycleState, {scene + kBoards + side * kBoardSize, static_cast<uint32_t>(slot), 1});
    Memory::Write32(scene + kSelectedSlots + side * 4, 0xFFFFFFFFu);
    Memory::Write32(scene + kSidePads + side * 4, 0xFFFFFFFFu);
}

// The captain in grid button `button` goes in the selected slot of the board `side` is choosing for.
void Assign(CpuContext* ctx, uint32_t scene, int side, int pad, int button) {
    const int32_t slot = static_cast<int32_t>(Memory::Read32(scene + kSelectedSlots + side * 4));
    if (slot < 0 || slot > 2) return;
    const int captain = kGridCaptains[button];
    g_picks[side][slot] = captain;
    SavePicks(side);
    FinishSlot(ctx, scene, side, slot);
    g_preview[side] = -1;
    ClearHover(ctx, pad);
    Call(ctx, kPlayAudioEvent, {Call(ctx, kCaptainAcceptSound, {static_cast<uint32_t>(captain)}), 0, 0, 1});
}

// The captain grid or the partner row slides in (the other out, if it was up), with the sound partner
// select plays when its row comes up.
void ShowRow(CpuContext* ctx, uint32_t scene, bool partners) {
    const uint32_t grid = Memory::Read32(g_captains + kCaptainsLayer);
    const uint32_t row = Memory::Read32(scene + kSidekicksLayer);
    Memory::Write8((partners ? row : grid) + kInstanceVisible, 1);
    SetActiveSlide(ctx, partners ? row : grid, "in");
    if (g_rowShown) SetActiveSlide(ctx, partners ? grid : row, "out");
    Call(ctx, kPlayAudioEvent, {0xDF52130Fu, 0, 0, 1});
    g_rowShown = true;
}

// The row slides away once no slot is being chosen, with the sound captain select's grid leaves with.
void HideRow(CpuContext* ctx, uint32_t scene) {
    SetActiveSlide(ctx, Memory::Read32(g_showPartners ? scene + kSidekicksLayer : g_captains + kCaptainsLayer), "out");
    Call(ctx, kPlayAudioEvent, {0x0B8C09FAu, 0, 0, 1});
    g_rowShown = false;
}

// - and + swap the captain grid and the partner row; with neither up, they pick which comes up next.
void ShowPartners(CpuContext* ctx, uint32_t scene, bool partners) {
    g_showPartners = partners;
    for (int pad = 0; pad < 4; ++pad) ClearHover(ctx, pad);
    g_preview[0] = g_preview[1] = -1;
    if (g_rowShown) ShowRow(ctx, scene, partners);
}

// A scene whose package is loaded and SceneCreated has run (FESceneManager::InitializeScene).
bool Loaded(uint32_t handler) {
    const uint32_t feScene = Memory::Read32(handler + kHandlerScene);
    return feScene != 0 && Memory::Read32(feScene + kSceneState) == 6 && Memory::Read32(handler + kHandlerPresentation) != 0;
}

// Captain select going straight on: its panels skip to the end of sliding out, which is what its
// state 2 (Done pressed) waits for before pushing partner select.
void Forward() {
    const uint32_t mgr = Memory::Read32(kSceneManager);
    const uint32_t depth = mgr ? Memory::Read32(mgr + 0x04) : 0;
    const uint32_t top = depth >= 1 && depth <= 32 ? Memory::Read32(mgr + 0x88 + (depth - 1) * 4) : 0;
    if (top != g_forwarding) {
        bool present = false;
        for (uint32_t i = 0; i < depth && depth <= 32; ++i) present |= Memory::Read32(mgr + 0x88 + i * 4) == g_forwarding;
        if (!present) g_forwarding = 0;
        return;
    }
    if (!Loaded(top)) return;
    GuestInterruptCallbackContext call;
    for (uint32_t side = 0; side < 2; ++side) {
        const uint32_t pda = Memory::Read32(top + kCaptainsPdas + side * 4);
        if (pda == 0) continue;
        Memory::Write8(pda + kInstanceVisible, 0);
        SetActiveSlide(call.get(), pda, "out");
        if (const uint32_t slide = ActiveSlide(pda))  // TLSlide: m_start +0x10, m_duration +0x14, m_time +0x18
            Memory::WriteFloat32(slide + 0x18, Memory::ReadFloat32(slide + 0x10) + Memory::ReadFloat32(slide + 0x14));
    }
    g_forwarding = 0;
}

void Update() {
    if (g_forwarding != 0) Forward();
    const uint32_t scene = PartnerScene();
    if (scene == 0 || !Loaded(scene) || !Loaded(g_captains)) return;
    GuestInterruptCallbackContext call;
    CpuContext* ctx = call.get();
    if (!g_gridShown) {
        Memory::Write8(Memory::Read32(g_captains + kCaptainsLayer) + kInstanceVisible, 0);  // until a slot is chosen
        const char* const titlesPath[] = {"Layer", "SCREEN_TITLES"};
        const uint32_t presentation = Memory::Read32(g_captains + kHandlerPresentation);
        if (const uint32_t titles = FindInSlide(Memory::Read32(presentation + 0x04), titlesPath))
            Memory::Write8(titles + kInstanceVisible, 0);  // partner select has its own
        for (int i = 0; i < 12; ++i)
            g_locked[i] = kGridCaptains[i] >= 9 && Call(ctx, kUnlockedFns[kGridCaptains[i] - 9], {}) == 0;
        g_gridShown = true;
    }
    for (uint32_t side = 0; side < 2; ++side)  // captain select's panels: partner select's boards replace them
        if (const uint32_t pda = Memory::Read32(g_captains + kCaptainsPdas + side * 4)) Memory::Write8(pda + kInstanceVisible, 0);
    // The partner row only comes up with - or +, not when a slot is chosen (OnSlotPointerPress).
    Memory::Write8(scene + kSidekicksShown, 1);
    for (uint32_t i = 0; i < 8; ++i)
        Memory::Write8(scene + kSidekickButtons + i * kPointerButtonSize + kPointerDisabled, g_showPartners && g_rowShown ? 0 : 1);
    // A team's picks that are partners are its board's partners too (pictures, and the game's own
    // choice when Done commits them).
    for (uint32_t side = 0; side < 2; ++side) {
        for (uint32_t slot = 0; slot < 3; ++slot) {
            const int character = g_picks[side][slot] >= 0 ? g_picks[side][slot] : g_spot[side];
            if (IsSidekick(character))
                Memory::Write32(scene + kBoards + side * kBoardSize + kBoardSidekicks + slot * 4,
                                Memory::Read32(CharacterInfo(static_cast<uint32_t>(character)) + kCharacterSidekickId));
        }
    }

    const uint32_t input = Memory::Read32(kFEInput);
    const auto justPressed = [&](int pad, uint32_t button) {
        return input != 0 && (Call(ctx, kJustPressed, {input, static_cast<uint32_t>(pad), button, 1, 0}) & 0xFF) != 0;
    };
    for (int pad = 0; pad < 4; ++pad) {
        if (justPressed(pad, kButtonMinus) || justPressed(pad, kButtonPlus)) {
            ShowPartners(ctx, scene, !g_showPartners);
            break;
        }
    }
    const bool choosingSlot = static_cast<int32_t>(Memory::Read32(scene + kSelectedSlots)) >= 0 ||
                              static_cast<int32_t>(Memory::Read32(scene + kSelectedSlots + 4)) >= 0;
    if (choosingSlot && !g_rowShown) ShowRow(ctx, scene, g_showPartners);
    else if (!choosingSlot && g_rowShown) HideRow(ctx, scene);
    for (int pad = 0; pad < 4; ++pad) {
        int side = -1;
        for (int s = 0; s < 2; ++s)
            if (static_cast<int32_t>(Memory::Read32(scene + kSidePads + s * 4)) == pad) side = s;
        const bool choosing = !g_showPartners && side >= 0 && static_cast<int32_t>(Memory::Read32(scene + kSelectedSlots + side * 4)) >= 0;
        int hit = -1;
        const float x = Memory::ReadFloat32(kPointerPositions + pad * 8), y = Memory::ReadFloat32(kPointerPositions + pad * 8 + 4);
        for (int i = 0; i < 12; ++i) {
            const uint32_t button = g_captains + kCaptainButtons + static_cast<uint32_t>(i) * kPointerButtonSize;
            const bool live = choosing && !g_locked[i];
            if (live && hit < 0 && ContainsPoint(button, x, y)) hit = i;
            // The event the button has seen (FEPointerListener::mPreviousEvents[pad]), as
            // ProcessPointerEvent records it: menu navigation (pad.cpp) only targets buttons that get
            // the pointer's events, so the grid is a target only for a pad choosing a slot.
            const uint32_t event = button + kPointerEvents + static_cast<uint32_t>(pad) * 0x10;
            Memory::Write32(event, live ? static_cast<uint32_t>(pad) : 0xFFFFFFFFu);
            Memory::WriteFloat32(event + 4, x);
            Memory::WriteFloat32(event + 8, y);
            Memory::Write32(event + 0x0C, 0);  // mPressed, mReleased
        }
        if (hit != g_hover[pad]) {
            ClearHover(ctx, pad);
            g_hover[pad] = hit;
            if (hit >= 0) {
                if (!HoveredByOther(hit, pad))
                    SetActiveSlide(ctx, Memory::Read32(g_captains + kCaptainInstances + static_cast<uint32_t>(hit) * 4), "over");
                Call(ctx, kPlayHoverFeedback, {g_captains + kCaptainButtons + static_cast<uint32_t>(hit) * kPointerButtonSize,
                                               static_cast<uint32_t>(pad)});
                Call(ctx, kPlayAudioEvent, {0xA183DBCDu + static_cast<uint32_t>(pad), 0, 0, 1});
            }
        }
        if (side >= 0) g_preview[side] = choosing && hit >= 0 ? kGridCaptains[hit] : -1;
        if (choosing && hit >= 0 && justPressed(pad, kButtonSelect)) Assign(ctx, scene, side, pad, hit);
    }
    for (int side = 0; side < 2; ++side)
        if (static_cast<int32_t>(Memory::Read32(scene + kSidePads + side * 4)) < 0) g_preview[side] = -1;
    RefreshGrid();
}

// Captain select's partner mode (captain-only teams): - and + swap the captain grid's portraits for
// the eight partners, so a partner can lead a team. A partner team wears a captain's colours,
// banners and goalie (its colourway: Mario at home and Luigi away by default, the next free captain
// if taken; X/Y or L/R cycle it once both teams are chosen), with the partner in the captain spot
// ([mods] captain_spot_home/away). In partner mode the grid buttons keep their regions (menu
// navigation) but their hover and press handlers are muted, and the picks are handled here.
constexpr uint32_t kCsSidePads = 0x28, kCsSelected = 0x30, kCsConfirmed = 0x38, kCsChangeTextShown = 0x3C,
                   kCsCaptainIds = 0x40, kCsButtonsInitialized = 0x4A, kCsSelectButtons = 0x920,
                   kCsSelectButtonInstances = 0x1304, kCsGreenArrows = 0x1318, kCsSelectDisplays = 0x132C,
                   kCsCaptainImages = 0x1338;  // ChooseCaptainsSceneV2
constexpr uint32_t kCallbacks[4] = {0x0C, 0x14, 0x1C, 0x24};  // FEPointerListener: enter, leave, inside, press
constexpr int kGridPartners[12] = {-1, 1, 0, 5, 4, -1, -1, 3, 2, 6, 7, -1};  // eSidekickID per grid button
constexpr int kSidekickCharacters[8] = {15, 14, 13, 12, 16, 17, 18, 19};     // eSidekickID -> character
constexpr const char* kPartnerTextures[8] = {"sidekick_2_toad_S",  "sidekick_1_koopa_S",    "sidekick_5_hammer_S",
                                             "sidekick_6_birdo_S", "sidekick_3_boo_S",      "sidekick_4_drybones_S",
                                             "sidekick_8_monty_S", "sidekick_9_shy_S"};  // in mainui.dmn
constexpr uint32_t kSetCaptainInfo = 0x801DEDD0u;    // FECharacterPDAComponent::SetCaptainInfo(int, int, ulong)
constexpr uint32_t kUpdateSelectText = 0x80227BCCu;  // ChooseCaptainsSceneV2::UpdateSelectText(int)
constexpr uint32_t kNlMalloc = 0x802AA79Cu;          // nlMalloc(size, alignment, fromEnd)

uint32_t g_csScene = 0;        // the captain select these belong to
bool g_csPartners = false;     // its grid shows the partners
bool g_csMuted = false;
uint32_t g_csSaved[12][4][2];  // the grid buttons' muted handlers (Function2: tag, functor)
int g_csHover[4] = {-1, -1, -1, -1};
int g_teamPartner[2] = {-1, -1};  // the partner leading each side's team (character), -1 = a captain
uint32_t g_partnerTextures[8] = {};

// Captain select's pages (- and +): the captains, the partners (captain-only teams) and the mod
// captains (mod packages' [[character]] kind "captain", runtime/src/mods), twelve to a page on the
// grid's buttons, each on its base captain's button when that's free. A mod captain picked from its
// page makes the team its base's (the team id), with a selected button of none, so its base stays
// free for the other side; the pick itself is [mod_selection] home/away, which the match reads.
constexpr uint32_t kOffGrid = 12;     // mSelectedCaptains for a mod captain: no grid button
int g_csModPage = -1;                 // the mods page shown, -1 = the captains or partners
int g_csSwapTo = -1;                  // the page the grid comes back with once it has slid out
const Mods::CharacterDef* g_teamMod[2] = {};
std::map<std::pair<const Mods::CharacterDef*, bool>, uint32_t> g_modPortraits;  // (character, greyed) -> FETextureResource

const Mods::CharacterDef* FindModCaptain(const std::string& name) {
    for (const Mods::Package* package : Mods::ActivePackages())
        for (const Mods::CharacterDef& c : package->characters)
            if (c.name == name && c.kind == "captain" && c.baseIndex >= 0 && c.baseIndex < 12) return &c;
    return nullptr;
}

uint32_t TextureHash(const char* name) {  // FE texture resource hash ("fe/" + name, nlStringLowerHash)
    uint32_t h = 0xEB076B20u;
    for (; *name; ++name) h = h * 33 + static_cast<uint8_t>(std::tolower(static_cast<unsigned char>(*name)));
    return h;
}

// FETextureResources for the partner portraits (permanent textures), made once from a captain portrait's.
uint32_t PartnerTexture(CpuContext* ctx, uint32_t scene, int sidekick) {
    if (g_partnerTextures[sidekick] == 0) {
        const uint32_t model = Memory::Read32(scene + kCaptainTextures + 4);  // mCaptainTextures[0][1]
        if (model == 0) return 0;
        const uint32_t texture = Call(ctx, kNlMalloc, {0x20, 8, 0});
        if (texture == 0) return 0;
        for (uint32_t i = 0; i < 0x20; i += 4) Memory::Write32(texture + i, Memory::Read32(model + i));
        const uint32_t hash = TextureHash(kPartnerTextures[sidekick]);
        Memory::Write32(texture + 0x00, 0);     // m_next
        Memory::Write32(texture + 0x04, 0);     // m_prev
        Memory::Write32(texture + 0x0C, hash);  // m_hashID
        Memory::Write8(texture + 0x10, 1);      // m_bValid
        Memory::Write32(texture + 0x18, hash);  // m_glTextureHandle (same size as the captain portraits)
        g_partnerTextures[sidekick] = texture;
    }
    return g_partnerTextures[sidekick];
}

bool CaptainLocked(CpuContext* ctx, int captain) {
    return captain >= 9 && captain <= 11 && Call(ctx, kUnlockedFns[captain - 9], {}) == 0;
}

int GridIndexOf(int captain) {
    for (int i = 0; i < 12; ++i)
        if (kGridCaptains[i] == captain) return i;
    return -1;
}

const std::vector<std::array<const Mods::CharacterDef*, 12>>& ModPages() {
    static std::vector<std::array<const Mods::CharacterDef*, 12>> pages;
    static bool laidOut = false;
    if (laidOut) return pages;
    laidOut = true;
    for (const Mods::Package* package : Mods::ActivePackages()) {
        for (const Mods::CharacterDef& c : package->characters) {
            if (c.kind != "captain" || c.baseIndex < 0 || c.baseIndex >= 12) continue;
            const int own = GridIndexOf(c.baseIndex);
            bool placed = false;
            for (auto& page : pages)  // its base's button on the first page where it's free
                if (!page[own]) { page[own] = &c; placed = true; break; }
            for (size_t p = 0; !placed && p < pages.size(); ++p)  // else any free button
                for (auto& slot : pages[p])
                    if (!slot) { slot = &c; placed = true; break; }
            if (!placed) {
                pages.emplace_back();
                pages.back().fill(nullptr);
                pages.back()[own] = &c;
            }
        }
    }
    return pages;
}

const Mods::CharacterDef* ModAt(int button) {
    const auto& pages = ModPages();
    return g_csModPage >= 0 && static_cast<size_t>(g_csModPage) < pages.size() && button >= 0 && button < 12 ? pages[g_csModPage][button] : nullptr;
}

bool ButtonUsed(int button) {
    if (g_csModPage >= 0) return ModAt(button) != nullptr;
    if (g_csPartners) return kGridPartners[button] >= 0;
    return true;
}

// A mod captain's portrait (captain select's FETextureResource, like the partners'): the texture its
// mod adds as fe/screens/images/captain_<name>_s (greyed: _ds), else its base captain's portrait.
uint32_t ModPortrait(CpuContext* ctx, uint32_t scene, const Mods::CharacterDef* def, bool greyed) {
    const std::string name = "captain_" + def->name + (greyed ? "_ds" : "_s");
    if (!Mods::Catalogs::HasFeTexture("fe/screens/images/" + name))
        return Memory::Read32(scene + kCaptainTextures + static_cast<uint32_t>(GridIndexOf(def->baseIndex)) * 8 + (greyed ? 0 : 4));
    uint32_t& texture = g_modPortraits[{def, greyed}];
    if (texture == 0) {
        const uint32_t model = Memory::Read32(scene + kCaptainTextures + 4);  // mCaptainTextures[0][1]
        if (model == 0 || (texture = Call(ctx, kNlMalloc, {0x20, 8, 0})) == 0) return 0;
        for (uint32_t i = 0; i < 0x20; i += 4) Memory::Write32(texture + i, Memory::Read32(model + i));
        const uint32_t hash = TextureHash(name.c_str());
        Memory::Write32(texture + 0x00, 0);     // m_next
        Memory::Write32(texture + 0x04, 0);     // m_prev
        Memory::Write32(texture + 0x0C, hash);  // m_hashID
        Memory::Write8(texture + 0x10, 1);      // m_bValid
        Memory::Write32(texture + 0x18, hash);  // m_glTextureHandle
    }
    return texture;
}

// The next captain after `from` (in grid order) that is unlocked and not the other team's.
int NextColourway(CpuContext* ctx, uint32_t scene, int side, int from) {
    const int other = Memory::Read8(scene + kCsConfirmed + (side ^ 1)) ? static_cast<int32_t>(Memory::Read32(scene + kCsCaptainIds + (side ^ 1) * 4)) : -1;
    const int start = GridIndexOf(from);
    for (int step = 1; step <= 12; ++step) {
        const int captain = kGridCaptains[(start + step + 12) % 12];
        if (captain != other && !CaptainLocked(ctx, captain)) return captain;
    }
    return from;
}

// Muted: the grid's hover and press handlers saved and emptied (the partner and mods pages handle
// their picks here). Either way, buttons the page doesn't use are off and hidden.
void MuteGrid(uint32_t scene, bool mute) {
    for (uint32_t i = 0; i < 12; ++i) {
        const uint32_t button = scene + kCaptainButtons + i * kPointerButtonSize;
        if (mute != g_csMuted) {
            for (uint32_t k = 0; k < 4; ++k) {
                const uint32_t callback = button + kCallbacks[k];
                if (mute) {
                    g_csSaved[i][k][0] = Memory::Read32(callback);
                    g_csSaved[i][k][1] = Memory::Read32(callback + 4);
                    Memory::Write32(callback, 0);  // FUNCTION_EMPTY
                } else {
                    Memory::Write32(callback, g_csSaved[i][k][0]);
                    Memory::Write32(callback + 4, g_csSaved[i][k][1]);
                }
            }
        }
        for (uint32_t p = 0; p < 4; ++p) Memory::Write32(button + kPointerStates + p * 4, 0);
        const bool unused = mute && !ButtonUsed(static_cast<int>(i));
        Memory::Write8(button + kPointerDisabled, unused ? 1 : 0);
        if (const uint32_t instance = Memory::Read32(scene + kCaptainInstances + i * 4)) Memory::Write8(instance + kInstanceVisible, unused ? 0 : 1);
    }
    g_csMuted = mute;
}

// Back to the captain grid, handlers restored, when captain select is kept under partner select or
// popped.
void LeavePartnerMode(uint32_t scene) {
    if (scene != g_csScene) return;
    g_csPartners = false;
    g_csModPage = g_csSwapTo = -1;
    MuteGrid(scene, false);
    for (int& hover : g_csHover) hover = -1;
    g_csScene = 0;  // a later captain select starts afresh (even at the same address)
}

void SaveTeamPartner(int side, int character) {
    if (RuntimeConfigFile::ModCaptainSpot(side) != character) RuntimeConfigFile::SetModCaptainSpot(side, character);
}

// The partner in grid button `button` leads `side`'s team, as OnCaptainPointerPress confirms a captain.
void ConfirmPartner(CpuContext* ctx, uint32_t scene, int side, int pad, int button) {
    const int sidekick = kGridPartners[button];
    const int colourway = NextColourway(ctx, scene, side, side == 0 ? 11 : 6);  // after petey: mario; after waluigi: luigi
    Memory::Write8(scene + kCsConfirmed + side, 1);
    Memory::Write32(scene + kCsSidePads + side * 4, 0xFFFFFFFFu);
    Memory::Write32(scene + kCsCaptainIds + side * 4, static_cast<uint32_t>(colourway));
    Memory::Write32(scene + kCsSelected + side * 4, static_cast<uint32_t>(GridIndexOf(colourway)));
    SetActiveSlide(ctx, Memory::Read32(scene + kCsSelectDisplays + side * 4), "off");
    Memory::Write32(scene + kCsSelectButtons + side * kPointerButtonSize + kPointerStates + pad * 4, 0);
    for (uint32_t p = 0; p < 4; ++p) Memory::Write32(scene + kCaptainButtons + button * kPointerButtonSize + kPointerStates + p * 4, 0);
    if (const uint32_t arrow = Memory::Read32(scene + kCsGreenArrows + side * 4)) Memory::Write8(arrow + kInstanceVisible, 0);
    Call(ctx, kPlayAudioEvent, {Call(ctx, kSidekickAcceptSound, {static_cast<uint32_t>(sidekick)}), 0, 0, 1});
    if (!Memory::Read8(scene + kCsChangeTextShown + side)) Call(ctx, kUpdateSelectText, {scene, static_cast<uint32_t>(side)});
    if (const uint32_t select = Memory::Read32(scene + kCsSelectButtonInstances + side * 4)) Memory::Write8(select + kInstanceVisible, 1);
    Call(ctx, kSetCaptainInfo, {scene + kSceneCaptainPanels + side * kCaptainPanelSize, static_cast<uint32_t>(colourway),
                                static_cast<uint32_t>(pad), 1});  // the panel shows whose colours the team wears
    g_teamPartner[side] = kSidekickCharacters[sidekick];
    SaveTeamPartner(side, g_teamPartner[side]);
    if (g_teamMod[side]) {
        g_teamMod[side] = nullptr;
        RuntimeConfigFile::SetModSelection(side, "");
    }
    g_csHover[pad] = -1;
}

// The mod captain in grid button `button` leads `side`'s team: the team is its base captain's, picked
// as OnCaptainPointerPress picks one, except the selected button (none: the base stays free).
void ConfirmModCaptain(CpuContext* ctx, uint32_t scene, int side, int pad, int button) {
    const Mods::CharacterDef* def = ModAt(button);
    if (def == nullptr || CaptainLocked(ctx, def->baseIndex)) return;
    if (Memory::Read8(scene + kCsConfirmed + (side ^ 1)) && g_teamMod[side ^ 1] == def) return;  // the other team's
    const uint32_t base = static_cast<uint32_t>(def->baseIndex);
    Memory::Write8(scene + kCsConfirmed + side, 1);
    Memory::Write32(scene + kCsSidePads + side * 4, 0xFFFFFFFFu);
    Memory::Write32(scene + kCsCaptainIds + side * 4, base);
    Memory::Write32(scene + kCsSelected + side * 4, kOffGrid);
    SetActiveSlide(ctx, Memory::Read32(scene + kCsSelectDisplays + side * 4), "off");
    Memory::Write32(scene + kCsSelectButtons + side * kPointerButtonSize + kPointerStates + pad * 4, 0);
    for (uint32_t p = 0; p < 4; ++p) Memory::Write32(scene + kCaptainButtons + button * kPointerButtonSize + kPointerStates + p * 4, 0);
    if (const uint32_t arrow = Memory::Read32(scene + kCsGreenArrows + side * 4)) Memory::Write8(arrow + kInstanceVisible, 0);
    if (!Mods::Variants::PlayMenuVoice(ctx, side, def))  // its own voice, else its base's accept sound
        Call(ctx, kPlayAudioEvent, {Call(ctx, kCaptainAcceptSound, {base}), 0, 0, 1});
    if (!Memory::Read8(scene + kCsChangeTextShown + side)) Call(ctx, kUpdateSelectText, {scene, static_cast<uint32_t>(side)});
    if (const uint32_t select = Memory::Read32(scene + kCsSelectButtonInstances + side * 4)) Memory::Write8(select + kInstanceVisible, 1);
    g_teamMod[side] = def;
    RuntimeConfigFile::SetModSelection(side, def->name);
    // The stats panel shows the mod character: its team id tagged for the side (mods/variants.h).
    Mods::Variants::SetMenuCharacter(side, def);
    Call(ctx, kSetCaptainInfo, {scene + kSceneCaptainPanels + side * kCaptainPanelSize, Mods::Variants::TagTeam(base, side),
                                static_cast<uint32_t>(pad), 1});
    if (g_teamPartner[side] >= 0) {
        g_teamPartner[side] = -1;
        SaveTeamPartner(side, -1);
    }
    g_csHover[pad] = -1;
}

bool PartnerTeam(uint32_t scene, int side) { return scene == g_csScene && g_teamPartner[side] >= 0; }

// X/Y (L/R) once both teams are chosen: a partner team's colourway moves to the next captain.
void CycleColourway(uint32_t scene, int side) {
    GuestInterruptCallbackContext call;
    const int current = static_cast<int32_t>(Memory::Read32(scene + kCsCaptainIds + side * 4));
    const int next = NextColourway(call.get(), scene, side, current);
    if (next == current) return;
    Memory::Write32(scene + kCsCaptainIds + side * 4, static_cast<uint32_t>(next));
    Memory::Write32(scene + kCsSelected + side * 4, static_cast<uint32_t>(GridIndexOf(next)));
    Call(call.get(), kSetCaptainInfo, {scene + kSceneCaptainPanels + side * kCaptainPanelSize, static_cast<uint32_t>(next), 0, 1});
    Call(call.get(), kPlayAudioEvent, {0xA183DBCDu, 0, 0, 1});
}

void CaptainSelect() {
    const uint32_t mgr = Memory::Read32(kSceneManager);
    const uint32_t depth = mgr ? Memory::Read32(mgr + 0x04) : 0;
    const uint32_t scene = depth >= 1 && depth <= 32 ? Memory::Read32(mgr + 0x88 + (depth - 1) * 4) : 0;
    const bool allCaptains = RuntimeConfigFile::ModAllCaptains();
    static bool s_menuVoices = false;  // mod captains' voice banks loaded for captain select
    if (scene == 0 || Memory::Read32(scene) != kChooseCaptainsVtable || !(allCaptains || !ModPages().empty()) ||
        Memory::Read32(scene + kCaptainsSceneType) != 0 || !Offline()) {
        if (s_menuVoices) {
            GuestInterruptCallbackContext release;
            s_menuVoices = !Mods::Variants::ReleaseMenuVoices(release.get());
            Mods::Variants::SetMenuCharacter(0, nullptr);
            Mods::Variants::SetMenuCharacter(1, nullptr);
        }
        return;
    }
    static bool s_fixSelected = false;
    if (scene != g_csScene) {  // a new captain select: its teams as left (coming back) or none
        g_csScene = scene;
        g_csPartners = g_csMuted = false;
        g_csModPage = g_csSwapTo = -1;
        for (int& hover : g_csHover) hover = -1;
        for (int side = 0; side < 2; ++side) {
            g_teamPartner[side] = allCaptains ? RuntimeConfigFile::ModCaptainSpot(side) : -1;
            g_teamMod[side] = FindModCaptain(RuntimeConfigFile::ModSelection(side));
        }
        s_fixSelected = true;
    }
    if (!Loaded(scene) || !Memory::Read8(scene + kCsButtonsInitialized) || Memory::Read32(scene + kCaptainsState) != 1 ||
        Memory::Read8(scene + kCaptainsInputSuppressed))
        return;
    GuestInterruptCallbackContext call;
    CpuContext* ctx = call.get();
    s_menuVoices = true;
    Mods::Variants::UpdateMenuVoices(ctx);
    if (s_fixSelected) {  // coming back: the scene marked buttons from the team ids; a mod captain has none
        s_fixSelected = false;
        for (int side = 0; side < 2; ++side) {
            if (!Memory::Read8(scene + kCsConfirmed + side)) continue;
            const int id = static_cast<int32_t>(Memory::Read32(scene + kCsCaptainIds + side * 4));
            const bool mod = g_teamMod[side] && g_teamMod[side]->baseIndex == id;
            if (!mod) g_teamMod[side] = nullptr;
            Memory::Write32(scene + kCsSelected + side * 4, mod ? kOffGrid : static_cast<uint32_t>(GridIndexOf(id)));
        }
    }
    for (int side = 0; side < 2; ++side) {  // choosing again: no longer a partner team or a mod captain
        const bool confirmed = Memory::Read8(scene + kCsConfirmed + side);
        if (!confirmed && g_teamPartner[side] >= 0) {
            g_teamPartner[side] = -1;
            SaveTeamPartner(side, -1);
        }
        if (g_teamMod[side] && (!confirmed || Memory::Read32(scene + kCsSelected + side * 4) != kOffGrid)) {
            g_teamMod[side] = nullptr;
            RuntimeConfigFile::SetModSelection(side, "");
        }
    }
    const bool bothChosen = Memory::Read8(scene + kCsConfirmed) && Memory::Read8(scene + kCsConfirmed + 1);
    const uint32_t input = Memory::Read32(kFEInput);
    const auto justPressed = [&](int pad, uint32_t button) {
        return input != 0 && (Call(ctx, kJustPressed, {input, static_cast<uint32_t>(pad), button, 1, 0}) & 0xFF) != 0;
    };
    // Changing pages: the grid slides out (as it leaves when partner select opens), the page changes,
    // and it slides back in, as partner select swaps its captain grid and partner row.
    const uint32_t layer = Memory::Read32(scene + kCaptainsLayer);
    if (g_csSwapTo >= 0) {
        const uint32_t slide = ActiveSlide(layer);
        constexpr uint32_t kSlideDuration = 0x14, kSlideTime = 0x18;
        if (slide == 0 || Memory::Read32(slide + kSlideHash) != LowerHash("out") ||
            Memory::ReadFloat32(slide + kSlideTime) >= Memory::ReadFloat32(slide + kSlideDuration)) {
            g_csPartners = g_csSwapTo == 1;
            g_csModPage = g_csSwapTo >= 2 ? g_csSwapTo - 2 : -1;
            MuteGrid(scene, g_csSwapTo != 0);
            if (layer) Memory::Write8(layer + kInstanceVisible, 1);
            SetActiveSlide(ctx, layer, "in");
            Call(ctx, kPlayAudioEvent, {0xDF52130Fu, 0, 0, 1});
            g_csSwapTo = -1;
        }
        return;
    }
    if (!bothChosen) {
        for (int pad = 0; pad < 4; ++pad) {
            const int step = justPressed(pad, kButtonPlus) ? 1 : justPressed(pad, kButtonMinus) ? -1 : 0;
            if (step == 0) continue;
            // Only a pad that's choosing a captain: before that the grid isn't up, and sliding it in
            // would show it without the selection that goes with it.
            if (static_cast<int32_t>(Memory::Read32(scene + kCsSidePads)) != pad &&
                static_cast<int32_t>(Memory::Read32(scene + kCsSidePads + 4)) != pad)
                continue;
            // The pages in order: captains (0), partners (1, captain-only teams), then the mods pages.
            std::vector<int> pages = {0};
            if (allCaptains) pages.push_back(1);
            for (size_t m = 0; m < ModPages().size(); ++m) pages.push_back(2 + static_cast<int>(m));
            const int current = g_csModPage >= 0 ? 2 + g_csModPage : g_csPartners ? 1 : 0;
            const auto at = std::find(pages.begin(), pages.end(), current);
            g_csSwapTo = pages[(static_cast<size_t>(at - pages.begin()) + pages.size() + static_cast<size_t>(step)) % pages.size()];
            MuteGrid(scene, true);  // nothing to pick while it's away
            for (int& hover : g_csHover) hover = -1;
            SetActiveSlide(ctx, layer, "out");
            Call(ctx, kPlayAudioEvent, {0x0B8C09FAu, 0, 0, 1});
            break;
        }
    }
    if (!g_csPartners && g_csModPage < 0) return;
    for (int pad = 0; pad < 4; ++pad) {
        int side = -1;
        for (int s = 1; s >= 0; --s)
            if (static_cast<int32_t>(Memory::Read32(scene + kCsSidePads + s * 4)) == pad) side = s;
        int hit = -1;
        if (side >= 0) {
            const float x = Memory::ReadFloat32(kPointerPositions + pad * 8), y = Memory::ReadFloat32(kPointerPositions + pad * 8 + 4);
            for (int i = 0; i < 12 && hit < 0; ++i)
                if (ButtonUsed(i) && ContainsPoint(scene + kCaptainButtons + static_cast<uint32_t>(i) * kPointerButtonSize, x, y)) hit = i;
        }
        // The pad's pointer state on each button (what UpdatePointerCursors keeps lit and counts as
        // choosing), as the muted handlers would set it.
        for (uint32_t i = 0; i < 12; ++i)
            Memory::Write32(scene + kCaptainButtons + i * kPointerButtonSize + kPointerStates + pad * 4, static_cast<int>(i) == hit ? 1 : 0);
        if (hit != g_csHover[pad]) {
            g_csHover[pad] = hit;
            if (const Mods::CharacterDef* def = side >= 0 && hit >= 0 ? ModAt(hit) : nullptr;
                def && !Memory::Read8(scene + kCsConfirmed + side)) {  // a mod captain's stats, as hovering a captain shows theirs
                Mods::Variants::SetMenuCharacter(side, def);
                Call(ctx, kSetCaptainInfo, {scene + kSceneCaptainPanels + static_cast<uint32_t>(side) * kCaptainPanelSize,
                                            Mods::Variants::TagTeam(static_cast<uint32_t>(def->baseIndex), side), static_cast<uint32_t>(pad), 1});
            }
            if (hit >= 0) {
                SetActiveSlide(ctx, Memory::Read32(scene + kCaptainInstances + static_cast<uint32_t>(hit) * 4), "over");
                Call(ctx, kPlayHoverFeedback, {scene + kCaptainButtons + static_cast<uint32_t>(hit) * kPointerButtonSize, static_cast<uint32_t>(pad)});
                Call(ctx, kPlayAudioEvent, {0xA183DBCDu + static_cast<uint32_t>(pad), 0, 0, 1});
            }
        }
        if (side >= 0 && hit >= 0 && justPressed(pad, kButtonSelect)) {
            if (g_csPartners) ConfirmPartner(ctx, scene, side, pad, hit);
            else ConfirmModCaptain(ctx, scene, side, pad, hit);
        }
    }
}

void SetOverallSlide(CpuContext* ctx, uint32_t overall, uint32_t info) {  // FECaptainComponent::SetOverallSlide
    static const char* const kRoles[] = {"offensive", "defensive", "playmaker", "power", "balanced"};
    const uint32_t role = Memory::Read32(info + kCharacterOverall);
    if (overall != 0 && role < 5) SetActiveSlide(ctx, overall, kRoles[role], false);
}

// The board's captain spot shows the partner leading the team: their picture as partner select's
// own partner slots show it (SetSidekickImage: its position image, turned to face the pitch).
void SetLeaderImage(CpuContext* ctx, uint32_t board, int side, int character) {
    constexpr uint32_t kPositionImages = 0x1998, kAsyncImageInstance = 0x08;  // ChooseSidekicksSceneV2, AsyncImage
    const uint32_t scene = PartnerScene();
    const int sidekick = static_cast<int32_t>(Memory::Read32(CharacterInfo(static_cast<uint32_t>(character)) + kCharacterSidekickId));
    if (scene == 0 || sidekick < 0 || sidekick > 7) return;
    const uint32_t async = Memory::Read32(scene + kPositionImages + static_cast<uint32_t>(side * 8 + sidekick) * 4);
    const uint32_t source = async ? Memory::Read32(async + kAsyncImageInstance) : 0;
    const uint32_t texture = source ? Memory::Read32(source + kImageTexture) : 0;
    const char* const path[] = {"positions", "field_positions", "idle", "dummies", "00_dummy_texture_positions"};
    const uint32_t dummy = FindInSlide(ActiveSlide(Memory::Read32(board + kBoardPositions)), path);
    const char* const imagePath[] = {"00_dummy_texture_positions"};
    const uint32_t image = FindInSlide(ActiveSlide(dummy), imagePath);  // FindPositionImage(3, 0)
    if (texture == 0 || image == 0) return;
    Memory::Write32(image + kImageTexture, texture);
    const bool facing = sidekick != 3 && sidekick != 5;  // SidekickFacingFlag
    Memory::Write32(image + kInstanceOverloadFlags, Memory::Read32(image + kInstanceOverloadFlags) | 2);
    Memory::WriteFloat32(image + kInstanceRotation, 0.0f);
    Memory::WriteFloat32(image + kInstanceRotation + 4, (facing && side == 0) || (!facing && side == 1) ? 3.1415927f : 0.0f);
    Memory::WriteFloat32(image + kInstanceRotation + 8, 0.0f);
}

// A mod character's own version of a front-end head texture: its base's "<base>_right" / "<base>_left"
// (partner select's board heads) becomes "<name>_right" / "_left" when the mod adds it.
std::map<std::pair<const Mods::CharacterDef*, std::string>, uint32_t> g_variantTextures;
uint32_t VariantTexture(CpuContext* ctx, uint32_t resource, const Mods::CharacterDef* def) {
    if (resource == 0 || def == nullptr) return resource;
    const uint32_t hash = Memory::Read32(resource + 0x0C);  // FETextureResource::m_hashID
    const std::string base = Mods::BaseCharacterName(def->baseIndex);
    for (const char* suffix : {"_right", "_left"}) {
        if (hash != TextureHash((base + suffix).c_str())) continue;
        const std::string name = def->name + suffix;
        if (!Mods::Catalogs::HasFeTexture("fe/screens/images/" + name)) return resource;
        uint32_t& copy = g_variantTextures[{def, name}];
        if (copy == 0 && (copy = Call(ctx, kNlMalloc, {0x20, 8, 0})) != 0) {
            for (uint32_t i = 0; i < 0x20; i += 4) Memory::Write32(copy + i, Memory::Read32(resource + i));
            const uint32_t own = TextureHash(name.c_str());
            Memory::Write32(copy + 0x00, 0);    // m_next
            Memory::Write32(copy + 0x04, 0);    // m_prev
            Memory::Write32(copy + 0x0C, own);  // m_hashID
            Memory::Write8(copy + 0x10, 1);     // m_bValid
            Memory::Write32(copy + 0x18, own);  // m_glTextureHandle (same size as the base's)
        }
        return copy ? copy : resource;
    }
    return resource;
}

// A mod character's heads on a board: the captain spot (as SetLeaderImage finds it) and, with
// `slot` 0-2, that slot's images (as SetSlotCaptain).
void SetVariantHead(CpuContext* ctx, uint32_t board, int slot, const Mods::CharacterDef* def) {
    static const char* const kDummies[] = {"00_dummy_texture_positions", "01_dummy_texture_positions",
                                           "02_dummy_texture_positions", "03_dummy_texture_positions"};
    const char* const dummyPath[] = {"positions", "field_positions", "idle", "dummies", kDummies[slot + 1]};
    const uint32_t dummy = FindInSlide(ActiveSlide(Memory::Read32(board + kBoardPositions)), dummyPath);
    auto apply = [&](uint32_t image) {
        if (image) Memory::Write32(image + kImageTexture, VariantTexture(ctx, Memory::Read32(image + kImageTexture), def));
    };
    if (slot < 0) {
        const char* const path[] = {kDummies[0]};
        apply(FindInSlide(ActiveSlide(dummy), path));
        return;
    }
    for (const char* state : {"off", "over", "down"}) {
        const char* const path[] = {state, kDummies[slot + 1]};
        apply(FindIn(dummy, path));
    }
}

// A board slot shows `captain`: their picture as the board's captain spot has it, unturned.
void SetSlotCaptain(CpuContext* ctx, uint32_t board, int side, int slot, int captain) {
    static const char* const kDummies[] = {"01_dummy_texture_positions", "02_dummy_texture_positions", "03_dummy_texture_positions"};
    // FindCaptainImage(captain, left) in partner select's art: "<name>_right" on the right board, else "positions_<name>".
    const uint32_t scene = TopPartnerScene();
    if (scene == 0) return;
    const uint32_t presentation = Memory::Read32(scene + kHandlerPresentation);
    const std::string name = CString(Memory::Read32(CharacterInfo(static_cast<uint32_t>(captain)) + kCharacterName));
    uint32_t source = 0;
    if (side != 0) {
        const std::string right = name + "_right";
        const char* const path[] = {"art", "Layer", right.c_str()};
        source = FindInPresentation(presentation, path, 3);
    }
    if (source == 0) {
        const std::string positions = "positions_" + name;
        const char* const path[] = {"art", "Layer", positions.c_str()};
        source = FindInPresentation(presentation, path, 3);
    }
    const uint32_t texture = source ? Memory::Read32(source + kImageTexture) : 0;
    if (texture == 0) return;
    // FindPositionImage(slot, off/over/down)
    const char* const dummyPath[] = {"positions", "field_positions", "idle", "dummies", kDummies[slot]};
    const uint32_t dummy = FindInSlide(ActiveSlide(Memory::Read32(board + kBoardPositions)), dummyPath);
    for (const char* state : {"off", "over", "down"}) {
        const char* const path[] = {state, kDummies[slot]};
        if (const uint32_t image = FindIn(dummy, path)) {
            Memory::Write32(image + kImageTexture, texture);
            Memory::Write32(image + kInstanceOverloadFlags, Memory::Read32(image + kInstanceOverloadFlags) | 2);  // SetAssetRotation(0, 0, 0)
            for (uint32_t k = 0; k < 3; ++k) Memory::WriteFloat32(image + kInstanceRotation + k * 4, 0.0f);
        }
    }
}
} // namespace PartnerGrid

// Captain-only teams, a partner leading a team (captain select's partner mode): the team's captain
// colour comes from its captain's CharacterInfo (cTeam::GetCaptain()->mUnidentified11C: the player
// markers, the defensive-play overlay), which for a partner has no team colours (black). The partner
// in the captain spot gets its own copy of its CharacterInfo with the team colour fields of the captain
// whose colours the team wears; name, stats and the rest stay the partner's.
void PartnerLeaders() {
    static uint32_t s_copies[2] = {};
    if (!RuntimeConfigFile::ModAllCaptains() || Memory::Read32(kGamePtr) == 0) return;
    constexpr uint32_t kPlayerInfo = 0x11C, kColourFields = 0x4C, kInfoSize = 0x5C;  // cPlayer, CharacterInfo
    const uint32_t info = Memory::Read32(kGameInfoManager);
    const uint32_t game = info ? Memory::Read32(info + 0x80 + Memory::Read32(info + 0x11C) * 4) : 0;  // GetTeam(side)
    if (game == 0) return;
    for (uint32_t t = 0; t < 2; ++t) {
        const uint32_t team = Memory::Read32(kTeams + t * 4);
        if (team == 0) continue;
        const uint32_t side = Memory::Read32(team) & 1;  // cTeam::m_nSide
        const int partner = RuntimeConfigFile::ModCaptainSpot(static_cast<int>(side));
        const uint32_t leader = Memory::Read32(team + kTeamPlayers);  // m_pPlayers[0]
        const uint32_t current = leader ? Memory::Read32(leader + kPlayerInfo) : 0;
        if (partner < 0 || current == 0) continue;
        if (current != s_copies[side]) {
            if (static_cast<int>(Memory::Read32(current)) != partner) continue;  // CharacterInfo::mIndex
            if (s_copies[side] == 0) {
                GuestInterruptCallbackContext call;
                call.get()->gpr[3] = kInfoSize;
                call.get()->gpr[4] = 8;
                call.get()->gpr[5] = 0;
                InvokeIndirectCpu(0x802AA79Cu, call.get());  // nlMalloc
                s_copies[side] = call.get()->gpr[3];
                if (s_copies[side] == 0) continue;
            }
            for (uint32_t i = 0; i < kInfoSize; i += 4) Memory::Write32(s_copies[side] + i, Memory::Read32(current + i));
            Memory::Write32(leader + kPlayerInfo, s_copies[side]);
        }
        const uint32_t captain = Memory::Read32(game + side * 4);
        if (captain > 11) continue;
        for (uint32_t i = kColourFields; i < kInfoSize; i += 4)  // mColourMask, mColourRank, mPrimaryColour, mAlternateColour
            Memory::Write32(s_copies[side] + i, Memory::Read32(kCharacterInfo + captain * kInfoSize + i));
    }
    // The stadium's big team banners (StadiumWorldDrawable::UpdateModelMaterials) show team 0's
    // "<captain>/<captain>_banners[_alt]", taken in a match from the character in its captain spot:
    // for a partner, a texture that does not exist. Once the game has set them up, they get the banner
    // of the captain whose colours the team wears (as the front end picks it, from GetTeam).
    constexpr uint32_t kStadiumBanner = 0x806E19A8u, kTeamBanner = 0x806E19ACu, kTeamBannerIndex = 0x806E19B0u;
    const uint32_t first = Memory::Read32(kTeams);
    const uint32_t firstSide = first ? Memory::Read32(first) & 1 : 0;
    if (first == 0 || RuntimeConfigFile::ModCaptainSpot(static_cast<int>(firstSide)) < 0 || Memory::Read32(kStadiumBanner) == 0 ||
        Memory::Read32(kTeamBanner) == 0)
        return;
    const uint32_t team = Memory::Read32(game + firstSide * 4), opponent = Memory::Read32(game + (firstSide ^ 1) * 4);
    if (team > 11 || opponent > 11) return;
    const uint32_t teamInfo = kCharacterInfo + team * kInfoSize, opponentInfo = kCharacterInfo + opponent * kInfoSize;
    const bool alternate = (Memory::Read32(teamInfo + kColourMask) & Memory::Read32(opponentInfo + kColourMask)) != 0 &&
                           static_cast<int32_t>(Memory::Read32(teamInfo + kColourRank)) > static_cast<int32_t>(Memory::Read32(opponentInfo + kColourRank));  // NeedsAlternateColour
    std::string name;
    for (uint32_t p = Memory::Read32(teamInfo + 0x04), i = 0; p != 0 && i < 32; ++i) {  // mName
        const char c = static_cast<char>(Memory::Read8(p + i));
        if (c == 0) break;
        name += c;
    }
    uint32_t texture = 0xFFFFFFFFu;  // glGetTexture: nlStringHash
    for (unsigned char c : name + "/" + name + (alternate ? "_banners_alt" : "_banners")) texture = texture * 33 + c;
    if (Memory::Read32(kTeamBanner) == texture) return;
    GuestInterruptCallbackContext call;
    call.get()->gpr[3] = Memory::Read32(kTextureManager);
    call.get()->gpr[4] = texture;
    InvokeIndirectCpu(kGetTextureIndex, call.get());
    if (call.get()->gpr[3] >= 0xFFFF) return;  // not loaded yet
    Memory::Write32(kTeamBanner, texture);
    Memory::Write32(kTeamBannerIndex, call.get()->gpr[3]);
}

// Fast menus: menu transitions are skipped, outside matches. 2D: one-shot FE slides (panels sliding
// in and out, button states) jump to their end (TLSlide::Update override below); long one-shot
// slides are content (credits), and looping ones idle animation, so those keep their pace. 3D: while
// the front-end presentation runs a transition script (FrontEndPresentation::IsActive: anything but
// "Idle", e.g. the zoom from the main menu), the game's time dilation runs the time-dilated tasks
// (camera animations and moves, FE scenes, effects) kDilation times faster, so they finish within a
// frame or two, and each scripted wait ends as it starts; audio keeps real time.
namespace FastMenus {
constexpr float kDilation = 30.0f;
constexpr float kLongestTransition = 2.5f;              // seconds; longer one-shot slides are content
constexpr uint32_t kTaskManager = 0x806E1DA0u;          // nlTaskManager::m_pInstance (mTimeDilation at +0x00)
constexpr uint32_t kAudioTask = 0x8056F870u;            // audioUpdateTask
constexpr uint32_t kTaskTimeDilated = 0x1C;             // nlTask::mTimeDilated
constexpr uint32_t kPresentationInstance = 0x801FEEACu; // FrontEndPresentation::GetInstance()
constexpr uint32_t kPresentationIsActive = 0x801FF168u; // FrontEndPresentation::IsActive() const
constexpr uint32_t kPresentationWait = 0xA8;            // FrontEndPresentation::mWaitTime

uint32_t g_presentation = 0;
bool g_dilating = false;

bool InMenus() { return RuntimeConfigFile::ModFastMenus() && Memory::Read32(kGamePtr) == 0; }

void Update() {
    bool transition = false;
    if (InMenus()) {
        const uint32_t mgr = Memory::Read32(kSceneManager);
        if (mgr != 0 && Memory::Read32(mgr + 0x04) != 0) {  // front end running: the presentation exists
            GuestInterruptCallbackContext call;
            if (g_presentation == 0) {
                InvokeIndirectCpu(kPresentationInstance, call.get());
                g_presentation = call.get()->gpr[3];
            }
            call.get()->gpr[3] = g_presentation;
            InvokeIndirectCpu(kPresentationIsActive, call.get());
            transition = (call.get()->gpr[3] & 0xFF) != 0;
        }
    }
    const uint32_t taskManager = Memory::Read32(kTaskManager);
    if (transition && taskManager != 0) {
        Memory::WriteFloat32(taskManager, kDilation);
        Memory::Write8(kAudioTask + kTaskTimeDilated, 0);
        g_dilating = true;
        if (Memory::ReadFloat32(g_presentation + kPresentationWait) > 0.0f)  // a wait started: over
            Memory::WriteFloat32(g_presentation + kPresentationWait, 0.0f);
    } else if (g_dilating) {
        if (taskManager != 0) Memory::WriteFloat32(taskManager, 1.0f);
        Memory::Write8(kAudioTask + kTaskTimeDilated, 1);
        g_dilating = false;
    }
}
} // namespace FastMenus

// The match intro (the presentation's "GameBegin") shows a black screen until it ends (frames are
// discarded meanwhile), so one that never ends looks like a hang. Captain-only teams skip it; a
// fixed team (mods/variants.h; captains in sidekick slots too) plays it, with this as a safety net:
// an intro still running after 90 seconds is ended as the skip button ends it (Presentation::Finish).
void IntroWatchdog() {
    constexpr uint32_t kPresentation = 0x8057ABBCu;  // GetPresentation()'s instance
    constexpr uint32_t kCurrentFunction = 0x30, kFinish = 0x8028518Cu;
    static uint64_t s_since = 0;
    static bool s_ended = false;
    const bool watched = Memory::Read32(kGamePtr) != 0 && !RuntimeConfigFile::ModAllCaptains() &&
                         (Mods::Variants::FixedTeamCaptain(0) || Mods::Variants::FixedTeamCaptain(1));
    if (!watched || MscGuest::CString(kPresentation + kCurrentFunction, 64) != "GameBegin") {
        s_since = 0;
        s_ended = false;
        return;
    }
    const uint64_t now = SDL_GetTicks();
    if (s_since == 0) s_since = now;
    if (s_ended || now - s_since < 90000) return;
    s_ended = true;
    RT_LOG(RT_TAG_MODS) << "match intro still running after 90 s (fixed team): ended" << std::endl;
    GuestInterruptCallbackContext call;
    call.get()->gpr[3] = kPresentation;
    InvokeIndirectCpu(kFinish, call.get());
}

void Apply() {
    try {
        UnlockEverything();
        CaptainVoices();
        IntroWatchdog();
        PartnerLeaders();
        FastMenus::Update();
        PartnerGrid::CaptainSelect();
        PartnerGrid::Update();
        KitChoice();
        BluePeach();
        ShotCounter();
        FastStadiums();
        WinByTwo();
        NkFix();
    } catch (const Memory::AccessViolation&) {
    }
    try {
        Mods::RunFrameEvents();  // mod plugins (they report their own bad accesses)
    } catch (const Memory::AccessViolation&) {
    }
}
} // namespace FrameMods

// void BaseGameSceneManager::Pop(). As the original (FESceneManager::QueueScenePop, then
// mBaseSceneHandlerStack[mCurrentStackDepth] = 0 and one less deep), plus the captain-only teams
// partner select (FrameMods::PartnerGrid): the pop closing captain select to open partner select is
// skipped, and popping partner select also pops the captain select kept under it.
extern "C" void MSC_BaseGameSceneManagerPop_801C5F1C(CpuContext* ctx)
{
    using namespace FrameMods::PartnerGrid;
    constexpr uint32_t kQueueScenePop = 0x802FF18Cu;  // FESceneManager::QueueScenePop()
    const uint32_t mgr = ctx->gpr[3];
    const uint32_t savedLr = ctx->lr;
    const uint32_t depth = Memory::Read32(mgr + 0x04);
    const uint32_t top = depth >= 1 && depth <= 32 ? Memory::Read32(mgr + 0x88 + (depth - 1) * 4) : 0;
    if (KeepCaptainSelect(top, savedLr)) return;
    if (top != 0 && Memory::Read32(top) == FrameMods::kChooseCaptainsVtable) LeavePartnerMode(top);  // its handlers back
    const bool both = g_captains != 0 && top != 0 && depth >= 2 && Memory::Read32(top) == kChooseSidekicksVtable &&
                      Memory::Read32(mgr + 0x88 + (depth - 2) * 4) == g_captains;
    uint32_t result = 0;
    for (int pop = 0; pop < (both ? 2 : 1); ++pop) {
        const uint32_t current = Memory::Read32(mgr + 0x04);
        if (current >= 1 && current <= 32 && Memory::Read32(mgr + 0x88 + (current - 1) * 4) == g_captains) g_captains = 0;
        ctx->gpr[3] = Memory::Read32(ctx->gpr[13] - 4944);  // FESceneManager instance
        InvokeIndirectCpu(kQueueScenePop, ctx);
        const uint32_t d = Memory::Read32(mgr + 0x04);
        Memory::Write32(mgr + 0x88 + d * 4, 0);
        Memory::Write32(mgr + 0x04, d - 1);
        result = d;
    }
    ctx->lr = savedLr;
    ctx->gpr[3] = result;
}
PPC_NATIVE_OVERRIDE_VOID(801C5F1C, MSC_BaseGameSceneManagerPop_801C5F1C, (CpuContext* ctx), (ctx));

// void SHChooseSides2::LeaveScene() (Back on choose sides). As the original: the scene pops itself;
// the Striker Cup goes back to its hub (presentation "TransitionChooseSidesToCup"), the Striker
// Challenge to its screen (scene 77, display mode 8), friendly matches remove the models and light
// cones and go back to partner select (scene 3); the pause menu's choose sides just resets the
// pointers. With captain-only teams a friendly match goes back through captain select, which goes
// straight on to partner select (FrameMods::PartnerGrid).
extern "C" void MSC_ChooseSidesLeaveScene_8021CBD0(CpuContext* ctx)
{
    using namespace FrameMods::PartnerGrid;
    constexpr uint32_t kPresentationInstance = 0x801FEEACu;  // FrontEndPresentation::GetInstance()
    constexpr uint32_t kPresentationCall = 0x801FF1B0u;      // FrontEndPresentation::Call(const char*)
    constexpr uint32_t kToCup = 0x8051C9E0u, kToChallenge = 0x8051C9FCu, kRemoveModels = 0x8051CA28u, kKillLightCones = 0x8051CA38u;
    constexpr uint32_t kPointerInstances = 0x80578450u;      // gFEPointerInstances[4]
    constexpr uint32_t kSetActiveSlideByName = 0x8030212Cu;  // TLComponentInstance::SetActiveSlide(const char*, bool, bool)
    constexpr uint32_t kContext = 0x3DC;                     // SHChooseSides2::mContext
    const uint32_t scene = ctx->gpr[3];
    const uint32_t mgr = Memory::Read32(ctx->gpr[13] - 7048);  // GameSceneManager
    const uint32_t cursor = ctx->gpr[13] - 21288;              // "cursor"
    const auto virtualCall = [&](uint32_t object, uint32_t slot, std::initializer_list<uint32_t> args) {
        std::vector<uint32_t> all{object};
        all.insert(all.end(), args.begin(), args.end());
        const uint32_t fn = Memory::Read32(Memory::Read32(object) + slot);
        const uint32_t savedLr = ctx->lr;
        for (size_t i = 0; i < all.size(); ++i) ctx->gpr[3 + i] = all[i];
        InvokeIndirectCpu(fn, ctx);
        ctx->lr = savedLr;
        return ctx->gpr[3];
    };
    const auto present = [&](uint32_t name) { Call(ctx, kPresentationCall, {Call(ctx, kPresentationInstance, {}), name}); };
    const auto pointersToCursor = [&] {
        for (uint32_t i = 0; i < 4; ++i) Call(ctx, kSetActiveSlideByName, {Memory::Read32(kPointerInstances + i * 4), cursor, 1, 0});
    };
    virtualCall(mgr, 16, {});  // Pop()
    const uint32_t context = Memory::Read32(scene + kContext);
    if (context == 1) {  // CUP
        Call(ctx, kPlayAudioEvent, {0xA6F93A5Du, 0, 0, 1});
        present(kToCup);
    } else if (context == 3) {  // TOURNAMENT
        present(kToChallenge);
        if (const uint32_t challenge = virtualCall(mgr, 12, {77, 2, 0})) virtualCall(challenge, 44, {8});  // SetDisplayMode(8)
    } else if (context != 4) {  // not PAUSE
        present(kRemoveModels);
        present(kKillLightCones);
        pointersToCursor();
        if (context == 0 && RuntimeConfigFile::ModAllCaptains() && Offline())
            PushForwardingCaptainSelect(ctx, mgr, Memory::Read32(Memory::Read32(mgr) + 12));
        else
            virtualCall(mgr, 12, {3, 2, 0});  // Push(SCENE_CHOOSE_SIDEKICKS_DOMINATION, SCREEN_BACK, false)
        Call(ctx, kPlayAudioEvent, {0xF8F6BB3Cu, 0, 0, 1});
    } else {
        pointersToCursor();
    }
}
PPC_NATIVE_OVERRIDE_VOID(8021CBD0, MSC_ChooseSidesLeaveScene_8021CBD0, (CpuContext* ctx), (ctx));

// void ChooseSidekicksSceneV2::OnSidekickPointerPress(int pad, void* button). As the original (the
// partner in that button goes in the side's selected slot: the board and the captain's rules get it,
// the slot is done, the partner's accept sound), plus partner select's captain grid: the slot's pick
// becomes that partner.
extern "C" void MSC_ChooseSidekicksOnSidekickPress_8022AB68(CpuContext* ctx)
{
    using namespace FrameMods::PartnerGrid;
    const uint32_t scene = ctx->gpr[3];
    const uint32_t pad = ctx->gpr[4];
    const uint32_t button = ctx->gpr[5];
    int side = -1;
    for (int s = 1; s >= 0; --s)  // GetSide: mSidePads[0] first
        if (Memory::Read32(scene + kSidePads + s * 4) == pad) side = s;
    if (side < 0 || button >= 8) return;
    const int32_t slot = static_cast<int32_t>(Memory::Read32(scene + kSelectedSlots + side * 4));
    if (slot < 0 || slot > 2) return;
    const uint32_t sidekick = static_cast<uint32_t>(kRowSidekicks[button]);
    Memory::Write32(scene + kBoards + side * kBoardSize + kBoardSidekicks + slot * 4, sidekick);  // SetSidekick
    Memory::Write32(scene + kSidekickButtons + button * kPointerButtonSize + kPointerStates + pad * 4, 0);
    FinishSlot(ctx, scene, side, slot);
    Call(ctx, kPlayAudioEvent, {Call(ctx, kSidekickAcceptSound, {sidekick}), 0, 0, 1});
    const uint32_t info = Memory::Read32(FrameMods::kGameInfoManager);
    const uint32_t team = Memory::Read32(scene + kTeams + side * 4);  // GetTeam(side)
    Memory::Write32(info + kRulesTable + team * kRulesSize + slot * 4, sidekick);
    if (BoardSide(scene + kBoards + side * kBoardSize) == side) {
        g_picks[side][slot] = static_cast<int>(Call(ctx, kSidekickCharacter, {sidekick}));
        SavePicks(side);
    }
}
PPC_NATIVE_OVERRIDE_VOID(8022AB68, MSC_ChooseSidekicksOnSidekickPress_8022AB68, (CpuContext* ctx), (ctx));

// void FECaptainComponent::UpdateOverallSlides(), called at the end of LoadSlotImages. As the original
// (each board position's role label, from the captain's and partners' CharacterInfo), plus partner
// select's captain grid (FrameMods::PartnerGrid): each slot shows its pick, a captain's picture and
// role, or a partner (LoadSlotImages has drawn the board's partners, which follow the picks).
extern "C" void MSC_CaptainComponentUpdateOverallSlides_801DAFC8(CpuContext* ctx)
{
    using namespace FrameMods::PartnerGrid;
    constexpr uint32_t kCaptainCharacter = 0x800FBD94u;  // GetCharacterIndexFromCaptain(int)
    const uint32_t board = ctx->gpr[3];
    const int32_t captain = static_cast<int32_t>(Memory::Read32(board + kBoardCaptain));
    if (captain == -1) return;
    const uint32_t slide = ActiveSlide(Memory::Read32(board + kBoardPositions));
    const char* const captainPath[] = {"positions", "overall_0"};
    const int side = BoardSide(board);
    const int leader = side >= 0 ? g_spot[side] : -1;  // a partner leading the team (captain select's partner mode)
    SetOverallSlide(ctx, FindInSlide(slide, captainPath),
                    CharacterInfo(leader >= 0 ? static_cast<uint32_t>(leader) : Call(ctx, kCaptainCharacter, {static_cast<uint32_t>(captain)})));
    if (leader >= 0) SetLeaderImage(ctx, board, side, leader);
    for (int slot = 0; slot < 3; ++slot) {
        static const char* const kOveralls[] = {"overall_1", "overall_2", "overall_3"};
        const char* const path[] = {"positions", kOveralls[slot]};
        const uint32_t overall = FindInSlide(slide, path);
        int character = -1;
        if (side >= 0) {
            character = SlotCharacter(board, side, slot);
            if (!IsSidekick(character)) SetSlotCaptain(ctx, board, side, slot, character);
        } else if (const int32_t sidekick = static_cast<int32_t>(Memory::Read32(board + kBoardSidekicks + slot * 4)); sidekick != -1) {
            character = static_cast<int32_t>(Call(ctx, kSidekickCharacter, {static_cast<uint32_t>(sidekick)}));
        }
        if (overall == 0) continue;
        Memory::Write8(overall + kInstanceVisible, character == -1 ? 0 : 1);
        if (character != -1) SetOverallSlide(ctx, overall, CharacterInfo(static_cast<uint32_t>(character)));
    }
    // A mod captain's team (mods/variants.h): its own heads where its mod adds them, for the captain
    // spot and the teammates its manifest makes mod characters too (captain-only teams).
    const int boardSide = side >= 0 ? side : static_cast<int>(Memory::Read32(board + kBoardSide) & 1);
    const Mods::CharacterDef* mod = Mods::Variants::SelectedCaptain(boardSide);
    if (mod == nullptr || mod->baseIndex != captain || leader >= 0) return;
    SetVariantHead(ctx, board, -1, mod);
    if (side < 0 && Mods::Variants::FixedTeamCaptain(boardSide) == mod) {
        // A fixed team (its teammates whatever is picked) on the usual partner select: its slots show
        // its teammates, with their role labels.
        for (int slot = 0; slot < 3; ++slot) {
            const int cc = Mods::Variants::TeammateClass(mod, slot + 1);
            if (cc < 0 || IsSidekick(cc)) continue;
            SetSlotCaptain(ctx, board, boardSide, slot, cc);
            uint32_t info = CharacterInfo(static_cast<uint32_t>(cc));
            if (const Mods::CharacterDef* mate = FindModCaptain(mod->teammates[static_cast<size_t>(slot)])) {
                SetVariantHead(ctx, board, slot, mate);
                if (const auto* v = Mods::Variants::ForCharacter(ctx, mate)) info = v->row;
            }
            static const char* const kOveralls[] = {"overall_1", "overall_2", "overall_3"};
            const char* const path[] = {"positions", kOveralls[slot]};
            if (const uint32_t overall = FindInSlide(slide, path)) {
                Memory::Write8(overall + kInstanceVisible, 1);
                SetOverallSlide(ctx, overall, info);
            }
        }
        return;
    }
    for (int slot = 0; side >= 0 && slot < 3 && static_cast<size_t>(slot) < mod->teammates.size(); ++slot) {
        const Mods::CharacterDef* mate = FindModCaptain(mod->teammates[static_cast<size_t>(slot)]);
        if (mate && mate->baseIndex == SlotCharacter(board, side, slot)) SetVariantHead(ctx, board, slot, mate);
    }
}
PPC_NATIVE_OVERRIDE_VOID(801DAFC8, MSC_CaptainComponentUpdateOverallSlides_801DAFC8, (CpuContext* ctx), (ctx));

// void FECaptainComponent::ResetSidekicks() (partner select's Default). As the original (the team's
// default partners: GameInfoManager::ResetRules, then ReloadSidekicks); on partner select's captain
// grid every slot goes back to the team's captain, or with the partner row up to those partners.
extern "C" void MSC_CaptainComponentResetSidekicks_801DCC28(CpuContext* ctx)
{
    using namespace FrameMods::PartnerGrid;
    const uint32_t board = ctx->gpr[3];
    const uint32_t info = Memory::Read32(FrameMods::kGameInfoManager);
    int32_t team = -1;
    if (Memory::Read32(info + kGameMode) == 3) {  // IsInMode3(): Striker Cup
        team = static_cast<int32_t>(Memory::Read32(Memory::Read32(kCupManager) + kPendingCupTeam));
    } else if (const uint32_t game = Memory::Read32(info + kGameInfos + Memory::Read32(info + kGameMode) * 4)) {  // GetTeam(mSide)
        team = static_cast<int32_t>(Memory::Read32(game + static_cast<int16_t>(Memory::Read32(board + kBoardSide)) * 4));
    }
    Call(ctx, kResetRules, {info, static_cast<uint32_t>(team)});
    Call(ctx, kReloadSidekicks, {board});
    const int side = BoardSide(board);
    if (side < 0) return;
    for (uint32_t slot = 0; slot < 3; ++slot)
        g_picks[side][slot] = g_showPartners ? static_cast<int>(Call(ctx, kSidekickCharacter, {Memory::Read32(board + kBoardSidekicks + slot * 4)})) : -1;
    SavePicks(side);
}
PPC_NATIVE_OVERRIDE_VOID(801DCC28, MSC_CaptainComponentResetSidekicks_801DCC28, (CpuContext* ctx), (ctx));

// void FECaptainComponent::RandomizeSidekicks() (partner select's Random). As the original (three
// partners drawn with nlRandom from those whose CharacterInfo marks them selectable, stored in the
// board and the captain's rules); on partner select's captain grid the slots get random unlocked
// captains, or with the partner row up those partners.
extern "C" void MSC_CaptainComponentRandomizeSidekicks_801DCB28(CpuContext* ctx)
{
    using namespace FrameMods::PartnerGrid;
    const uint32_t board = ctx->gpr[3];
    uint32_t sidekicks[8] = {}, count = 0;
    for (uint32_t i = 0; i < 8; ++i)
        if (Memory::Read32(CharacterInfo(Call(ctx, kSidekickCharacter, {i})) + kCharacterUnlockable) == 1) sidekicks[count++] = i;
    uint32_t rules[3];
    for (uint32_t& value : rules) value = sidekicks[Call(ctx, kNlRandom, {count, kDefaultSeed}) & 7];
    const uint32_t info = Memory::Read32(FrameMods::kGameInfoManager);
    const uint32_t captain = Memory::Read32(board + kBoardCaptain);
    for (uint32_t k = 0; k < 3; ++k) {
        Memory::Write32(board + kBoardSidekicks + k * 4, rules[k]);
        if (captain < 12) Memory::Write32(info + kRulesTable + captain * kRulesSize + k * 4, rules[k]);
    }
    const int side = BoardSide(board);
    if (side < 0) return;
    uint32_t captains[12] = {}, unlocked = 0;
    for (int i = 0; i < 12; ++i)
        if (!g_locked[i]) captains[unlocked++] = static_cast<uint32_t>(kGridCaptains[i]);
    for (uint32_t slot = 0; slot < 3; ++slot) {
        g_picks[side][slot] = g_showPartners ? static_cast<int>(Call(ctx, kSidekickCharacter, {rules[slot]}))
                                             : static_cast<int>(captains[Call(ctx, kNlRandom, {unlocked, kDefaultSeed}) % 12]);
    }
    SavePicks(side);
}
PPC_NATIVE_OVERRIDE_VOID(801DCB28, MSC_CaptainComponentRandomizeSidekicks_801DCB28, (CpuContext* ctx), (ctx));

// void TLSlide::Update(float dt). As the original (FE timeline: the slide's time advances by dt,
// looping or stopping at its end, its animations are evaluated at that time, and its children
// updated with dt: components' active slides, then UpdateAsset), plus fast menus
// (FrameMods::FastMenus): a one-shot slide no longer than a transition goes straight to its end.
extern "C" void MSC_TLSlideUpdate_802FFCD4(CpuContext* ctx)
{
    using namespace FrameMods::FastMenus;
    constexpr uint32_t kAnimationUpdate = 0x802FA19Cu;  // FEAnimation::Update(float time)
    constexpr uint32_t kComponentUpdate = 0x80302114u;  // TLComponentInstance::Update(float dt)
    constexpr uint32_t kUpdateAsset = 0x802FFAFCu;      // TLSlide::UpdateAsset(TLInstance*, float dt)
    constexpr uint32_t kChildren = 0x08, kAnimations = 0x0C, kStart = 0x10, kDuration = 0x14, kTime = 0x18,
                       kPlayMode = 0x1C, kPaused = 0x44;  // TLSlide
    const uint32_t slide = ctx->gpr[3];
    const uint32_t savedLr = ctx->lr;
    const double dt = Memory::Read8(slide + kPaused) ? 0.0 : ctx->fpr[1].d;
    const int32_t mode = static_cast<int32_t>(Memory::Read32(slide + kPlayMode));
    const float start = Memory::ReadFloat32(slide + kStart), duration = Memory::ReadFloat32(slide + kDuration);
    float time = Memory::ReadFloat32(slide + kTime) + static_cast<float>(dt);
    const float end = start + duration;
    if (mode == 0 && duration <= kLongestTransition && !Memory::Read8(slide + kPaused) && InMenus()) time = end;
    if (time > end) {
        if (mode == 1) time -= end;      // TLPM_LOOPING
        else if (mode == 0) time = end;  // TLPM_STOP_AT_END
    }
    Memory::WriteFloat32(slide + kTime, time);
    if (const uint32_t animations = Memory::Read32(slide + kAnimations)) {  // ring of FEAnimation (m_next at +4)
        for (uint32_t anim = Memory::Read32(animations + 4), guard = 0; anim != 0 && guard < 4096; ++guard) {
            ctx->gpr[3] = anim;
            ctx->fpr[1].d = Memory::ReadFloat32(slide + kTime);
            InvokeIndirectCpu(kAnimationUpdate, ctx);
            if (anim == Memory::Read32(slide + kAnimations)) break;
            anim = Memory::Read32(anim + 4);
        }
    }
    if (const uint32_t children = Memory::Read32(slide + kChildren)) {  // ring of TLInstance (m_next at +0)
        for (uint32_t child = Memory::Read32(children), guard = 0; child != 0 && guard < 4096; ++guard) {
            if (Memory::Read32(child + 0x88) == 4) {  // TLAT_COMPONENT
                ctx->gpr[3] = child;
                ctx->fpr[1].d = dt;
                InvokeIndirectCpu(kComponentUpdate, ctx);
            }
            ctx->gpr[3] = slide;
            ctx->gpr[4] = child;
            ctx->fpr[1].d = dt;
            InvokeIndirectCpu(kUpdateAsset, ctx);
            if (child == Memory::Read32(slide + kChildren)) break;
            child = Memory::Read32(child);
        }
    }
    ctx->lr = savedLr;
}
PPC_NATIVE_OVERRIDE_VOID(802FFCD4, MSC_TLSlideUpdate_802FFCD4, (CpuContext* ctx), (ctx));

// void ChooseCaptainsSceneV2::RefreshCaptainImages(). As the original (each grid button's portrait
// for its current state: greyed for a team's chosen captain or one taken in an online draft, static
// for a locked captain, else lit; mCaptainImages kept up to date), plus captain select's partner mode
// (FrameMods::PartnerGrid): the grid shows the partners' portraits.
extern "C" void MSC_ChooseCaptainsRefreshImages_80227608(CpuContext* ctx)
{
    using namespace FrameMods::PartnerGrid;
    constexpr uint32_t kIsCaptainTaken = 0x8012C69Cu;  // NetworkDraft::IsCaptainTaken(int)
    const uint32_t scene = ctx->gpr[3];
    const uint32_t savedLr = ctx->lr;
    const uint32_t draft = Memory::Read32(ctx->gpr[13] - 8784);  // NetworkDraft
    const bool partners = scene == g_csScene && g_csPartners;
    const bool mods = scene == g_csScene && g_csModPage >= 0;
    for (uint32_t i = 0; i < 12; ++i) {
        const int captain = kGridCaptains[i];
        char group[8], texture[32];
        std::snprintf(group, sizeof(group), "%u", i);
        std::snprintf(texture, sizeof(texture), "%02u_dummy_texture", i);
        const char* const groupPath[] = {group};
        const uint32_t groupInstance = FindInSlide(ActiveSlide(Memory::Read32(scene + kCaptainInstances + i * 4)), groupPath);
        const char* const imagePath[] = {texture};
        const char* const noisePath[] = {"noise"};
        const uint32_t image = FindIn(groupInstance, imagePath);
        const uint32_t noise = FindIn(groupInstance, noisePath);
        Memory::Write32(scene + kCsCaptainImages + i * 4, image);
        if (noise) Memory::Write8(noise + kInstanceVisible, 0);
        uint32_t resource = 0;
        if (partners) {
            if (kGridPartners[i] >= 0) resource = PartnerTexture(ctx, scene, kGridPartners[i]);
        } else if (mods) {
            if (const Mods::CharacterDef* def = ModAt(static_cast<int>(i))) {
                const bool taken = (Memory::Read8(scene + kCsConfirmed) && g_teamMod[0] == def) ||
                                   (Memory::Read8(scene + kCsConfirmed + 1) && g_teamMod[1] == def);
                resource = ModPortrait(ctx, scene, def, taken);
            }
        } else if ((Memory::Read8(scene + kCsConfirmed) && Memory::Read32(scene + kCsSelected) == i) ||
                   (Memory::Read8(scene + kCsConfirmed + 1) && Memory::Read32(scene + kCsSelected + 4) == i)) {
            resource = Memory::Read32(scene + kCaptainTextures + i * 8);  // greyed
        } else if (draft != 0 && Memory::Read32(draft + 4) != 0 && Call(ctx, kIsCaptainTaken, {draft, static_cast<uint32_t>(captain)}) != 0) {
            resource = Memory::Read32(scene + kCaptainTextures + i * 8);
        } else if (CaptainLocked(ctx, captain)) {
            if (noise) Memory::Write8(noise + kInstanceVisible, 1);
        } else {
            resource = Memory::Read32(scene + kCaptainTextures + i * 8 + 4);  // lit
        }
        if (image != 0 && resource != 0) Memory::Write32(image + kImageTexture, resource);  // SetTextureResource
    }
    ctx->lr = savedLr;
}
PPC_NATIVE_OVERRIDE_VOID(80227608, MSC_ChooseCaptainsRefreshImages_80227608, (CpuContext* ctx), (ctx));

// void FECharacterPDAComponent::ApplyCaptainColours(int captain, int opponent): a stats panel in its
// team's colour against the other team's (captain select, once captains are chosen). As the original;
// on captain select, a side led by (or hovering) a mod captain passes its team id tagged, so the
// colours are the mod character's, and a mod captain against its base isn't taken for a clash.
extern "C" void func_801E0B8C(CpuContext* ctx);
static void ApplyCaptainColoursForMods(CpuContext* ctx)
{
    using namespace FrameMods;
    using namespace FrameMods::PartnerGrid;
    const uint32_t panel = ctx->gpr[3];
    const uint32_t panels = g_csScene ? g_csScene + kSceneCaptainPanels : 0;
    if (panels != 0 && panel >= panels && panel < panels + 2 * kCaptainPanelSize && (panel - panels) % kCaptainPanelSize == 0) {
        const int side = static_cast<int>((panel - panels) / kCaptainPanelSize);
        const auto effective = [&](int s) -> const Mods::CharacterDef* {
            if (g_teamMod[s]) return g_teamMod[s];
            return Memory::Read8(g_csScene + kCsConfirmed + s) ? nullptr : Mods::Variants::MenuCharacter(s);
        };
        for (int arg = 0; arg < 2; ++arg) {  // r4 = this side's captain, r5 = the other side's
            const int s = arg == 0 ? side : side ^ 1;
            const Mods::CharacterDef* def = effective(s);
            if (def && ctx->gpr[4 + arg] == static_cast<uint32_t>(def->baseIndex))
                ctx->gpr[4 + arg] = Mods::Variants::TagTeam(ctx->gpr[4 + arg], s);
        }
    }
    func_801E0B8C(ctx);
}
PPC_NATIVE_WRAP(801E0B8C, ApplyCaptainColoursForMods);

// Partner holograms (choose sides, captain select's partner mode), made from a copy of the partner's
// template:
// - Its class is the captain whose colours the team wears: FinishLoadModel sizes a hologram's sprite
//   from a captains-only table indexed by class (lbl_80515D18, 12 entries; a partner's class reads past
//   it: a garbage, zero-height sprite, invisible), and the class names the impostor, whose render
//   target is looked up by name (two holograms of one name share it: one picture for both, and a
//   crash when the second is destroyed). The two teams never wear the same captain's colours.
// - Its alternate textures are the team bundle: a partner's own bundle only has a stand-in texture
//   ("<name>/<prefix>_mario"; the match loader skips it and swaps in "<prefix>_<captain>/<prefix>_<captain>"
//   from "<prefix>_<captain>.rlt"). With the alternate flag set the bundle is loaded, and where
//   FEImpostorCharacter sets its alternate texture ("<captain>/<captain>" -> "<captain>_alt/...") the
//   stand-in is swapped for the team texture instead (ImpostorModel::SetReplacementTexture override).
namespace PartnerHolograms {
struct Swap { uint32_t marker, original, replacement; };  // nlStringHash texture names
std::vector<Swap> g_pending;
std::map<uint32_t, uint32_t> g_templates;  // character, captain, alternate -> template copy (kept: models refer to it)

uint32_t TextureHash(const std::string& name) {  // nlStringHash (glGetTexture)
    uint32_t h = 0xFFFFFFFFu;
    for (unsigned char c : name) h = h * 33 + c;
    return h;
}

std::string Name(uint32_t character) {
    std::string s;
    const uint32_t name = Memory::Read32(FrameMods::kCharacterInfo + character * 0x5C + 0x04);  // CharacterInfo::mName
    for (uint32_t i = 0; name != 0 && i < 32; ++i) {
        const char c = static_cast<char>(Memory::Read8(name + i));
        if (c == 0) break;
        s += c;
    }
    return s;
}

// The partner's template, its alternate textures being its team bundle for `captain`.
uint32_t TeamTemplate(CpuContext* ctx, uint32_t templateInfo, uint32_t character, uint32_t captain, bool alternate) {
    constexpr uint32_t kTemplateSize = 0x5C, kAlternateTextures = 0x18, kNlMalloc = 0x802AA79Cu;
    const std::string name = Name(character), prefix = character == 13 ? "hammer" : name;  // Hammer Bro: hammer_<captain>
    const std::string team = prefix + "_" + Name(captain) + (alternate ? "_alt" : "");
    const uint32_t key = (character << 16) | (captain << 1) | (alternate ? 1u : 0u);
    uint32_t copy = g_templates.count(key) ? g_templates[key] : 0;
    if (copy == 0) {
        const std::string path = "art/characters/" + name + "/" + team + ".rlt";
        const uint32_t savedLr = ctx->lr;
        ctx->gpr[3] = kTemplateSize + static_cast<uint32_t>(path.size()) + 1;
        ctx->gpr[4] = 8;
        ctx->gpr[5] = 0;
        InvokeIndirectCpu(kNlMalloc, ctx);
        ctx->lr = savedLr;
        copy = ctx->gpr[3];
        if (copy == 0) return templateInfo;
        for (uint32_t i = 0; i < kTemplateSize; i += 4) Memory::Write32(copy + i, Memory::Read32(templateInfo + i));
        Memory::Write32(copy + 0x00, captain);  // mUnidentified00 (class): the colours' captain's
        for (size_t i = 0; i <= path.size(); ++i) Memory::Write8(copy + kTemplateSize + static_cast<uint32_t>(i), i < path.size() ? path[i] : 0);
        Memory::Write32(copy + kAlternateTextures, copy + kTemplateSize);
        g_templates[key] = copy;
    }
    const std::string impostor = Name(captain);  // FEImpostorCharacter's name: the class's
    g_pending.push_back({TextureHash(impostor + "/" + impostor), TextureHash(name + "/" + prefix + "_mario"), TextureHash(team + "/" + team)});
    return copy;
}
} // namespace PartnerHolograms

// FEModelHandle* FEModelManager::CreateModel(FEModelType, const char* name, int captain, bool, void*,
// void*, bool alternate). As the original (the captain's character model: GetCharacterIndexFromCaptain,
// then the template overload), except that the front-end presentation's team holograms (choose sides)
// show the partner leading a team (captain select's partner mode), in that team's colours, instead of
// the captain whose colours it wears.
extern "C" void MSC_FEModelManagerCreateModel_801C27C4(CpuContext* ctx)
{
    constexpr uint32_t kCaptainCharacter = 0x800FBD94u;  // GetCharacterIndexFromCaptain(int)
    constexpr uint32_t kTemplateInfo = 0x8002600Cu;      // GetCharacterTemplateInfo(eCharacterClass)
    constexpr uint32_t kCreateFromTemplate = 0x801C2844u; // CreateModel(FEModelType, const char*, tCharacterTemplateInfo*, ...)
    constexpr uint32_t kPresentationCallsBegin = 0x801FF5B0u, kPresentationCallsEnd = 0x80200360u;  // FrontEndPresentation::DoFunctionCall
    const uint32_t savedLr = ctx->lr;
    const uint32_t manager = ctx->gpr[3], type = ctx->gpr[4], name = ctx->gpr[5];
    const int32_t captain = static_cast<int32_t>(ctx->gpr[6]);
    const uint32_t a7 = ctx->gpr[7], a8 = ctx->gpr[8], a9 = ctx->gpr[9];
    uint32_t alternate = ctx->gpr[10];
    int32_t character = -1;
    if (RuntimeConfigFile::ModAllCaptains() && savedLr >= kPresentationCallsBegin && savedLr < kPresentationCallsEnd) {
        const uint32_t info = Memory::Read32(FrameMods::kGameInfoManager);
        const uint32_t game = info ? Memory::Read32(info + 0x80 + Memory::Read32(info + 0x11C) * 4) : 0;  // GetTeam(side)
        for (int side = 0; side < 2 && game != 0 && character < 0; ++side)
            if (static_cast<int32_t>(Memory::Read32(game + side * 4)) == captain) character = RuntimeConfigFile::ModCaptainSpot(side);
    }
    const bool partner = character >= 12 && character <= 19 && captain >= 0 && captain <= 11;
    // A mod captain's team (homemodel / awaymodel: which side the presentation asks for): the mod
    // character's own template, so the hologram is its model and textures.
    uint32_t variantInfo = 0;
    if (savedLr >= kPresentationCallsBegin && savedLr < kPresentationCallsEnd) {
        const std::string model = MscGuest::CString(name);
        const int side = model == "homemodel" ? 0 : model == "awaymodel" ? 1 : -1;
        if (const Mods::CharacterDef* def = side >= 0 ? Mods::Variants::SelectedCaptain(side) : nullptr; def && def->baseIndex == captain)
            if (const auto* variant = Mods::Variants::ForCharacter(ctx, def)) variantInfo = variant->templateInfo;
    }
    if (character < 0) {
        ctx->gpr[3] = static_cast<uint32_t>(captain);
        InvokeIndirectCpu(kCaptainCharacter, ctx);
        character = static_cast<int32_t>(ctx->gpr[3]);
    }
    if (character == -1) {
        ctx->lr = savedLr;
        ctx->gpr[3] = 0;
        return;
    }
    ctx->gpr[3] = static_cast<uint32_t>(character);
    InvokeIndirectCpu(kTemplateInfo, ctx);
    uint32_t templateInfo = ctx->gpr[3];
    if (variantInfo != 0 && !partner) templateInfo = variantInfo;
    if (partner) {
        templateInfo = PartnerHolograms::TeamTemplate(ctx, templateInfo, static_cast<uint32_t>(character), static_cast<uint32_t>(captain), (alternate & 0xFF) != 0);
        alternate = 1;  // load the team bundle as the alternate textures
    }
    ctx->gpr[3] = manager;
    ctx->gpr[4] = type;
    ctx->gpr[5] = name;
    ctx->gpr[6] = templateInfo;
    ctx->gpr[7] = a7;
    ctx->gpr[8] = a8;
    ctx->gpr[9] = a9;
    ctx->gpr[10] = alternate;
    InvokeIndirectCpu(kCreateFromTemplate, ctx);
    ctx->lr = savedLr;
}
PPC_NATIVE_OVERRIDE_VOID(801C27C4, MSC_FEModelManagerCreateModel_801C27C4, (CpuContext* ctx), (ctx));

// void ImpostorModel::SetReplacementTexture(ulong texture). As the original (the texture drawn in place
// of mOriginalTexture, and its texture index), except for a partner hologram (PartnerHolograms):
// FEImpostorCharacter's "<name>/<name>" -> "<name>_alt/<name>_alt" becomes stand-in -> team texture.
extern "C" void MSC_ImpostorModelSetReplacementTexture_802DB4EC(CpuContext* ctx)
{
    constexpr uint32_t kOriginal = 0x68, kReplacement = 0x6C, kResolved = 0x70;  // ImpostorModel
    const uint32_t model = ctx->gpr[3];
    uint32_t texture = ctx->gpr[4];
    auto& pending = PartnerHolograms::g_pending;
    const uint32_t original = Memory::Read32(model + kOriginal);
    for (auto it = pending.begin(); it != pending.end(); ++it) {
        if (it->marker != original) continue;
        Memory::Write32(model + kOriginal, it->original);
        texture = it->replacement;
        pending.erase(it);
        break;
    }
    Memory::Write32(model + kReplacement, texture);
    const uint32_t savedLr = ctx->lr;
    ctx->gpr[3] = Memory::Read32(FrameMods::kTextureManager);  // glGetTextureManager()
    ctx->gpr[4] = texture;
    InvokeIndirectCpu(FrameMods::kGetTextureIndex, ctx);
    ctx->lr = savedLr;
    Memory::Write32(model + kResolved, ctx->gpr[3]);
}
PPC_NATIVE_OVERRIDE_VOID(802DB4EC, MSC_ImpostorModelSetReplacementTexture_802DB4EC, (CpuContext* ctx), (ctx));

// void NisPlayer::SetTeamLogo(NisTarget, NisWinnerType). As the original (the cutscene target's logo
// texture, "<filter>/<filter>_logo", and its texture index), except that a goal scored by a partner
// leading a team (captain select's partner mode; target 14, the scorer: the scorer's own name, which
// has no logo) shows the logo of the captain whose colours the team wears.
extern "C" void MSC_NisPlayerSetTeamLogo_8027F4B0(CpuContext* ctx)
{
    constexpr uint32_t kGetTargetFilter = 0x8027F9D4u;  // NisPlayer::GetTargetFilter(NisTarget, NisWinnerType) const
    constexpr uint32_t kScorerCharIndex = 0x340C0, kLogoTexture = 0x34438, kLogoIndex = 0x3443C;  // NisPlayer
    constexpr uint32_t kCharacters = 0x8056B800u;  // g_pCharacters[10]
    constexpr uint32_t kPlayerTeam = 0x314;        // cPlayer::m_pTeam
    const uint32_t nis = ctx->gpr[3], target = ctx->gpr[4], winner = ctx->gpr[5];
    const uint32_t savedLr = ctx->lr;
    ctx->gpr[3] = nis;
    ctx->gpr[4] = target;
    ctx->gpr[5] = winner;
    InvokeIndirectCpu(kGetTargetFilter, ctx);
    std::string filter;
    for (uint32_t p = ctx->gpr[3], i = 0; p != 0 && i < 64; ++i) {
        const char c = static_cast<char>(Memory::Read8(p + i));
        if (c == 0) break;
        filter += c;
    }
    if (target == 14 && RuntimeConfigFile::ModAllCaptains()) {
        const uint32_t index = Memory::Read32(nis + kScorerCharIndex);
        const uint32_t scorer = index < 10 ? Memory::Read32(kCharacters + index * 4) : 0;
        const uint32_t team = scorer ? Memory::Read32(scorer + kPlayerTeam) : 0;
        const uint32_t side = team ? Memory::Read32(team) & 1 : 0;  // cTeam::m_nSide
        const uint32_t info = Memory::Read32(FrameMods::kGameInfoManager);
        const uint32_t game = info ? Memory::Read32(info + 0x80 + Memory::Read32(info + 0x11C) * 4) : 0;
        if (team != 0 && game != 0 && Memory::Read32(team + FrameMods::kTeamPlayers) == scorer && RuntimeConfigFile::ModCaptainSpot(static_cast<int>(side)) >= 0)
            filter = PartnerHolograms::Name(Memory::Read32(game + side * 4));  // the colours' captain
    }
    const uint32_t texture = PartnerHolograms::TextureHash(filter + "/" + filter + "_logo");  // glGetTexture
    Memory::Write32(nis + kLogoTexture, texture);
    ctx->gpr[3] = Memory::Read32(FrameMods::kTextureManager);
    ctx->gpr[4] = texture;
    InvokeIndirectCpu(FrameMods::kGetTextureIndex, ctx);
    Memory::Write32(nis + kLogoIndex, ctx->gpr[3]);
    ctx->lr = savedLr;
}
PPC_NATIVE_OVERRIDE_VOID(8027F4B0, MSC_NisPlayerSetTeamLogo_8027F4B0, (CpuContext* ctx), (ctx));

// bool GetTweakBool(const char* path, bool defaultValue). As the original (TweakRegistry.cpp:
// FindTweakNode, then the value's storage kind), plus the captain-only teams mod: the game's
// character loader still reads the developers' "/user/allcaptains" switch, which fills every
// sidekick slot with the team's captain and flags them as captains; it is never registered on
// retail discs, so the mod answers it here.
extern "C" void MSC_GetTweakBool_802C2C84(CpuContext* ctx)
{
    constexpr uint32_t kTweakRoot = 0x8057C4E4u;     // sTweakRootEntry
    constexpr uint32_t kFindTweakNode = 0x802C41B4u; // FindTweakNode(TweakNode*, const char*)
    const uint32_t path = ctx->gpr[3];
    const uint32_t fallback = ctx->gpr[4] & 0xFF;
    if (RuntimeConfigFile::ModAllCaptains()) {
        static constexpr char kAllCaptains[] = "/user/allcaptains";
        bool match = true;
        for (uint32_t i = 0; i < sizeof(kAllCaptains) && match; ++i) match = Memory::Read8(path + i) == static_cast<uint8_t>(kAllCaptains[i]);
        if (match) {
            ctx->gpr[3] = 1;
            return;
        }
    }
    const uint32_t savedLr = ctx->lr;
    ctx->gpr[3] = kTweakRoot;
    ctx->gpr[4] = path;
    InvokeIndirectCpu(kFindTweakNode, ctx);
    const uint32_t node = ctx->gpr[3];
    uint32_t result = fallback;
    if (node != 0) {
        const uint32_t value = Memory::Read32(node + 0x0C);  // TweakNode::m_Value
        ctx->gpr[3] = value;
        InvokeIndirectCpu(Memory::Read32(Memory::Read32(value) + 0x10), ctx);  // GetStorageKind()
        const uint32_t kind = ctx->gpr[3];
        if (kind == 1) result = Memory::Read8(value + 0x0A);                       // TweakValueBool::mValue
        else if (kind == 2) result = Memory::Read8(Memory::Read32(value + 0x0C));  // TweakFloatBinding::m_pValue
    }
    ctx->lr = savedLr;
    ctx->gpr[3] = result;
}
PPC_NATIVE_OVERRIDE_VOID(802C2C84, MSC_GetTweakBool_802C2C84, (CpuContext* ctx), (ctx));

// void Presentation::PlayGameBegin(). The match intro is a scripted presentation ("GameBegin") whose
// team walk-outs never finish with the captain-only teams mod (the screen stays black: frames are
// discarded until it ends). With the mod on, the match starts the way cGame::BeginGame does for a
// rematch (bStraightToKickoff): straight to the kickoff state. Otherwise as the original:
// Call("GameBegin", "").
extern "C" void MSC_PresentationPlayGameBegin_80286288(CpuContext* ctx)
{
    constexpr uint32_t kGameBeginName = 0x80523B94u;     // "GameBegin"
    constexpr uint32_t kPresentationCall = 0x80286064u;  // Presentation::Call(const char*, const char*)
    constexpr uint32_t kChangeGameState = 0x8005BFB4u;   // cGame::ChangeGameState(int)
    constexpr uint32_t kFixedUpdateTask = 0x8056E35Cu;   // GetFixedUpdateTask()'s instance
    if (RuntimeConfigFile::ModAllCaptains()) {
        if (const uint32_t game = Memory::Read32(FrameMods::kGamePtr)) {
            const uint32_t savedLr = ctx->lr;
            ctx->gpr[3] = game;
            ctx->gpr[4] = 1;  // kickoff
            InvokeIndirectCpu(kChangeGameState, ctx);
            ctx->lr = savedLr;
            Memory::Write8(kFixedUpdateTask + 0x38, 1);
            return;
        }
    }
    ctx->gpr[4] = kGameBeginName;
    ctx->gpr[5] = ctx->gpr[13] - 17472;  // "" (small data)
    InvokeIndirectCpu(kPresentationCall, ctx);
}
PPC_NATIVE_OVERRIDE_VOID(80286288, MSC_PresentationPlayGameBegin_80286288, (CpuContext* ctx), (ctx));

// void NisPlayer::fn_8028041C(const char* nis, const char* side, NisTarget target,
//     NisUseStadiumOffset useStadiumOffset, NisWinnerType winnerType, bool mirrored, int param6)
// Plays a cutscene's optional sidekick companion, "<nis>_<sidekick>_same/other.nis" (goal, outraged
// and defeated scenes), when the disc has one for the chosen sidekick. With the captain-only teams
// mod, or a fixed team (mods/variants.h), the picked sidekicks aren't on the pitch, and a companion
// waited forever for its missing actor (the game hung after a goal), so they are skipped. Otherwise
// as the original.
extern "C" void MSC_NisPlayCompanion_8028041C(CpuContext* ctx)
{
    if (RuntimeConfigFile::ModAllCaptains() || Mods::Variants::FixedTeamCaptain(0) || Mods::Variants::FixedTeamCaptain(1)) return;
    constexpr uint32_t kGetTargetFilter = 0x8027F9D4u;  // NisPlayer::GetTargetFilter(NisTarget, NisWinnerType)
    constexpr uint32_t kPlayNis = 0x802805B4u;          // NisPlayer::fn_802805B4(NisHeader&, ...)
    constexpr uint32_t kDictSize = 0x30, kDict = 0x34, kHeaderSize = 0x1A0, kHeaderMirrored = 0x195;
    const uint32_t self = ctx->gpr[3], nis = ctx->gpr[4], side = ctx->gpr[5], target = ctx->gpr[6];
    const uint32_t useStadiumOffset = ctx->gpr[7], winnerType = ctx->gpr[8], mirrored = ctx->gpr[9] & 0xFF;
    const uint32_t param6 = ctx->gpr[10];
    const auto readString = [](uint32_t at, size_t max) {
        std::string out;
        for (uint32_t i = 0; at != 0 && i < max; ++i) {
            const char c = static_cast<char>(Memory::Read8(at + i));
            if (c == 0) break;
            out.push_back(c);
        }
        return out;
    };
    std::string name = readString(nis, 63);
    name = name.substr(0, name.find('.'));  // base name, up to the extension
    const uint32_t savedLr = ctx->lr;
    ctx->gpr[3] = self;
    ctx->gpr[4] = target;
    ctx->gpr[5] = winnerType;
    InvokeIndirectCpu(kGetTargetFilter, ctx);
    name += "_" + readString(ctx->gpr[3], 63) + "_" + readString(side, 63) + ".nis";  // "%s_%s_%s.nis"
    if (name.size() > 63) name.resize(63);
    const uint32_t count = Memory::Read32(self + kDictSize);
    for (uint32_t i = 0; i < count; ++i) {
        const uint32_t header = self + kDict + i * kHeaderSize;
        if (readString(header, 64) != name) continue;
        Memory::Write8(header + kHeaderMirrored, static_cast<uint8_t>(mirrored));
        ctx->gpr[3] = self;
        ctx->gpr[4] = header;
        ctx->gpr[5] = target;
        ctx->gpr[6] = useStadiumOffset;
        ctx->gpr[7] = winnerType;
        ctx->gpr[8] = param6;
        ctx->gpr[9] = 1;
        InvokeIndirectCpu(kPlayNis, ctx);
        break;
    }
    ctx->lr = savedLr;
}
PPC_NATIVE_OVERRIDE_VOID(8028041C, MSC_NisPlayCompanion_8028041C, (CpuContext* ctx), (ctx));

// bool Nis::IsLoading(). A cutscene that animates a character it has no animation for loads
// "Art/Animation/<skeleton>/<anim>.sanim" (Nis::LoadCharacterAnimation); when the disc has no such
// file, nlLoadEntireFileAsync returns no handle and never calls back, the entry stays "loading"
// and the presentation waits for it forever. That is what froze captain-only matches at the intro
// and after goals: sidekick cheers and walk-outs played on captains. As the original, except that
// an entry whose load never started is dropped (its request returned to the pool) instead.
extern "C" void MSC_NisIsLoading_80283930(CpuContext* ctx)
{
    constexpr uint32_t kScriptStarted = 0xBAC, kPending = 0x864, kPendingSize = 0x1C, kMaxCharacters = 10;
    constexpr uint32_t kRequestPool = 0x8057AB80u, kPoolFreeList = 0x0C;  // SlotPool<PendingAnimationRequest>
    const uint32_t nis = ctx->gpr[3];
    if (Memory::Read8(nis + kScriptStarted) == 0) {
        ctx->gpr[3] = 1;
        return;
    }
    bool loading = false;
    for (uint32_t i = 0; i < kMaxCharacters; ++i) {
        const uint32_t entry = nis + kPending + i * kPendingSize;  // {name, characterIndex, loaded, loadHandle, data, size, request}
        const uint32_t name = Memory::Read32(entry);
        if (name == 0 || static_cast<int32_t>(Memory::Read32(entry + 4)) == -1 || Memory::Read8(entry + 8) != 0) continue;
        if (Memory::Read32(entry + 12) != 0) {
            loading = true;
            continue;
        }
        static std::set<std::string> s_reported;
        std::string anim;
        for (uint32_t c = 0; c < 64; ++c) {
            const char ch = static_cast<char>(Memory::Read8(name + c));
            if (ch == 0) break;
            anim.push_back(ch);
        }
        if (s_reported.insert(anim).second) {
            RT_LOGF(RT_TAG_HLE, "Cutscene animation %s is not on the disc for character %d: skipped\n", anim.c_str(),
                    static_cast<int32_t>(Memory::Read32(entry + 4)));
        }
        if (const uint32_t request = Memory::Read32(entry + 24)) {
            Memory::Write8(request + 4, 0);  // inactive
            Memory::Write32(request, Memory::Read32(kRequestPool + kPoolFreeList));
            Memory::Write32(kRequestPool + kPoolFreeList, request);
        }
        Memory::Write32(entry, 0);
        Memory::Write32(entry + 4, 0xFFFFFFFFu);
        Memory::Write32(entry + 24, 0);
    }
    ctx->gpr[3] = loading ? 1u : 0u;
}
PPC_NATIVE_OVERRIDE_VOID(80283930, MSC_NisIsLoading_80283930, (CpuContext* ctx), (ctx));

// Mixed captain teams. With the captain-only teams mod the developers' "/user/allcaptains" switch
// fills every sidekick slot with the team's captain and flags those entries as captains; these two
// CharacterLoader functions then swap in the captains picked in F10 for each slot. Everything a
// character loads comes from its entry (model, textures, animations, INI, effects), so a Yoshi in
// Wario's team loads as Yoshi.
namespace CaptainTeams {
constexpr uint32_t kEntrySize = 0x14;  // CharacterLoader_8056B290::Entry
constexpr uint32_t kEntryTeam = 0x00, kEntryPlayer = 0x08, kEntryClass = 0x0C, kEntryCaptain = 0x10,
                   kEntryGoalie = 0x11, kEntrySidekick = 0x12;
constexpr uint32_t kCurrentIndex = 0xC8, kCurrent = 0xCC, kTemplate = 0xD0, kTemplateInfo = 0xD4, kSidekicks = 0xE0;
constexpr uint32_t kAudioRequestCount = 0x180;

void ApplyRoster(uint32_t loader)
{
    for (uint32_t i = 0; i < 10; ++i) {
        const uint32_t entry = loader + i * kEntrySize;
        const uint32_t player = Memory::Read32(entry + kEntryPlayer);
        if (Memory::Read8(entry + kEntryGoalie) != 0 || player > 3) continue;
        const uint32_t side = Memory::Read32(entry + kEntryTeam) & 1;
        if (player == 0) {
            // A partner in the captain spot, entered as the loader enters a partner; the team (the
            // loader's captain[], the game's team) stays the captain's: colours, banners, goalie.
            const int partner = RuntimeConfigFile::ModCaptainSpot(static_cast<int>(side));
            if (partner < 0) continue;
            Memory::Write32(entry + kEntryClass, static_cast<uint32_t>(partner));
            Memory::Write8(entry + kEntryCaptain, 0);
            Memory::Write8(entry + kEntrySidekick, 1);
            continue;
        }
        int pick = RuntimeConfigFile::ModCaptainTeammates(static_cast<int>(side))[player - 1];
        if (pick < 0) pick = RuntimeConfigFile::ModCaptainSpot(static_cast<int>(side));  // the team's leader
        if (pick < 0) continue;  // the team's own captain (the switch already put it there)
        // A captain, flagged as one; or a partner (character 12-19), as the loader makes a partner's entry.
        const bool sidekick = pick >= 12;
        Memory::Write32(entry + kEntryClass, static_cast<uint32_t>(pick));
        Memory::Write8(entry + kEntryCaptain, sidekick ? 0 : 1);
        Memory::Write8(entry + kEntrySidekick, sidekick ? 1 : 0);
        Memory::Write32(loader + kSidekicks + (side * 3 + player - 1) * 4, static_cast<uint32_t>(pick));
    }
}

// A side led by a fixed-team mod character (mods/variants.h; SMS's Super Team): its players 1-3 are
// its teammates whatever partner select picked, with or without captain-only teams. Entered as
// ApplyRoster enters a captain or a partner; a mod character as its base, which the variant
// machinery then makes the mod character (Variants::ForSlot).
void ApplyFixedTeams(uint32_t loader)
{
    for (uint32_t side = 0; side < 2; ++side) {
        const Mods::CharacterDef* fixed = Mods::Variants::FixedTeamCaptain(static_cast<int>(side));
        if (fixed == nullptr) continue;
        for (uint32_t i = 0; i < 10; ++i) {
            const uint32_t entry = loader + i * kEntrySize;
            const uint32_t player = Memory::Read32(entry + kEntryPlayer);
            if (Memory::Read8(entry + kEntryGoalie) != 0 || player < 1 || player > 3 || (Memory::Read32(entry + kEntryTeam) & 1) != side) continue;
            const int pick = Mods::Variants::TeammateClass(fixed, static_cast<int>(player));
            if (pick < 0) continue;
            const bool sidekick = pick >= 12;
            Memory::Write32(entry + kEntryClass, static_cast<uint32_t>(pick));
            Memory::Write8(entry + kEntryCaptain, sidekick ? 0 : 1);
            Memory::Write8(entry + kEntrySidekick, sidekick ? 1 : 0);
            Memory::Write32(loader + kSidekicks + (side * 3 + player - 1) * 4, static_cast<uint32_t>(pick));
        }
    }
}
} // namespace CaptainTeams

// bool CharacterLoader_8056B290::fn_80009EFC(): moves to the next character entry. As the original;
// before the first entry, the captain-only teams roster is applied.
extern "C" void MSC_CharacterLoaderNextEntry_80009EFC(CpuContext* ctx)
{
    using namespace CaptainTeams;
    const uint32_t loader = ctx->gpr[3];
    const int32_t index = static_cast<int32_t>(Memory::Read32(loader + kCurrentIndex)) + 1;
    Memory::Write32(loader + kCurrentIndex, static_cast<uint32_t>(index));
    if (index == 0) {
        if (RuntimeConfigFile::ModAllCaptains()) ApplyRoster(loader);
        ApplyFixedTeams(loader);
    }
    const bool more = index < 10;
    Memory::Write32(loader + kCurrent, more ? loader + static_cast<uint32_t>(index) * kEntrySize : 0);
    Memory::Write32(loader + kTemplate, 0);
    Memory::Write32(loader + kTemplateInfo, 0);
    ctx->gpr[3] = more ? 1u : 0u;
}
PPC_NATIVE_OVERRIDE_VOID(80009EFC, MSC_CharacterLoaderNextEntry_80009EFC, (CpuContext* ctx), (ctx));

// bool CharacterLoader_8056B290::fn_8000A67C(): starts loading a player's ExtraTextures.rlt (next to its
// texture bundle; a captain's has the team's banners, logo, gloves and backgrounds; partners have
// none). As the original, except that a partner leading a team (captain select's partner mode) loads
// the ExtraTextures of the captain whose colours the team wears, which no other player loads.
extern "C" void MSC_CharacterLoaderExtraTextures_8000A67C(CpuContext* ctx)
{
    using namespace CaptainTeams;
    constexpr uint32_t kExtraTextureData = 0x110, kExtraTextureSize = 0x114;  // CharacterLoader_8056B290
    constexpr uint32_t kGetTemplateInfo = 0x8002600Cu;     // GetCharacterTemplateInfo(eCharacterClass)
    constexpr uint32_t kBeginLoadTextures = 0x802C8204u;   // glBeginLoadTextureBundle(path, callback, param, pool)
    constexpr uint32_t kExtraTexturesLoaded = 0x8000A668u; // fn_8000A668
    const uint32_t loader = ctx->gpr[3];
    const uint32_t savedLr = ctx->lr;
    const uint32_t entry = Memory::Read32(loader + kCurrent);
    Memory::Write32(loader + kExtraTextureData, 0);
    Memory::Write32(loader + kExtraTextureSize, 0);
    if (Memory::Read8(entry + kEntryGoalie) != 0) {
        ctx->gpr[3] = 0;
        return;
    }
    uint32_t character = Memory::Read32(entry + kEntryClass);
    if (RuntimeConfigFile::ModAllCaptains() && Memory::Read32(entry + kEntryPlayer) == 0) {
        const int side = static_cast<int>(Memory::Read32(entry + kEntryTeam) & 1);
        const uint32_t info = Memory::Read32(FrameMods::kGameInfoManager);
        const uint32_t game = info ? Memory::Read32(info + 0x80 + Memory::Read32(info + 0x11C) * 4) : 0;  // GetTeam(side)
        if (game != 0 && RuntimeConfigFile::ModCaptainSpot(side) == static_cast<int>(character)) character = Memory::Read32(game + side * 4);
    }
    std::string path;
    {
        Mods::Variants::LoaderScope scope;  // a mod character's template: its own folder
        ctx->gpr[3] = character;
        InvokeIndirectCpu(kGetTemplateInfo, ctx);
    }
    path = MscGuest::CString(Memory::Read32(ctx->gpr[3] + 0x14), 127);  // szTextureFilename
    path = path.substr(0, path.rfind('/')) + "/ExtraTextures.rlt";
    if (!DVDPathExistsForRuntime(path.c_str())) {  // a mod character without its own: its base's
        const std::string base = MscGuest::CString(Memory::Read32(0x804F7F18u + (character < 20 ? character : 0) * 0x5C + 0x14), 127);
        path = base.substr(0, base.rfind('/')) + "/ExtraTextures.rlt";
    }
    if (path.size() > 127) path.resize(127);
    // The path lives in a stack frame of ours for the call, as the original's szPath[128].
    const uint32_t sp = ctx->gpr[1], frame = sp - 0x90;
    Memory::Write32(frame, sp);
    for (size_t i = 0; i <= path.size(); ++i) Memory::Write8(frame + 8 + static_cast<uint32_t>(i), i < path.size() ? path[i] : 0);
    ctx->gpr[1] = frame;
    ctx->gpr[3] = frame + 8;
    ctx->gpr[4] = kExtraTexturesLoaded;
    ctx->gpr[5] = entry;
    ctx->gpr[6] = Memory::Read32(ctx->gpr[13] - 5352);  // glGetCurrentResourcePool()
    InvokeIndirectCpu(kBeginLoadTextures, ctx);
    ctx->gpr[1] = sp;
    ctx->lr = savedLr;
}
PPC_NATIVE_OVERRIDE_VOID(8000A67C, MSC_CharacterLoaderExtraTextures_8000A67C, (CpuContext* ctx), (ctx));

// void CharacterLoader_8056B290::fn_8000C130(): loads a captain's voice bank into the team's captain
// slot (1 home, 5 away). As the original, except that a captain in a sidekick slot (captain-only
// teams, a fixed team) loads into that player's own slot, as a sidekick would, so the team captain
// keeps its voice.
extern "C" void MSC_CharacterLoaderCaptainAudio_8000C130(CpuContext* ctx)
{
    using namespace CaptainTeams;
    constexpr uint32_t kLoadSoundBank = 0x800EBB04u;   // LoadSoundBank(GameAudio*, bank, slot, cb, param)
    constexpr uint32_t kAudioLoaded = 0x8000C0ECu;     // fn_8000C0EC
    const uint32_t loader = ctx->gpr[3];
    const uint32_t entry = Memory::Read32(loader + kCurrent);
    const int32_t cc = static_cast<int32_t>(Memory::Read32(entry + kEntryClass));
    uint32_t info = FrameMods::kCharacterInfo + static_cast<uint32_t>(cc >= 0 && cc < 32 ? cc : 32) * 0x5C;
    if (const auto* variant = Mods::Variants::ForSlot(ctx, static_cast<int>(Memory::Read32(entry + kEntryTeam) & 1),
                                                      static_cast<int>(Memory::Read32(entry + kEntryPlayer)), cc))
        info = variant->row;  // a mod character's voice bank
    const uint32_t count = Memory::Read32(loader + kAudioRequestCount) + Memory::Read8(ctx->gpr[13] - 28528);  // + gAudioEnabled
    Memory::Write32(loader + kAudioRequestCount, count);
    uint32_t slot = Memory::Read32(entry + kEntryTeam) == 0 ? 1u : 5u;
    slot += Memory::Read32(entry + kEntryPlayer);  // 0 for the team captain
    const uint32_t savedLr = ctx->lr;
    ctx->gpr[3] = Memory::Read32(ctx->gpr[13] - 5028);  // g_pAudioSystem
    ctx->gpr[4] = Memory::Read32(info + 0x1C);           // CharacterInfo voice bank
    ctx->gpr[5] = slot;
    ctx->gpr[6] = kAudioLoaded;
    ctx->gpr[7] = count;
    InvokeIndirectCpu(kLoadSoundBank, ctx);
    ctx->lr = savedLr;
}
PPC_NATIVE_OVERRIDE_VOID(8000C130, MSC_CharacterLoaderCaptainAudio_8000C130, (CpuContext* ctx), (ctx));

// Missing away-kit images. Sidekick select loads "fe/sidekick_images/{position,attributes}_<sidekick>
// _<captain>_alt" from art/fe/sidekicksui.res whenever a captain wears the away kit, but the disc
// only has them for the captains the game can switch: Peach has a blue kit and every other blue
// texture, yet no captain shares her pink, so her _alt portraits were never made. Forced into blue
// (kit choice, Blue Peach) she made BundleFile look up 16 missing files; AsyncImage then allocated
// an uninitialised length and read from directory entry -1, and the game crashed. The two by-name
// lookups below are the originals (nlBundleFile.cpp) except that a missing "<name>_alt" falls back
// to "<name>": the home-kit portrait.
namespace {
constexpr uint32_t kBundleNumFiles = 0x04, kBundleFile = 0x10, kBundleDirectory = 0x20;  // BundleFile
constexpr uint32_t kBundleEntrySize = 12;  // BundleFileDirectoryEntry {hash, block, length}
constexpr uint32_t kNotFound = ~0u;

// BundleFile::HashFilename: lower case, backslashes as slashes, nlStringHash.
uint32_t BundleHash(const std::string& name)
{
    uint32_t h = 0xFFFFFFFFu;
    for (char c : name) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c | 0x20);
        if (c == '\\') c = '/';
        h += h << 5;
        h += static_cast<unsigned char>(c);
    }
    return h;
}

uint32_t BundleFindHash(uint32_t bundle, uint32_t hash)
{
    const uint32_t count = Memory::Read32(bundle + kBundleNumFiles), directory = Memory::Read32(bundle + kBundleDirectory);
    for (uint32_t i = 0; i < count; ++i) {
        if (Memory::Read32(directory + i * kBundleEntrySize) == hash) return i;
    }
    return kNotFound;
}

// The directory index for a file name, with the "_alt" fallback.
uint32_t BundleFindName(uint32_t bundle, uint32_t namePtr, bool printError)
{
    std::string name;
    for (uint32_t i = 0; i < 255; ++i) {
        const char c = static_cast<char>(Memory::Read8(namePtr + i));
        if (c == 0) break;
        name.push_back(c);
    }
    uint32_t index = BundleFindHash(bundle, BundleHash(name));
    if (index != kNotFound) return index;
    static std::set<std::string> s_reported;
    const bool first = s_reported.insert(name).second;
    if (name.size() > 4 && name.compare(name.size() - 4, 4, "_alt") == 0) {
        index = BundleFindHash(bundle, BundleHash(name.substr(0, name.size() - 4)));
        if (index != kNotFound) {
            if (first) RT_LOGF(RT_TAG_HLE, "%s is not on the disc: using the home kit's image\n", name.c_str());
            return index;
        }
    }
    // A mod character's image (its name in the file name) its mod doesn't add: its base's.
    if (std::string base; Mods::Variants::ToBaseName(name, base)) {
        index = BundleFindHash(bundle, BundleHash(base));
        if (index == kNotFound && base.size() > 4 && base.compare(base.size() - 4, 4, "_alt") == 0)
            index = BundleFindHash(bundle, BundleHash(base.substr(0, base.size() - 4)));
        if (index != kNotFound) return index;
    }
    if (printError && first) RT_LOGF(RT_TAG_HLE, "Bundle file not found: %s\n", name.c_str());
    return kNotFound;
}
} // namespace

// bool BundleFile::GetFileInfo(const char* filename, BundleFileDirectoryEntry* entry, bool printError)
extern "C" void MSC_BundleFileGetFileInfo_802BE030(CpuContext* ctx)
{
    const uint32_t bundle = ctx->gpr[3], entry = ctx->gpr[5];
    const uint32_t index = BundleFindName(bundle, ctx->gpr[4], (ctx->gpr[6] & 0xFF) != 0);
    bool found = false;
    if (index < Memory::Read32(bundle + kBundleNumFiles)) {
        const uint32_t from = Memory::Read32(bundle + kBundleDirectory) + index * kBundleEntrySize;
        for (uint32_t i = 0; i < kBundleEntrySize; i += 4) Memory::Write32(entry + i, Memory::Read32(from + i));
        found = true;
    }
    ctx->gpr[3] = found ? 1u : 0u;
}
PPC_NATIVE_OVERRIDE_VOID(802BE030, MSC_BundleFileGetFileInfo_802BE030, (CpuContext* ctx), (ctx));

// void BundleFile::ReadFileAsync(const char* filename, void* buffer, unsigned long size,
//                                FileReadAsyncCallback callback, unsigned long userParam)
extern "C" void MSC_BundleFileReadFileAsync_802BE34C(CpuContext* ctx)
{
    constexpr uint32_t kNlMalloc = 0x802AA79Cu;              // nlMalloc(size, alignment, fromEnd)
    constexpr uint32_t kNlSeek = 0x80367824u;                // nlSeek(file, offset, origin)
    constexpr uint32_t kNlReadAsync = 0x80367720u;           // nlReadAsync(file, buffer, size, cb, param, 0)
    constexpr uint32_t kCbFileReadAsyncCallback = 0x802BDC38u;
    const uint32_t bundle = ctx->gpr[3], buffer = ctx->gpr[5], size = ctx->gpr[6];
    const uint32_t callback = ctx->gpr[7], userParam = ctx->gpr[8];
    const uint32_t index = BundleFindName(bundle, ctx->gpr[4], true);  // not found: entry -1, as the original
    const uint32_t savedLr = ctx->lr;
    ctx->gpr[3] = 8;
    ctx->gpr[4] = 8;
    ctx->gpr[5] = 1;
    InvokeIndirectCpu(kNlMalloc, ctx);
    const uint32_t data = ctx->gpr[3];  // AsyncReadCallbackData {callback, userParam}
    Memory::Write32(data, callback);
    Memory::Write32(data + 4, userParam);
    const uint32_t entry = Memory::Read32(bundle + kBundleDirectory) + index * kBundleEntrySize;
    const uint32_t file = Memory::Read32(bundle + kBundleFile);
    ctx->gpr[3] = file;
    ctx->gpr[4] = Memory::Read32(entry + 4) * Memory::Read32(bundle);  // block * nSectorSize
    ctx->gpr[5] = 0;
    InvokeIndirectCpu(kNlSeek, ctx);
    ctx->gpr[3] = file;
    ctx->gpr[4] = buffer;
    ctx->gpr[5] = size;
    ctx->gpr[6] = kCbFileReadAsyncCallback;
    ctx->gpr[7] = data;
    ctx->gpr[8] = 0;
    InvokeIndirectCpu(kNlReadAsync, ctx);
    ctx->lr = savedLr;
}
PPC_NATIVE_OVERRIDE_VOID(802BE34C, MSC_BundleFileReadFileAsync_802BE34C, (CpuContext* ctx), (ctx));

extern "C" void MSC_UpdatePlatPad_8037537C(CpuContext* ctx)
{
    FrameMods::Apply();
    constexpr uint32_t kConnected = 0x2F0;
    constexpr uint32_t kUpdateChannel = 0x803753D8u; // PlatPadManager::UpdateChannel(int)
    const uint32_t manager = ctx->gpr[3];
    MSC_PollRemoteConnections();
    const uint32_t savedLr = ctx->lr;
    for (uint32_t chan = 0; chan < 4; ++chan) {
        if (Memory::Read8(manager + kConnected + chan) != 0) {
            ctx->gpr[3] = manager;
            ctx->gpr[4] = chan;
            InvokeIndirectCpu(kUpdateChannel, ctx);
        }
    }
    ctx->lr = savedLr;
}
PPC_NATIVE_OVERRIDE_VOID(8037537C, MSC_UpdatePlatPad_8037537C, (CpuContext* ctx), (ctx));

// OSYieldThread: on hardware the AI DMA interrupt keeps firing while a thread yield-spins
// (e.g. DestroyFEState waiting for audio to go idle). Here audio only pumps when the
// scheduler idles, and a yield-spinning main thread never lets it idle, so voices never stop.
// Service the pending audio interrupt first, then do the SDK's disable/SelectThread(TRUE)/restore.
extern "C" int32_t OS__DisableInterrupts_803B8F34();
extern "C" int32_t OS__RestoreInterrupts_803B8F5C(int32_t level);
extern "C" void SelectThread_803BBCB0(CpuContext* ctx);

extern "C" void MSC_OSYieldThread_803BBEF0(CpuContext* ctx)
{
    Audio_HLE_PollDeferred();
    const int32_t level = OS__DisableInterrupts_803B8F34();
    const uint32_t lr = ctx->lr;
    ctx->gpr[3] = 1;
    SelectThread_803BBCB0(ctx);
    ctx->lr = lr;
    OS__RestoreInterrupts_803B8F5C(level);
}

PPC_NATIVE_OVERRIDE_VOID(803BBEF0, MSC_OSYieldThread_803BBEF0, (CpuContext* ctx), (ctx));

// glxSetSwapMode: menus run in swap mode 3, which waits an extra retrace whenever a frame ends more
// than 12 ms after the last one. Frames here can take longer than on a Wii while still fitting in a
// 60 Hz refresh, so mode 3 dropped those menus to 30 fps; wait one retrace per frame (mode 1) instead.
extern "C" void MSC_glxSetSwapMode_8036CEB0(int32_t mode)
{
    constexpr uint32_t kGlxSwapMode = 0x806DFA7Cu;  // glx_SwapMode
    Memory::Write32(kGlxSwapMode, static_cast<uint32_t>(mode == 3 ? 1 : mode));
}
PPC_NATIVE_OVERRIDE_VOID(8036CEB0, MSC_glxSetSwapMode_8036CEB0, (int32_t mode), (mode));

// ---------------------------------------------------------------------------------------------
// Super Mario Strikers controls on GameCube-style controllers, with Charged's extras on top.
//
// The engine is Super Mario Strikers': DetInput still has its GameCube kind (m_nConnected 3) and
// remaps game actions through the GameCube table (g_pPadRemapArray), so pass, shoot/charge,
// switch player, use item and the lob modifier come out on SMS's buttons by themselves. Retail
// Charged stripped the GameCube backend and turned SMS's button actions into Wii gestures read in
// four places; with a GameCube DetInput those read SMS's buttons again (decided as SMS does, from
// who holds the ball):
//   big hit       Y without the ball                 (was: shake the Wii Remote)
//   cycle item    Z                                  (was: shake the Nunchuk)
//   deke          Y or C-stick with the ball         (was: D-pad)
//   slide tackle  B while the other team has the ball (was: D-pad)
// Charged extras: R does the character's special move (SMS's turbo slot); the D-pad keeps
// Charged's own behaviour. Real Wii Remotes keep the original code paths, ported below.
namespace {
constexpr uint32_t kDetConnected = 0x10, kDetButtons = 0x12, kDetLeftTrigger = 0x14, kDetRightTrigger = 0x15;
constexpr uint32_t kDetAnalogRightX = 0x08, kDetAnalogRightY = 0x0C;
constexpr uint32_t kDetRemoteAccel = 0x18, kDetDpdTargets = 0x30, kDetDpdCoord = 0x34;
constexpr uint32_t kDetPolarLeft = 0x44, kDetPolarRight = 0x4C, kDetRemapAngle = 0x88;
constexpr uint8_t kConnectedGameCube = 3, kConnectedFreestyle = 2;
constexpr uint16_t kGameCubeTag = 0x0080;  // set by the emulated remote (pad.cpp), channel in 0x0060
constexpr uint16_t kPadTriggerR = 0x0020;
// Game action indices (shared by SMS and Charged): DetInput::IsPressed(action, remap = true).
enum : int32_t { kActToggleItem = 22, kActHit = 24, kActSlide = 25, kActDeke = 29 };
constexpr uint32_t kBallPtr = 0x806E0BC0u;                                    // g_pBall
constexpr uint32_t kBallOwner = 0xC8, kPlayerController = 0x30C, kPlayerBall = 0x310, kPlayerTeam = 0x314;
constexpr uint32_t kAIPadDetInput = 0x2DC;                                    // cAIPad::m_pGlobalPad (DetInput*)
constexpr uint32_t kPlayerFacing = 0x24 + 0x3E;                               // m_aActualFacingDirection

bool DetCall(CpuContext* ctx, uint32_t fn, uint32_t det, int32_t action)
{
    ctx->gpr[3] = det;
    ctx->gpr[4] = static_cast<uint32_t>(action);
    ctx->gpr[5] = 1;  // remap through the controller kind's table
    InvokeIndirectCpu(fn, ctx);
    return ctx->gpr[3] != 0;
}
bool DetIsPressed(CpuContext* ctx, uint32_t det, int32_t action) { return DetCall(ctx, 0x80331C04u, det, action); }
bool DetJustPressed(CpuContext* ctx, uint32_t det, int32_t action) { return DetCall(ctx, 0x80331C70u, det, action); }
bool IsGameCube(uint32_t det) { return det && Memory::Read8(det + kDetConnected) == kConnectedGameCube; }

uint32_t PlayerDetInput(uint32_t player)
{
    const uint32_t pad = player ? Memory::Read32(player + kPlayerController) : 0;
    return pad ? Memory::Read32(pad + kAIPadDetInput) : 0;
}
uint32_t BallOwner()
{
    const uint32_t ball = Memory::Read32(kBallPtr);
    return ball ? Memory::Read32(ball + kBallOwner) : 0;
}
} // namespace

// DetInput::UpdatePolarAnalog. Its one translated caller is NetworkPeerChannel::
// ApplyNetworkPeerChannelInput, which unpacks each frame's captured input into the DetInput the
// game plays from and sets m_nConnected just before this call. A controller player's input (Wii
// kind, tagged by pad.cpp) becomes the GameCube kind here: buttons, C-stick and analog L/R from
// aurora's GameCube pad, no motion or pointer. Then the stick polars are computed as the original
// does. (Reads the local controller at apply time: right offline; online play would need the
// conversion at capture.)
extern "C" void MSC_DetInputUpdatePolarAnalog_80331D80(CpuContext* ctx)
{
    const uint32_t det = ctx->gpr[3];
    const uint16_t buttons = Memory::Read16(det + kDetButtons);
    if (Memory::Read8(det + kDetConnected) == kConnectedFreestyle && (buttons & kGameCubeTag)) {
        PADStatus pad{};
        if (MscEmulatedRemote::ReadGameCubePad((buttons >> 5) & 3u, pad)) {
            Memory::Write8(det + kDetConnected, kConnectedGameCube);
            Memory::Write16(det + kDetButtons, pad.button & 0x1F7F);
            Memory::Write8(det + kDetLeftTrigger, pad.triggerL);
            Memory::Write8(det + kDetRightTrigger, pad.triggerR);
            Memory::WriteFloat32(det + kDetAnalogRightX, std::clamp(pad.substickX / 72.0f, -1.0f, 1.0f));
            Memory::WriteFloat32(det + kDetAnalogRightY, std::clamp(pad.substickY / 72.0f, -1.0f, 1.0f));
            for (uint32_t i = 0; i < 6; ++i) Memory::WriteFloat32(det + kDetRemoteAccel + i * 4, 0.0f);
            Memory::Write8(det + kDetDpdTargets, 0);
            Memory::WriteFloat32(det + kDetDpdCoord, 0.0f);
            Memory::WriteFloat32(det + kDetDpdCoord + 4, 0.0f);
        }
    }
    // nlCartesianToPolar(m_PolarAnalogLeft, AnalogLeftX(), AnalogLeftY()), then the right stick.
    for (uint32_t side = 0; side < 2; ++side) {
        ctx->gpr[3] = det + (side ? kDetPolarRight : kDetPolarLeft);
        ctx->fpr[1].d = Memory::ReadFloat32(det + side * 8);
        ctx->fpr[2].d = Memory::ReadFloat32(det + side * 8 + 4);
        InvokeIndirectCpu(0x802B5DF4u, ctx);
    }
}
PPC_NATIVE_OVERRIDE_VOID(80331D80, MSC_DetInputUpdatePolarAnalog_80331D80, (CpuContext* ctx), (ctx));

namespace {
// cAIPad::GetMax{Remote,Freestyle}AccelDelta: the largest change against the last `count` samples
// of the pad's 30-entry acceleration history (x >= 999 marks an empty slot).
int MaxAccelDelta(uint32_t aipad, uint32_t history, uint32_t count, float delta[3])
{
    const uint32_t head = Memory::Read32(aipad + 0x2D4);
    const auto sample = [&](uint32_t slot, float v[3]) {
        for (int k = 0; k < 3; ++k) v[k] = Memory::ReadFloat32(aipad + history + slot * 12 + k * 4);
    };
    float current[3], bestPrevious[3] = {}, maximum = 0.0f;
    delta[0] = delta[1] = delta[2] = 0.0f;
    int bestOffset = 0;
    sample((head + 30) % 30, current);
    if (current[0] < 999.0f) {
        const uint32_t samples = count > 30 ? 30 : count;
        for (uint32_t offset = 1; offset < samples; ++offset) {
            float previous[3];
            sample((head + 30 - std::min(offset, 29u)) % 30, previous);
            if (previous[0] > 999.0f) break;
            const float dx = current[0] - previous[0], dy = current[1] - previous[1], dz = current[2] - previous[2];
            const float magnitude = dx * dx + dy * dy + dz * dz;
            if (magnitude > maximum) {
                maximum = magnitude;
                bestOffset = static_cast<int>(offset);
                std::copy(previous, previous + 3, bestPrevious);
            }
        }
        if (bestOffset != 0)
            for (int k = 0; k < 3; ++k) delta[k] = current[k] - bestPrevious[k];
    }
    return bestOffset;
}

// Each detected shake gesture in console.log (at most 4 a second), with the controls that made it,
// so a tester's log shows whether hits come from real motion.
void LogShake(const char* gesture, uint32_t det, float magnitude, float threshold)
{
    static uint64_t s_lastMs = 0;
    const uint64_t now = SDL_GetTicks();
    if (now - s_lastMs < 250) return;
    s_lastMs = now;
    const uint8_t kind = det ? Memory::Read8(det + kDetConnected) : 0;
    if (kind == kConnectedGameCube) {
        RT_LOGF(RT_TAG_HLE, "%s: GameCube-style controls (button)\n", gesture);
    } else {
        RT_LOGF(RT_TAG_HLE, "%s: Wii Remote motion %.2f (threshold %.2f, controller kind %u)\n", gesture, magnitude,
                threshold, kind);
    }
}

// cAIPad::Detect{Left,Right}Shake for a Wii Remote: a big enough jolt, as a facing angle.
bool DetectShake(uint32_t aipad, uint32_t history, uint32_t thresholdTweak, uint32_t dirOut, const char* gesture)
{
    const float threshold = Memory::ReadFloat32(thresholdTweak + 0x0C);  // TweakValueFloat::value
    float a[3];
    if (MaxAccelDelta(aipad, history, 5, a) > 0) {
        a[1] = std::fabs(a[1]) < std::fabs(a[2]) ? a[2] : -a[1];
        if (a[0] * a[0] + a[1] * a[1] > threshold * threshold) {
            LogShake(gesture, Memory::Read32(aipad + kAIPadDetInput), std::sqrt(a[0] * a[0] + a[1] * a[1]), threshold);
            const uint32_t det = Memory::Read32(aipad + kAIPadDetInput);
            const uint16_t remap = det ? Memory::Read16(det + kDetRemapAngle) : 0;
            Memory::Write16(dirOut, static_cast<uint16_t>(static_cast<int32_t>(std::atan2(a[1], -a[0]) * 10430.378f) + remap));
            return true;
        }
    }
    Memory::Write16(dirOut, 0);
    return false;
}
} // namespace

// bool cAIPad::DetectRightShake(u16* direction): the big hit. GameCube controls: Y, without the ball.
extern "C" void MSC_DetectRightShake_80007688(CpuContext* ctx)
{
    const uint32_t aipad = ctx->gpr[3], dirOut = ctx->gpr[4];
    const uint32_t det = Memory::Read32(aipad + kAIPadDetInput);
    if (IsGameCube(det)) {
        const uint32_t owner = BallOwner();
        const bool hasBall = owner && PlayerDetInput(owner) == det;
        Memory::Write16(dirOut, 0);
        ctx->gpr[3] = (!hasBall && DetJustPressed(ctx, det, kActHit)) ? 1u : 0u;
        if (ctx->gpr[3]) LogShake("Big hit", det, 0.0f, 0.0f);
        return;
    }
    ctx->gpr[3] = DetectShake(aipad, 0x004, 0x80568450u, dirOut, "Big hit") ? 1u : 0u;  // remote history, gfRightShakeThreshold
}
PPC_NATIVE_OVERRIDE_VOID(80007688, MSC_DetectRightShake_80007688, (CpuContext* ctx), (ctx));

// bool cAIPad::DetectLeftShake(u16* direction): cycle the item. GameCube controls: Z.
extern "C" void MSC_DetectLeftShake_80007594(CpuContext* ctx)
{
    const uint32_t aipad = ctx->gpr[3], dirOut = ctx->gpr[4];
    const uint32_t det = Memory::Read32(aipad + kAIPadDetInput);
    if (IsGameCube(det)) {
        Memory::Write16(dirOut, 0);
        ctx->gpr[3] = DetJustPressed(ctx, det, kActToggleItem) ? 1u : 0u;
        if (ctx->gpr[3]) LogShake("Item toggle", det, 0.0f, 0.0f);
        return;
    }
    ctx->gpr[3] = DetectShake(aipad, 0x16C, 0x80568430u, dirOut, "Item toggle") ? 1u : 0u;  // Nunchuk history, gfLeftShakeThreshold
}
PPC_NATIVE_OVERRIDE_VOID(80007594, MSC_DetectLeftShake_80007594, (CpuContext* ctx), (ctx));

namespace {
// The D-pad-driven actions for a GameCube controller (slide tackle, deke, Charged special), as the
// angle fn_80036D74 reports: right 0, up 0x4000 (screen space, like the D-pad and the sticks).
bool GameCubeActionAngle(CpuContext* ctx, uint32_t fielder, uint32_t det, uint16_t& angle)
{
    const auto polar = [&](uint32_t at, uint16_t& a) {
        a = Memory::Read16(det + at);
        const float r = Memory::ReadFloat32(det + at + 4);
        return r == r && r > 0.3f;  // NaN-safe: the game's fast sqrt gives NaN for a centred stick
    };
    uint16_t leftAngle = 0, cAngle = 0;
    const bool left = polar(kDetPolarLeft, leftAngle);
    // Centred stick: the way the player faces, back in screen space.
    const uint16_t facing = static_cast<uint16_t>(Memory::Read16(fielder + kPlayerFacing) - Memory::Read16(det + kDetRemapAngle));
    const uint16_t moveAngle = left ? leftAngle : facing;
    if (Memory::Read32(fielder + kPlayerBall) != 0) {
        // With the ball: deke on Y (toward the left stick) or the C-stick (toward the C-stick).
        if (polar(kDetPolarRight, cAngle)) { angle = cAngle; return true; }
        if (DetIsPressed(ctx, det, kActDeke)) { angle = moveAngle; return true; }
    } else {
        // Without it: slide tackle on B, only while the other team has the ball.
        const uint32_t owner = BallOwner();
        if (owner && Memory::Read32(owner + kPlayerTeam) != Memory::Read32(fielder + kPlayerTeam) &&
            DetIsPressed(ctx, det, kActSlide)) {
            angle = moveAngle;
            return true;
        }
    }
    // Charged extra: R does what the D-pad does in Charged (special move, slide or deke).
    if (Memory::Read16(det + kDetButtons) & kPadTriggerR) { angle = moveAngle; return true; }
    return false;
}
} // namespace

// bool fn_80036D74(cFielder*, u16* angleOut): the D-pad action (slide tackle / deke / special) and
// its direction. Real remotes: the original D-pad fold into 8 angles. GameCube controllers: SMS's
// buttons first (GameCubeActionAngle), then the same D-pad fold.
extern "C" void MSC_DpadActionDirection_80036D74(CpuContext* ctx)
{
    const uint32_t fielder = ctx->gpr[3], out = ctx->gpr[4];
    const uint32_t det = PlayerDetInput(fielder);
    uint16_t angle = 0;
    if (IsGameCube(det) && GameCubeActionAngle(ctx, fielder, det, angle)) {
        Memory::Write16(out, angle);
        ctx->gpr[3] = 1;
        return;
    }
    enum : int32_t { kLeft = 11, kRight = 12, kUp = 13, kDown = 14 };
    bool found = false;
    uint32_t a = 0;
    if (det && DetIsPressed(ctx, det, kLeft)) {
        a = 0x8000;
        if (DetIsPressed(ctx, det, kUp)) a -= 0x2000;
        else if (DetIsPressed(ctx, det, kDown)) a += 0x2000;
        found = true;
    }
    if (det && DetIsPressed(ctx, det, kRight)) {
        a = 0;
        if (DetIsPressed(ctx, det, kUp)) a = 0x2000;
        else if (DetIsPressed(ctx, det, kDown)) a = 0xE000;
        found = true;
    }
    if (det && DetIsPressed(ctx, det, kUp)) {
        a = 0x4000;
        if (DetIsPressed(ctx, det, kLeft)) a = 0x6000;
        else if (DetIsPressed(ctx, det, kRight)) a = 0x2000;
        found = true;
    }
    if (det && DetIsPressed(ctx, det, kDown)) {
        a = 0xC000;
        if (DetIsPressed(ctx, det, kLeft)) a = 0xA000;
        else if (DetIsPressed(ctx, det, kRight)) a = 0xE000;
        found = true;
    }
    if (found) Memory::Write16(out, static_cast<uint16_t>(a));
    ctx->gpr[3] = found ? 1u : 0u;
}
PPC_NATIVE_OVERRIDE_VOID(80036D74, MSC_DpadActionDirection_80036D74, (CpuContext* ctx), (ctx));

// bool fn_80036F88(cFielder*): is a D-pad action requested (deke / special trigger).
extern "C" void MSC_DpadActionHeld_80036F88(CpuContext* ctx)
{
    const uint32_t fielder = ctx->gpr[3];
    const uint32_t det = PlayerDetInput(fielder);
    uint16_t angle = 0;
    const bool held = det && ((IsGameCube(det) && GameCubeActionAngle(ctx, fielder, det, angle)) ||
                              DetIsPressed(ctx, det, 11) || DetIsPressed(ctx, det, 12) ||
                              DetIsPressed(ctx, det, 14) || DetIsPressed(ctx, det, 13));
    ctx->gpr[3] = held ? 1u : 0u;
}
PPC_NATIVE_OVERRIDE_VOID(80036F88, MSC_DpadActionHeld_80036F88, (CpuContext* ctx), (ctx));
