// SPDX-License-Identifier: Apache-2.0
// FFXShaderReuse: FFX creates the same vertex shaders, pixel shaders and input layouts again for every model it
// loads (thousands of creates of a few hundred distinct objects per session). Each create compiles in the driver,
// and on AMD's D3D11 driver that holds a device lock the render thread then waits on, so frames hitch when new
// models load. Shaders and input layouts are immutable, so a create whose bytecode (and, for layouts, element
// list) matches a live object returns that object with an extra reference instead of compiling again.
//
// Hooks: ID3D11Device vtable slots 11 (CreateInputLayout), 12 (CreateVertexShader), 15 (CreatePixelShader).
// The vtable is shared by every device of the same implementation, so patching the vtable of a throwaway device
// covers the game's device too, whichever d3d11.dll is loaded (system, ReShade or DXVK).
// Objects are kept for the whole session (one reference held by this module), which is what makes reuse safe.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d11.h>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <iterator>
#include <unordered_map>
#pragma comment(lib, "d3d11.lib")

namespace {
using CreateShader = HRESULT(STDMETHODCALLTYPE*)(ID3D11Device*, const void*, SIZE_T, ID3D11ClassLinkage*, void**);
using CreateLayout = HRESULT(STDMETHODCALLTYPE*)(ID3D11Device*, const D3D11_INPUT_ELEMENT_DESC*, UINT, const void*,
                                                  SIZE_T, ID3D11InputLayout**);
CreateShader origVertex = nullptr, origPixel = nullptr;
CreateLayout origLayout = nullptr;

SRWLOCK lock = SRWLOCK_INIT;
std::unordered_map<uint64_t, IUnknown*>* objects = nullptr;
volatile LONG created = 0, reused = 0;
bool logging = true;
wchar_t logPath[MAX_PATH]{};

uint64_t fnv(const void* data, size_t size, uint64_t h)
{
    auto p = static_cast<const uint8_t*>(data);
    for (size_t i = 0; i < size; i++) { h ^= p[i]; h *= 0x100000001B3ull; }
    return h;
}
uint64_t start(uint64_t kind, size_t size) { return fnv(&size, sizeof(size), 0xCBF29CE484222325ull ^ kind); }

void logLine(const char* text)
{
    if (!logging || !logPath[0]) return;
    HANDLE f = CreateFileW(logPath, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return;
    SYSTEMTIME t; GetLocalTime(&t);
    char line[512];
    int n = std::snprintf(line, sizeof(line), "%02u:%02u:%02u.%03u | %s\r\n", t.wHour, t.wMinute, t.wSecond,
                          t.wMilliseconds, text);
    DWORD written = 0;
    if (n > 0) WriteFile(f, line, DWORD(n), &written, nullptr);
    CloseHandle(f);
}

bool findLive(uint64_t key, void** out)
{
    if (!out) return false;
    IUnknown* found = nullptr;
    AcquireSRWLockShared(&lock);
    auto it = objects->find(key);
    if (it != objects->end()) { found = it->second; found->AddRef(); }
    ReleaseSRWLockShared(&lock);
    if (!found) return false;
    *out = found;
    LONG n = InterlockedIncrement(&reused);
    if (n % 1000 == 0) {
        char text[128];
        std::snprintf(text, sizeof(text), "%ld creates reused, %ld compiled", n, created);
        logLine(text);
    }
    return true;
}

void remember(uint64_t key, void* object)
{
    if (!object) return;
    auto unknown = static_cast<IUnknown*>(object);
    AcquireSRWLockExclusive(&lock);
    if (objects->emplace(key, unknown).second) unknown->AddRef();
    ReleaseSRWLockExclusive(&lock);
    InterlockedIncrement(&created);
}

HRESULT STDMETHODCALLTYPE hookVertex(ID3D11Device* d, const void* code, SIZE_T size, ID3D11ClassLinkage* link, void** out)
{
    if (!code || link) return origVertex(d, code, size, link, out);
    const uint64_t key = fnv(code, size, start('V', size));
    if (findLive(key, out)) return S_OK;
    HRESULT hr = origVertex(d, code, size, link, out);
    if (SUCCEEDED(hr) && out) remember(key, *out);
    return hr;
}

HRESULT STDMETHODCALLTYPE hookPixel(ID3D11Device* d, const void* code, SIZE_T size, ID3D11ClassLinkage* link, void** out)
{
    if (!code || link) return origPixel(d, code, size, link, out);
    const uint64_t key = fnv(code, size, start('P', size));
    if (findLive(key, out)) return S_OK;
    HRESULT hr = origPixel(d, code, size, link, out);
    if (SUCCEEDED(hr) && out) remember(key, *out);
    return hr;
}

HRESULT STDMETHODCALLTYPE hookLayout(ID3D11Device* d, const D3D11_INPUT_ELEMENT_DESC* elements, UINT count,
                                     const void* code, SIZE_T size, ID3D11InputLayout** out)
{
    if (!elements || !code) return origLayout(d, elements, count, code, size, out);
    uint64_t key = fnv(code, size, start('L', size));
    key = fnv(&count, sizeof(count), key);
    for (UINT i = 0; i < count; i++) {
        if (elements[i].SemanticName) key = fnv(elements[i].SemanticName, std::strlen(elements[i].SemanticName) + 1, key);
        const uint32_t fields[6] = {elements[i].SemanticIndex, uint32_t(elements[i].Format), elements[i].InputSlot,
                                    elements[i].AlignedByteOffset, uint32_t(elements[i].InputSlotClass),
                                    elements[i].InstanceDataStepRate};
        key = fnv(fields, sizeof(fields), key);
    }
    if (findLive(key, reinterpret_cast<void**>(out))) return S_OK;
    HRESULT hr = origLayout(d, elements, count, code, size, out);
    if (SUCCEEDED(hr) && out) remember(key, *out);
    return hr;
}

bool patch(void** slot, void* hook, void** original)
{
    DWORD old = 0;
    if (!VirtualProtect(slot, sizeof(void*), PAGE_EXECUTE_READWRITE, &old)) return false;
    *original = *slot;
    *slot = hook;
    DWORD ignored = 0;
    VirtualProtect(slot, sizeof(void*), old, &ignored);
    FlushInstructionCache(GetCurrentProcess(), slot, sizeof(void*));
    return true;
}

bool install()
{
    objects = new std::unordered_map<uint64_t, IUnknown*>();
    objects->reserve(4096);
    ID3D11Device* device = nullptr;
    if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &device,
                                 nullptr, nullptr)) || !device) {
        logLine("no Direct3D 11 device: inactive");
        return false;
    }
    void** table = *reinterpret_cast<void***>(device);
    bool ok = table[12] != reinterpret_cast<void*>(&hookVertex) &&
              patch(&table[11], reinterpret_cast<void*>(&hookLayout), reinterpret_cast<void**>(&origLayout)) &&
              patch(&table[12], reinterpret_cast<void*>(&hookVertex), reinterpret_cast<void**>(&origVertex)) &&
              patch(&table[15], reinterpret_cast<void*>(&hookPixel), reinterpret_cast<void**>(&origPixel));
    device->Release();
    logLine(ok ? "hooks on: duplicate shader and input layout creates return the live object"
               : "hooks FAILED: inactive");
    return ok;
}
} // namespace

#ifdef FFX_SHADER_REUSE_TEST
#include <d3dcompiler.h>
#pragma comment(lib, "d3dcompiler.lib")
int main()
{
    logging = false;
    if (!install()) { std::puts("FAIL: install"); return 1; }
    ID3D11Device* device = nullptr;
    if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &device,
                                 nullptr, nullptr))) { std::puts("FAIL: device"); return 1; }
    const char vs[] = "float4 main(float4 p : POSITION) : SV_Position { return p; }";
    const char ps[] = "float4 main() : SV_Target { return float4(1, 0, 0, 1); }";
    const char ps2[] = "float4 main() : SV_Target { return float4(0, 1, 0, 1); }";
    ID3DBlob *bv = nullptr, *bp = nullptr, *bp2 = nullptr;
    D3DCompile(vs, sizeof(vs) - 1, nullptr, nullptr, nullptr, "main", "vs_4_0", 0, 0, &bv, nullptr);
    D3DCompile(ps, sizeof(ps) - 1, nullptr, nullptr, nullptr, "main", "ps_4_0", 0, 0, &bp, nullptr);
    D3DCompile(ps2, sizeof(ps2) - 1, nullptr, nullptr, nullptr, "main", "ps_4_0", 0, 0, &bp2, nullptr);
    if (!bv || !bp || !bp2) { std::puts("FAIL: compile"); return 1; }
    ID3D11VertexShader *v1 = nullptr, *v2 = nullptr;
    ID3D11PixelShader *p1 = nullptr, *p2 = nullptr, *p3 = nullptr;
    device->CreateVertexShader(bv->GetBufferPointer(), bv->GetBufferSize(), nullptr, &v1);
    device->CreateVertexShader(bv->GetBufferPointer(), bv->GetBufferSize(), nullptr, &v2);
    device->CreatePixelShader(bp->GetBufferPointer(), bp->GetBufferSize(), nullptr, &p1);
    device->CreatePixelShader(bp->GetBufferPointer(), bp->GetBufferSize(), nullptr, &p2);
    device->CreatePixelShader(bp2->GetBufferPointer(), bp2->GetBufferSize(), nullptr, &p3);
    const D3D11_INPUT_ELEMENT_DESC e1[] = {{"POSITION", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0}};
    const D3D11_INPUT_ELEMENT_DESC e2[] = {{"POSITION", 0, DXGI_FORMAT_R8G8B8A8_UNORM, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0}};
    ID3D11InputLayout *l1 = nullptr, *l2 = nullptr, *l3 = nullptr;
    device->CreateInputLayout(e1, 1, bv->GetBufferPointer(), bv->GetBufferSize(), &l1);
    device->CreateInputLayout(e1, 1, bv->GetBufferPointer(), bv->GetBufferSize(), &l2);
    device->CreateInputLayout(e2, 1, bv->GetBufferPointer(), bv->GetBufferSize(), &l3);
    bool pass = v1 && v1 == v2 && p1 && p1 == p2 && p3 && p3 != p1 && l1 && l1 == l2 && l3 && l3 != l1 &&
                reused == 3 && created == 5;
    // Releasing the game's references must leave the shared object alive for the next create.
    v1->Release(); v2->Release();
    ID3D11VertexShader* v3 = nullptr;
    device->CreateVertexShader(bv->GetBufferPointer(), bv->GetBufferSize(), nullptr, &v3);
    pass = pass && v3 == v1;
    std::printf("%s: %ld compiled, %ld reused\n", pass ? "PASS" : "FAIL", created, reused);
    return pass ? 0 : 1;
}
#else
struct ModuleVersion { int major, minor, patch; };
bool active = false;

DWORD WINAPI installThread(LPVOID)
{
    active = install();
    return 0;
}

extern "C" __declspec(dllexport) const char* FF10HgetName()
{
    return active ? "FFX Shader Reuse" : "FFX Shader Reuse (inactive)";
}
extern "C" __declspec(dllexport) ModuleVersion FF10HgetVer() { return {0, 1, 0}; }

BOOL WINAPI DllMain(HMODULE module, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH) {
        wchar_t path[MAX_PATH]{};
        const auto length = GetModuleFileNameW(nullptr, path, MAX_PATH);
        if (length < 8 || length >= MAX_PATH || lstrcmpiW(path + length - 8, L"\\FFX.exe") != 0) return TRUE;
        DisableThreadLibraryCalls(module);
        HMODULE pinned = nullptr;
        if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
                                reinterpret_cast<LPCWSTR>(&installThread), &pinned)) return TRUE;
        const size_t directory = length - 7;
        constexpr wchar_t ini[] = L"modules\\config\\FFXShaderReuse.ini";
        constexpr wchar_t log[] = L"ffx-shader-reuse.log";
        wchar_t iniPath[MAX_PATH]{};
        if (directory + std::size(ini) <= MAX_PATH) {
            std::wmemcpy(iniPath, path, directory);
            std::wmemcpy(iniPath + directory, ini, std::size(ini));
            logging = GetPrivateProfileIntW(L"Diagnostics", L"Logging", 1, iniPath) != 0;
            if (GetPrivateProfileIntW(L"General", L"Enabled", 1, iniPath) == 0) return TRUE;
        }
        if (directory + std::size(log) <= MAX_PATH) {
            std::wmemcpy(logPath, path, directory);
            std::wmemcpy(logPath + directory, log, std::size(log));
        }
        if (HANDLE t = CreateThread(nullptr, 0, installThread, nullptr, 0, nullptr)) CloseHandle(t);
    }
    return TRUE;
}
#endif
