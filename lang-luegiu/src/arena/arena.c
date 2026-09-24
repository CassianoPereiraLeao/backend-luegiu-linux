#include "../../include/arena.h"

size_t align_size(size_t size, size_t alignment) {
    if(alignment <= 1) return size;
    return (size + (alignment - 1))& ~(alignment - 1);
}

void init_arena(Arena *arena, size_t initial_size) {
    arena->first = NULL;
    arena->last = NULL;
    arena->size = (initial_size > KB) ? initial_size : KB;
}

void* arena_alloc(Arena *arena, size_t size) {
    size_t align = align_size(size, 8);

    if(arena->last && (align <= arena->last->capacity - arena->last->count)) {
        void* ptr = &arena->last->data[arena->last->count];
        arena->last->count += align;
        return ptr;
    }

    size_t new_chunk_size = (align > arena->size) ? align : arena->size;
    size_t total = sizeof(ArenaChunk) + new_chunk_size;
    ArenaChunk* new_chunk = (ArenaChunk*)malloc(total);

    if(!new_chunk) return NULL;

    new_chunk->next = NULL;
    new_chunk->capacity = new_chunk_size;
    new_chunk->count = align;

    if(!arena->first) arena->first = new_chunk;
    else arena->last->next = new_chunk;

    arena->last = new_chunk;
    return (void*)new_chunk->data;
}

void arena_free(Arena *arena) {
    ArenaChunk* current = arena->first;

    while(current) {
        ArenaChunk* next = current->next;
        free(current);
        current = next;
    }

    arena->first = NULL;
    arena->last = NULL;
}
