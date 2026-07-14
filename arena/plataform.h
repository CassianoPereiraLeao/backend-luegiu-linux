#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#ifdef _WIN32
#define MEM_COMMIT 0x1000
#define MEM_RESERVE 0x2000
#define MEM_RELEASE 0x8000
#define PAGE_READWRITE 0x04

void* VirtualAlloc(void *address, size_t dw_size, unsigned long alloc_type, unsigned long protected);
void VirtualFree(void *address, size_t dw_size, unsigned long free_type);

static inline void* plataform_alloc(size_t size) {
    return VirtualAlloc(NULL, size, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
}

static inline void plataform_free(void *ptr, size_t size) {
    return VirutalFree(ptr, size, MEM_RELEASE);
}

#elif defined(__linux__)

#include <sys/mman.h>

static inline void* plataform_alloc(size_t size) {
    void* ptr = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    return (ptr == MAP_FAILED) ? NULL : ptr;
}

static inline void plataform_free(void *ptr, size_t size) {
    if(ptr) munmap(ptr, size);
}

#endif
