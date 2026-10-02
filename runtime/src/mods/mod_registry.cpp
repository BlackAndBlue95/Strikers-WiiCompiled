// Mod packages: discovery and manifest validation (see mods/mod_registry.h, docs/modding/manifest.md).
#include "mods/mod_registry.h"

#include <algorithm>
#include <cctype>
#include <map>
#include <mutex>
#include <set>
#include <toml.hpp>
#include "runtime_config.h"
#include "runtime_log.h"

namespace Mods {
namespace {

constexpr const char* kBaseNames[20] = {
    "mario", "bowser", "daisy", "donkeykong", "luigi", "peach", "waluigi", "wario", "yoshi", "bowserjr",
    "diddykong", "petey",                                                                  // captains 0-11
    "birdo", "hammerbro", "koopa", "toad", "boo", "drybones", "montymole", "shyguy",      // partners 12-19
};
constexpr int kFirstPartner = 12;

#if defined(_WIN32)
constexpr const char* kPlatformKey = "windows";
#elif defined(__APPLE__)
constexpr const char* kPlatformKey = "macos";
#else
constexpr const char* kPlatformKey = "linux";
#endif

std::vector<Package> g_packages;
std::map<std::string, std::pair<bool, bool>> g_sessionSwitches;  // id -> (enabled, plugin) as loaded
bool g_restartRequired = false;

bool ValidId(const std::string& id) {
    if (id.empty() || id.size() > 64 || !(std::islower(static_cast<unsigned char>(id[0])) || std::isdigit(static_cast<unsigned char>(id[0]))))
        return false;
    return std::all_of(id.begin(), id.end(), [](char c) {
        return std::islower(static_cast<unsigned char>(c)) || std::isdigit(static_cast<unsigned char>(c)) || c == '.' || c == '_' || c == '-';
    });
}

bool ValidCharacterName(const std::string& name) {
    return !name.empty() && name.size() <= 15 && std::all_of(name.begin(), name.end(), [](char c) {
        return std::islower(static_cast<unsigned char>(c)) || std::isdigit(static_cast<unsigned char>(c)) || c == '_';
    });
}

// Reads a table's keys, reporting the unknown ones (typos would otherwise be silently ignored).
class Table {
public:
    Table(const toml::value& value, std::string where, Package& package) : value_(value), where_(std::move(where)), package_(package) {}
    ~Table() {
        if (!value_.is_table()) return;
        for (const auto& [key, unused] : value_.as_table())
            if (!read_.count(key)) package_.warnings.push_back(where_ + ": unknown key \"" + key + "\"");
    }
    const toml::value* Get(const std::string& key) {
        read_.insert(key);
        return value_.is_table() && value_.contains(key) ? &value_.at(key) : nullptr;
    }
    std::string String(const std::string& key, bool required = false) {
        const toml::value* v = Get(key);
        if (v && v->is_string()) return v->as_string();
        if (v) package_.errors.push_back(where_ + "." + key + " must be a string");
        else if (required) package_.errors.push_back(where_ + "." + key + " is required");
        return {};
    }
    std::optional<int64_t> Integer(const std::string& key, bool required = false) {
        const toml::value* v = Get(key);
        if (v && v->is_integer()) return v->as_integer();
        if (v) package_.errors.push_back(where_ + "." + key + " must be an integer");
        else if (required) package_.errors.push_back(where_ + "." + key + " is required");
        return std::nullopt;
    }
    bool Bool(const std::string& key) {
        const toml::value* v = Get(key);
        if (v && v->is_boolean()) return v->as_boolean();
        if (v) package_.errors.push_back(where_ + "." + key + " must be true or false");
        return false;
    }
    std::vector<std::string> Strings(const std::string& key) {
        std::vector<std::string> out;
        const toml::value* v = Get(key);
        if (!v) return out;
        if (v->is_string()) return {v->as_string()};
        if (v->is_array()) {
            for (const auto& item : v->as_array()) {
                if (item.is_string()) out.push_back(item.as_string());
                else package_.errors.push_back(where_ + "." + key + " must hold strings");
            }
            return out;
        }
        package_.errors.push_back(where_ + "." + key + " must be a string or an array of strings");
        return out;
    }

private:
    const toml::value& value_;
    std::string where_;
    Package& package_;
    std::set<std::string> read_;
};

void ReadCharacter(const toml::value& value, size_t index, Package& package) {
    CharacterDef c;
    {
        Table t(value, "character[" + std::to_string(index) + "]", package);
        c.name = t.String("name", true);
        c.kind = t.String("kind", true);
        c.base = t.String("base", true);
        c.displayName = t.String("display_name");
        c.voiceBank = t.String("voice_bank");
        c.goalieKit = t.String("goalie_kit");
        c.stats = t.String("stats");
        c.teammates = t.Strings("teammates");
        c.fixedTeam = t.Bool("fixed_team");
        c.extraEffects = t.Strings("extra_effects");
        if (const toml::value* bars = t.Get("stats_bars")) {
            std::array<float, 4> values{};
            bool ok = bars->is_array() && bars->as_array().size() == 4;
            for (size_t i = 0; ok && i < 4; ++i) {
                const toml::value& v = bars->as_array()[i];
                if (v.is_floating()) values[i] = static_cast<float>(v.as_floating());
                else if (v.is_integer()) values[i] = static_cast<float>(v.as_integer());
                else ok = false;
            }
            if (ok) c.statsBars = values;
            else package.errors.push_back("character[" + std::to_string(index) + "].stats_bars must be 4 numbers (movement, shooting, passing, defense)");
        }
        if (const std::string ability = t.String("ability"); !ability.empty()) {
            if (ability == "none") c.noAbility = true;
            else if (ability != "base") package.errors.push_back("character[" + std::to_string(index) + "].ability must be \"base\" or \"none\"");
        }
        if (const std::string deke = t.String("deke"); !deke.empty()) {
            if (deke == "plain") c.deke = CharacterDef::Deke::Plain;
            else if (deke == "default") c.deke = CharacterDef::Deke::Default;
            else if (deke != "base") package.errors.push_back("character[" + std::to_string(index) + "].deke must be \"base\", \"plain\" or \"default\"");
        }
        if (const std::string role = t.String("role"); !role.empty()) {
            static const char* const kRoles[] = {"offensive", "defensive", "playmaker", "power", "balanced"};
            for (int r = 0; r < 5; ++r)
                if (role == kRoles[r]) c.role = r;
            if (c.role < 0) package.errors.push_back("character[" + std::to_string(index) + "].role must be offensive, defensive, playmaker, power or balanced");
        }
        if (const toml::value* colours = t.Get("colours")) {
            Table ct(*colours, "character[" + std::to_string(index) + "].colours", package);
            if (auto v = ct.Integer("primary")) c.primaryColour = static_cast<uint32_t>(*v & 0xFFFFFF);
            if (auto v = ct.Integer("alternate")) c.alternateColour = static_cast<uint32_t>(*v & 0xFFFFFF);
        }
    }
    const std::string where = "character \"" + c.name + "\"";
    if (!c.name.empty() && !ValidCharacterName(c.name))
        package.errors.push_back(where + ": name must be 1-15 characters of a-z, 0-9 and _");
    if (BaseCharacterIndex(c.name) >= 0) package.errors.push_back(where + ": name is already a game character");
    c.baseIndex = BaseCharacterIndex(c.base);
    if (c.kind == "captain") {
        if (!c.base.empty() && (c.baseIndex < 0 || c.baseIndex >= kFirstPartner))
            package.errors.push_back(where + ": base must be a captain (mario, bowser, ... petey)");
    } else if (c.kind == "partner") {
        if (!c.base.empty() && c.baseIndex < kFirstPartner)
            package.errors.push_back(where + ": base must be a partner (birdo, hammerbro, ... shyguy)");
        package.warnings.push_back(where + ": partner characters aren't supported yet; it isn't used");
    } else if (!c.kind.empty()) {
        package.errors.push_back(where + ": kind must be \"captain\" or \"partner\"");
    }
    if (c.teammates.size() > 3) package.errors.push_back(where + ": at most 3 teammates");
    if (c.fixedTeam) c.teammates.resize(3, c.name);  // the slots it doesn't name: copies of itself
    package.characters.push_back(std::move(c));
}

Package ReadPackage(const std::filesystem::path& root) {
    Package package;
    package.root = root;
    const std::filesystem::path manifest = root / "mod.toml";
    toml::value document;
    try {
        document = toml::parse(manifest);
    } catch (const std::exception& e) {
        package.id = RuntimeConfigFile::PathToUtf8(root.filename());
        package.name = package.id;
        package.errors.push_back(std::string("mod.toml: ") + e.what());
        return package;
    }
    {
        Table top(document, "mod.toml", package);
        if (const toml::value* mod = top.Get("mod")) {
            Table t(*mod, "mod", package);
            package.id = t.String("id", true);
            package.name = t.String("name", true);
            package.version = t.String("version");
            package.description = t.String("description");
            package.authors = t.Strings("authors");
            for (auto& author : t.Strings("author")) package.authors.push_back(std::move(author));
            package.framework = static_cast<int>(t.Integer("framework", true).value_or(0));
            package.loadOrder = static_cast<int>(t.Integer("load_order").value_or(0));
        } else {
            package.errors.push_back("mod.toml: missing the [mod] table");
        }
        if (const toml::value* plugin = top.Get("plugin")) {
            Table t(*plugin, "plugin", package);
            for (const char* key : {"windows", "macos", "linux"}) {
                const std::string library = t.String(key);
                if (library.empty() || key != std::string(kPlatformKey)) continue;
                package.pluginLibrary = (root / RuntimeConfigFile::PathFromUtf8(library)).lexically_normal();
                std::error_code ec;
                if (!std::filesystem::is_regular_file(package.pluginLibrary, ec))
                    package.errors.push_back("plugin." + std::string(key) + ": " + library + " not found");
            }
        }
        if (const toml::value* characters = top.Get("character")) {
            if (characters->is_array()) {
                size_t index = 0;
                for (const auto& c : characters->as_array()) ReadCharacter(c, index++, package);
            } else {
                package.errors.push_back("character must be an array of tables ([[character]])");
            }
        }
    }
    if (package.id.empty()) package.id = RuntimeConfigFile::PathToUtf8(root.filename());
    else if (!ValidId(package.id)) package.errors.push_back("mod.id must be lower-case a-z, 0-9, '.', '_' or '-'");
    if (package.name.empty()) package.name = package.id;
    if (package.framework > kFrameworkVersion)
        package.errors.push_back("needs framework " + std::to_string(package.framework) + "; this build reads " +
                                 std::to_string(kFrameworkVersion) + " (update Strikers-WiiCompiled)");
    else if (package.framework < 1 && package.errors.empty())
        package.errors.push_back("mod.framework must be 1 or more");
    return package;
}

void Load() {
    const std::filesystem::path dir = Directory();
    std::error_code ec;
    std::vector<std::filesystem::path> roots;
    for (const auto& entry : std::filesystem::directory_iterator(dir, ec))
        if (entry.is_directory(ec) && std::filesystem::is_regular_file(entry.path() / "mod.toml", ec)) roots.push_back(entry.path());
    std::sort(roots.begin(), roots.end());
    for (const auto& root : roots) g_packages.push_back(ReadPackage(root));

    std::stable_sort(g_packages.begin(), g_packages.end(), [](const Package& a, const Package& b) {
        return a.loadOrder != b.loadOrder ? a.loadOrder < b.loadOrder : a.id < b.id;
    });
    std::set<std::string> ids;
    for (Package& package : g_packages) {
        if (!ids.insert(package.id).second)
            package.errors.push_back("another package already uses the id \"" + package.id + "\"");
        package.enabledSetting = RuntimeConfigFile::ModPackageEnabled(package.id).value_or(true);
        package.pluginSetting = RuntimeConfigFile::ModPluginEnabled(package.id).value_or(false);
    }
    // Character names are one namespace across the active packages (they name files and textures).
    std::map<std::string, std::string> owners;
    for (Package& package : g_packages) {
        if (!package.enabledSetting || !package.errors.empty()) continue;
        for (const CharacterDef& c : package.characters) {
            const auto [it, added] = owners.emplace(c.name, package.id);
            if (!added) package.errors.push_back("character \"" + c.name + "\" is already added by " + it->second);
        }
    }
    for (Package& package : g_packages) {
        package.active = package.enabledSetting && package.errors.empty();
        for (const CharacterDef& c : package.characters)
            for (const std::string& mate : c.teammates)
                if (BaseCharacterIndex(mate) < 0 && !owners.count(mate))
                    package.warnings.push_back("character \"" + c.name + "\": unknown teammate \"" + mate + "\"");
        g_sessionSwitches[package.id] = {package.enabledSetting, package.pluginSetting};
    }

    RT_LOG(RT_TAG_MODS) << RuntimeConfigFile::PathToUtf8(dir) << ": " << g_packages.size() << " package(s)" << std::endl;
    for (const Package& package : g_packages) {
        RT_LOG(RT_TAG_MODS) << "  " << package.id << " " << (package.version.empty() ? "" : package.version + " ") << "("
                            << package.name << "): "
                            << (package.active ? "active" : !package.enabledSetting ? "disabled" : "not loaded (errors)")
                            << ", " << package.characters.size() << " character(s)"
                            << (package.HasPlugin() ? package.pluginSetting ? ", native plugin allowed" : ", native plugin not allowed" : "")
                            << std::endl;
        for (const std::string& e : package.errors) RT_LOG(RT_TAG_MODS) << "    error: " << e << std::endl;
        for (const std::string& w : package.warnings) RT_LOG(RT_TAG_MODS) << "    warning: " << w << std::endl;
    }
}

std::once_flag g_loaded;

}  // namespace

const std::vector<Package>& Packages() {
    std::call_once(g_loaded, Load);
    return g_packages;
}

std::vector<const Package*> ActivePackages() {
    std::vector<const Package*> out;
    for (const Package& package : Packages())
        if (package.active) out.push_back(&package);
    return out;
}

void SetEnabled(const std::string& id, bool enabled) {
    Packages();
    RuntimeConfigFile::SetModPackageEnabled(id, enabled);
    g_restartRequired = false;
    for (const Package& package : g_packages)
        if (const auto it = g_sessionSwitches.find(package.id); it != g_sessionSwitches.end())
            g_restartRequired |= RuntimeConfigFile::ModPackageEnabled(package.id).value_or(true) != it->second.first ||
                                 (package.HasPlugin() && RuntimeConfigFile::ModPluginEnabled(package.id).value_or(false) != it->second.second);
}

void SetPluginAllowed(const std::string& id, bool allowed) {
    Packages();
    RuntimeConfigFile::SetModPluginEnabled(id, allowed);
    SetEnabled(id, RuntimeConfigFile::ModPackageEnabled(id).value_or(true));
}

bool RestartRequired() { return g_restartRequired; }

namespace {
std::mutex g_mountMutex;
std::map<std::string, MountStats> g_mounts;
}  // namespace

void SetMountStats(const std::string& id, MountStats stats) {
    std::lock_guard lock(g_mountMutex);
    g_mounts[id] = stats;
}

MountStats GetMountStats(const std::string& id) {
    std::lock_guard lock(g_mountMutex);
    const auto it = g_mounts.find(id);
    return it == g_mounts.end() ? MountStats{} : it->second;
}

std::filesystem::path Directory() {
    static const std::filesystem::path dir = [] {
        std::filesystem::path path = RuntimeConfigFile::ModsDirectory();
        std::error_code ec;
        std::filesystem::create_directories(path, ec);
        return path;
    }();
    return dir;
}

const char* BaseCharacterName(int index) { return index >= 0 && index < 20 ? kBaseNames[index] : nullptr; }

int BaseCharacterIndex(const std::string& name) {
    for (int i = 0; i < 20; ++i)
        if (name == kBaseNames[i]) return i;
    return -1;
}

}  // namespace Mods
