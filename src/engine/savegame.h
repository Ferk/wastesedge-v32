#ifndef WASTESEDGE_ENGINE_SAVEGAME_H
#define WASTESEDGE_ENGINE_SAVEGAME_H

#include "engine/world.h"

#define SAVEGAME_FORMAT_VERSION 4
#define SAVEGAME_MINIMUM_VERSION 1
#define SAVEGAME_MAX_WORDS 1000

int savegame_encode(const struct world_state *world, int *words, int capacity);
int savegame_decode(struct world_state *world, const int *words, int word_count);
unsigned int savegame_checksum(const int *words, int word_count);
unsigned int savegame_content_fingerprint(void);

#endif
