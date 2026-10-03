// Super Mario Strikers' Super Team: the robot team, a character of its own built on Waluigi.
#include "characters.h"

namespace {

const unsigned short kName[] = {'S', 'U', 'P', 'E', 'R', ' ', 'T', 'E', 'A', 'M', 0};

const ModTexture kTextures[] = {
    {"fe/screens/images/attributes_superteam", "/mods/superteam/attributes_superteam.gxt"},
    {"fe/screens/images/logos_TEAM_superteam", "/mods/superteam/logos_TEAM_superteam.gxt"},
    {"fe/screens/images/logos_TEAM_superteam_bg", "/mods/superteam/logos_TEAM_superteam_bg.gxt"},
    {"fe/screens/images/lowerthird_superteam", "/mods/superteam/lowerthird_superteam.gxt"},
    {"fe/screens/images/superteam_left", "/mods/superteam/superteam_left.gxt"},
    {"fe/screens/images/superteam_right", "/mods/superteam/superteam_right.gxt"},
    {"fe/captain_icons/captain_icons_superteam", "/mods/superteam/captain_icons_superteam.gxt"},
    {"fe/screens/images/LOGOS_TEAMS_superteam_knockout", "/mods/superteam/logos_TEAM_superteam.gxt"},
    // The Striker Times' photos of him (three each for a win, a draw and a loss): its logo.
    {"fe/striker_times_textures/superteam_positive_00", "/mods/superteam/news_superteam.gxt"},
    {"fe/striker_times_textures/superteam_positive_01", "/mods/superteam/news_superteam.gxt"},
    {"fe/striker_times_textures/superteam_positive_02", "/mods/superteam/news_superteam.gxt"},
    {"fe/striker_times_textures/superteam_neutral_00", "/mods/superteam/news_superteam.gxt"},
    {"fe/striker_times_textures/superteam_neutral_01", "/mods/superteam/news_superteam.gxt"},
    {"fe/striker_times_textures/superteam_neutral_02", "/mods/superteam/news_superteam.gxt"},
    {"fe/striker_times_textures/superteam_negative_00", "/mods/superteam/news_superteam.gxt"},
    {"fe/striker_times_textures/superteam_negative_01", "/mods/superteam/news_superteam.gxt"},
    {"fe/striker_times_textures/superteam_negative_02", "/mods/superteam/news_superteam.gxt"},
};

// Its cutscenes run the trigger scripts Charged kept from its unfinished port of the team (SMS's
// own: the charge-up and the rocket boots, whose effects are in unusedeffects.bun).
const char* const kTriggerAliases[][2] = {
    {"superteam_goal_winner_high_0", "mystery_goal_winner_high_0"},
    {"superteam_goal_winner_high_1", "mystery_goal_winner_high_1"},
    {"superteam_goal_winner_low_0", "mystery_goal_winner_low_0"},
    {"superteam_home_capt_intro_1", "mystery_enter_stadium_home_0"},
    {"superteam_away_capt_intro_1", "mystery_enter_stadium_away_0"},
    {"superteam_home_capt_intro_3", "mystery_attitude_home_0"},
    {"superteam_away_capt_intro_2_home", "mystery_attitude_home_0"},
    {"superteam_end_of_game_holotron_home", "mystery_end_of_game_home_0"},
};

// Its Mega Strike, Super Mario Strikers' Super Strike: its cutscenes (superteam_megastrike_<side>_<n>,
// made by megastrike.py: Waluigi's shots with SMS's animation) and SMS's effects for them
// (fx/megastrike.fx), here when they play. 0: it winds up, charging, and rockets after the ball; 1 it
// rises; 2 it floats in its vortex, then kicks (frame 41); 3 the balls fly. SMS played the vortex while
// the robot floated: its pieces are short, so it starts again every 10 frames.
const ModNisTrigger kNisTriggers[] = {
    {"superteam_megastrike_*_0", 0, kNisEffect, "superteam_sts_windup", "bip01", 0},
    {"superteam_megastrike_*_0", 12, kNisEffect, "mystery_end_of_game_smoke_puff", "bip01", 0},
    {"superteam_megastrike_*_0", 13, kNisEffect, "mystery_end_of_game_rockets", "bip01", 0},
    {"superteam_megastrike_*_0", 13, kNisEffect, "mystery_end_of_game_rockets_2", "bip01", 0},
    {"superteam_megastrike_*_0", 14, kNisEffect, "mystery_end_of_game_smoke_trail", "bip01", 0},
    {"superteam_megastrike_*_1", 0, kNisEffect, "mystery_end_of_game_rockets", "bip01", 0},
    {"superteam_megastrike_*_1", 0, kNisEffect, "mystery_end_of_game_rockets_2", "bip01", 0},
    {"superteam_megastrike_*_1", 0, kNisEffect, "mystery_end_of_game_smoke_trail", "bip01", 0},
    {"superteam_megastrike_*_2", 0, kNisDepthOfFieldOff, 0, 0, 0},
    {"superteam_megastrike_*_2", 2, kNisEffect, "superteam_sts_vortex", "bip01", 0},
    {"superteam_megastrike_*_2", 12, kNisEffect, "superteam_sts_vortex", "bip01", 0},
    {"superteam_megastrike_*_2", 22, kNisEffect, "superteam_sts_vortex", "bip01", 0},
    {"superteam_megastrike_*_2", 32, kNisEffect, "superteam_sts_vortex", "bip01", 0},
    {"superteam_megastrike_*_2", 33, kNisTimeDilation, 0, 0, 0.4f},  // a beat before the kick, as Charged's
    {"superteam_megastrike_*_2", 36, kNisTimeDilation, 0, 0, 0.08f},
    {"superteam_megastrike_*_2", 39, kNisTimeDilation, 0, 0, 1.0f},
    {"superteam_megastrike_*_2", 41, kNisEffect, "superteam_sts_shot", "ball", 0},
    {"superteam_megastrike_*_3", 0, kNisDepthOfFieldOff, 0, 0, 0},
    {"superteam_megastrike_*_3", 0, kNisEffect, "superteam_megastrike_ball", "ball", 0},
    {"superteam_megastrike_*_3", 0, kNisEffect, "superteam_megastrike_ball", "nis_ball01", 0},
    {"superteam_megastrike_*_3", 0, kNisEffect, "superteam_megastrike_ball", "nis_ball02", 0},
    {"superteam_megastrike_*_3", 0, kNisEffect, "superteam_megastrike_ball", "nis_ball04", 0},
    {"superteam_megastrike_*_3", 0, kNisEffect, "superteam_megastrike_ball", "nis_ball05", 0},
    {"superteam_megastrike_*_3", 0, kNisEffect, "superteam_megastrike_ball", "nis_ball06", 0},
    {"superteam_megastrike_*_3", 15, kNisEffect, "superteam_megastrike_ball", "nis_ball01", 0},
    {"superteam_megastrike_*_3", 15, kNisEffect, "superteam_megastrike_ball", "nis_ball02", 0},
    {"superteam_megastrike_*_3", 15, kNisEffect, "superteam_megastrike_ball", "nis_ball04", 0},
    {"superteam_megastrike_*_3", 15, kNisEffect, "superteam_megastrike_ball", "nis_ball05", 0},
    {"superteam_megastrike_*_3", 15, kNisEffect, "superteam_megastrike_ball", "nis_ball06", 0},
};

}  // namespace

const ModCharacter kModCharacters[] = {
    {
        "superteam",
        6,  // waluigi
        "art/characters/superteam/superteam.rlg",
        "art/characters/superteam/superteam.rlt",
        "art/animation/superteam.sanim.zlib",
        "/ini/characters/superteam.ini",  // on the retail disc, left over from Super Mario Strikers
        "/Game/Gameplay/ini/chars/superteam",
        "/mods/superteam/captain_superteam_s.gxt",
        "/mods/superteam/captain_superteam_ds.gxt",
        kName,
        "NAME_SUPERTEAM",
        0x3A6EA5,
        {0.6f, 0.6f, 0.6f, 0.6f},  // balanced
        4,                         // balanced
        true,                      // no super ability (no Waluigi wall)
        "superteamgoalie",
        "art/characters/superteamgoalie/superteamgoalie.rlt",
        "CHAR_SUPERTEAM_Sfx",
        kTextures,
        sizeof(kTextures) / sizeof(kTextures[0]),
        true,  // always three robots, as in SMS
        "art/effects/superteam_sms.bun",  // SMS's Super Team cutscene effects (mystery_*), converted
        kTriggerAliases,
        sizeof(kTriggerAliases) / sizeof(kTriggerAliases[0]),
        true,  // the generic deke (SMS's spin), not Waluigi's teleport
        kNisTriggers,
        sizeof(kNisTriggers) / sizeof(kNisTriggers[0]),
    },
};

const int kModCharacterCount = sizeof(kModCharacters) / sizeof(kModCharacters[0]);
