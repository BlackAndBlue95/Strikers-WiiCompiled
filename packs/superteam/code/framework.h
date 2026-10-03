// Shared between the framework's files.
#ifndef MOD_FRAMEWORK_H
#define MOD_FRAMEWORK_H

#include "characters.h"

// The mod characters' own slots, after the game's (slots.cpp): character i is team kFirstModTeam + i,
// character class kFirstModClass + i, goalie class kFirstModGoalie + i and sound bank kFirstModBank + i.
const int kFirstModTeam = 12, kFirstModClass = 40, kFirstModGoalie = 50, kFirstModBank = 100;
int ModCharacterOfTeam(int team);  // -1 for the game's own
int ModCharacterOfClass(int cc);
const unsigned char* ModCharacterRow(int c);  // its CharacterInfo

// The mod character picked on captain select, or hovered on its page, -1 for none (captain select's
// voice and panel).
void SetActiveCharacter(int c);
int ActiveCharacter();

// The pack's textures: a .gxt read once into the framework's own memory (it outlives every screen),
// added to the front end's resource pool under its path's hash whenever the pool lacks it (a screen
// change can empty the pool). Returns that hash, or 0 when the file isn't there.
unsigned long PackTexture(const char* path);
void LoadPackTextures();
class FETextureResource;
FETextureResource* PackResource(const char* path);  // a front-end texture resource for a pack texture
bool EnsurePackTexture(unsigned long hash);

// The base's internal name ("waluigi"), its voice bank index, and the sound map's bank table (0
// until a bank has loaded).
const char* BaseName(int c);
int BaseVoiceBank(int c);
void* VoiceBankTable();

// Captain select: the character's voice when a team picks it (voice.cpp).
void PlayPickVoice(int side, int c);
void UpdatePickVoices();

#endif
