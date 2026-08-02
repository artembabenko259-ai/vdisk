#include "vram_allocator.h"
#include <stdio.h>

#ifndef CUDAAPI
#define CUDAAPI __stdcall
#endif

typedef int (CUDAAPI *PFN_cuInit)(unsigned int Flags);
typedef int (CUDAAPI *PFN_cuDeviceGet)(int *device, int ordinal);
typedef int (CUDAAPI *PFN_cuDeviceGetName)(char *name, int len, int dev);
typedef int (CUDAAPI *PFN_cuDeviceTotalMem)(size_t *bytes, int dev);
typedef int (CUDAAPI *PFN_cuCtxCreate)(void **pctx, unsigned int flags, int dev);
typedef int (CUDAAPI *PFN_cuMemAlloc)(size_t *dptr, size_t bytesize);
typedef int (CUDAAPI *PFN_cuMemFree)(size_t dptr);
typedef int (CUDAAPI *PFN_cuMemcpyHtoD)(size_t dstDevice, const void *srcHost, size_t ByteCount);
typedef int (CUDAAPI *PFN_cuMemcpyDtoH)(void *dstHost, size_t srcDevice, size_t ByteCount);
typedef int (CUDAAPI *PFN_cuMemGetInfo)(size_t *free_bytes, size_t *total_bytes);

static HMODULE g_hCuda = NULL;
static PFN_cuInit p_cuInit = NULL;
static PFN_cuDeviceGet p_cuDeviceGet = NULL;
static PFN_cuDeviceGetName p_cuDeviceGetName = NULL;
static PFN_cuDeviceTotalMem p_cuDeviceTotalMem = NULL;
static PFN_cuCtxCreate p_cuCtxCreate = NULL;
static PFN_cuMemAlloc p_cuMemAlloc = NULL;
static PFN_cuMemFree p_cuMemFree = NULL;
static PFN_cuMemcpyHtoD p_cuMemcpyHtoD = NULL;
static PFN_cuMemcpyDtoH p_cuMemcpyDtoH = NULL;
static PFN_cuMemGetInfo p_cuMemGetInfo = NULL;

static void *g_cuContext = NULL;
static int g_initialized = 0;

static int load_cuda_dll() {
    if (g_hCuda) return 1;

    g_hCuda = LoadLibraryA("nvcuda.dll");
    if (!g_hCuda) return 0;

    p_cuInit = (PFN_cuInit)GetProcAddress(g_hCuda, "cuInit");
    p_cuDeviceGet = (PFN_cuDeviceGet)GetProcAddress(g_hCuda, "cuDeviceGet");
    p_cuDeviceGetName = (PFN_cuDeviceGetName)GetProcAddress(g_hCuda, "cuDeviceGetName");
    p_cuDeviceTotalMem = (PFN_cuDeviceTotalMem)GetProcAddress(g_hCuda, "cuDeviceTotalMem");
    
    p_cuCtxCreate = (PFN_cuCtxCreate)GetProcAddress(g_hCuda, "cuCtxCreate_v2");
    if (!p_cuCtxCreate) p_cuCtxCreate = (PFN_cuCtxCreate)GetProcAddress(g_hCuda, "cuCtxCreate");
    
    p_cuMemAlloc = (PFN_cuMemAlloc)GetProcAddress(g_hCuda, "cuMemAlloc_v2");
    if (!p_cuMemAlloc) p_cuMemAlloc = (PFN_cuMemAlloc)GetProcAddress(g_hCuda, "cuMemAlloc");
    
    p_cuMemFree = (PFN_cuMemFree)GetProcAddress(g_hCuda, "cuMemFree_v2");
    if (!p_cuMemFree) p_cuMemFree = (PFN_cuMemFree)GetProcAddress(g_hCuda, "cuMemFree");
    
    p_cuMemcpyHtoD = (PFN_cuMemcpyHtoD)GetProcAddress(g_hCuda, "cuMemcpyHtoD_v2");
    if (!p_cuMemcpyHtoD) p_cuMemcpyHtoD = (PFN_cuMemcpyHtoD)GetProcAddress(g_hCuda, "cuMemcpyHtoD");
    
    p_cuMemcpyDtoH = (PFN_cuMemcpyDtoH)GetProcAddress(g_hCuda, "cuMemcpyDtoH_v2");
    if (!p_cuMemcpyDtoH) p_cuMemcpyDtoH = (PFN_cuMemcpyDtoH)GetProcAddress(g_hCuda, "cuMemcpyDtoH");

    p_cuMemGetInfo = (PFN_cuMemGetInfo)GetProcAddress(g_hCuda, "cuMemGetInfo_v2");
    if (!p_cuMemGetInfo) p_cuMemGetInfo = (PFN_cuMemGetInfo)GetProcAddress(g_hCuda, "cuMemGetInfo");

    if (!p_cuInit || !p_cuDeviceGet || !p_cuCtxCreate || !p_cuMemAlloc || !p_cuMemFree || !p_cuMemcpyHtoD || !p_cuMemcpyDtoH) {
        FreeLibrary(g_hCuda);
        g_hCuda = NULL;
        return 0;
    }
    return 1;
}

int vram_init(vram_info_t *info) {
    if (!load_cuda_dll()) {
        if (info) info->is_cuda_available = 0;
        return 0;
    }

    if (p_cuInit(0) != 0) {
        if (info) info->is_cuda_available = 0;
        return 0;
    }

    int dev = 0;
    if (p_cuDeviceGet(&dev, 0) != 0) {
        if (info) info->is_cuda_available = 0;
        return 0;
    }

    if (!g_cuContext) {
        if (p_cuCtxCreate(&g_cuContext, 0, dev) != 0) {
            if (info) info->is_cuda_available = 0;
            return 0;
        }
    }

    g_initialized = 1;

    if (info) {
        info->device_id = dev;
        info->is_cuda_available = 1;
        info->device_name[0] = '\0';
        p_cuDeviceGetName(info->device_name, sizeof(info->device_name), dev);
        info->total_vram_bytes = 0;
        p_cuDeviceTotalMem(&info->total_vram_bytes, dev);
        
        size_t free_b = 0, total_b = 0;
        if (p_cuMemGetInfo && p_cuMemGetInfo(&free_b, &total_b) == 0) {
            info->free_vram_bytes = free_b;
        } else {
            info->free_vram_bytes = info->total_vram_bytes;
        }
    }
    return 1;
}

size_t vram_allocate(size_t bytes) {
    if (!g_initialized) {
        if (!vram_init(NULL)) return 0;
    }

    size_t dptr = 0;
    int res = p_cuMemAlloc(&dptr, bytes);
    if (res != 0) {
        printf("[VRAM] cuMemAlloc failed for %llu bytes (error %d)\n", (unsigned long long)bytes, res);
        return 0;
    }
    return dptr;
}

void vram_free(size_t dptr) {
    if (!dptr || !p_cuMemFree) return;
    p_cuMemFree(dptr);
}

int vram_read(size_t dptr, size_t offset, void *dst_host, size_t length) {
    if (!dptr || !dst_host || !p_cuMemcpyDtoH) return 0;
    int res = p_cuMemcpyDtoH(dst_host, dptr + offset, length);
    return (res == 0);
}

int vram_write(size_t dptr, size_t offset, const void *src_host, size_t length) {
    if (!dptr || !src_host || !p_cuMemcpyHtoD) return 0;
    int res = p_cuMemcpyHtoD(dptr + offset, src_host, length);
    return (res == 0);
}
