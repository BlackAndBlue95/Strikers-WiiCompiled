/*
 * A small plugin showing each part of the API: settings on the F10 Mods page, a frame event, reading
 * the game's memory, and a hook on a game function. It logs each match's score as it changes and the
 * names cutscenes are picked by (console.log, "[mods] example.hello: ...").
 */
#include <stdio.h>
#include "msc_mod_api.h"

static const MscModApi* api;
static MscMod* self;

#define TEAMS 0x806E0DF8u      /* g_pTeams[2] (cTeam*) */
#define TEAM_SCORE 0x04u       /* cTeam::m_nScore */
#define GET_TARGET_FILTER 0x8027F9D4u /* const char* NisPlayer::GetTargetFilter(NisTarget, NisWinnerType) */

static void OnFrame(MscCpu* cpu, void* user) {
    static uint32_t last[2] = {0xFFFFFFFFu, 0xFFFFFFFFu};
    (void)cpu;
    (void)user;
    if (!api->get_bool(self, "log_score")) return;
    uint32_t score[2];
    for (int side = 0; side < 2; ++side) {
        const uint32_t team = api->read32(TEAMS + 4u * (uint32_t)side);
        score[side] = team ? api->read32(team + TEAM_SCORE) : 0xFFFFFFFFu;
    }
    if (score[0] == last[0] && score[1] == last[1]) return;
    last[0] = score[0];
    last[1] = score[1];
    if (score[0] == 0xFFFFFFFFu || score[1] == 0xFFFFFFFFu) return; /* no match running */
    char message[64];
    snprintf(message, sizeof(message), "score %u - %u", score[0], score[1]);
    api->log(self, MSC_LOG_INFO, message);
}

/* Runs instead of NisPlayer::GetTargetFilter: r3 = this, r4 = target; the original returns the name
   in r3. A hook could return a different name here to make the game pick other cutscenes. */
static void OnGetTargetFilter(MscCpu* cpu, void* user) {
    const uint32_t target = api->get_gpr(cpu, 4);
    (void)user;
    api->call_original(cpu);
    if (!api->get_bool(self, "log_cutscenes")) return;
    char name[32] = {0};
    const uint32_t filter = api->get_gpr(cpu, 3);
    if (filter) api->read_bytes(filter, name, sizeof(name) - 1);
    char message[80];
    snprintf(message, sizeof(message), "cutscene target %u picks by \"%s\"", target, name);
    api->log(self, MSC_LOG_INFO, message);
}

MSC_MOD_EXPORT int msc_mod_init(const MscModApi* runtime, MscMod* mod) {
    if (runtime->version < 1) return 1;
    api = runtime;
    self = mod;
    api->declare_bool(mod, "log_score", "Log the score", "Writes each match's score to console.log as it changes.", 1);
    api->declare_bool(mod, "log_cutscenes", "Log cutscene names", "Writes the name each cutscene is picked by.", 1);
    api->subscribe(mod, MSC_EVENT_FRAME, OnFrame, NULL);
    if (api->hook(mod, GET_TARGET_FILTER, OnGetTargetFilter, NULL) != 0)
        api->log(mod, MSC_LOG_WARNING, "GetTargetFilter isn't hookable in this build");
    api->log(mod, MSC_LOG_INFO, "hello from a native plugin");
    return 0;
}
