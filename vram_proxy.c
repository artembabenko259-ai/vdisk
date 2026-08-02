#include "vram_proxy.h"
#include "vram_allocator.h"
#include <stdio.h>

#pragma pack(push, 1)
typedef struct _IMDPROXY_REQ {
    ULONGLONG request_code; // 0 = READ, 1 = WRITE
    ULONGLONG offset;
    ULONGLONG length;
} IMDPROXY_REQ;
#pragma pack(pop)

#define BUFFER_SIZE (1024 * 1024) // 1MB I/O buffer

static DWORD WINAPI vram_proxy_thread_proc(LPVOID lpParam) {
    vram_proxy_server_t *server = (vram_proxy_server_t *)lpParam;
    vram_proxy_loop(server);
    return 0;
}

int vram_proxy_start(vram_proxy_server_t *server, size_t dptr, size_t size_bytes, char drive_letter) {
    if (!server) return 0;

    server->dptr = dptr;
    server->size_bytes = size_bytes;
    server->drive_letter = drive_letter;
    server->running = 1;

    snprintf(server->pipe_name, sizeof(server->pipe_name), "\\\\.\\pipe\\vdisk_vram_%c", drive_letter);

    server->hPipe = CreateNamedPipeA(
        server->pipe_name,
        PIPE_ACCESS_DUPLEX,
        PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
        1,
        BUFFER_SIZE,
        BUFFER_SIZE,
        0,
        NULL
    );

    if (server->hPipe == INVALID_HANDLE_VALUE) {
        printf("[VRAM Proxy] CreateNamedPipeA failed (%lu)\n", GetLastError());
        return 0;
    }

    server->hThread = CreateThread(NULL, 0, vram_proxy_thread_proc, server, 0, NULL);
    if (!server->hThread) {
        CloseHandle(server->hPipe);
        server->hPipe = INVALID_HANDLE_VALUE;
        return 0;
    }

    return 1;
}

void vram_proxy_loop(vram_proxy_server_t *server) {
    if (!server || server->hPipe == INVALID_HANDLE_VALUE) return;

    printf("[VRAM Proxy] Listening on pipe %s...\n", server->pipe_name);
    
    BOOL connected = ConnectNamedPipe(server->hPipe, NULL) ? TRUE : (GetLastError() == ERROR_PIPE_CONNECTED);
    if (!connected) {
        printf("[VRAM Proxy] ConnectNamedPipe failed (%lu)\n", GetLastError());
        return;
    }

    printf("[VRAM Proxy] Connected to ImDisk driver for drive %c:\n", server->drive_letter);

    unsigned char *iobuf = (unsigned char *)malloc(BUFFER_SIZE);
    if (!iobuf) {
        printf("[VRAM Proxy] Failed to allocate I/O buffer\n");
        return;
    }

    while (server->running) {
        IMDPROXY_REQ req;
        DWORD bytesRead = 0;

        BOOL res = ReadFile(server->hPipe, &req, sizeof(req), &bytesRead, NULL);
        if (!res || bytesRead == 0) {
            break; // Pipe closed or error
        }

        if (bytesRead < sizeof(req)) {
            continue;
        }

        if (req.request_code == 0) {
            // READ request
            size_t remaining = (size_t)req.length;
            size_t offset = (size_t)req.offset;

            while (remaining > 0 && server->running) {
                size_t chunk = (remaining > BUFFER_SIZE) ? BUFFER_SIZE : remaining;
                if (!vram_read(server->dptr, offset, iobuf, chunk)) {
                    memset(iobuf, 0, chunk);
                }
                DWORD bytesWritten = 0;
                WriteFile(server->hPipe, iobuf, (DWORD)chunk, &bytesWritten, NULL);
                remaining -= chunk;
                offset += chunk;
            }

            ULONGLONG status = 0; // Success status
            DWORD bytesWritten = 0;
            WriteFile(server->hPipe, &status, sizeof(status), &bytesWritten, NULL);

        } else if (req.request_code == 1) {
            // WRITE request
            size_t remaining = (size_t)req.length;
            size_t offset = (size_t)req.offset;

            while (remaining > 0 && server->running) {
                size_t chunk = (remaining > BUFFER_SIZE) ? BUFFER_SIZE : remaining;
                DWORD chunkRead = 0;
                ReadFile(server->hPipe, iobuf, (DWORD)chunk, &chunkRead, NULL);
                if (chunkRead > 0) {
                    vram_write(server->dptr, offset, iobuf, chunkRead);
                    remaining -= chunkRead;
                    offset += chunkRead;
                } else {
                    break;
                }
            }

            ULONGLONG status = 0; // Success status
            DWORD bytesWritten = 0;
            WriteFile(server->hPipe, &status, sizeof(status), &bytesWritten, NULL);
        } else {
            // Unknown or flush request
            ULONGLONG status = 0;
            DWORD bytesWritten = 0;
            WriteFile(server->hPipe, &status, sizeof(status), &bytesWritten, NULL);
        }
    }

    free(iobuf);
    DisconnectNamedPipe(server->hPipe);
    printf("[VRAM Proxy] Stopped proxy for drive %c:\n", server->drive_letter);
}

void vram_proxy_stop(vram_proxy_server_t *server) {
    if (!server) return;
    server->running = 0;
    if (server->hPipe != INVALID_HANDLE_VALUE) {
        CloseHandle(server->hPipe);
        server->hPipe = INVALID_HANDLE_VALUE;
    }
    if (server->hThread) {
        WaitForSingleObject(server->hThread, 2000);
        CloseHandle(server->hThread);
        server->hThread = NULL;
    }
}
