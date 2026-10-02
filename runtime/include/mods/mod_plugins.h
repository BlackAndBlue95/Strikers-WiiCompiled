// Native plugins of mod packages (the C API is sdk/include/msc_mod_api.h; docs/modding/plugins.md).
#pragma once

#include <cstdint>
#include <string>
#include <vector>

struct CpuContext;

namespace Mods {

// Loads the allowed plugins of the active packages and runs their msc_mod_init. Once, on the main
// thread, before the game starts.
void LoadPlugins();

// The plugins' frame events. Once per game frame, on the game's thread.
void RunFrameEvents();

struct PluginSetting {
    std::string key, label, help;
    bool defaultValue = false;
};

struct PluginStatus {
    bool loaded = false;      // the library loaded and msc_mod_init returned 0
    std::string error;        // why not
    std::vector<PluginSetting> settings;
};

// A package's plugin state this session, or nullptr when it has no plugin or it wasn't loaded.
const PluginStatus* GetPluginStatus(const std::string& id);
bool PluginSettingValue(const std::string& id, const PluginSetting& setting);
void SetPluginSettingValue(const std::string& id, const PluginSetting& setting, bool value);

namespace Hooks {

using Inner = void (*)(CpuContext*);

// Wrap points: guest functions the runtime replaces with a native function that still calls the
// translated original (PPC_NATIVE_WRAP in hle_stubs.h). Every wrap point can carry plugin hooks.
void RegisterWrapPoint(uint32_t address, const char* name);
bool IsWrapPoint(uint32_t address);

// Runs the plugin hooks on `address` (the mod loaded last first), then `inner`: the runtime's own
// wrapper or the translated original. Without hooks this is a direct call to `inner`.
void Run(uint32_t address, CpuContext* ctx, Inner inner);

}  // namespace Hooks

}  // namespace Mods
