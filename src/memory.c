#include "server.h"

#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void free_last_block(struct memory_arena *arena) {
    struct memory_arena_block *block = arena->current_block;
    arena->current_block = block->next;
    free(block);
}

void arena_clear(struct memory_arena *arena) {
    while (arena->current_block) {
        bool is_last_block = (arena->current_block->next == 0);
        free_last_block(arena);
        if (is_last_block) {
            break;
        }
    }
}

static size_t get_alignment_offset(struct memory_arena *arena, size_t align) {
    size_t result_ptr = (size_t)arena->current_block->base + arena->current_block->used;
    size_t align_mask = align - 1;
    size_t offset = 0;
    if (result_ptr & align_mask) {
        offset = align - (result_ptr & align_mask);
    }
    return offset;
}

void *arena_alloc(struct memory_arena *arena, size_t size_init) {
    void *result = 0;

    if (size_init) {
        size_t size = (size_init + 15) & ~15;

        if (!arena->current_block || (arena->current_block->used + size > arena->current_block->size)) {
            size = size_init;
            if (!arena->minimum_block_size) {
                arena->minimum_block_size = 1 << 16;
            }

            size_t block_size = size;
            if (arena->minimum_block_size > block_size) {
                block_size = arena->minimum_block_size;
            }

            struct memory_arena_block *new_block = calloc(1, block_size + sizeof(struct memory_arena_block));
            if (new_block == NULL) {
                return NULL;
            }
            new_block->size = block_size;
            new_block->next = arena->current_block;
            new_block->base = (char *)(new_block + 1);
            arena->current_block = new_block;
        }

        assert(arena->current_block->used + size <= arena->current_block->size);

        size_t align_offset = get_alignment_offset(arena, 16);
        size_t block_offset = arena->current_block->used + align_offset;
        assert(block_offset + size <= arena->current_block->size);
        result = arena->current_block->base + block_offset;
        arena->current_block->used += size;

        assert(size >= size_init);
        memset(result, 0, size_init);
    }
    return result;
}

void *arena_realloc(struct memory_arena *arena, void *memory, size_t old_size, size_t new_size) {
    void *new_mem = arena_alloc(arena, new_size);
    memcpy(new_mem, memory, old_size < new_size ? old_size : new_size);
    return new_mem;
}

void *server_alloc(size_t size) {
    if (g_memory_arena) {
        void *mem = arena_alloc(g_memory_arena, size);
        if (mem == NULL) {
            log_msg(LOG_FATAL, "OOM");
            exit(EXIT_FAILURE);
        }
        return mem;
    }
    void *mem = calloc(1, size);
    if (mem == NULL) {
        log_msg(LOG_FATAL, "OOM");
        exit(EXIT_FAILURE);
    }
    return mem;
}

void *server_realloc(void *memory, size_t old_size, size_t size) {
    if (g_memory_arena) {
        void *mem = arena_realloc(g_memory_arena, memory, old_size, size);
        if (mem == NULL) {
            log_msg(LOG_FATAL, "OOM");
            exit(EXIT_FAILURE);
        }
        return mem;
    }
    void *mem = realloc(memory, size);
    if (mem == NULL) {
        log_msg(LOG_FATAL, "OOM");
        exit(EXIT_FAILURE);
    }
    return mem;
}

char *server_strdup(const char *src) {
    size_t len = strlen(src);
    char *mem = server_alloc(len + 1);
    memcpy(mem, src, len);
    mem[len] = '\0';
    return mem;
}

__attribute__((format(printf, 1, 2))) char *server_memfmt(const char *fmt, ...) {
    char buf[4096];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    return server_strdup(buf);
}

void server_free(void *memory) {
    if (g_memory_arena) {

    } else {
        free(memory);
    }
}
