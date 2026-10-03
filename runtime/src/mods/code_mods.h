// Code mods: Kamek modules from Riivolution packs, translated into the game by build.sh. See
// code_mods.cpp.
#pragma once

#include <string>
#include <vector>

namespace CodeMods {

struct Module {
    enum class State {
        BuiltIn,      // built in and installed as built
        NotInstalled, // built in, but no installed pack puts it on the disc any more
        Changed,      // built in, but the installed file differs from the one built in
        NotBuiltIn,   // installed, but this game was built without it
    };
    std::string name; // disc-root file name, lower case
    State state = State::BuiltIn;
    std::string path; // the installed file, if any
};

// The built-in and installed code mods, compared once the packs are loaded (at the game's init hook,
// or on first use).
const std::vector<Module>& Status();

// Whether the installed code mods differ from the built-in ones, so build.sh has to run again.
bool RebuildNeeded();

} // namespace CodeMods
