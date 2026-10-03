// Peach's away kit. Blue Peach (and Strikers Recharged's kit choice) puts her in it, but the game never
// does: she's the one captain with an away kit and none of the team's things to go with it, as no
// other captain shares her pink. Where the game asks for those, she gets her home kit's instead (as
// does any captain without them, Super Team say), here whether or not a tweak is on:
//  - the crowd's list (ini/CrowdCharacterLists/<name>Alt.ini): missing, the match load crashed;
//  - the sidekick select portraits (fe/sidekick_images/..._<name>_alt): missing, sidekick select
//    crashed (an image read from directory entry -1);
//  - the team's banners and Mega Strike hand (<name>/<name>_banners_alt, <name>/mega_hand_alt, from
//    the captain's ExtraTextures): missing, black.
// Strikers Recharged has the first two built in as well.
#include <kamek.h>

#include "NL/nlBundleFile.h"
#include "NL/nlFile.h"
#include "NL/nlPrint.h"
#include "NL/nlString.h"

extern "C" {
int DVDConvertPathToEntrynum(const char* path);
void OSReport(const char* format, ...);
}

namespace {

int Length(const char* s) {
    int n = 0;
    while (s[n] != '\0') ++n;
    return n;
}

bool Same(const char* a, const char* b) {
    while (*a != '\0' && *a == *b) ++a, ++b;
    return *a == *b;
}

// name without its "_alt" ending, in home; false when it has none or doesn't fit.
bool HomeName(const char* name, char* home, int size) {
    const int n = Length(name);
    if (n <= 4 || n - 4 >= size || !Same(name + n - 4, "_alt")) return false;
    for (int i = 0; i < n - 4; ++i) home[i] = name[i];
    home[n - 4] = '\0';
    return true;
}

}  // namespace

// ---- The crowd

// The match's crowd (LoadCrowdCharacterList) is the home captain's list, its Alt list when the home
// team wears its away kit.
static int CrowdListPath(char* buffer, unsigned long size, const char* format, const char* name, const char* suffix) {
    const int length = nlSNPrintf(buffer, size, format, name, suffix);
    if (suffix[0] == '\0' || DVDConvertPathToEntrynum(buffer) >= 0) return length;
    OSReport("[Strikers Tweaks] %s isn't on the disc: the home kit's crowd instead\n", buffer);
    return nlSNPrintf(buffer, size, format, name, "");
}
kmCall(0x801A42A0, CrowdListPath);  // nlSNPrintf(path, 256, "ini/CrowdCharacterLists/%s%s.ini", name, "Alt" or "")

// ---- Bundled images

asm static void GetFileInfo_Original() {
    nofralloc
    stwu r1, -288(r1)
    lis r12, 0x802B
    ori r12, r12, 0xE034
    mtctr r12
    bctr
}

asm static void ReadFileAsync_Original() {
    nofralloc
    stwu r1, -288(r1)
    lis r12, 0x802B
    ori r12, r12, 0xE350
    mtctr r12
    bctr
}

typedef bool (*GetFileInfoFn)(BundleFile*, const char*, BundleFileDirectoryEntry*, bool);
typedef void (*ReadFileAsyncFn)(BundleFile*, const char*, void*, unsigned long, FileReadAsyncCallback, unsigned long);

// bool BundleFile::GetFileInfo(const char* name, BundleFileDirectoryEntry*, bool printError): a
// "<name>_alt" the bundle doesn't have is "<name>".
static bool GetFileInfo(BundleFile* bundle, const char* name, BundleFileDirectoryEntry* entry, bool printError) {
    const GetFileInfoFn original = (GetFileInfoFn)GetFileInfo_Original;
    char home[128];
    if (!HomeName(name, home, sizeof(home))) return original(bundle, name, entry, printError);
    return original(bundle, name, entry, false) || original(bundle, home, entry, printError);
}
kmBranch(0x802BE030, GetFileInfo);

// void BundleFile::ReadFileAsync(const char* name, ...): looks the file up by name first, without a
// check, and reads from entry -1 when it isn't there. The same fallback.
static void ReadFileAsync(BundleFile* bundle, const char* name, void* buffer, unsigned long size,
                          FileReadAsyncCallback callback, unsigned long param) {
    const GetFileInfoFn find = (GetFileInfoFn)GetFileInfo_Original;
    char home[128];
    BundleFileDirectoryEntry entry;
    if (HomeName(name, home, sizeof(home)) && !find(bundle, name, &entry, false) && find(bundle, home, &entry, false))
        name = home;
    ((ReadFileAsyncFn)ReadFileAsync_Original)(bundle, name, buffer, size, callback, param);
}
kmBranch(0x802BE34C, ReadFileAsync);

// ---- Team art

namespace {

// The textures in a captain's ExtraTextures (art/characters/<name>/extratextures.rlt, a texture
// bundle: "PTLG", the count at +4, 16-byte entries {hash, offset, size} from +0x10), read once each.
struct Extras {
    char name[24];
    int count;  // -1: no bundle, or not one
    unsigned long hashes[32];
};
const int kMaxExtras = 16;
Extras s_extras[kMaxExtras];
int s_extrasCount;
unsigned char s_header[544] __attribute__((aligned(32)));

const Extras* ExtrasOf(const char* name) {
    for (int i = 0; i < s_extrasCount; ++i)
        if (Same(s_extras[i].name, name)) return &s_extras[i];
    if (s_extrasCount == kMaxExtras || Length(name) >= (int)sizeof(s_extras[0].name)) return 0;
    Extras& extras = s_extras[s_extrasCount++];
    for (int i = 0; i <= Length(name); ++i) extras.name[i] = name[i];
    extras.count = -1;
    char path[96];
    nlSNPrintf(path, sizeof(path), "art/characters/%s/extratextures.rlt", name);
    if (!nlFileExists(path)) return &extras;
    nlFile* file = nlOpen(path);
    if (file == 0) return &extras;
    unsigned int padded = 0;
    const unsigned int size = nlFileSize(file, &padded);
    const unsigned int length = size < sizeof(s_header) ? size & ~31u : sizeof(s_header);
    if (length >= 32) {
        nlRead(file, s_header, length, sizeof(s_header));
        if (s_header[0] == 'P' && s_header[1] == 'T' && s_header[2] == 'L' && s_header[3] == 'G') {
            const unsigned long count = *(unsigned long*)(s_header + 4);
            extras.count = 0;
            for (unsigned long i = 0; i < count && extras.count < 32 && 16 + i * 16 + 4 <= length; ++i)
                extras.hashes[extras.count++] = *(unsigned long*)(s_header + 16 + i * 16);
        }
    }
    nlClose(file);
    return &extras;
}

// Once per texture: the stadium's banners are looked up again as its models update.
bool FirstTime(unsigned long hash) {
    static unsigned long s_seen[16];
    static int s_seenCount;
    for (int i = 0; i < s_seenCount; ++i)
        if (s_seen[i] == hash) return false;
    if (s_seenCount < 16) s_seen[s_seenCount++] = hash;
    return true;
}

// Whether the captain's ExtraTextures has the texture; true when that can't be told.
bool HasExtraTexture(const char* captain, const char* texture) {
    const Extras* extras = ExtrasOf(captain);
    if (extras == 0 || extras->count < 0) return true;
    const unsigned long hash = nlStringLowerHash(texture);
    for (int i = 0; i < extras->count; ++i)
        if (extras->hashes[i] == hash) return true;
    return false;
}

}  // namespace

// The away kit's team textures, by name: "%s/%s_banners_alt" (the stadium's banners and the crowd's
// flags) and "%s/mega_hand_alt" (the Mega Strike hand), each of the captain's name. One the captain's
// ExtraTextures don't have is the home kit's, the same name without "_alt".
static int AwayTextureName(char* buffer, unsigned long size, const char* format, const char* name, const char* again) {
    const int length = nlSNPrintf(buffer, size, format, name, again);
    char home[32];
    if (HasExtraTexture(name, buffer) || !HomeName(format, home, sizeof(home))) return length;
    if (FirstTime(nlStringLowerHash(buffer)))
        OSReport("[Strikers Tweaks] %s isn't in the captain's textures: the home kit's instead\n", buffer);
    return nlSNPrintf(buffer, size, home, name, again);
}
kmCall(0x80276910, AwayTextureName);  // SetStadiumBannerTextures: the crowd's flags
kmCall(0x80279CB4, AwayTextureName);  // StadiumWorldDrawable::UpdateModelMaterials: the stadium's banners
kmCall(0x8008F4EC, AwayTextureName);  // the Mega Strike hand
