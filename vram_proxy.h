#ifndef VRAM_PROXY_H
#define VRAM_PROXY_H

#include <windows.h>
#include <stddef.h>

typedef struct {
    size_t dptr;
    size_t size_bytes;
    char drive_letter;
    char pipe_name[256];
    HANDLE hPipe;
    volatile int running;
    HANDLE hThread;
} vram_proxy_server_t;

int vram_proxy_start(vram_proxy_server_t *server, size_t dptr, size_t size_bytes, char drive_letter);
void vram_proxy_stop(vram_proxy_server_t *server);
void vram_proxy_loop(vram_proxy_server_t *server);

#endif // VRAM_PROXY_H
