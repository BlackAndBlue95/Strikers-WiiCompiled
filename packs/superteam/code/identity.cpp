// A mod character's own textures, by the names the game asks for them under (its HUD icon, logos,
// menu art, its Striker Times photos), read from the pack into the framework's own memory; its voice
// bank (its own sound bank number, loading its bank file); and the bundle files a mod character's
// name makes that the game's bundles lack (its base's instead).
#include <kamek.h>

#include "Game/FE/feAsyncImage.h"
#include "Game/FE/feResourceManager.h"
#include "NL/nlFile.h"
#include "NL/nlMemory.h"
#include "NL/gl/glTexture.h"

#include "framework.h"

extern "C" void* memcpy(void* dst, const void* src, unsigned long n);
bool nlFileExists(const char* path);
extern unsigned char sCharacterInfo[];  // CharacterInfo[33], 0x5C each

class AudioResourceLoadOwner;
class MemoryAllocator;
void LoadAudioResource(AudioResourceLoadOwner* owner, const char* name,
                       void (*callback)(AudioResourceLoadOwner*, void*), void* param, MemoryAllocator* allocator);

namespace {

template <typename T> inline T& At(const void* base, unsigned long offset) {
    return *(T*)((char*)base + offset);
}

const unsigned long kRowSize = 0x5C, kRowVoiceBank = 0x1C;  // CharacterInfo


// ---- the pack's textures ----

struct Texture {
    unsigned long hash;
    unsigned long size;
    unsigned char* data;
};

unsigned char s_memory[0x30000];
unsigned long s_memoryUsed;
Texture s_textures[32];
int s_textureCount;

unsigned char* Memory(unsigned long size) {
    const unsigned long start = (((unsigned long)s_memory + s_memoryUsed + 31) & ~31UL) - (unsigned long)s_memory;
    if (start + size > sizeof(s_memory)) return 0;
    s_memoryUsed = start + size;
    return s_memory + start;
}

Texture* FindTexture(unsigned long hash) {
    for (int i = 0; i < s_textureCount; ++i)
        if (s_textures[i].hash == hash) return &s_textures[i];
    return 0;
}

}  // namespace

unsigned long PackTexture(const char* path) {
    if (path == 0) return 0;
    const unsigned long hash = nlStringLowerHash(path);
    if (FindTexture(hash)) return hash;
    if (s_textureCount == 32 || !nlFileExists(path)) return 0;
    // The file's size first, from a read into a scratch buffer of the largest size there's room for.
    unsigned long room = sizeof(s_memory) - s_memoryUsed;
    unsigned char* data = room > 64 ? Memory(room - 64) : 0;
    if (data == 0) return 0;
    unsigned long size = 0;
    if (nlLoadEntireFile(path, &size, 32, AllocateEnd, data, room - 64, 0) == 0 || size <= 0x20) {
        s_memoryUsed = data - s_memory;  // give it back
        return 0;
    }
    s_memoryUsed = (data - s_memory) + size;
    Texture& texture = s_textures[s_textureCount++];
    texture.hash = hash;
    texture.size = size;
    texture.data = data;
    return hash;
}

bool EnsurePackTexture(unsigned long hash) {
    Texture* texture = FindTexture(hash);
    if (texture == 0) return false;
    if (!glTextureLoad(hash) && FEResourceManager::s_pInstance)
        glTextureAdd(hash, texture->data, texture->size, FEResourceManager::s_pInstance->GetResourcePool());
    return glTextureLoad(hash);
}

// ---- the character's textures, by the names the game asks for them under ----

namespace {

struct Swap {
    unsigned long game;
    unsigned long pack;
};
const int kMaxSwaps = 32;
Swap s_swaps[kMaxSwaps];
int s_swapCount = -1;  // not yet read

void LoadSwaps() {
    if (s_swapCount >= 0) return;
    s_swapCount = 0;
    for (int c = 0; c < kModCharacterCount; ++c) {
        const ModCharacter& m = kModCharacters[c];
        for (int i = 0; i < m.ownTextureCount && s_swapCount < kMaxSwaps; ++i) {
            const unsigned long pack = PackTexture(m.ownTextures[i].file);
            if (pack == 0) continue;
            s_swaps[s_swapCount].game = nlStringLowerHash(m.ownTextures[i].game);
            s_swaps[s_swapCount].pack = pack;
            ++s_swapCount;
        }
    }
}

unsigned long SwapFor(unsigned long game) {
    for (int i = 0; i < s_swapCount; ++i)
        if (s_swaps[i].game == game) return s_swaps[i].pack;
    return 0;
}

int s_active = -1;

}  // namespace

void LoadPackTextures() {
    LoadSwaps();
}

void SetActiveCharacter(int c) {
    s_active = c;
}

int ActiveCharacter() {
    return s_active;
}

const char* BaseName(int c) {
    return c >= 0 && c < kModCharacterCount ? At<const char*>(sCharacterInfo + kModCharacters[c].base * kRowSize, 0x04) : 0;
}

int BaseVoiceBank(int c) {
    return c >= 0 && c < kModCharacterCount ? At<int>(ModCharacterRow(c), kRowVoiceBank) : -1;
}

static void* s_bankTable;

void* VoiceBankTable() {
    return s_bankTable;
}

// ---- hooks ----
// Each replaces the first instruction of a game function (a stwu, which runs the same anywhere) with
// a branch to the hook; the hook calls the original through a trampoline: that instruction, then a
// jump to the function's second instruction.

// void AudioBankTable::LoadBank(int bank, ulong slot, callback, param, allocator): the base's voice
// bank loads the character's. As the original otherwise; the record the slot keeps names the bank.
struct BankRecord {
    unsigned long unknown;
    const char* name;
};
static BankRecord s_bankRecord;

asm static void LoadBank_Original() {
    nofralloc
    stwu r1, -32(r1)
    lis r12, 0x802E
    ori r12, r12, 0xBBF4
    mtctr r12
    bctr
}

static void LoadBank(void* table, int bank, unsigned long slot, void* callback, void* param, void* allocator) {
    s_bankTable = table;
    const int c = bank - kFirstModBank;
    if (c >= 0 && c < kModCharacterCount && kModCharacters[c].voiceBank) {
        s_bankRecord.unknown = 0;
        s_bankRecord.name = kModCharacters[c].voiceBank;
        unsigned char* source = At<unsigned char*>(table, 0x0C) + slot * 0x18;  // AudioResourceSource
        At<BankRecord*>(source, 0x08) = &s_bankRecord;
        At<unsigned char>(source, 0x14) = 1;
        LoadAudioResource(At<AudioResourceLoadOwner*>(source, 0x10), s_bankRecord.name,
                          (void (*)(AudioResourceLoadOwner*, void*))callback, param, (MemoryAllocator*)allocator);
        return;
    }
    ((void (*)(void*, int, unsigned long, void*, void*, void*))LoadBank_Original)(table, bank, slot, callback, param, allocator);
}
kmBranch(0x802EBBF0, LoadBank);

// PlatTexture* glx_GetTex(ulong handle): a texture by name hash. The base's textures the character
// has its own of are the character's.
asm static void GetTex_Original() {
    nofralloc
    stwu r1, -48(r1)
    lis r12, 0x802D
    ori r12, r12, 0x0650
    mtctr r12
    bctr
}

static bool s_inGetTex;  // EnsurePackTexture asks for the texture itself: not again from there

static void* GetTex(unsigned long handle) {
    if (!s_inGetTex) {
        s_inGetTex = true;
        const unsigned long pack = SwapFor(handle);
        if (pack && EnsurePackTexture(pack)) handle = pack;
        else if (FindTexture(handle)) EnsurePackTexture(handle);  // a pack texture itself: back in the pool if it fell out
        s_inGetTex = false;
    }
    return ((void* (*)(unsigned long))GetTex_Original)(handle);
}
kmBranch(0x802D064C, GetTex);

// bool AsyncImage::Update(bool autoswap): an image streamed from a front-end bundle by name (the
// Striker Times' photos), into a texture of the image's own. One of the base's the character has its
// own of is the character's, from the pack: handed over as if read, and the bundle isn't asked.
asm static void UpdateImage_Original() {
    nofralloc
    stwu r1, -48(r1)
    lis r12, 0x801B
    ori r12, r12, 0xF21C
    mtctr r12
    bctr
}

// A name the bundle lacks that a mod character's name made ("fe/sidekick_images/position_boo_superteam"):
// its base's instead, as the game would crash reading a missing entry.
static void FallBackToBase(AsyncImage* image) {
    unsigned char* bundle = (unsigned char*)image->mBundleFile;
    if (bundle == 0) return;
    const unsigned long hash = nlStringLowerHash(image->mLoadPath);
    const unsigned long count = At<unsigned long>(bundle, 0x04);
    const unsigned char* directory = At<const unsigned char*>(bundle, 0x20);  // {hash, block, length}
    for (unsigned long i = 0; directory && i < count; ++i)
        if (At<unsigned long>(directory, i * 12) == hash) return;
    for (int c = 0; c < kModCharacterCount; ++c) {
        const char* name = kModCharacters[c].name;
        const char* base = BaseName(c);
        char* at = image->mLoadPath;
        for (; *at; ++at) {
            int n = 0;
            while (name[n] && at[n] == name[n]) ++n;
            if (name[n] == 0) break;
        }
        if (*at == 0 || base == 0) continue;
        char rest[0x80];
        int n = 0, r = 0, len = 0;
        while (name[len]) ++len;
        for (const char* t = at + len; *t && r < 0x7F; ++t) rest[r++] = *t;
        rest[r] = 0;
        char* out = at;
        for (n = 0; base[n] && (out - image->mLoadPath) < 0x7E; ++n) *out++ = base[n];
        for (n = 0; rest[n] && (out - image->mLoadPath) < 0x7F; ++n) *out++ = rest[n];
        *out = 0;
        return;
    }
}

static bool UpdateImage(AsyncImage* image, bool autoswap) {
    if (image->mLoadState == LS_READY_TO_LOAD) {
        LoadSwaps();
        const unsigned long hash = nlStringLowerHash(image->mLoadPath);
        for (int i = 0; i < s_swapCount; ++i) {
            if (s_swaps[i].game != hash) continue;
            Texture* texture = FindTexture(s_swaps[i].pack);
            // The read buffer: the game's (it frees it once the image has it), new or big enough.
            if (texture && (image->m_loadBuffer == 0 || image->mTextureSize >= texture->size)) {
                if (image->m_loadBuffer == 0) image->m_loadBuffer = nlMalloc(texture->size, 32, true);
                if (image->m_loadBuffer) {
                    memcpy(image->m_loadBuffer, texture->data, texture->size);
                    image->mTextureSize = texture->size;
                    image->mLoadState = LS_LOAD_COMPLETE;
                }
            }
            break;
        }
        if (image->mLoadState == LS_READY_TO_LOAD) FallBackToBase(image);
    }
    return ((bool (*)(AsyncImage*, bool))UpdateImage_Original)(image, autoswap);
}
kmBranch(0x801BF218, UpdateImage);


