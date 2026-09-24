#pragma once

#include "common.h"

#define KB 1024
#define ARENA_ALIGN 8

typedef struct ArenaChunk {
    struct ArenaChunk* next;
    size_t count;
    size_t capacity;
    uint8_t data[];
} ArenaChunk;

typedef struct {
    ArenaChunk* first;
    ArenaChunk* last;
    size_t size;
} Arena;

void init_arena(Arena *arena, size_t initial_size);
void* arena_alloc(Arena *arena, size_t size);
void arena_free(Arena *arena);
size_t align_size(size_t size, size_t alignment);
