// The smallest code mod: a data patch, a constructor and a hook. Build it into a pack with
// ./build-pack.sh, copy the pack into the data folder's Riivolution folder and run build.sh; its lines
// show up in console.log as [OSReport] [hello] ...
#include <kamek.h>

// The game's own functions and globals, by the names the decomp gives them (externals-R4QE01.txt).
extern "C" void OSReport(const char* format, ...);
void nlInitTicker();
extern unsigned int sDebugPassSpaceSearch;  // a word of the game's .sbss that retail never reads

// A data patch, applied when the game starts its code mods, before any constructor runs.
kmWrite32(&sDebugPassSpaceSearch, 0x48454C4F);  // "HELO"

// A constructor: runs once, when the game starts its code mods (in nlInit, once the OS is up).
static struct Hello {
    Hello() {
        OSReport("[hello] constructed; the data patch reads 0x%08X\n", sDebugPassSpaceSearch);
        sDebugPassSpaceSearch = 0;  // as it was
    }
} s_hello;

// A hook: nlInit's call to nlInitTicker (the bl at 0x802B44F0) calls this instead.
kmCallDefCpp(0x802B44F0, void, void) {
    nlInitTicker();
    OSReport("[hello] nlInitTicker hooked\n");
}
