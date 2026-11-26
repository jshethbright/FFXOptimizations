#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#define D3D11_NO_HELPERS
#define CINTERFACE
#define COBJMACROS
#include <windows.h>
#include <d3d11.h>
#include <dxgi1_4.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cwchar>
#include <share.h>
#ifndef FFX_GPU_TRACE_TEST
#include <reshade.hpp>
#endif

namespace {
using Tick = std::uint64_t;
constexpr Tick oneSecondUs = 1000000;
LARGE_INTEGER frequency{};

Tick qpc()
{
    LARGE_INTEGER now{};
    QueryPerformanceCounter(&now);
    return static_cast<Tick>(now.QuadPart);
}

Tick micros(Tick ticks)
{
    return ticks * oneSecondUs / static_cast<Tick>(frequency.QuadPart);
}

struct Event {
    const char* kind;
    Tick start, end;
    unsigned detailA, detailB;
    Tick gpuUs;
    DWORD thread;
};

SRWLOCK eventLock = SRWLOCK_INIT;
std::array<Event, 4096> events{};
size_t readIndex = 0, writeIndex = 0;
std::atomic<unsigned> lost{0};
std::atomic<unsigned> samplesMade{0};
std::atomic<unsigned> textureCalls{0}, updateCalls{0};

void record(const char* kind, Tick start, Tick end, unsigned a = 0, unsigned b = 0, Tick gpuUs = 0)
{
    if (!TryAcquireSRWLockExclusive(&eventLock)) {
        lost.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    const size_t next = (writeIndex + 1) % events.size();
    if (next != readIndex) {
        events[writeIndex] = {kind, start, end, a, b, gpuUs, GetCurrentThreadId()};
        writeIndex = next;
    } else {
        lost.fetch_add(1, std::memory_order_relaxed);
    }
    ReleaseSRWLockExclusive(&eventLock);
}

void cpu(const char* kind, Tick start, unsigned a = 0, unsigned b = 0)
{
    const auto end = qpc();
    if (micros(end - start) >= 500) record(kind, start, end, a, b);
}

constexpr DWORD intervalMs = 500;
std::atomic<bool> stopWriter{false};
HANDLE writerThread = nullptr;

[[maybe_unused]] DWORD WINAPI writer(void*)
{
    FILE* out = _wfsopen(L"ffx-gpu-texture-trace.csv", L"a", _SH_DENYNO);
    if (!out) return 0;
    std::fprintf(out, "# pid=%lu qpc_hz=%llu uptime_ms=%llu qpc=%llu\n",
                 GetCurrentProcessId(), static_cast<unsigned long long>(frequency.QuadPart),
                 static_cast<unsigned long long>(GetTickCount64()),
                 static_cast<unsigned long long>(qpc()));
    std::fprintf(out, "kind,start_qpc,end_qpc,cpu_us,detail_a,detail_b,gpu_us,thread\n");
    do {
        for (;;) {
            Event event{};
            AcquireSRWLockExclusive(&eventLock);
            if (readIndex == writeIndex) { ReleaseSRWLockExclusive(&eventLock); break; }
            event = events[readIndex];
            readIndex = (readIndex + 1) % events.size();
            ReleaseSRWLockExclusive(&eventLock);
            std::fprintf(out, "%s,%llu,%llu,%llu,%u,%u,%llu,%lu\n", event.kind,
                         event.start, event.end, micros(event.end - event.start), event.detailA,
                         event.detailB, event.gpuUs, event.thread);
        }
        const auto dropped = lost.exchange(0, std::memory_order_relaxed);
        if (dropped) std::fprintf(out, "# dropped_events=%u\n", dropped);
        std::fflush(out);
        if (!stopWriter.load(std::memory_order_relaxed)) Sleep(intervalMs);
    } while (!stopWriter.load(std::memory_order_relaxed));
    std::fclose(out);
    return 0;
}

struct GpuQuery {
    ID3D11Query* first = nullptr;
    ID3D11Query* last = nullptr;
    ID3D11Query* disjoint = nullptr;
    bool pending = false;
    const char* kind = nullptr;
    Tick cpuStart = 0, cpuEnd = 0;
    unsigned a = 0, b = 0;
};

ID3D11Device* activeDevice = nullptr;
std::atomic<ID3D11DeviceContext*> immediateContext{nullptr};
IDXGIAdapter3* adapter = nullptr;
std::array<GpuQuery, 8> queries{};
std::atomic_flag queryLock = ATOMIC_FLAG_INIT;
Tick lastGpuSample = 0;
Tick lastVram = 0;

void releaseQueries()
{
    for (auto& slot : queries) {
        if (slot.first) ID3D11Query_Release(slot.first);
        if (slot.last) ID3D11Query_Release(slot.last);
        if (slot.disjoint) ID3D11Query_Release(slot.disjoint);
        slot = {};
    }
}

void initQueries(ID3D11Device* device)
{
    const D3D11_QUERY_DESC ts{D3D11_QUERY_TIMESTAMP, 0};
    const D3D11_QUERY_DESC range{D3D11_QUERY_TIMESTAMP_DISJOINT, 0};
    for (auto& slot : queries) {
        if (FAILED(ID3D11Device_CreateQuery(device, &ts, &slot.first)) ||
            FAILED(ID3D11Device_CreateQuery(device, &ts, &slot.last)) ||
            FAILED(ID3D11Device_CreateQuery(device, &range, &slot.disjoint))) {
            releaseQueries();
            return;
        }
    }
}

GpuQuery* beginGpu(ID3D11DeviceContext* ctx, const char* kind, Tick start, unsigned a, unsigned b)
{
    if (ctx != immediateContext.load(std::memory_order_acquire) || !a || !b ||
        static_cast<Tick>(a) * b < 512 * 512 ||
        queryLock.test_and_set(std::memory_order_acquire)) return nullptr;
    if (ctx != immediateContext.load(std::memory_order_acquire) ||
        (lastGpuSample && (start <= lastGpuSample || micros(start - lastGpuSample) < 50000))) {
        queryLock.clear(std::memory_order_release);
        return nullptr;
    }
    for (auto& slot : queries) {
        if (!slot.disjoint || slot.pending) continue;
        slot.kind = kind;
        slot.cpuStart = start;
        slot.a = a;
        slot.b = b;
        ID3D11DeviceContext_Begin(ctx, reinterpret_cast<ID3D11Asynchronous*>(slot.disjoint));
        ID3D11DeviceContext_End(ctx, reinterpret_cast<ID3D11Asynchronous*>(slot.first));
        lastGpuSample = start;
        record("gpu_sample_queued", start, start, a, b);
        return &slot;
    }
    queryLock.clear(std::memory_order_release);
    return nullptr;
}

void finishGpu(ID3D11DeviceContext* ctx, GpuQuery* slot)
{
    if (!slot) return;
    slot->cpuEnd = qpc();
    ID3D11DeviceContext_End(ctx, reinterpret_cast<ID3D11Asynchronous*>(slot->last));
    ID3D11DeviceContext_End(ctx, reinterpret_cast<ID3D11Asynchronous*>(slot->disjoint));
    slot->pending = true;
    queryLock.clear(std::memory_order_release);
}

[[maybe_unused]] void pollGpu()
{
    if (queryLock.test_and_set(std::memory_order_acquire)) return;
    auto* ctx = immediateContext.load(std::memory_order_acquire);
    if (!ctx) {
        queryLock.clear(std::memory_order_release);
        return;
    }
    for (auto& slot : queries) {
        if (!slot.pending) continue;
        D3D11_QUERY_DATA_TIMESTAMP_DISJOINT span{};
        const auto query = ID3D11DeviceContext_GetData(ctx, reinterpret_cast<ID3D11Asynchronous*>(slot.disjoint),
            &span, sizeof span, D3D11_ASYNC_GETDATA_DONOTFLUSH);
        if (query == S_FALSE) continue;
        UINT64 first = 0, last = 0;
        if (query == S_OK && !span.Disjoint && span.Frequency) {
            const auto get = [ctx](ID3D11Query* item, UINT64& value) {
                return ID3D11DeviceContext_GetData(ctx, reinterpret_cast<ID3D11Asynchronous*>(item), &value,
                    sizeof value, D3D11_ASYNC_GETDATA_DONOTFLUSH);
            };
            const auto begin = get(slot.first, first);
            const auto end = get(slot.last, last);
            if (begin == S_FALSE || end == S_FALSE) continue;
            if (begin == S_OK && end == S_OK && last >= first) {
                record(slot.kind, slot.cpuStart, slot.cpuEnd, slot.a, slot.b,
                       (last - first) * oneSecondUs / span.Frequency);
                samplesMade.fetch_add(1, std::memory_order_relaxed);
            }
        }
        if (query != S_OK || span.Disjoint || !span.Frequency)
            record("gpu_sample_unavailable", slot.cpuStart, slot.cpuEnd,
                   static_cast<unsigned>(query), span.Disjoint);
        slot.pending = false;
    }
    queryLock.clear(std::memory_order_release);
}

struct Dimensions { UINT width = 0, height = 0; };
Dimensions sizeOf(ID3D11Resource* resource)
{
    D3D11_RESOURCE_DIMENSION type = D3D11_RESOURCE_DIMENSION_UNKNOWN;
    if (resource) ID3D11Resource_GetType(resource, &type);
    if (type != D3D11_RESOURCE_DIMENSION_TEXTURE2D) return {};
    D3D11_TEXTURE2D_DESC desc{};
    ID3D11Texture2D_GetDesc(reinterpret_cast<ID3D11Texture2D*>(resource), &desc);
    return {desc.Width, desc.Height};
}

decltype(ID3D11DeviceVtbl::CreateTexture2D) createTexture = nullptr;
decltype(ID3D11DeviceVtbl::CreateVertexShader) createVS = nullptr;
decltype(ID3D11DeviceVtbl::CreatePixelShader) createPS = nullptr;
decltype(ID3D11DeviceVtbl::CreateComputeShader) createCS = nullptr;
decltype(ID3D11DeviceContextVtbl::UpdateSubresource) update = nullptr;
decltype(ID3D11DeviceContextVtbl::CopyResource) copy = nullptr;
decltype(ID3D11DeviceContextVtbl::CopySubresourceRegion) copyRegion = nullptr;
decltype(ID3D11DeviceContextVtbl::Map) map = nullptr;
decltype(ID3D11DeviceContextVtbl::Unmap) unmap = nullptr;
decltype(ID3D11DeviceContextVtbl::GenerateMips) generateMips = nullptr;

HRESULT STDMETHODCALLTYPE textureHook(ID3D11Device* device, const D3D11_TEXTURE2D_DESC* desc,
                                      const D3D11_SUBRESOURCE_DATA* initial, ID3D11Texture2D** texture)
{
    const auto start = qpc();
    const auto result = createTexture(device, desc, initial, texture);
    textureCalls.fetch_add(1, std::memory_order_relaxed);
    if (desc) cpu(initial ? "create_texture_with_data" : "create_texture_empty", start, desc->Width, desc->Height);
    return result;
}

template<typename Shader, typename Fn>
HRESULT shaderHook(const char* kind, Fn original, ID3D11Device* device, const void* data,
                   SIZE_T bytes, ID3D11ClassLinkage* linkage, Shader** result)
{
    const auto start = qpc();
    const auto status = original(device, data, bytes, linkage, result);
    cpu(kind, start, static_cast<unsigned>(bytes), static_cast<unsigned>(status));
    return status;
}

HRESULT STDMETHODCALLTYPE vertexHook(ID3D11Device* d, const void* b, SIZE_T n, ID3D11ClassLinkage* l, ID3D11VertexShader** o)
{ return shaderHook("create_vertex_shader", createVS, d, b, n, l, o); }
HRESULT STDMETHODCALLTYPE pixelHook(ID3D11Device* d, const void* b, SIZE_T n, ID3D11ClassLinkage* l, ID3D11PixelShader** o)
{ return shaderHook("create_pixel_shader", createPS, d, b, n, l, o); }
HRESULT STDMETHODCALLTYPE computeHook(ID3D11Device* d, const void* b, SIZE_T n, ID3D11ClassLinkage* l, ID3D11ComputeShader** o)
{ return shaderHook("create_compute_shader", createCS, d, b, n, l, o); }

void STDMETHODCALLTYPE updateHook(ID3D11DeviceContext* c, ID3D11Resource* r, UINT sub,
                                  const D3D11_BOX* box, const void* data, UINT pitch, UINT depth)
{
    updateCalls.fetch_add(1, std::memory_order_relaxed);
    const auto dimensions = sizeOf(r);
    const auto w = box ? box->right - box->left : dimensions.width;
    const auto h = box ? box->bottom - box->top : dimensions.height;
    const auto start = qpc();
    auto* sample = dimensions.width ? beginGpu(c, "gpu_update_texture", start, w, h) : nullptr;
    update(c, r, sub, box, data, pitch, depth);
    finishGpu(c, sample);
    if (dimensions.width) cpu("update_texture_cpu", start, w, h);
}

void STDMETHODCALLTYPE copyHook(ID3D11DeviceContext* c, ID3D11Resource* dest, ID3D11Resource* src)
{
    const auto dim = sizeOf(dest);
    const auto start = qpc();
    auto* sample = beginGpu(c, "gpu_copy_texture", start, dim.width, dim.height);
    copy(c, dest, src);
    finishGpu(c, sample);
    if (dim.width) cpu("copy_texture_cpu", start, dim.width, dim.height);
}

void STDMETHODCALLTYPE copyRegionHook(ID3D11DeviceContext* c, ID3D11Resource* dest, UINT d, UINT x, UINT y,
                                      UINT z, ID3D11Resource* src, UINT s, const D3D11_BOX* box)
{
    const auto dim = sizeOf(src);
    const auto w = box ? box->right - box->left : dim.width;
    const auto h = box ? box->bottom - box->top : dim.height;
    const auto start = qpc();
    auto* sample = dim.width ? beginGpu(c, "gpu_copy_texture_region", start, w, h) : nullptr;
    copyRegion(c, dest, d, x, y, z, src, s, box);
    finishGpu(c, sample);
    if (dim.width) cpu("copy_texture_region_cpu", start, w, h);
}

HRESULT STDMETHODCALLTYPE mapHook(ID3D11DeviceContext* c, ID3D11Resource* r, UINT sub,
                                  D3D11_MAP mode, UINT flags, D3D11_MAPPED_SUBRESOURCE* data)
{
    const auto start = qpc();
    const auto result = map(c, r, sub, mode, flags, data);
    cpu("map_cpu", start, static_cast<unsigned>(mode), static_cast<unsigned>(result));
    return result;
}
void STDMETHODCALLTYPE unmapHook(ID3D11DeviceContext* c, ID3D11Resource* r, UINT sub)
{
    const auto start = qpc();
    unmap(c, r, sub);
    cpu("unmap_cpu", start, sub);
}
void STDMETHODCALLTYPE mipsHook(ID3D11DeviceContext* c, ID3D11ShaderResourceView* view)
{
    const auto start = qpc();
    generateMips(c, view);
    cpu("generate_mips_cpu", start);
}

template<typename Fn>
bool hook(Fn const* cell, Fn replacement, Fn& original)
{
    if (*cell == replacement) return original != nullptr;
    if (original && *cell != original) return false;
    DWORD protection = 0;
    auto* slot = const_cast<Fn*>(cell);
    if (!VirtualProtect(slot, sizeof(Fn), PAGE_READWRITE, &protection)) return false;
    const auto previous = *cell;
    original = previous;
    const bool ok = InterlockedCompareExchangePointer(reinterpret_cast<void* volatile*>(slot),
        reinterpret_cast<void*>(replacement), reinterpret_cast<void*>(previous)) == reinterpret_cast<void*>(previous);
    DWORD ignored = 0;
    VirtualProtect(slot, sizeof(Fn), protection, &ignored);
    return ok;
}

void activate(ID3D11Device* device)
{
    if (!device || activeDevice) return;
    activeDevice = device;
    initQueries(device);

    IDXGIDevice* dxgi = nullptr;
    IDXGIAdapter* base = nullptr;
    if (SUCCEEDED(ID3D11Device_QueryInterface(device, IID_IDXGIDevice, reinterpret_cast<void**>(&dxgi)))) {
        if (SUCCEEDED(IDXGIDevice_GetAdapter(dxgi, &base))) {
            IDXGIAdapter_QueryInterface(base, IID_IDXGIAdapter3, reinterpret_cast<void**>(&adapter));
            IDXGIAdapter_Release(base);
        }
        IDXGIDevice_Release(dxgi);
    }

    unsigned flags = 0;
    if (hook(&device->lpVtbl->CreateTexture2D, textureHook, createTexture)) flags |= 1;
    if (hook(&device->lpVtbl->CreateVertexShader, vertexHook, createVS)) flags |= 2;
    if (hook(&device->lpVtbl->CreatePixelShader, pixelHook, createPS)) flags |= 4;
    if (hook(&device->lpVtbl->CreateComputeShader, computeHook, createCS)) flags |= 8;
    const auto now = qpc();
    record("device_hooks", now, now, flags, queries.front().disjoint ? 1 : 0);
}

void activateContext(ID3D11DeviceContext* context)
{
    if (!context || immediateContext.load(std::memory_order_acquire)) return;
    immediateContext.store(context, std::memory_order_release);
    unsigned flags = 0;
    if (hook(&context->lpVtbl->UpdateSubresource, updateHook, update)) flags |= 16;
    if (hook(&context->lpVtbl->CopyResource, copyHook, copy)) flags |= 32;
    if (hook(&context->lpVtbl->CopySubresourceRegion, copyRegionHook, copyRegion)) flags |= 64;
    if (hook(&context->lpVtbl->Map, mapHook, map)) flags |= 128;
    if (hook(&context->lpVtbl->Unmap, unmapHook, unmap)) flags |= 256;
    if (hook(&context->lpVtbl->GenerateMips, mipsHook, generateMips)) flags |= 512;
    const auto now = qpc();
    record("context_hooks", now, now, flags);
}

void deactivate(ID3D11Device* device)
{
    if (device != activeDevice) return;
    immediateContext.store(nullptr, std::memory_order_release);
    while (queryLock.test_and_set(std::memory_order_acquire)) SwitchToThread();
    releaseQueries();
    lastGpuSample = 0;
    queryLock.clear(std::memory_order_release);
    activeDevice = nullptr;
    if (adapter) IDXGIAdapter3_Release(adapter);
    adapter = nullptr;
}

[[maybe_unused]] void sampleVram(Tick now)
{
    if (!adapter || micros(now - lastVram) < oneSecondUs) return;
    DXGI_QUERY_VIDEO_MEMORY_INFO info{};
    lastVram = now;
    if (SUCCEEDED(IDXGIAdapter3_QueryVideoMemoryInfo(adapter, 0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &info)))
        record("vram_local_mib", now, now, static_cast<unsigned>(info.CurrentUsage >> 20),
               static_cast<unsigned>(info.Budget >> 20));
}

#ifndef FFX_GPU_TRACE_TEST
Tick previousPresent = 0;
Tick enteringPresent = 0;
Tick lastCoverage = 0;
std::atomic<unsigned> reshadeUpdates{0}, reshadeCopies{0};
reshade::api::swapchain* trackedSwapchain = nullptr;

void onDevice(reshade::api::device* device)
{
    if (device->get_api() == reshade::api::device_api::d3d11)
        activate(reinterpret_cast<ID3D11Device*>(device->get_native()));
}
void onQueue(reshade::api::command_queue* queue)
{
    if (!activeDevice || queue->get_device()->get_api() != reshade::api::device_api::d3d11) return;

    // ReShade stores its COM proxy on the native device. The application can submit
    // through a different runtime context behind this proxy after a DXGI upgrade.
    constexpr GUID proxyId{0x72299288, 0x2C68, 0x4AD8, {0x94, 0x5D, 0x2B, 0xFB, 0x5A, 0xA9, 0xC6, 0x09}};
    IUnknown* proxy = nullptr;
    UINT bytes = sizeof proxy;
    ID3D11Device* device = nullptr;
    if (SUCCEEDED(ID3D11Device_GetPrivateData(activeDevice, proxyId, &bytes, &proxy)) && proxy &&
        bytes == sizeof proxy &&
        SUCCEEDED(IUnknown_QueryInterface(proxy, IID_ID3D11Device, reinterpret_cast<void**>(&device)))) {
        ID3D11DeviceContext* context = nullptr;
        ID3D11Device_GetImmediateContext(device, &context);
        if (context) {
            activateContext(context);
            ID3D11DeviceContext_Release(context);
        }
        ID3D11Device_Release(device);
    }
    if (!immediateContext.load(std::memory_order_acquire))
        activateContext(reinterpret_cast<ID3D11DeviceContext*>(queue->get_native()));
}

bool onTextureUpdate(reshade::api::device*, const reshade::api::subresource_data&,
                     reshade::api::resource, uint32_t, const reshade::api::subresource_box*)
{
    reshadeUpdates.fetch_add(1, std::memory_order_relaxed);
    return false;
}

bool onTextureCopy(reshade::api::command_list*, reshade::api::resource, uint32_t,
                   const reshade::api::subresource_box*, reshade::api::resource, uint32_t,
                   const reshade::api::subresource_box*, reshade::api::filter_mode)
{
    reshadeCopies.fetch_add(1, std::memory_order_relaxed);
    return false;
}

void onDeviceExit(reshade::api::device* device)
{
    if (device->get_api() == reshade::api::device_api::d3d11)
        deactivate(reinterpret_cast<ID3D11Device*>(device->get_native()));
}
void onSwapExit(reshade::api::swapchain* chain, bool)
{
    if (chain == trackedSwapchain) {
        trackedSwapchain = nullptr;
        previousPresent = enteringPresent = 0;
    }
}
void onPresent(reshade::api::command_queue* queue, reshade::api::swapchain* chain, const reshade::api::rect*,
               const reshade::api::rect*, uint32_t, const reshade::api::rect*)
{
    if (chain->get_device()->get_api() != reshade::api::device_api::d3d11) return;
    if (!trackedSwapchain) trackedSwapchain = chain;
    if (chain != trackedSwapchain) return;
    const auto now = qpc();
    if (previousPresent && micros(now - previousPresent) >= 12500)
        record("frame_interval", previousPresent, now);
    previousPresent = enteringPresent = now;
    if (!immediateContext.load(std::memory_order_acquire)) onQueue(queue);
    if (!lastCoverage || micros(now - lastCoverage) >= oneSecondUs) {
        lastCoverage = now;
        record("native_call_counts", now, now, textureCalls.exchange(0, std::memory_order_relaxed),
               updateCalls.exchange(0, std::memory_order_relaxed));
        record("reshade_transfer_counts", now, now, reshadeUpdates.exchange(0, std::memory_order_relaxed),
               reshadeCopies.exchange(0, std::memory_order_relaxed));
    }
    pollGpu();
    sampleVram(now);
}
void onPresentDone(reshade::api::command_queue*, reshade::api::swapchain* chain)
{
    if (chain == trackedSwapchain && enteringPresent) cpu("present_cpu", enteringPresent);
}
#endif
}

#ifndef FFX_GPU_TRACE_TEST
extern "C" __declspec(dllexport) const char* const NAME = "FFX GPU Texture Trace";
extern "C" __declspec(dllexport) const char* const DESCRIPTION = "Records D3D11 texture creation, transfers, shader creation and frame timing.";
BOOL WINAPI DllMain(HMODULE module, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH) {
        wchar_t path[MAX_PATH]{};
        const auto length = GetModuleFileNameW(nullptr, path, MAX_PATH);
        if (length < 8 || length >= MAX_PATH || _wcsicmp(path + length - 8, L"\\FFX.exe") != 0)
            return FALSE;
        DisableThreadLibraryCalls(module);
        QueryPerformanceFrequency(&frequency);
        if (!reshade::register_addon(module)) return FALSE;
        HMODULE pinned = nullptr;
        if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
                               reinterpret_cast<LPCWSTR>(&writer), &pinned)) {
            reshade::unregister_addon(module);
            return FALSE;
        }
        writerThread = CreateThread(nullptr, 0, writer, nullptr, 0, nullptr);
        if (writerThread) CloseHandle(writerThread);
        reshade::register_event<reshade::addon_event::init_device>(onDevice);
        reshade::register_event<reshade::addon_event::update_texture_region>(onTextureUpdate);
        reshade::register_event<reshade::addon_event::copy_texture_region>(onTextureCopy);
        reshade::register_event<reshade::addon_event::destroy_device>(onDeviceExit);
        reshade::register_event<reshade::addon_event::destroy_swapchain>(onSwapExit);
        reshade::register_event<reshade::addon_event::present>(onPresent);
        reshade::register_event<reshade::addon_event::finish_present>(onPresentDone);
    } else if (reason == DLL_PROCESS_DETACH) {
        stopWriter.store(true, std::memory_order_relaxed);
    }
    return TRUE;
}
#else
int main()
{
    QueryPerformanceFrequency(&frequency);
    ID3D11Device* device = nullptr;
    ID3D11DeviceContext* context = nullptr;
    const auto hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0,
                                     D3D11_SDK_VERSION, &device, nullptr, &context);
    if (FAILED(hr)) return 1;
    activate(device);
    activateContext(context);
    if (!createTexture || !update || !copy || !copyRegion || !map || !unmap) return 2;
    constexpr UINT side = 1024;
    D3D11_TEXTURE2D_DESC desc{side, side, 1, 1, DXGI_FORMAT_R8G8B8A8_UNORM,
        {1, 0}, D3D11_USAGE_DEFAULT, D3D11_BIND_SHADER_RESOURCE, 0, 0};
    ID3D11Texture2D* tex = nullptr;
    if (FAILED(ID3D11Device_CreateTexture2D(device, &desc, nullptr, &tex))) return 3;
    auto* data = VirtualAlloc(nullptr, side * side * 4, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!data) return 4;
    ID3D11DeviceContext_UpdateSubresource(context, reinterpret_cast<ID3D11Resource*>(tex), 0,
                                       nullptr, data, side * 4, 0);
    ID3D11DeviceContext_Flush(context);
    for (unsigned i = 0; i != 30 && samplesMade.load() == 0; ++i) {
        Sleep(10);
        pollGpu();
    }
    VirtualFree(data, 0, MEM_RELEASE);
    ID3D11Texture2D_Release(tex);
    deactivate(device);
    ID3D11DeviceContext_Release(context);
    ID3D11Device_Release(device);
    if (!samplesMade.load()) return 5;
    std::printf("WARP texture hook and nonblocking GPU timestamp sample passed.\n");
    return 0;
}
#endif
