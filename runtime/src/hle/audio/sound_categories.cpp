// A volume per kind of sound (F10 > Audio: music, sound effects, voices, menus, cutscenes), on top of
// the game's own Music / SFX / Voice options.
//
// Every sound's volume, in dB, comes from SoundInstance::GetVolume: its voice's volume plus the value
// of its calculation slider (AudioCalculationTable, from the audio bundle). Those sliders form a
// tree, and the game's Options sliders are three of its nodes: music (2), voices (3) and sound
// effects (4), whose targets AudioSettings::ApplyMusicVolume and co. set. A sound's category is the
// node its slider's chain of parents reaches. Menu sounds and cutscenes aren't nodes of the tree, so
// their handles are tagged as CreateSoundHandle makes them: FEAudio names its sounds "FESFX", the
// cutscene player its main cue "Nis Cue", and a cutscene's timed sounds are played from
// Nis::Trigger::Fire. Movies (THP) count as cutscenes.

#include "hle_stubs.h"
#include "hle/msc_guest.h"
#include "memory.h"
#include "ppc_runtime.h"
#include "runtime_config.h"
#include "runtime_log.h"

#include <cmath>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace {

constexpr uint32_t kAudioSystem = 0x806E201Cu;  // g_pAudioSystem
constexpr uint32_t kBundleManager = 0xCC;       // AudioSystem::m_BundleManager
constexpr uint32_t kCalculationTable = 0x10;    // AudioBundleManager::m_Chunk13400
// AudioCalculationTable: count, definitions, sliders. A definition (0x18 bytes) has its parent's
// index behind a pointer at +0x0C, 0 at the root.
constexpr uint32_t kDefinitions = 0x04, kDefinitionSize = 0x18, kDefinitionParent = 0x0C;
constexpr uint32_t kMusicSlider = 2, kVoicesSlider = 3, kEffectsSlider = 4;
// SoundInstance: its handle (XSoundCueHandle) and voice (AudioVoiceDefinition, slider index at +0x0C).
constexpr uint32_t kInstanceHandle = 0x00, kInstanceVoice = 0x04, kVoiceSliderIndex = 0x0C;

constexpr int kNoCategory = -1, kNotLookedUp = -2;

uint32_t g_table = 0;
std::vector<int> g_sliderCategories;                         // per calculation slider
std::unordered_map<uint32_t, SoundCategory> g_taggedHandles;  // menu and cutscene sounds
// The same sounds by slot, cue and context: a tracked sound paused with the game is made again on
// resume under another name ("resumed cue"), and keeps its category.
std::unordered_map<uint64_t, SoundCategory> g_taggedCues;
int g_cutsceneTriggers = 0;  // inside Nis::Trigger::Fire

const char* CategoryName(int category) {
    static const char* const kNames[] = {"music", "sound effects", "voices", "menus", "cutscenes"};
    return category >= 0 && category < 5 ? kNames[category] : "none";
}

int SliderCategory(uint32_t slider) {
    const uint32_t system = Memory::Read32(kAudioSystem);
    const uint32_t bundle = system ? Memory::Read32(system + kBundleManager) : 0;
    const uint32_t table = bundle ? Memory::Read32(bundle + kCalculationTable) : 0;
    if (table == 0) return kNoCategory;
    const uint32_t count = Memory::Read32(table);
    if (table != g_table || g_sliderCategories.size() != count) {
        g_table = table;
        g_sliderCategories.assign(count, kNotLookedUp);
    }
    if (slider >= count) return kNoCategory;
    int& category = g_sliderCategories[slider];
    if (category != kNotLookedUp) return category;
    category = kNoCategory;
    const uint32_t definitions = Memory::Read32(table + kDefinitions);
    std::string chain;
    for (uint32_t index = slider, depth = 0; index < count && depth < 16; ++depth) {
        chain += (depth ? " < " : "") + std::to_string(index);
        if (index == kMusicSlider || index == kVoicesSlider || index == kEffectsSlider) {
            category = static_cast<int>(index == kMusicSlider    ? SoundCategory::Music
                                        : index == kVoicesSlider ? SoundCategory::Voices
                                                                 : SoundCategory::Effects);
            break;
        }
        const uint32_t parent = Memory::Read32(definitions + index * kDefinitionSize + kDefinitionParent);
        if (parent == 0) break;
        index = Memory::Read32(parent);
    }
    RT_LOG(RT_TAG_AUDIO) << "sound slider " << chain << ": " << CategoryName(category) << std::endl;
    return category;
}

} // namespace

// float SoundInstance::GetVolume(). As the original (the voice's volume + its slider + the
// instance's offset, in dB), plus the F10 volume of the sound's category. The mixer clamps a
// playback's volume to -96 dB, so 0% is silence.
extern "C" void func_802F29F8(CpuContext* ctx);
static void SoundCategoryVolume(CpuContext* ctx)
{
    const uint32_t instance = ctx->gpr[3];
    func_802F29F8(ctx);
    if (instance == 0) return;
    int category = kNoCategory;
    if (const auto it = g_taggedHandles.find(Memory::Read32(instance + kInstanceHandle)); it != g_taggedHandles.end()) {
        category = static_cast<int>(it->second);
    } else if (const uint32_t voice = Memory::Read32(instance + kInstanceVoice)) {
        category = SliderCategory(Memory::Read32(voice + kVoiceSliderIndex));
    }
    if (category == kNoCategory) return;
    const float volume = RuntimeConfigFile::SoundCategoryVolume(static_cast<SoundCategory>(category));
    if (volume < 1.0f) ctx->fpr[1].d += volume > 0.0f ? 20.0 * std::log10(volume) : -96.0;
}
PPC_NATIVE_WRAP(802F29F8, SoundCategoryVolume);

// XSoundHandle* CreateSoundHandle(int slot, ulong cue, XSoundOwner*, const void* debugName,
// void* context, bool findExisting): menu sounds and cutscenes are tagged.
extern "C" void func_800EBD00(CpuContext* ctx);
static void TagSoundHandle(CpuContext* ctx)
{
    const uint64_t cue = (static_cast<uint64_t>(ctx->gpr[3] & 0xFF) << 56) ^ (static_cast<uint64_t>(ctx->gpr[4]) << 24) ^ ctx->gpr[7];
    const std::string name = ctx->gpr[6] != 0 ? MscGuest::CString(ctx->gpr[6], 16) : std::string();
    func_800EBD00(ctx);
    const uint32_t handle = ctx->gpr[3];
    if (handle == 0) return;
    if (g_cutsceneTriggers > 0 || name == "Nis Cue") {
        g_taggedHandles[handle] = g_taggedCues[cue] = SoundCategory::Cutscenes;
    } else if (name == "FESFX") {
        g_taggedHandles[handle] = g_taggedCues[cue] = SoundCategory::Menus;
    } else if (const auto it = g_taggedCues.find(cue); it != g_taggedCues.end()) {
        g_taggedHandles[handle] = it->second;
    } else {
        g_taggedHandles.erase(handle);  // a handle from the pool, tagged when it last played
    }
}
PPC_NATIVE_WRAP(800EBD00, TagSoundHandle);

// XSoundCueHandle::~XSoundCueHandle(): the handle goes back to its pool.
extern "C" void func_802F194C(CpuContext* ctx);
static void UntagSoundHandle(CpuContext* ctx)
{
    g_taggedHandles.erase(ctx->gpr[3]);
    func_802F194C(ctx);
}
PPC_NATIVE_WRAP(802F194C, UntagSoundHandle);

// void Nis::Trigger::Fire(Nis&) const: a cutscene's timed events, its sounds among them.
extern "C" void func_80282BE8(CpuContext* ctx);
static void CutsceneTrigger(CpuContext* ctx)
{
    struct Depth {
        Depth() { ++g_cutsceneTriggers; }
        ~Depth() { --g_cutsceneTriggers; }
    } depth;
    func_80282BE8(ctx);
}
PPC_NATIVE_WRAP(80282BE8, CutsceneTrigger);

// void THPSimpleSetVolume(int volume, int fadeMilliseconds): a movie's volume (0-127), set as it
// starts. Movies count as cutscenes.
extern "C" void func_80372848(CpuContext* ctx);
static void MovieVolume(CpuContext* ctx)
{
    const float volume = RuntimeConfigFile::SoundCategoryVolume(SoundCategory::Cutscenes);
    ctx->gpr[3] = static_cast<uint32_t>(std::lround(static_cast<double>(static_cast<int32_t>(ctx->gpr[3])) * volume));
    func_80372848(ctx);
}
PPC_NATIVE_WRAP(80372848, MovieVolume);
