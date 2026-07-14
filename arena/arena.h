#pragma once

#define ARENA_CHUNK_SIZE (1024 * 1024)
#define ARENA_ALIGN 8

#include "../common.h"
#include "plataform.h"

static inline size_t align_up(size_t size) {
    return (size + (ARENA_ALIGN - 1))& ~(ARENA_ALIGN - 1);
}

void arena_init(Arena *arena, size_t size);
void *arena_alloc(Arena *arena, size_t size);
void arena_free(Arena *arena);
