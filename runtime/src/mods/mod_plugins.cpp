// Native plugins of mod packages: loading, the C API table (sdk/include/msc_mod_api.h), hook chains
// on wrap points, frame events and plugin settings.
#include "mods/mod_plugins.h"

#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>
#include "guest_interrupt_context.h"
#include "hle/msc_guest.h"
#include "memory.h"
#include "mods/mod_registry.h"
#include "msc_mod_api.h"
#include "ppc_runtime.h"
#include "recomp_mod_loader.h"
#include "runtime_config.h"
#include "runtime_log.h"

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <dlfcn.h>
#endif

// The opaque handle the C API hands to a plugin.
struct MscMod {
    const Mods::Package* package = nullptr;
    std::string directory;  // UTF-8, kept alive for mod_directory()
    Mods::PluginStatus status;
};

extern "C" void GxNotifyGuestRamDmaWrite(uint32_t addr, uint32_t size);  // hle/gx

namespace Mods {
namespace {

CpuContext* Ctx(MscCpu* cpu) { return reinterpret_cast<CpuContext*>(cpu); }
MscCpu* Cpu(CpuContext* ctx) { return reinterpret_cast<MscCpu*>(ctx); }

std::vector<std::unique_ptr<MscMod>> g_mods;  // in load order
bool g_initPhase = false;                     // hooks and events may be registered

// --- Wrap points and hook chains --------------------------------------------------------------

struct Hook {
    MscHookFn fn;
    void* user;
    MscMod* mod;
};

std::map<uint32_t, const char*>& WrapPoints() {  // filled by static registrars, before main
    static std::map<uint32_t, const char*> points;
    return points;
}
std::unordered_map<uint32_t, std::vector<Hook>> g_hooks;  // read-only once the game runs

// The hook chain being run on this thread (innermost first).
struct ChainFrame {
    const std::vector<Hook>* hooks;
    Hooks::Inner inner;
    size_t current;  // index of the hook running now
    ChainFrame* previous;
};
thread_local ChainFrame* t_chain = nullptr;

void Invoke(ChainFrame* frame, size_t index, CpuContext* ctx) {
    if (index < frame->hooks->size()) {
        const size_t saved = frame->current;
        frame->current = index;
        const Hook& hook = (*frame->hooks)[index];
        hook.fn(Cpu(ctx), hook.user);
        frame->current = saved;
    } else {
        frame->inner(ctx);
    }
}

// --- Events --------------------------------------------------------------------------------

struct Subscriber {
    MscEventFn fn;
    void* user;
    MscMod* mod;
};
std::vector<Subscriber> g_frameSubscribers;

// --- The C API ---------------------------------------------------------------------------------

void ReportBadAccess(const char* what, uint32_t address) {
    static int reported = 0;
    if (reported++ < 32) RT_LOGF(RT_TAG_MODS, "plugin %s at invalid address 0x%08X\n", what, address);
}

template <typename T, typename F>
T Guarded(const char* what, uint32_t address, F&& body, T fallback = T{}) {
    try {
        return body();
    } catch (const Memory::AccessViolation&) {
        ReportBadAccess(what, address);
        return fallback;
    }
}

void ApiLog(MscMod* mod, MscLogLevel level, const char* message) {
    const char* kind = level == MSC_LOG_ERROR ? "error: " : level == MSC_LOG_WARNING ? "warning: " : "";
    RT_LOG(RT_TAG_MODS) << (mod && mod->package ? mod->package->id : std::string("?")) << ": " << kind
                        << (message ? message : "") << std::endl;
}

int ApiIsValid(uint32_t address, uint32_t length) { return Memory::Contains(address, length) ? 1 : 0; }
uint8_t ApiRead8(uint32_t a) { return Guarded<uint8_t>("read8", a, [&] { return Memory::Read8(a); }); }
uint16_t ApiRead16(uint32_t a) { return Guarded<uint16_t>("read16", a, [&] { return Memory::Read16(a); }); }
uint32_t ApiRead32(uint32_t a) { return Guarded<uint32_t>("read32", a, [&] { return Memory::Read32(a); }); }
float ApiReadF32(uint32_t a) { return Guarded<float>("read_f32", a, [&] { return Memory::ReadFloat32(a); }); }
void ApiWrite8(uint32_t a, uint8_t v) { Guarded<int>("write8", a, [&] { Memory::Write8(a, v); return 0; }); }
void ApiWrite16(uint32_t a, uint16_t v) { Guarded<int>("write16", a, [&] { Memory::Write16(a, v); return 0; }); }
void ApiWrite32(uint32_t a, uint32_t v) { Guarded<int>("write32", a, [&] { Memory::Write32(a, v); return 0; }); }
void ApiWriteF32(uint32_t a, float v) { Guarded<int>("write_f32", a, [&] { Memory::WriteFloat32(a, v); return 0; }); }
void ApiReadBytes(uint32_t a, void* out, uint32_t n) {
    if (const uint8_t* p = Memory::Contains(a, n) ? Memory::GetPointer(a, n) : nullptr) std::memcpy(out, p, n);
    else { std::memset(out, 0, n); ReportBadAccess("read_bytes", a); }
}
void ApiWriteBytes(uint32_t a, const void* data, uint32_t n) {
    uint8_t* p = Memory::Contains(a, n) ? Memory::GetPointer(a, n) : nullptr;
    if (!p) {
        ReportBadAccess("write_bytes", a);
        return;
    }
    // The scalar writes' executable-memory rule, then the renderer learns the bytes changed, so a
    // texture or display list a plugin rewrote isn't drawn from a stale copy.
    RecompMod::CheckExecutableWrite(a, n, 0);
    std::memcpy(p, data, n);
    GxNotifyGuestRamDmaWrite(a, n);
}
uint32_t ApiAlloc(MscCpu* cpu, uint32_t size, uint32_t alignment) {
    if (cpu == nullptr || g_initPhase) return 0;
    return Guarded<uint32_t>("alloc", 0, [&] { return MscGuest::Alloc(Ctx(cpu), size, alignment ? alignment : 4); });
}
uint32_t ApiGetGpr(MscCpu* cpu, int i) { return cpu && i >= 0 && i < 32 ? Ctx(cpu)->gpr[i] : 0; }
void ApiSetGpr(MscCpu* cpu, int i, uint32_t v) { if (cpu && i >= 0 && i < 32) Ctx(cpu)->gpr[i] = v; }
double ApiGetFpr(MscCpu* cpu, int i) { return cpu && i >= 0 && i < 32 ? Ctx(cpu)->fpr[i].d : 0.0; }
void ApiSetFpr(MscCpu* cpu, int i, double v) { if (cpu && i >= 0 && i < 32) Ctx(cpu)->fpr[i].d = v; }
uint32_t ApiCall(MscCpu* cpu, uint32_t address, const uint32_t* args, int count) {
    if (cpu == nullptr || g_initPhase || count < 0 || count > 8) return 0;
    CpuContext* ctx = Ctx(cpu);
    for (int i = 0; i < count; ++i) ctx->gpr[3 + i] = args[i];
    const uint32_t savedLr = ctx->lr;
    InvokeIndirectCpu(address, ctx);
    ctx->lr = savedLr;
    return ctx->gpr[3];
}
int ApiIsHookable(uint32_t address) { return Hooks::IsWrapPoint(address) ? 1 : 0; }
int ApiHook(MscMod* mod, uint32_t address, MscHookFn fn, void* user) {
    if (!g_initPhase || mod == nullptr || fn == nullptr || !Hooks::IsWrapPoint(address)) return -1;
    auto& chain = g_hooks[address];
    chain.insert(chain.begin(), Hook{fn, user, mod});  // later mods run first
    return 0;
}
void ApiCallOriginal(MscCpu* cpu) {
    if (t_chain == nullptr || cpu == nullptr) {
        RT_LOG(RT_TAG_MODS) << "call_original outside a hook (ignored)" << std::endl;
        return;
    }
    Invoke(t_chain, t_chain->current + 1, Ctx(cpu));
}
int ApiSubscribe(MscMod* mod, MscEvent event, MscEventFn fn, void* user) {
    if (!g_initPhase || mod == nullptr || fn == nullptr || event != MSC_EVENT_FRAME) return -1;
    g_frameSubscribers.push_back(Subscriber{fn, user, mod});
    return 0;
}
void ApiDeclareBool(MscMod* mod, const char* key, const char* label, const char* help, int defaultValue) {
    if (mod == nullptr || key == nullptr || *key == 0) return;
    for (const PluginSetting& s : mod->status.settings)
        if (s.key == key) return;
    mod->status.settings.push_back(PluginSetting{key, label ? label : key, help ? help : "", defaultValue != 0});
}
int ApiGetBool(MscMod* mod, const char* key) {
    if (mod == nullptr || key == nullptr) return 0;
    for (const PluginSetting& s : mod->status.settings)
        if (s.key == key) return PluginSettingValue(mod->package->id, s) ? 1 : 0;
    return RuntimeConfigFile::ModSetting(mod->package->id, key).value_or(false) ? 1 : 0;
}
const char* ApiModId(MscMod* mod) { return mod && mod->package ? mod->package->id.c_str() : ""; }
const char* ApiModDirectory(MscMod* mod) { return mod ? mod->directory.c_str() : ""; }

const MscModApi kApi = {
    MSC_MOD_API_VERSION, sizeof(MscModApi), ApiLog, ApiIsValid, ApiRead8, ApiRead16, ApiRead32, ApiReadF32, ApiWrite8,
    ApiWrite16, ApiWrite32, ApiWriteF32, ApiReadBytes, ApiWriteBytes, ApiAlloc, ApiGetGpr, ApiSetGpr, ApiGetFpr,
    ApiSetFpr, ApiCall, ApiIsHookable, ApiHook, ApiCallOriginal, ApiSubscribe, ApiDeclareBool, ApiGetBool, ApiModId,
    ApiModDirectory,
};

void* OpenLibrary(const std::filesystem::path& path, std::string& error) {
#if defined(_WIN32)
    HMODULE module = LoadLibraryW(path.wstring().c_str());
    if (!module) error = "LoadLibrary failed (error " + std::to_string(GetLastError()) + ")";
    return reinterpret_cast<void*>(module);
#else
    void* handle = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (!handle) error = dlerror();
    return handle;
#endif
}

void* FindSymbol(void* library, const char* name) {
#if defined(_WIN32)
    return reinterpret_cast<void*>(GetProcAddress(reinterpret_cast<HMODULE>(library), name));
#else
    return dlsym(library, name);
#endif
}

}  // namespace

void LoadPlugins() {
    for (const Package* package : ActivePackages()) {
        if (!package->HasPlugin()) continue;
        auto mod = std::make_unique<MscMod>();
        mod->package = package;
        mod->directory = RuntimeConfigFile::PathToUtf8(package->root);
        if (!package->pluginSetting) {
            mod->status.error = "native code not allowed (F10 > Mods)";
            g_mods.push_back(std::move(mod));
            continue;
        }
        std::string error;
        void* library = OpenLibrary(package->pluginLibrary, error);
        auto init = library ? reinterpret_cast<MscModInitFn>(FindSymbol(library, MSC_MOD_INIT_SYMBOL)) : nullptr;
        if (library && !init) error = std::string("no ") + MSC_MOD_INIT_SYMBOL + " export";
        if (init) {
            g_initPhase = true;
            const int result = init(&kApi, mod.get());
            g_initPhase = false;
            if (result == 0) {
                mod->status.loaded = true;
            } else {
                error = "msc_mod_init returned " + std::to_string(result);
                // Drop what it registered: a failed plugin shouldn't half-run.
                for (auto& [address, chain] : g_hooks)
                    std::erase_if(chain, [&](const Hook& h) { return h.mod == mod.get(); });
                std::erase_if(g_frameSubscribers, [&](const Subscriber& s) { return s.mod == mod.get(); });
            }
        }
        mod->status.error = error;
        RT_LOG(RT_TAG_MODS) << package->id << ": native plugin " << RuntimeConfigFile::PathToUtf8(package->pluginLibrary.filename())
                            << (mod->status.loaded ? " loaded" : " failed: " + error) << std::endl;
        g_mods.push_back(std::move(mod));
    }
    size_t hooks = 0;
    for (const auto& [address, chain] : g_hooks) hooks += chain.size();
    if (hooks || !g_frameSubscribers.empty())
        RT_LOG(RT_TAG_MODS) << hooks << " plugin hook(s), " << g_frameSubscribers.size() << " frame subscriber(s)" << std::endl;
}

void RunFrameEvents() {
    if (g_frameSubscribers.empty()) return;
    GuestInterruptCallbackContext call;
    for (const Subscriber& s : g_frameSubscribers) s.fn(Cpu(call.get()), s.user);
}

const PluginStatus* GetPluginStatus(const std::string& id) {
    for (const auto& mod : g_mods)
        if (mod->package->id == id) return &mod->status;
    return nullptr;
}

bool PluginSettingValue(const std::string& id, const PluginSetting& setting) {
    return RuntimeConfigFile::ModSetting(id, setting.key).value_or(setting.defaultValue);
}

void SetPluginSettingValue(const std::string& id, const PluginSetting& setting, bool value) {
    RuntimeConfigFile::SetModSetting(id, setting.key, value);
}

namespace Hooks {

void RegisterWrapPoint(uint32_t address, const char* name) { WrapPoints()[address] = name; }

bool IsWrapPoint(uint32_t address) { return WrapPoints().count(address) != 0; }

void Run(uint32_t address, CpuContext* ctx, Inner inner) {
    const auto it = g_hooks.find(address);
    if (it == g_hooks.end() || it->second.empty()) {
        inner(ctx);
        return;
    }
    ChainFrame frame{&it->second, inner, 0, t_chain};
    t_chain = &frame;
    try {
        Invoke(&frame, 0, ctx);
    } catch (...) {
        t_chain = frame.previous;
        throw;
    }
    t_chain = frame.previous;
}

}  // namespace Hooks

}  // namespace Mods
