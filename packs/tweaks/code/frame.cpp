// The tweaks that act once a frame, from the game's main loop (nlTaskManager::RunAllTasks): each
// keeps a value of the game's as the tweak wants it. As Strikers Recharged had them natively.
#include <kamek.h>

#include "tweaks.h"

namespace {

const unsigned long kTeams = 0x806E0DF8;          // g_pTeams[2]
const unsigned long kTeamScore = 0x04;            // cTeam::m_nScore
const unsigned long kTeamPlayers = 0xA4;          // cTeam::m_pPlayers[5], the goalie last
const unsigned long kUseCurGameSettings = 0x27C;  // GameInfoManager: GetCurrentSettings() gives the match's own copy
const unsigned long kCharacterInfo = 0x80505944;  // sCharacterInfo[], 0x5C each
const unsigned long kColourMask = 0x4C, kColourRank = 0x50;

// Unlock everything: all characters, stadiums and cheats, through the game's own override, off again
// when the tweak is.
void UnlockEverything() {
    static bool s_applied;
    if (TweakOn(kUnlockAll)) {
        *(unsigned char*)0x806E0F98 = 1;  // gUnlockAll
        s_applied = true;
    } else if (s_applied) {
        *(unsigned char*)0x806E0F98 = 0;
        s_applied = false;
    }
}

// Blue Peach: kits clash when two captains' colour masks overlap, and the higher colour rank switches
// to its away kit. Peach's mask is pink only (0x04, rank 5), so she stays pink against red teams; with
// red in her mask and the highest rank, against red captains she alone switches, to blue. From the
// next captain select on.
void BluePeach() {
    static bool s_applied;
    const bool want = TweakOn(kBluePeach);
    if (want == s_applied) return;
    const unsigned long peach = kCharacterInfo + 5 * 0x5C;
    At<unsigned long>(peach, kColourMask) = want ? 0x05 : 0x04;
    At<unsigned long>(peach, kColourRank) = want ? 240 : 5;
    s_applied = want;
}

// Shot counter: the match summary's Mega Strike row shows a team's PlayerStats +0x18 / +0x16. The game
// counts every shot by charge (+0x00 white, +0x02 yellow, +0x04 red); those fill the Mega Strike fields
// of the team totals both summary screens copy from (StatsTracker: current and cumulative).
void FillShots(unsigned long team) {
    if (team == 0) return;
    const unsigned long stats = team + 0x1C;  // TeamStats::mPlayerTotalStats
    At<unsigned short>(stats, 0x16) = (unsigned short)(At<unsigned short>(stats, 0x00) + At<unsigned short>(stats, 0x02));
    At<unsigned short>(stats, 0x18) = At<unsigned short>(stats, 0x04);
}

void ShotCounter() {
    if (!TweakOn(kShotCounter)) return;
    const unsigned long tracker = *(unsigned long*)0x806E0F58;  // nlSingleton<StatsTracker>::s_pInstance
    if (tracker == 0) return;
    for (unsigned long side = 0; side < 2; ++side) {
        FillShots(tracker + 0x0C + side * 0x70);             // current TeamStats[2]
        FillShots(At<unsigned long>(tracker, 0x04 + side * 4));  // cumulative TeamStats*[2]
    }
}

// All stadiums fast-paced: the pitch's surface is the stadium's TerrainTweaks (gGameTweaks.mTerrainTweaks);
// during a match its four values are DryTerrain.ini's, the fast surface. Each TweakFloatBinding keeps
// its value's pointer at +0x0C.
void FastStadiums() {
    if (!TweakOn(kFastStadiums) || !InMatch()) return;
    const unsigned long terrain = At<unsigned long>(0x8056CF08, 0x04);  // gGameTweaks.mTerrainTweaks
    if (terrain == 0) return;
    static const float kDry[4] = {0.65f, 0.0f, 0.2f, 0.6f};  // speed, slipperiness, friction, bounce
    for (unsigned long i = 0; i < 4; ++i) {
        const unsigned long value = At<unsigned long>(terrain, 0x04 + i * 0x10 + 0x0C);
        if (value != 0) *(float*)value = kDry[i];
    }
}

// Win by 2: every win check reads GetCurrentSettings()->GoalLimit, which in a local match is the match's
// own copy, never the saved options. Tied one short of the target, the target becomes the score + 2;
// the original comes back between matches.
long s_originalLimit = -1;
unsigned long s_limitInfo;

void RestoreLimit() {
    if (s_originalLimit >= 0 && s_limitInfo != 0) At<long>(s_limitInfo, 0x04 + 0x0C) = s_originalLimit;
    s_originalLimit = -1;
    s_limitInfo = 0;
}

void WinByTwo() {
    const unsigned long info = *(unsigned long*)kGameInfoManager;
    if (!TweakOn(kWinByTwo) || info == 0 || !InMatch() || !At<unsigned char>(info, kUseCurGameSettings) ||
        At<long>(info, 0x04 + 0x04) != 1) {  // mCurGameGameplayOptions.GameLimitType: 1, goals
        RestoreLimit();
        return;
    }
    const unsigned long home = At<unsigned long>(kTeams, 0), away = At<unsigned long>(kTeams, 4);
    if (home == 0 || away == 0) return;
    const long s0 = At<long>(home, kTeamScore), s1 = At<long>(away, kTeamScore);
    if (s_originalLimit < 0 || s_limitInfo != info || (s0 == 0 && s1 == 0)) {
        RestoreLimit();
        s_limitInfo = info;
        s_originalLimit = At<long>(info, 0x04 + 0x0C);  // mCurGameGameplayOptions.GoalLimit
    }
    long& limit = At<long>(info, 0x04 + 0x0C);
    if (s0 == s1 && s0 >= s_originalLimit - 1 && limit < s0 + 2) limit = s0 + 2;
}

// The NK bug: action 0x21 (a fielder going through the opposing goalie) turns off that goalie's ball
// collision and the ball's player and goalie collision; only the action's normal exit turns them back
// on. Cut short (Boo deking into his own Kritter, Dry Bones teleporting behind the goal), they stay off
// and every shot passes through Kritter. When a fielder leaves 0x21 by any route, this does what the
// exit does; and since nothing else turns the ball's off, they're kept on while no fielder is in 0x21.
bool s_throughGoalie[2][4];

void RestoreBall(unsigned long ballPhysics) {
    if (ballPhysics == 0) return;
    At<unsigned char>(ballPhysics, 0x80) = 1;  // PhysicsAIBall::mbCanCollidePlayer
    At<unsigned char>(ballPhysics, 0x81) = 1;  // ... goalie
}

void NkFix() {
    if (!TweakOn(kNkFix) || !InMatch()) {
        for (int t = 0; t < 2; ++t)
            for (int j = 0; j < 4; ++j) s_throughGoalie[t][j] = false;
        return;
    }
    const unsigned long teams[2] = {At<unsigned long>(kTeams, 0), At<unsigned long>(kTeams, 4)};
    const unsigned long ball = *(unsigned long*)0x806E0BC0;  // g_pBall
    if (teams[0] == 0 || teams[1] == 0 || ball == 0) return;
    const unsigned long ballPhysics = At<unsigned long>(ball, 0xE8);  // cBall::m_pPhysicsBall
    bool any = false;
    for (int t = 0; t < 2; ++t) {
        for (int j = 0; j < 4; ++j) {
            const unsigned long fielder = At<unsigned long>(teams[t], kTeamPlayers + j * 4);
            const bool in = fielder != 0 && At<unsigned long>(fielder, 0x430) == 0x21;  // cFielder::m_eActionState
            if (s_throughGoalie[t][j] && !in) {
                const unsigned long goalie = At<unsigned long>(teams[1 - t], kTeamPlayers + 4 * 4);
                const unsigned long physics = goalie != 0 ? At<unsigned long>(goalie, 0x20) : 0;  // m_pPhysicsCharacter
                if (physics != 0) At<unsigned long>(physics, 0x98) |= 0x40000000;  // can collide with the ball
                RestoreBall(ballPhysics);
            }
            s_throughGoalie[t][j] = in;
            any = any || in;
        }
    }
    if (!any) RestoreBall(ballPhysics);
}

// No Mega Strikes with controllers: defending one means pointing at the incoming balls, which a
// controller that isn't a Wii Remote can't do. While one plays (a remote whose buttons carry the
// controller tag, 0x0080: Strikers Recharged's controllers, the GameCube controllers pack's), the
// match's own settings have Mega Strikes off for both sides, the game's own switch, so a full charge is
// a strong shot for players and AI alike. The saved options are untouched.
bool ControllerPlaying() {
    const unsigned long pads = *(unsigned long*)0x806E2478;  // g_pPlatPadManager
    if (pads == 0) return false;
    for (unsigned long chan = 0; chan < 4; ++chan) {
        if (At<long>(pads, 0x2F4 + chan * 4) == 2 &&             // PlatPadManager::type: remote and Nunchuk
            (At<unsigned short>(pads, chan * 0xBC) & 0x0080))  // status[chan].freestyle.wpad.button
            return true;
    }
    return false;
}

void NoMegaStrikes() {
    if (!TweakOn(kNoMegaStrikes) || !ControllerPlaying()) return;
    const unsigned long info = *(unsigned long*)kGameInfoManager;
    if (info == 0 || !At<unsigned char>(info, kUseCurGameSettings)) return;
    At<unsigned char>(info, 0x04 + 0x16) = 0;  // mCurGameGameplayOptions: home Mega Strikes
    At<unsigned char>(info, 0x04 + 0x17) = 0;  // away
}

}  // namespace

asm static void RunAllTasks_Original() {
    nofralloc
    stwu r1, -32(r1)
    lis r12, 0x802B
    ori r12, r12, 0x4FE8
    mtctr r12
    bctr
}

// void nlTaskManager::RunAllTasks(): the game's frame.
static void RunAllTasks() {
    RefreshTweaks();
    UnlockEverything();
    BluePeach();
    ShotCounter();
    FastStadiums();
    WinByTwo();
    NkFix();
    NoMegaStrikes();
    FastMenusFrame();
    ((void (*)())RunAllTasks_Original)();
}
kmBranch(0x802B4FE4, RunAllTasks);
