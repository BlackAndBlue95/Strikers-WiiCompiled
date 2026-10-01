// Mario Strikers Charged (R4QE01) game-specific HLE.
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>
#include "abi_bridge.h"
#include "hle_stubs.h"
#include "msc_away_kits.h"
#include "runtime_config.h"
#include "runtime_log.h"
#include "wiimote_hid.h"
#include <SDL3/SDL_timer.h>
#include "guest_interrupt_context.h"
#include "ppc_runtime.h"

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
// Gameplay extras (F10 > Mods), applied once per frame from UpdatePlatPad.
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
    return ((pad.button & PAD_BUTTON_X) ? 1u : 0u) | ((pad.button & PAD_BUTTON_Y) ? 2u : 0u);
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

void Apply() {
    try {
        UnlockEverything();
        KitChoice();
        BluePeach();
        ShotCounter();
        FastStadiums();
        WinByTwo();
        NkFix();
    } catch (const Memory::AccessViolation&) {
    }
}
} // namespace FrameMods

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
