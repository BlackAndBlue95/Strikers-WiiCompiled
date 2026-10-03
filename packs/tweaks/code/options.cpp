// The tweaks' options. Each is an option of the pack that, turned on, puts a small file on the disc
// (/tweaks/<name>), and the code looks for those files: first when it asks, then every half second.
// On a Wii and in Dolphin the disc is put together once, at launch, so a change applies at the next
// launch there; Strikers Recharged adds and removes the files as F10 > Mods changes the options, so
// there a tweak goes on or off while the game runs. Either way the code itself never changes, and
// Strikers Recharged builds the pack in once.
#include <kamek.h>

#include "tweaks.h"

extern "C" {
int DVDConvertPathToEntrynum(const char* path);
void OSReport(const char* format, ...);
}

namespace {

const char* const kFiles[kTweakCount] = {
    "/tweaks/skipintro",  "/tweaks/fastmenus", "/tweaks/faststadiums", "/tweaks/nomegastrikes", "/tweaks/unlockall",
    "/tweaks/winbytwo",   "/tweaks/nkfix",     "/tweaks/bluepeach",    "/tweaks/shotcounter",
};
const int kRefreshFrames = 30;

bool s_on[kTweakCount];
bool s_read;
int s_frames;

void ReadTweaks() {
    const bool first = !s_read;
    s_read = true;
    int on = 0;
    for (int i = 0; i < kTweakCount; ++i) {
        const bool now = DVDConvertPathToEntrynum(kFiles[i]) >= 0;
        if (first ? now : now != s_on[i]) OSReport("[Strikers Tweaks] %s%s\n", kFiles[i] + 8, first ? "" : now ? " on" : " off");
        s_on[i] = now;
        if (now) ++on;
    }
    if (first) OSReport("[Strikers Tweaks] %d tweak(s) on\n", on);
}

}  // namespace

bool TweakOn(Tweak tweak) {
    if (!s_read) ReadTweaks();
    return tweak >= 0 && tweak < kTweakCount && s_on[tweak];
}

void RefreshTweaks() {
    if (++s_frames < kRefreshFrames) return;
    s_frames = 0;
    ReadTweaks();
}
