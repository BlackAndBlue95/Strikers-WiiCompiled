// The Strikers Mod Loader: loads a pack's code (Kamek modules, "sml_<NN>_<name>.bin" at the disc
// root, in name order) on a Wii or in Dolphin. Riivolution puts it where MetroTRK's interrupt table
// would be copied from under a debugger (0x80004060-0x80005F88, unused otherwise) and calls it from
// nlInit, once the game's memory and file system are up. Kamek's loader (kamekLoader.cpp) does the
// rest: memory from the game's heap, the module's relocations and hooks, its constructors.
//
// Strikers Recharged builds the same modules into the game ahead of time and never runs this.
#include "kamekLoader.h"

// OSReport, OSFatal and sprintf: as Kamek's headers declare them.
extern "C" {
int DVDConvertPathToEntrynum(const char* path);
bool DVDFastOpen(int entrynum, DVDHandle* handle);
bool DVDReadAsyncPrio(DVDHandle* handle, void* buffer, int length, int offset, void* callback, int prio);
int DVDGetCommandBlockStatus(const void* block);
bool DVDClose(DVDHandle* handle);
}
void* nlMalloc(unsigned long size, unsigned int alignment, bool fromEnd);
void nlFree(void* buffer);
void nlInitFileSystem();

namespace {

// The game links DVDReadAsyncPrio but not DVDReadPrio: its wait, as the SDK's does.
int ReadPrio(DVDHandle* handle, void* buffer, int length, int offset, int prio) {
    if (!DVDReadAsyncPrio(handle, buffer, length, offset, 0, prio)) return -1;
    for (;;) {  // the command block is the handle's start
        const int status = DVDGetCommandBlockStatus(handle);
        if (status == 0) return length;                // DVD_STATE_END
        if (status == -1 || status == 10) return -1;  // fatal error, cancelled
    }
}

// Code lives as long as the game does: from the heap's start. A file is read, applied and freed:
// from its end.
void* Alloc(u32 size, bool isForCode, const loaderFunctions*) {
    return nlMalloc(size, 32, !isForCode);
}

void Free(void* buffer, bool, const loaderFunctions*) {
    nlFree(buffer);
}

const loaderFunctions kFunctions = {
    OSReport, (OSFatal_t)OSFatal, DVDConvertPathToEntrynum, DVDFastOpen, ReadPrio, DVDClose, sprintf, Alloc, Free,
};

char Lower(char c) {
    return c >= 'A' && c <= 'Z' ? (char)(c - 'A' + 'a') : c;
}

// "sml_*.bin", in any case.
bool IsModule(const char* name) {
    const char* prefix = "sml_";
    for (int i = 0; prefix[i]; ++i)
        if (Lower(name[i]) != prefix[i]) return false;
    int n = 0;
    while (name[n]) ++n;
    return n > 8 && Lower(name[n - 4]) == '.' && Lower(name[n - 3]) == 'b' && Lower(name[n - 2]) == 'i' &&
           Lower(name[n - 1]) == 'n';
}

int Compare(const char* a, const char* b) {
    while (*a && Lower(*a) == Lower(*b)) ++a, ++b;
    return (unsigned char)Lower(*a) - (unsigned char)Lower(*b);
}

struct FSTEntry {
    u32 typeAndName;  // directory flag (top byte), name's offset in the string table
    u32 offset;       // a file's disc offset, a directory's parent
    u32 length;       // a file's size, a directory's end (the entry after its last)
};

// The modules: the files at the root of the disc's file table (Riivolution's, with the packs' files
// in it), in name order.
void LoadModules() {
    const FSTEntry* fst = *(const FSTEntry* const*)0x80000038;
    if (fst == 0) return;
    const u32 count = fst[0].length;
    const char* names = (const char*)(fst + count);
    const char* modules[32];
    int found = 0;
    for (u32 i = 1; i < count;) {
        if (fst[i].typeAndName >> 24) {  // a directory: past it
            i = fst[i].length > i ? fst[i].length : i + 1;
            continue;
        }
        const char* name = names + (fst[i].typeAndName & 0xFFFFFF);
        if (IsModule(name) && found < 32) {
            int at = found++;
            while (at > 0 && Compare(modules[at - 1], name) > 0) {
                modules[at] = modules[at - 1];
                --at;
            }
            modules[at] = name;
        }
        ++i;
    }
    OSReport("[Strikers Mod Loader] %d code mod(s)\n", found);
    for (int i = 0; i < found; ++i) {
        char path[96];
        sprintf(path, "/%s", modules[i]);
        loadKamekBinaryFromDisc(&kFunctions, path);
    }
}

}  // namespace

// nlInit's call to nlInitFileSystem (memory is already up): the file system, then the modules.
static void InitFileSystemThenLoadModules() {
    nlInitFileSystem();
    LoadModules();
}
kmCall(0x802B44FC, InitFileSystemThenLoadModules);
