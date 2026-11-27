// SPDX-License-Identifier: Apache-2.0
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <cstdint>
#include <cstring>
#include <cwchar>
#include <iterator>
#ifdef FFX_RUNTIME_FIXES_TEST
#include <cstdio>
#endif

namespace {
struct Image {
    BYTE* base;
    const IMAGE_NT_HEADERS32* nt;
};

Image x86Image(HMODULE module)
{
    auto* base = reinterpret_cast<BYTE*>(module);
    if (!base) return {};
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew < sizeof(IMAGE_DOS_HEADER))
        return {};
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS32*>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE || nt->FileHeader.Machine != IMAGE_FILE_MACHINE_I386 ||
        nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR32_MAGIC) return {};
    return {base, nt};
}

bool replaceDword(volatile LONG* cell, LONG expected, LONG replacement, DWORD writable)
{
    DWORD previousProtection = 0;
    if (!VirtualProtect(const_cast<LONG*>(cell), sizeof(*cell), writable, &previousProtection))
        return false;
    const bool replaced = InterlockedCompareExchange(cell, replacement, expected) == expected;
    DWORD ignored = 0;
    VirtualProtect(const_cast<LONG*>(cell), sizeof(*cell), previousProtection, &ignored);
    if (replaced && writable == PAGE_EXECUTE_READWRITE)
        FlushInstructionCache(GetCurrentProcess(), const_cast<LONG*>(cell), sizeof(*cell));
    return replaced;
}

using RegisterClassFn = ATOM(WINAPI*)(const WNDCLASSA*);
RegisterClassFn registerOriginal = nullptr;

ATOM WINAPI registerWindow(const WNDCLASSA* description)
{
    if (!description || !description->lpszClassName || IS_INTRESOURCE(description->lpszClassName) ||
        lstrcmpA(description->lpszClassName, "PhyreFrameworkClass") != 0)
        return registerOriginal(description);

    WNDCLASSA windowClass = *description;
    const DWORD callerError = GetLastError();
    const auto brush = static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
    if (brush) windowClass.hbrBackground = brush;
    SetLastError(callerError);
    return registerOriginal(&windowClass);
}

IMAGE_THUNK_DATA32* registerClassImport(const Image& image)
{
    const auto rva = image.nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT].VirtualAddress;
    if (!rva) return nullptr;
    const auto* libraries = reinterpret_cast<const IMAGE_IMPORT_DESCRIPTOR*>(image.base + rva);
    for (auto* lib = libraries; lib->Name; ++lib) {
        if (!lib->OriginalFirstThunk || !lib->FirstThunk ||
            lstrcmpiA(reinterpret_cast<const char*>(image.base + lib->Name), "USER32.dll") != 0) continue;
        const auto* symbol = reinterpret_cast<const IMAGE_THUNK_DATA32*>(image.base + lib->OriginalFirstThunk);
        auto* target = reinterpret_cast<IMAGE_THUNK_DATA32*>(image.base + lib->FirstThunk);
        for (; symbol->u1.AddressOfData; ++symbol, ++target) {
            if (IMAGE_SNAP_BY_ORDINAL32(symbol->u1.Ordinal)) continue;
            const auto* name = reinterpret_cast<const IMAGE_IMPORT_BY_NAME*>(image.base + symbol->u1.AddressOfData);
            if (lstrcmpA(reinterpret_cast<const char*>(name->Name), "RegisterClassA") == 0) return target;
        }
    }
    return nullptr;
}

bool installBlackWindow(const Image& image)
{
    auto* entry = registerClassImport(image);
    if (!entry || !entry->u1.Function) return false;
    const auto previous = entry->u1.Function;
    if (previous == reinterpret_cast<DWORD>(&registerWindow)) return false;
    // Chain any existing RegisterClassA hook, including the cursor module.
    registerOriginal = reinterpret_cast<RegisterClassFn>(previous);
    return replaceDword(reinterpret_cast<volatile LONG*>(&entry->u1.Function),
                        static_cast<LONG>(previous), reinterpret_cast<LONG>(&registerWindow), PAGE_READWRITE);
}

using PlayCue = void(__cdecl*)(int sound, int flags);
PlayCue playCue = nullptr;

struct CueHeader {
    uint16_t marker;
    int16_t sound;
};
static_assert(sizeof(CueHeader) == 4);

void __cdecl playOptionalCue(const void* address, int)
{
    CueHeader cue{};
    SIZE_T copied = 0;
    if (ReadProcessMemory(GetCurrentProcess(), address, &cue, sizeof cue, &copied) &&
        copied == sizeof cue && cue.marker == 0x4457) {
        // The stock WD decoder sign-extends the cue ID and forces flags zero.
        playCue(cue.sound, 0);
    }
}

bool installBattleCueGuard(const Image& image)
{
    constexpr DWORD bankRva = 0x4874A5;
    constexpr DWORD decoderRva = 0x41CF90;
    constexpr DWORD playerRva = 0x27A360;
    constexpr BYTE bank[] = {0xFF,0x75,0x0C,0xFF,0x75,0x08,0xE8,0xE0,0x5A,0xF9,0xFF,
                             0x83,0xC4,0x08,0xE8,0x58,0xFE,0xFF,0xFF,0xA1};
    constexpr BYTE decoder[] = {0x55,0x8B,0xEC,0x8B,0x45,0x08,0xB9,0x57,0x44,0x00,0x00,
                                 0x66,0x39,0x08,0x75,0x14,0xC7,0x45,0x0C,0x00,0x00,0x00,0x00,
                                 0x0F,0xBF,0x40,0x02,0x89,0x45,0x08,0x5D,0xE9,0xAC,0xD3,0xE5,0xFF,
                                 0x5D,0xC3};
    constexpr BYTE playerStart[] = {0x55,0x8B,0xEC,0xFF,0x75,0x0C,0x8B,0x0D};
    constexpr BYTE playerEnd[] = {0xFF,0x75,0x08,0x6A,0x30,0xE8,0x4A,0xFF,0x07,0x00,0x5D,0xC3};
    if (image.nt->OptionalHeader.SizeOfImage < bankRva + sizeof(bank) ||
        std::memcmp(image.base + bankRva, bank, sizeof bank) != 0 ||
        std::memcmp(image.base + decoderRva, decoder, sizeof decoder) != 0 ||
        std::memcmp(image.base + playerRva, playerStart, sizeof playerStart) != 0 ||
        std::memcmp(image.base + playerRva + 12, playerEnd, sizeof playerEnd) != 0)
        return false;

    auto* call = image.base + bankRva + 6;
    playCue = reinterpret_cast<PlayCue>(image.base + playerRva);
    const auto displacement = static_cast<LONG>(reinterpret_cast<uintptr_t>(&playOptionalCue) -
                                                reinterpret_cast<uintptr_t>(call + 5));
    return replaceDword(reinterpret_cast<volatile LONG*>(call + 1), static_cast<LONG>(0xFFF95AE0),
                        displacement, PAGE_EXECUTE_READWRITE);
}

volatile LONG movieVolume = 50;
alignas(8) volatile LONG64 moviePcmScaleBits = 0;
bool movieVolumeInstalled = false;
wchar_t runtimeIni[MAX_PATH]{};

int clampMovieVolume(int value)
{
    return value < 0 ? 0 : value > 100 ? 100 : value;
}

void applyMovieVolume(int level)
{
    const double scale = 32767.0 * (static_cast<double>(level) / 50.0);
    LONG64 bits = 0;
    std::memcpy(&bits, &scale, sizeof bits);
    InterlockedExchange64(&moviePcmScaleBits, bits);
    InterlockedExchange(&movieVolume, level);
}

bool installMovieVolume(const Image& image)
{
    // The bundled FMOD Ex clamps channel volume to unity. Instead, scale the
    // movie decoder's floating-point PCM before its existing signed 16-bit
    // saturation. FMOD still handles all original movie fades and mutes.
    constexpr DWORD convertRva = 0x6272C0;
    constexpr DWORD constantRva = 0x741D70;
    constexpr BYTE convert[] = {0xD9,0x04,0xB8,0x83,0xEC,0x08,0xDC,0x0D};
    constexpr BYTE rounding[] = {0xDC,0x05};
    constexpr BYTE clamp[] = {0x3D,0xFF,0x7F,0x00,0x00,0x7E,0x0F};
    if (image.nt->OptionalHeader.SizeOfImage < constantRva + sizeof(double) ||
        std::memcmp(image.base + convertRva, convert, sizeof convert) != 0 ||
        std::memcmp(image.base + convertRva + 12, rounding, sizeof rounding) != 0 ||
        std::memcmp(image.base + convertRva + 46, clamp, sizeof clamp) != 0) return false;
    const auto* stock = image.base + constantRva;
    double stockScale = 0.0;
    std::memcpy(&stockScale, stock, sizeof stockScale);
    if (stockScale != 32767.0) return false;
    auto* operand = reinterpret_cast<volatile LONG*>(image.base + convertRva + sizeof convert);
    return replaceDword(operand, reinterpret_cast<LONG>(stock),
                        reinterpret_cast<LONG>(&moviePcmScaleBits), PAGE_EXECUTE_READWRITE);
}

// The in-game Config menu reads with -1 and sets 0–100. The module owns persistence.
int movieVolumeSetting(int value)
{
    if (!movieVolumeInstalled) return -1;
    const int current = InterlockedCompareExchange(&movieVolume, 0, 0);
    if (value < 0 || value == current) return current;
    value = clampMovieVolume(value);
    wchar_t text[8]{};
    std::swprintf(text, std::size(text), L"%d", value);
    if (!WritePrivateProfileStringW(L"Audio", L"FMVVolume", text, runtimeIni)) return current;
    applyMovieVolume(value);
    return value;
}

#ifdef FFX_RUNTIME_FIXES_TEST
unsigned calls = 0;
int cueId = 0, cueFlags = -1;
double moviePcmScale()
{
    const LONG64 bits = InterlockedCompareExchange64(&moviePcmScaleBits, 0, 0);
    double scale = 0.0;
    std::memcpy(&scale, &bits, sizeof scale);
    return scale;
}

void __cdecl verifyCue(int id, int flags)
{
    ++calls;
    cueId = id;
    cueFlags = flags;
}
#else
bool blackWindow = false, guardedCue = false;
#endif
}

#ifdef FFX_RUNTIME_FIXES_TEST
int wmain(int argc, wchar_t** argv)
{
    if (argc != 2) return 1;
    auto game = LoadLibraryExW(argv[1], nullptr, DONT_RESOLVE_DLL_REFERENCES);
    const auto image = x86Image(game);
    if (!image.base || !registerClassImport(image) || !installBattleCueGuard(image) ||
        installBattleCueGuard(image) || !installMovieVolume(image) || installMovieVolume(image)) return 2;
    FreeLibrary(game);

    const auto harness = x86Image(GetModuleHandleW(nullptr));
    if (!harness.base || !installBlackWindow(harness) || installBlackWindow(harness)) return 3;
    const auto instance = GetModuleHandleW(nullptr);
    WNDCLASSA description{};
    description.lpfnWndProc = DefWindowProcA;
    description.hInstance = instance;
    description.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    description.lpszClassName = "PhyreFrameworkClass";
    if (!RegisterClassA(&description)) return 4;
    WNDCLASSA registered{};
    LOGBRUSH color{};
    if (!GetClassInfoA(instance, description.lpszClassName, &registered) ||
        registered.lpfnWndProc != description.lpfnWndProc ||
        !GetObjectA(registered.hbrBackground, sizeof color, &color) ||
        color.lbStyle != BS_SOLID || color.lbColor != RGB(0, 0, 0)) return 5;
    UnregisterClassA(description.lpszClassName, instance);

    description.lpszClassName = "FFXRuntimeFixesTestOtherWindow";
    if (!RegisterClassA(&description) || !GetClassInfoA(instance, description.lpszClassName, &registered) ||
        registered.hbrBackground != description.hbrBackground) return 6;
    UnregisterClassA(description.lpszClassName, instance);

    playCue = verifyCue;
    const CueHeader valid{0x4457, -123}, other{0x1234, 10};
    playOptionalCue(&other, 99);
    playOptionalCue(reinterpret_cast<void*>(0x666AAAA6), 0);
    playOptionalCue(nullptr, 0);
    if (calls != 0) return 7;
    playOptionalCue(&valid, 99);
    if (calls != 1 || cueId != -123 || cueFlags != 0) return 8;

    if (movieVolumeSetting(-1) != -1 || clampMovieVolume(-20) != 0 ||
        clampMovieVolume(140) != 100) return 9;
    wchar_t directory[MAX_PATH]{};
    if (!GetTempPathW(MAX_PATH, directory) || !GetTempFileNameW(directory, L"ffx", 0, runtimeIni)) return 10;
    movieVolumeInstalled = true;
    applyMovieVolume(50);
    const bool volumePassed =
        movieVolumeSetting(-1) == 50 && moviePcmScale() == 32767.0 &&
        movieVolumeSetting(25) == 25 && moviePcmScale() == 16383.5 &&
        movieVolumeSetting(100) == 100 &&
        GetPrivateProfileIntW(L"Audio", L"FMVVolume", 50, runtimeIni) == 100 &&
        moviePcmScale() == 65534.0 && movieVolumeSetting(0) == 0 && moviePcmScale() == 0.0;
    DeleteFileW(runtimeIni);
    if (!volumePassed) return 11;
    std::puts("FFX signatures, startup window, battle cues, and persistent FMV gain passed.");
    return 0;
}
#else
struct ModuleVersion { int major, minor, patch; };

extern "C" __declspec(dllexport) const char* FF10HgetName()
{
    if (blackWindow && guardedCue) return "FFX Runtime Fixes";
    if (blackWindow) return "FFX Runtime Fixes (black startup)";
    if (guardedCue) return "FFX Runtime Fixes (battle sound guard)";
    return "FFX Runtime Fixes (inactive)";
}

extern "C" __declspec(dllexport) ModuleVersion FF10HgetVer() { return {0, 2, 0}; }

extern "C" __declspec(dllexport) int __cdecl FFXGetSetFmvVolume(int value)
{
    return movieVolumeSetting(value);
}

BOOL WINAPI DllMain(HMODULE module, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH) {
        wchar_t path[MAX_PATH]{};
        const auto length = GetModuleFileNameW(nullptr, path, MAX_PATH);
        if (length < 8 || length >= MAX_PATH || lstrcmpiW(path + length - 8, L"\\FFX.exe") != 0)
            return TRUE;
        const auto image = x86Image(GetModuleHandleW(nullptr));
        if (!image.base) return TRUE;
        DisableThreadLibraryCalls(module);
        HMODULE pinned = nullptr;
        if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
                                reinterpret_cast<LPCWSTR>(&registerWindow), &pinned)) return TRUE;
        blackWindow = installBlackWindow(image);
        guardedCue = installBattleCueGuard(image);
        constexpr wchar_t suffix[] = L"modules\\config\\FFXRuntimeFixes.ini";
        const size_t directoryLength = length - 7; // includes the backslash before FFX.exe
        if (directoryLength + std::size(suffix) <= MAX_PATH) {
            std::wmemcpy(runtimeIni, path, directoryLength);
            std::wmemcpy(runtimeIni + directoryLength, suffix, std::size(suffix));
            applyMovieVolume(clampMovieVolume(static_cast<int>(
                GetPrivateProfileIntW(L"Audio", L"FMVVolume", 50, runtimeIni))));
            movieVolumeInstalled = installMovieVolume(image);
        }
    }
    return TRUE;
}
#endif
