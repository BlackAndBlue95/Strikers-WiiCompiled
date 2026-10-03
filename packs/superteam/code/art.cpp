// A mod character's art where the front end looks for a character's by name: its pictures in the
// screens' art layers ("art/Layer/logos_TEAM_<name>", "positions_<name>", "<name>_right",
// "captain_<name>_s": the game copies their texture to the image it shows), its name in the string
// table, and the results screen's captain pictures (a 12-name table).
#include <kamek.h>

#include "Game/FE/feFinder.h"
#include "Game/FE/feTextureResource.h"
#include "Game/FE/tlComponent.h"
#include "Game/FE/tlTextInstance.h"
#include "NL/nlLocalization.h"
#include "NL/nlMemory.h"
#include "NL/nlString.h"

#include "framework.h"

extern "C" void* memcpy(void* dst, const void* src, unsigned long n);
extern "C" int stricmp(const char* a, const char* b);
extern unsigned char sCharacterInfo[];  // CharacterInfo[33], 0x5C each

static const unsigned char* sCharacterInfoRow(int i) {
    return sCharacterInfo + i * 0x5C;
}

namespace {

template <typename T> inline T& At(const void* base, unsigned long offset) {
    return *(T*)((char*)base + offset);
}

const unsigned long kImageTexture = 0x90;  // TLImageInstance::m_pTextureResource

void Join(char* out, unsigned long size, const char* a, const char* b, const char* c) {
    unsigned long n = 0;
    for (; a && *a && n + 1 < size; ++a) out[n++] = *a;
    for (; b && *b && n + 1 < size; ++b) out[n++] = *b;
    for (; c && *c && n + 1 < size; ++c) out[n++] = *c;
    out[n] = 0;
}

// ---- texture resources for pack textures ----

struct Resource {
    unsigned long hash;
    unsigned char data[0x20];  // FETextureResource
};
Resource s_resources[16];
int s_resourceCount;

}  // namespace

// An FETextureResource for a pack texture (a .gxt path), or 0.
FETextureResource* PackResource(const char* path) {
    const unsigned long hash = PackTexture(path);
    if (hash == 0) return 0;
    Resource* resource = 0;
    for (int i = 0; i < s_resourceCount && !resource; ++i)
        if (s_resources[i].hash == hash) resource = &s_resources[i];
    if (resource == 0) {
        if (s_resourceCount == 16) return 0;
        resource = &s_resources[s_resourceCount++];
        resource->hash = hash;
        unsigned char* data = resource->data;
        for (int i = 0; i < 0x20; ++i) data[i] = 0;
        At<unsigned long>(data, 0x0C) = hash;  // m_hashID
        At<unsigned char>(data, 0x10) = 1;     // m_bValid
        ((FETextureResource*)data)->m_glTextureHandle = hash;
    }
    FETextureResource* texture = (FETextureResource*)resource->data;
    if (EnsurePackTexture(hash)) texture->SetTextureHandle(hash);  // its size, which the image is drawn at
    return texture;
}

// ---- pictures in the art layers ----

namespace {

// The pack file for a character's picture named `art` in an art layer, or 0.
const char* ArtFile(const ModCharacter& m, unsigned long art) {
    char name[64];
    Join(name, sizeof(name), "captain_", m.name, "_s");
    if (art == nlStringLowerHash(name)) return m.portrait;
    Join(name, sizeof(name), "captain_", m.name, "_ds");
    if (art == nlStringLowerHash(name)) return m.portraitTaken;
    char texture[96];
    Join(name, sizeof(name), "positions_", m.name, 0);  // the left board's: "<name>_left"
    if (art == nlStringLowerHash(name)) Join(texture, sizeof(texture), "fe/screens/images/", m.name, "_left");
    else {
        // Otherwise a picture is named as its texture ("logos_TEAM_<name>", "<name>_right").
        texture[0] = 0;
        for (int i = 0; i < m.ownTextureCount; ++i) {
            const char* game = m.ownTextures[i].game;
            const char* base = game;
            for (const char* p = game; *p; ++p)
                if (*p == '/') base = p + 1;
            if (nlStringLowerHash(base) == art) return m.ownTextures[i].file;
        }
        return 0;
    }
    for (int i = 0; i < m.ownTextureCount; ++i)
        if (nlStringLowerHash(m.ownTextures[i].game) == nlStringLowerHash(texture)) return m.ownTextures[i].file;
    return 0;
}

unsigned char s_stand[16][0x100];  // TLImageInstance stand-ins: only their texture is read
int s_standCount;

void* StandIn(FETextureResource* resource) {
    for (int i = 0; i < s_standCount; ++i)
        if (At<FETextureResource*>(s_stand[i], kImageTexture) == resource) return s_stand[i];
    if (s_standCount == 16) return 0;
    unsigned char* stand = s_stand[s_standCount++];
    At<FETextureResource*>(stand, kImageTexture) = resource;
    At<unsigned char>(stand, 0x8E) = 1;  // visible
    return stand;
}

}  // namespace

// void* FEFindInstance(FEPresentation*, ulong level1..level6): an instance by name path. A
// character's picture the art layer lacks is a stand-in carrying its pack texture.
asm static void FindInstance_Original() {
    nofralloc
    cmpwi r3, 0
    lis r12, 0x8030
    ori r12, r12, 0x6780
    mtctr r12
    bctr
}

static void* FindInstance(void* presentation, unsigned long a, unsigned long b, unsigned long c, unsigned long d,
                          unsigned long e, unsigned long f) {
    void* found = ((void* (*)(void*, unsigned long, unsigned long, unsigned long, unsigned long, unsigned long,
                              unsigned long))FindInstance_Original)(presentation, a, b, c, d, e, f);
    if (found || d || a != nlStringLowerHash("art") || b != nlStringLowerHash("Layer")) return found;
    for (int i = 0; i < kModCharacterCount; ++i) {
        const char* file = ArtFile(kModCharacters[i], c);
        if (file == 0) continue;
        if (FETextureResource* resource = PackResource(file)) return StandIn(resource);
    }
    return found;
}
kmBranch(0x8030677C, FindInstance);

// ---- the results screen: its captain pictures ----

// void MatchSummary::DisplayMatchSummary(TeamStats home, TeamStats away, FEPresentation*): each team's
// picture is "captain_<x>_s" from a 12-name table by team. A mod character's team plays it as its
// base's, then gets its own picture.
asm static void DisplayMatchSummary_Original() {
    nofralloc
    stwu r1, -352(r1)
    lis r12, 0x8020
    ori r12, r12, 0x95D4
    mtctr r12
    bctr
}

static void DisplayMatchSummary_(unsigned char* summary, int* home, int* away, void* presentation) {
    int* teams[2] = {home, away};  // TeamStats::mTeamIndex
    int own[2];
    for (int s = 0; s < 2; ++s) {
        own[s] = *teams[s];
        const int c = ModCharacterOfTeam(own[s]);
        if (c >= 0) *teams[s] = kModCharacters[c].base;
    }
    ((void (*)(unsigned char*, int*, int*, void*))DisplayMatchSummary_Original)(summary, home, away, presentation);
    for (int s = 0; s < 2; ++s) {
        *teams[s] = own[s];
        At<int>(summary, 0xAC + s * 4) = own[s];  // mTeamIDs
        const int c = ModCharacterOfTeam(own[s]);
        if (c < 0) continue;
        FETextureResource* picture = PackResource(kModCharacters[c].portrait);
        void* layer = FEFindInstance((FEPresentation*)presentation, nlStringLowerHash("game summary"), nlStringLowerHash("Layer"),
                                     nlStringLowerHash("game summary"), 0, 0, 0);
        void* icon = layer ? FEFindInstanceRecursive((TLInstance*)layer, nlStringLowerHash(s == 0 ? "team_icon_left" : "team_icon_right"),
                                                     nlStringLowerHash("team_icon"), 0, 0, 0, 0)
                           : 0;
        if (icon && picture) At<FETextureResource*>(icon, kImageTexture) = picture;
    }
}
kmBranch(0x802095D0, DisplayMatchSummary_);

// ---- names ----

// void OnTableLoaded(LOCHeader*, ulong, nlLocalization*): a string table in, its lookup (sorted by
// key hash, searched by halves) set up in the file. A copy with each character's name key added
// replaces it.
asm static void OnTableLoaded_Original() {
    nofralloc
    stwu r1, -16(r1)
    lis r12, 0x8030
    ori r12, r12, 0x2D08
    mtctr r12
    bctr
}

static void OnTableLoaded(LOCHeader* header, unsigned long size, nlLocalization* table) {
    ((void (*)(LOCHeader*, unsigned long, nlLocalization*))OnTableLoaded_Original)(header, size, table);
    if (table == 0 || table->m_LookupTable == 0 || table->m_pFile == 0 || kModCharacterCount == 0) return;
    const unsigned long count = table->m_pFile->StringCount;
    nlLocalization::StringLookup* copy =
        (nlLocalization::StringLookup*)nlMalloc((count + kModCharacterCount) * sizeof(nlLocalization::StringLookup), 8, false);
    if (copy == 0) return;
    unsigned long n = 0;
    for (unsigned long i = 0; i < count; ++i) copy[n++] = table->m_LookupTable[i];
    for (int c = 0; c < kModCharacterCount; ++c) {
        const ModCharacter& m = kModCharacters[c];
        if (!m.displayName || !m.nameKey) continue;
        nlLocalization::StringLookup entry;
        entry.hash = nlStringLowerHash(m.nameKey);
        // The offset (in characters from the table's first string) reaching its name, wrapping.
        entry.StringOffset = ((unsigned long)m.displayName - (unsigned long)table->m_FirstString) / 2;
        unsigned long at = n;
        while (at > 0 && copy[at - 1].hash > entry.hash) {
            copy[at] = copy[at - 1];
            --at;
        }
        copy[at] = entry;
        ++n;
    }
    table->m_LookupTable = copy;  // the file's own stays in the file, which is what the game frees
    table->m_pFile->StringCount = n;
}
kmBranch(0x80302D04, OnTableLoaded);

// ---- slides named after a character ----
// Screens show a character's name and pictures by switching a component to a slide named after it
// (the loading screen's "HOME NAMES", the captain panels). A mod character has none: the component
// takes its base's slide, dressed in the character's name and textures.

namespace {

int CharacterNamed(const char* name) {
    for (int c = 0; name && c < kModCharacterCount; ++c)
        if (stricmp(name, kModCharacters[c].name) == 0) return c;
    return -1;
}

// The character's file for a texture its base has under its own name ("fe/screens/images/
// attributes_waluigi" for "..._superteam"), or 0.
const char* FileForBaseTexture(int c, unsigned long handle) {
    const ModCharacter& m = kModCharacters[c];
    const char* base = BaseName(c);
    for (int i = 0; i < m.ownTextureCount && base; ++i) {
        const char* game = m.ownTextures[i].game;
        char swapped[128];
        unsigned long n = 0;
        for (const char* p = game; *p && n + 1 < sizeof(swapped);) {
            int k = 0;
            while (m.name[k] && p[k] == m.name[k]) ++k;
            if (m.name[k] == 0) {
                for (const char* b = base; *b && n + 1 < sizeof(swapped); ++b) swapped[n++] = *b;
                p += k;
            } else {
                swapped[n++] = *p++;
            }
        }
        swapped[n] = 0;
        if (nlStringLowerHash(swapped) == handle) return m.ownTextures[i].file;
    }
    return 0;
}

// A component showing its base's slide for a character. The slide's instances are shared with every
// other component showing it (both captain panels), so they wear the character's name and textures
// only while this component draws, and their own again after.
struct Dressed {
    TLComponent* component;
    int c;
    unsigned long baseKey;  // the base's name key, as the slide's text names it
};
// Screens come and go without saying so: when the list is full, the oldest record (of a component
// likely gone with its screen) makes room.
const int kMaxDressed = 16;
Dressed s_dressed[kMaxDressed];
int s_oldest;

Dressed* DressedFor(TLComponent* component) {
    for (int i = 0; i < kMaxDressed; ++i)
        if (s_dressed[i].component == component) return &s_dressed[i];
    return 0;
}

Dressed* NewDressed() {
    if (Dressed* free = DressedFor(0)) return free;
    Dressed* oldest = &s_dressed[s_oldest];
    s_oldest = (s_oldest + 1) % kMaxDressed;
    return oldest;
}

struct Change {
    unsigned long* field;
    unsigned long value;
};

struct Changes {
    Change list[48];
    int count;
    void Set(unsigned long* field, unsigned long value) {
        if (count == 48 || *field == value) return;
        list[count].field = field;
        list[count].value = *field;
        ++count;
        *field = value;
    }
    void Undo() {
        while (count > 0) {
            --count;
            *list[count].field = list[count].value;
        }
    }
};

void Wear(TLInstance* list, const Dressed& d, Changes& changes, int depth);

void WearInstance(TLInstance* instance, const Dressed& d, Changes& changes, int depth) {
    const int type = At<int>(instance, 0x88);
    if (type == TLAT_TEXT) {
        if (At<unsigned long>(instance, 0x90) == d.baseKey) {  // m_LocStrId, and its override flag
            changes.Set(&At<unsigned long>(instance, 0x90), nlStringLowerHash(kModCharacters[d.c].nameKey));
            changes.Set(&At<unsigned long>(instance, 0xA0), At<unsigned long>(instance, 0xA0) | 8);
        }
    } else if (type == TLAT_IMAGE) {
        FETextureResource* resource = At<FETextureResource*>(instance, kImageTexture);
        const char* file = resource ? FileForBaseTexture(d.c, resource->m_glTextureHandle) : 0;
        if (file)
            if (FETextureResource* own = PackResource(file))
                changes.Set(&At<unsigned long>(instance, kImageTexture), (unsigned long)own);
    } else if (type == TLAT_COMPONENT && depth < 6) {  // a component in the slide: what it shows now
        TLComponent* component = At<TLComponent*>(instance, 0x0C);
        TLSlide* slide = component ? At<TLSlide*>(component, 0x7C) : 0;
        if (slide) Wear(slide->pChildren, d, changes, depth + 1);
    }
    if (depth < 6) Wear(At<TLInstance*>(instance, 0x08), d, changes, depth + 1);
}

// Every instance of a ring (its list pointer is its last entry).
void Wear(TLInstance* list, const Dressed& d, Changes& changes, int depth) {
    if (list == 0) return;
    TLInstance* instance = list->m_next;
    for (int guard = 0; instance && guard < 256; ++guard) {
        WearInstance(instance, d, changes, depth);
        if (instance == list) break;
        instance = instance->m_next;
    }
}

unsigned long BaseKey(int c) {
    for (int i = 0; i < 12; ++i)
        if (stricmp(At<const char*>(sCharacterInfoRow(i), 0x04), BaseName(c)) == 0)
            return nlStringLowerHash(At<const char*>(sCharacterInfoRow(i), 0x08));
    return 0;
}

}  // namespace

// void TLComponent::SetActiveSlide(const char* name, bool restart, bool keepTime): a mod character's
// name, which no component has a slide of, takes its base's slide (worn as the character's).
asm static void SetActiveSlide_Original() {
    nofralloc
    stwu r1, -32(r1)
    lis r12, 0x8030
    ori r12, r12, 0x1DA4
    mtctr r12
    bctr
}

static void SetActiveSlide(TLComponent* component, const char* name, bool restart, bool keepTime) {
    const int c = CharacterNamed(name);
    Dressed* dressed = DressedFor(component);
    if (c < 0 || BaseName(c) == 0 || FindItemByHashID(At<TLSlide*>(component, 0x78), nlStringLowerHash(name))) {
        if (dressed) dressed->component = 0;
        ((void (*)(TLComponent*, const char*, bool, bool))SetActiveSlide_Original)(component, name, restart, keepTime);
        return;
    }
    ((void (*)(TLComponent*, const char*, bool, bool))SetActiveSlide_Original)(component, BaseName(c), restart, keepTime);
    if (dressed == 0) dressed = NewDressed();
    if (dressed) {
        dressed->component = component;
        dressed->c = c;
        dressed->baseKey = BaseKey(c);
    }
}
kmBranch(0x80301DA0, SetActiveSlide);

// void FERender::RenderTimeLineAsset(TLInstance*, float time, const nlMatrix4& parent): draws an
// instance and what's in it. A component wearing a character's slide wears it while drawn.
asm static void RenderTimeLineAsset_Original() {
    nofralloc
    stwu r1, -288(r1)
    lis r12, 0x802F
    ori r12, r12, 0xBF98
    mtctr r12
    bctr
}

static void RenderTimeLineAsset(TLInstance* instance, float time, const void* parent) {
    typedef void (*Fn)(TLInstance*, float, const void*);
    Dressed* dressed = 0;
    if (instance && At<int>(instance, 0x88) == TLAT_COMPONENT) {
        TLComponent* component = At<TLComponent*>(instance, 0x0C);
        if (component) dressed = DressedFor(component);
    }
    if (dressed == 0) {
        ((Fn)RenderTimeLineAsset_Original)(instance, time, parent);
        return;
    }
    Changes changes;
    changes.count = 0;
    TLSlide* slide = At<TLSlide*>(dressed->component, 0x7C);
    if (slide) Wear(slide->pChildren, *dressed, changes, 0);
    ((Fn)RenderTimeLineAsset_Original)(instance, time, parent);
    changes.Undo();
}
kmBranch(0x802FBF94, RenderTimeLineAsset);
