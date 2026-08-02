#ifndef VRAM_ALLOCATOR_H
#define VRAM_ALLOCATOR_H

#include <windows.h>
#include <stddef.h>

typedef struct {
    int device_id;
    char device_name[256];
    size_t total_vram_bytes;
    size_t free_vram_bytes;
    int is_cuda_available;
} vram_info_t;

int vram_init(vram_info_t *info);
size_t vram_allocate(size_t bytes);
void vram_free(size_t dptr);
int vram_read(size_t dptr, size_t offset, void *dst_host, size_t length);
int vram_write(size_t dptr, size_t offset, const void *src_host, size_t length);

#endif // VRAM_ALLOCATOR_H
