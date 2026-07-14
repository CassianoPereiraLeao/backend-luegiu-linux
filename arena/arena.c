#include "arena.h"

void arena_init(Arena *arena, size_t size) {
    arena->size = (size > ARENA_CHUNK_SIZE) ? size : ARENA_CHUNK_SIZE;
    arena->first = NULL;
    arena->last = NULL;
}

void *arena_alloc(Arena *arena, size_t size) {
    size_t align = align_up(size);

    if(arena->last && (arena->last->used + align <= arena->last->capacity)) {
        void *ptr = &arena->last->data[arena->last->used];
        arena->last->used += align;
        return ptr;
    }

    size_t new_chunk_size = (align > arena->size) ? align : arena->size;
    size_t total = sizeof(ArenaChunk) + new_chunk_size;
    ArenaChunk* new_chunk = (ArenaChunk*)plataform_alloc(total);

    if(!new_chunk) return NULL;

    new_chunk->next = NULL;
    new_chunk->capacity = new_chunk_size;
    new_chunk->used = align;

    if(!arena->first) arena->first = new_chunk;
    else arena->last->next = new_chunk;

    arena->last = new_chunk;
    return (void*)new_chunk->data;
}

void arena_free(Arena *arena) {
    ArenaChunk* current = arena->first;
    while(current) {
        ArenaChunk* next = current->next;
        size_t total = sizeof(ArenaChunk) + current->capacity;
        plataform_free(current, total);
        current = next;
    }

    arena->first = NULL;
    arena->last = NULL;
}
