#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <filesystem>
#include <iterator>
#include <deque>
#include <fstream>
#include <limits>
#include <memory>
#include <new>
#include <string>
#include <unordered_set>
#include <unordered_map>
#include <vector>

namespace {
namespace fs = std::filesystem;
using Bytes = std::uint64_t;
constexpr Bytes mib = 1024 * 1024;
constexpr Bytes sessionBudget = 2048 * mib;
constexpr Bytes maxTexture = 48 * mib;
constexpr Bytes residentBudget = 1024 * mib;
constexpr size_t maxResidentFiles = 128;
constexpr size_t maxPath = 768;
constexpr wchar_t assetRoot[] = L"ffx_data\\gamedata\\ps3data\\";
using OpenFile = decltype(&CreateFileW);
using FileAttributes = decltype(&GetFileAttributesExW);
using Read = decltype(&ReadFile);
using Close = decltype(&CloseHandle);

OpenFile originalOpen = CreateFileW;
FileAttributes originalAttributes = GetFileAttributesExW;
Read originalRead = ReadFile;
Close originalClose = CloseHandle;

std::array<std::atomic<HANDLE>, 2048> handles{};
LARGE_INTEGER frequency{};
std::atomic<unsigned> readCalls{0}, slowReads{0}, slowOpens{0}, maxReadUs{0}, maxOpenUs{0};
std::atomic<Bytes> bytesRead{0}, residentBytesRead{0}, residentBytes{0};
std::atomic<unsigned> residentHits{0};
std::atomic<std::uint64_t> lastTextureRead{0};

// The actual file identity prevents aliases and a replaced override from serving
// bytes from a different file. Cacheable game handles deny writes and deletes.
struct FileStamp {
    DWORD volume = 0, indexHigh = 0, indexLow = 0;
    ULONGLONG version = 0, size = 0;

    bool operator==(const FileStamp& other) const
    {
        return volume == other.volume && indexHigh == other.indexHigh && indexLow == other.indexLow &&
               version == other.version && size == other.size;
    }
};

FileStamp inspectFile(HANDLE file)
{
    BY_HANDLE_FILE_INFORMATION info{};
    if (!GetFileInformationByHandle(file, &info)) return {};
    return {info.dwVolumeSerialNumber, info.nFileIndexHigh, info.nFileIndexLow,
            (ULONGLONG(info.ftLastWriteTime.dwHighDateTime) << 32) | info.ftLastWriteTime.dwLowDateTime,
            (ULONGLONG(info.nFileSizeHigh) << 32) | info.nFileSizeLow};
}

struct ResidentTexture {
    FileStamp stamp;
    std::unique_ptr<BYTE[]> data;
    std::atomic<ULONGLONG> lastUsed{0};
};

SRWLOCK residentLock = SRWLOCK_INIT;
std::vector<std::shared_ptr<ResidentTexture>> residentTextures;
std::array<SRWLOCK, 32> fileReadLocks{};
SRWLOCK namesLock = SRWLOCK_INIT;
struct FileName { HANDLE handle; FileStamp stamp; wchar_t path[maxPath]; };
std::array<FileName, 512> fileNames{};

size_t nameSlot(HANDLE handle)
{
    return (reinterpret_cast<std::uintptr_t>(handle) >> 2) % fileNames.size();
}

void rememberName(HANDLE handle, const wchar_t* path, size_t length, FileStamp stamp)
{
    AcquireSRWLockExclusive(&namesLock);
    auto& item = fileNames[nameSlot(handle)];
    item.handle = handle;
    item.stamp = stamp;
    std::wmemcpy(item.path, path, length + 1);
    ReleaseSRWLockExclusive(&namesLock);
}

std::shared_ptr<ResidentTexture> findResident(const FileStamp& stamp)
{
    if (!stamp.size) return {};
    std::shared_ptr<ResidentTexture> match;
    AcquireSRWLockShared(&residentLock);
    for (const auto& texture : residentTextures) {
        if (texture->stamp == stamp) {
            match = texture;
            break;
        }
    }
    ReleaseSRWLockShared(&residentLock);
    return match;
}

bool readResident(HANDLE file, const FileStamp& stamp, void* buffer, DWORD count, DWORD* actual)
{
    if (!buffer || !actual || !count) return false;
    const auto texture = findResident(stamp);
    if (!texture) return false;

    // Preserve the kernel file pointer for native seeks and later cache misses.
    LARGE_INTEGER position{};
    if (!SetFilePointerEx(file, {}, &position, FILE_CURRENT) || position.QuadPart < 0)
        return false;
    const auto offset = static_cast<ULONGLONG>(position.QuadPart);
    if (offset >= stamp.size || count > stamp.size - offset) return false;
    position.QuadPart += count;
    if (!SetFilePointerEx(file, position, nullptr, FILE_BEGIN)) return false;
    std::memcpy(buffer, texture->data.get() + static_cast<size_t>(offset), count);
    *actual = count;
    texture->lastUsed.store(GetTickCount64(), std::memory_order_relaxed);
    residentHits.fetch_add(1, std::memory_order_relaxed);
    residentBytesRead.fetch_add(count, std::memory_order_relaxed);
    return true;
}

struct Observation { wchar_t path[maxPath]; bool opened; unsigned readUs; DWORD readBytes; std::uint64_t readQpc; DWORD thread; };
SRWLOCK queueLock = SRWLOCK_INIT;
std::array<Observation, 192> queue{};
size_t readIndex = 0, writeIndex = 0;
HANDLE wake = nullptr;

bool endsWith(const std::wstring& value, const wchar_t* ending)
{
    const size_t length = wcslen(ending);
    return value.size() >= length &&
           _wcsicmp(value.c_str() + value.size() - length, ending) == 0;
}

bool isTexture(const wchar_t* path, size_t length)
{
    constexpr wchar_t suffix[] = L".dds.phyre";
    return length >= std::size(suffix) - 1 &&
           _wcsicmp(path + length - std::size(suffix) + 1, suffix) == 0;
}

bool isBattleTexturePath(const wchar_t* path)
{
    for (auto* part = path; part && *part; ++part) {
        if ((*part == L'\\' || *part == L'/') && _wcsnicmp(part + 1, L"btlmap", 6) == 0 &&
            (part[7] == L'\\' || part[7] == L'/')) return true;
    }
    return false;
}

bool mayBeMap(const wchar_t* path)
{
    return wcsstr(path, L"\\map\\") || wcsstr(path, L"/map/") ||
           wcsstr(path, L"\\btlmap\\") || wcsstr(path, L"/btlmap/");
}

void observe(const wchar_t* path, size_t length, bool opened, unsigned readUs = 0, DWORD readBytes = 0)
{
    if (!length || length >= maxPath || !TryAcquireSRWLockExclusive(&queueLock)) return;
    const size_t next = (writeIndex + 1) % queue.size();
    if (next != readIndex) {
        std::wmemcpy(queue[writeIndex].path, path, length + 1);
        queue[writeIndex].opened = opened;
        queue[writeIndex].readUs = readUs;
        queue[writeIndex].readBytes = readBytes;
        LARGE_INTEGER now{};
        if (readUs) QueryPerformanceCounter(&now);
        queue[writeIndex].readQpc = static_cast<std::uint64_t>(now.QuadPart);
        queue[writeIndex].thread = readUs ? GetCurrentThreadId() : 0;
        writeIndex = next;
        SetEvent(wake);
    }
    ReleaseSRWLockExclusive(&queueLock);
}

std::atomic<HANDLE>& handleSlot(HANDLE handle)
{
    return handles[(reinterpret_cast<std::uintptr_t>(handle) >> 2) % handles.size()];
}

void maximum(std::atomic<unsigned>& location, unsigned value)
{
    auto current = location.load(std::memory_order_relaxed);
    while (current < value &&
           !location.compare_exchange_weak(current, value, std::memory_order_relaxed)) {}
}

unsigned elapsedUs(LARGE_INTEGER start)
{
    LARGE_INTEGER now{};
    QueryPerformanceCounter(&now);
    return static_cast<unsigned>((now.QuadPart - start.QuadPart) * 1000000 / frequency.QuadPart);
}

HANDLE WINAPI openHook(LPCWSTR path, DWORD access, DWORD share, LPSECURITY_ATTRIBUTES security,
                       DWORD disposition, DWORD flags, HANDLE templateFile)
{
    const DWORD originalError = GetLastError();
    const size_t length = path ? wcsnlen(path, maxPath) : 0;
    const bool texture = length < maxPath && access && disposition == OPEN_EXISTING &&
                         isTexture(path, length);
    LARGE_INTEGER start{};
    if (texture) QueryPerformanceCounter(&start);
    SetLastError(originalError);
    const auto result = originalOpen(path, access, share, security, disposition, flags, templateFile);
    const auto error = GetLastError();
    if (texture && result != INVALID_HANDLE_VALUE) {
        const auto elapsed = elapsedUs(start);
        maximum(maxOpenUs, elapsed);
        if (elapsed > 2000) slowOpens.fetch_add(1, std::memory_order_relaxed);
        handleSlot(result).store(result, std::memory_order_relaxed);
        const bool eligible = (access == GENERIC_READ || access == FILE_READ_DATA) &&
                              !(share & (FILE_SHARE_WRITE | FILE_SHARE_DELETE)) &&
                              !(flags & (FILE_FLAG_OVERLAPPED | FILE_FLAG_NO_BUFFERING)) &&
                              isBattleTexturePath(path);
        rememberName(result, path, length, eligible ? inspectFile(result) : FileStamp{});
        observe(path, length, true);
    }
    SetLastError(error);
    return result;
}

BOOL WINAPI attributesHook(LPCWSTR path, GET_FILEEX_INFO_LEVELS level, LPVOID info)
{
    const auto result = originalAttributes(path, level, info);
    const auto error = GetLastError();
    if (path && mayBeMap(path)) observe(path, wcsnlen(path, maxPath), false);
    SetLastError(error);
    return result;
}

BOOL WINAPI readHook(HANDLE file, LPVOID buffer, DWORD count, LPDWORD actual, LPOVERLAPPED overlapped)
{
    const auto errorBefore = GetLastError();
    const bool track = !overlapped && file && handleSlot(file).load(std::memory_order_relaxed) == file;
    LARGE_INTEGER start{};
    if (track) QueryPerformanceCounter(&start);
    FileStamp stamp{};
    if (track) {
        AcquireSRWLockShared(&namesLock);
        const auto& entry = fileNames[nameSlot(file)];
        if (entry.handle == file) stamp = entry.stamp;
        ReleaseSRWLockShared(&namesLock);
    }
    auto& serial = fileReadLocks[nameSlot(file) % fileReadLocks.size()];
    if (stamp.size) AcquireSRWLockExclusive(&serial);
    const bool served = stamp.size && readResident(file, stamp, buffer, count, actual);
    SetLastError(errorBefore);
    const BOOL result = served ? TRUE : originalRead(file, buffer, count, actual, overlapped);
    const auto error = GetLastError();
    if (stamp.size) ReleaseSRWLockExclusive(&serial);
    if (track) {
        const auto elapsed = elapsedUs(start);
        readCalls.fetch_add(1, std::memory_order_relaxed);
        maximum(maxReadUs, elapsed);
        lastTextureRead.store(GetTickCount64(), std::memory_order_relaxed);
        if (elapsed > 2000) slowReads.fetch_add(1, std::memory_order_relaxed);
        if (!served && elapsed >= 8000 && TryAcquireSRWLockShared(&namesLock)) {
            const auto& entry = fileNames[nameSlot(file)];
            if (entry.handle == file)
                observe(entry.path, wcslen(entry.path), false, elapsed, result && actual ? *actual : 0);
            ReleaseSRWLockShared(&namesLock);
        }
        if (result && actual) bytesRead.fetch_add(*actual, std::memory_order_relaxed);
    }
    SetLastError(error);
    return result;
}

BOOL WINAPI closeHook(HANDLE handle)
{
    auto expected = handle;
    if (handleSlot(handle).compare_exchange_strong(expected, nullptr, std::memory_order_relaxed)) {
        AcquireSRWLockExclusive(&namesLock);
        auto& name = fileNames[nameSlot(handle)];
        if (name.handle == handle) name.handle = nullptr;
        ReleaseSRWLockExclusive(&namesLock);
    }
    return originalClose(handle);
}

// Each caller still gets a normal synchronous handle. Only the background worker
// reads ahead. The IAT hooks observe requests and leave Win32 results intact.
template<typename Fn>
bool replaceImport(HMODULE module, const char* symbol, Fn current, Fn replacement)
{
    auto* base = reinterpret_cast<BYTE*>(module);
    const auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return false;
    const auto* nt = reinterpret_cast<IMAGE_NT_HEADERS32*>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE || nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR32_MAGIC)
        return false;
    const auto rva = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT].VirtualAddress;
    if (!rva) return false;
    for (auto* library = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(base + rva); library->Name; ++library) {
        const auto* name = reinterpret_cast<const char*>(base + library->Name);
        if (_stricmp(name, "kernel32.dll") || !library->OriginalFirstThunk) continue;
        auto* names = reinterpret_cast<IMAGE_THUNK_DATA32*>(base + library->OriginalFirstThunk);
        auto* target = reinterpret_cast<IMAGE_THUNK_DATA32*>(base + library->FirstThunk);
        for (; names->u1.AddressOfData; ++names, ++target) {
            if (IMAGE_SNAP_BY_ORDINAL32(names->u1.Ordinal)) continue;
            const auto* entry = reinterpret_cast<IMAGE_IMPORT_BY_NAME*>(base + names->u1.AddressOfData);
            if (std::strcmp(reinterpret_cast<const char*>(entry->Name), symbol)) continue;
            auto* pointer = reinterpret_cast<void* volatile*>(&target->u1.Function);
            DWORD old = 0;
            if (!VirtualProtect(&target->u1.Function, sizeof(DWORD), PAGE_READWRITE, &old))
                return false;
            const bool changed = InterlockedCompareExchangePointer(
                pointer, reinterpret_cast<void*>(replacement), reinterpret_cast<void*>(current)) ==
                reinterpret_cast<void*>(current);
            DWORD ignored = 0;
            VirtualProtect(&target->u1.Function, sizeof(DWORD), old, &ignored);
            return changed;
        }
    }
    return false;
}

std::wstring assetPath(std::wstring path)
{
    std::replace(path.begin(), path.end(), L'/', L'\\');
    std::transform(path.begin(), path.end(), path.begin(),
                   [](wchar_t c) { return c >= L'A' && c <= L'Z' ? static_cast<wchar_t>(c + 32) : c; });
    const auto start = path.find(assetRoot);
    if (start == std::wstring::npos || (start && path[start - 1] != L'\\')) return {};
    auto suffix = path.substr(start + std::size(assetRoot) - 1);
    // Paths are used to enumerate mods only when these are ordinary relative asset names.
    if (suffix.empty() || suffix.front() == L'\\' || suffix.find(L"..") != std::wstring::npos ||
        suffix.find(L':') != std::wstring::npos) return {};
    return suffix;
}

std::vector<fs::path> getRoots(const fs::path& config, const fs::path& game)
{
    std::vector<fs::path> roots;
    wchar_t paths[8192]{};
    GetPrivateProfileSectionW(L"Paths", paths, DWORD(std::size(paths)), config.c_str());
    for (const wchar_t* line = paths; *line; line += wcslen(line) + 1) {
        const auto* eq = wcschr(line, L'=');
        if (!eq || !eq[1]) continue;
        fs::path root(eq + 1);
        if (root.is_relative()) root = game / root;
        roots.push_back(root / assetRoot);
    }
    if (roots.empty()) roots.push_back(game / L"data\\mods" / assetRoot);
    return roots;
}

struct Texture { fs::path relative; Bytes size; };
struct Job { std::deque<Texture> textures; Bytes remaining; bool retain = false; };

void addTree(Job& job, const fs::path& root, const fs::path& relative)
{
    std::error_code error;
    const fs::recursive_directory_iterator end;
    for (fs::recursive_directory_iterator it(root / relative,
             fs::directory_options::skip_permission_denied, error); !error && it != end; it.increment(error)) {
        std::error_code entryError;
        if (it->is_symlink(entryError)) { it.disable_recursion_pending(); continue; }
        if (!endsWith(it->path().native(), L".dds.phyre")) continue;
        const auto size = it->file_size(entryError);
        if (!entryError && size && size <= maxTexture)
            job.textures.push_back({it->path().lexically_relative(root), size});
    }
}

using SlowTextures = std::unordered_map<std::wstring, unsigned>;

SlowTextures readHotSet(const fs::path& path)
{
    SlowTextures result;
    std::wifstream in(path);
    unsigned score = 0;
    std::wstring name;
    unsigned count = 0;
    while (count++ < 384 && in >> score && std::getline(in >> std::ws, name)) {
        auto relative = assetPath(std::wstring(assetRoot) + name);
        if (!relative.empty() && endsWith(relative, L".dds.phyre"))
            result.emplace(std::move(relative), score);
    }
    return result;
}

void saveHotSet(const fs::path& path, const SlowTextures& hotSet)
{
    std::vector<std::pair<std::wstring,unsigned>> sorted(hotSet.begin(), hotSet.end());
    std::sort(sorted.begin(), sorted.end(), [](const auto& a, const auto& b) {
        return a.second > b.second;
    });
    auto temp = path;
    temp += L".tmp";
    std::wofstream out(temp, std::ios::trunc);
    if (!out) return;
    for (size_t i = 0; i < std::min<size_t>(sorted.size(), 384); ++i)
        out << sorted[i].second << L" " << sorted[i].first << L"\n";
    out.close();
    if (out) MoveFileExW(temp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING);
}

Job hotJob(const std::vector<fs::path>& roots, const SlowTextures& hotSet, bool battle)
{
    Job job{{}, battle ? residentBudget : 384 * mib, battle};
    std::vector<std::pair<std::wstring,unsigned>> sorted(hotSet.begin(), hotSet.end());
    std::sort(sorted.begin(), sorted.end(), [](const auto& a, const auto& b) {
        return a.second > b.second;
    });
    for (const auto& entry : sorted) {
        const fs::path relative(entry.first);
        if ((entry.first.rfind(L"btlmap\\", 0) == 0) != battle) continue;
        for (const auto& root : roots) {
            std::error_code error;
            const auto size = fs::file_size(root / relative, error);
            if (!error) {
                if (size && size <= maxTexture) job.textures.push_back({relative, size});
                break;
            }
        }
    }
    return job;
}

void addAreaJobs(std::deque<Job>& jobs, const std::vector<fs::path>& roots, const std::wstring& asset,
                 std::unordered_set<std::wstring>& planned, const SlowTextures& hotSet)
{
    const fs::path name(asset);
    auto part = name.begin();
    if (part == name.end() || (*part != L"map" && *part != L"btlmap")) return;
    ++part;
    if (part == name.end()) return;
    const auto area = (part++)->wstring();
    if (part == name.end() || part->wstring().size() < 6 || area.size() != 4) return;
    const auto stage = part->wstring().substr(0, 6);
    if (stage.compare(0, 4, area) != 0 || stage[4] < L'0' || stage[4] > L'9' ||
        stage[5] < L'0' || stage[5] > L'9' || !planned.insert(stage).second) return;

    Job job{{}, 384 * mib, true};
    for (const auto& root : roots) {
        const auto folder = fs::path(L"btlmap") / area;
        std::error_code error;
        for (const auto& child : fs::directory_iterator(root / folder, error)) {
            if (child.path().filename().wstring().compare(0, stage.size(), stage) == 0)
                addTree(job, root, folder / child.path().filename());
        }
    }
    if (job.textures.empty() && planned.insert(L"battle:" + area).second)
        for (const auto& root : roots) addTree(job, root, fs::path(L"btlmap") / area);
    const auto score = [&](const fs::path& relative) {
        const auto it = hotSet.find(relative.native());
        return it == hotSet.end() ? 0 : it->second;
    };
    std::stable_sort(job.textures.begin(), job.textures.end(), [&](const auto& a, const auto& b) {
        return score(a.relative) > score(b.relative);
    });
    if (!job.textures.empty()) jobs.push_front(std::move(job));
}

// The worker submits offset-zero overlapped I/O for the complete texture.
// Only fully completed files become visible to the consumer.
Bytes loadResident(const fs::path& path, Bytes limit, HANDLE ioEvent)
{
    auto texture = std::make_shared<ResidentTexture>();
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                              FILE_FLAG_OVERLAPPED | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (file == INVALID_HANDLE_VALUE) return 0;
    const auto stamp = inspectFile(file);
    if (!stamp.size || stamp.size > limit || stamp.size > maxTexture) {
        CloseHandle(file);
        return 0;
    }
    if (findResident(stamp)) {
        CloseHandle(file);
        return stamp.size;
    }
    MEMORYSTATUSEX memory{};
    memory.dwLength = sizeof memory;
    if (!GlobalMemoryStatusEx(&memory) || memory.ullAvailPhys < 3 * 1024 * mib ||
        memory.ullAvailVirtual < 1024 * mib + stamp.size) {
        CloseHandle(file);
        return 0;
    }

    texture->stamp = stamp;
    texture->data.reset(new (std::nothrow) BYTE[static_cast<size_t>(stamp.size)]);
    if (!texture->data) {
        CloseHandle(file);
        return 0;
    }
    OVERLAPPED request{};
    request.hEvent = ioEvent;
    ResetEvent(ioEvent);
    DWORD actual = 0;
    BOOL ok = ReadFile(file, texture->data.get(), static_cast<DWORD>(stamp.size), nullptr, &request);
    if (ok || GetLastError() == ERROR_IO_PENDING)
        ok = GetOverlappedResult(file, &request, &actual, TRUE);
    CloseHandle(file);
    if (!ok || actual != stamp.size) return 0;
    texture->lastUsed.store(GetTickCount64(), std::memory_order_relaxed);

    // Release evicted buffers after unlocking so consumers only wait for the lookup.
    std::array<std::shared_ptr<ResidentTexture>, maxResidentFiles> retired{};
    size_t evicted = 0;
    AcquireSRWLockExclusive(&residentLock);
    auto bytes = residentBytes.load(std::memory_order_relaxed);
    while (bytes + stamp.size > residentBudget && !residentTextures.empty()) {
        const auto oldest = std::min_element(residentTextures.begin(), residentTextures.end(),
            [](const auto& a, const auto& b) {
                return a->lastUsed.load(std::memory_order_relaxed) < b->lastUsed.load(std::memory_order_relaxed);
            });
        bytes -= (*oldest)->stamp.size;
        retired[evicted++] = std::move(*oldest);
        residentTextures.erase(oldest);
    }
    if (residentTextures.size() == maxResidentFiles) {
        bytes -= residentTextures.front()->stamp.size;
        retired[evicted++] = std::move(residentTextures.front());
        residentTextures.erase(residentTextures.begin());
    }
    residentTextures.push_back(std::move(texture));
    residentBytes.store(bytes + stamp.size, std::memory_order_relaxed);
    ReleaseSRWLockExclusive(&residentLock);
    return stamp.size;
}

Bytes warmFile(const fs::path& path, Bytes limit, std::vector<BYTE>& buffer)
{
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
        FILE_FLAG_RANDOM_ACCESS, nullptr);
    if (file == INVALID_HANDLE_VALUE) return 0;
    LARGE_INTEGER size{};
    Bytes total = 0;
    if (GetFileSizeEx(file, &size) && size.QuadPart > 0 && static_cast<Bytes>(size.QuadPart) <= limit) {
        const auto length = static_cast<Bytes>(size.QuadPart);
        while (total < length) {
            DWORD count = 0;
            const auto next = static_cast<DWORD>(std::min<Bytes>(buffer.size(), length - total));
            if (!ReadFile(file, buffer.data(), next, &count, nullptr) || count == 0) break;
            total += count;
        }
        if (total != length) total = 0;
    }
    CloseHandle(file);
    return total;
}

void report(const fs::path& path, Bytes warmed, unsigned files, bool opens, bool attrs, bool reads)
{
    std::ofstream out(path, std::ios::app);
    SYSTEMTIME now{}; GetLocalTime(&now);
    out << now.wHour << ":" << now.wMinute << ":" << now.wSecond << " uptime_ms=" << GetTickCount64() << " prefetch_files=" << files << " prefetch_bytes=" << warmed
        << " open_hook=" << opens << " area_hook=" << attrs << " read_hook=" << reads
        << " texture_read_calls=" << readCalls.exchange(0) << " texture_read_bytes=" << bytesRead.exchange(0)
        << " max_read_us=" << maxReadUs.exchange(0) << " reads_over_2ms=" << slowReads.exchange(0)
        << " max_open_us=" << maxOpenUs.exchange(0) << " opens_over_2ms=" << slowOpens.exchange(0)
        << " resident_bytes=" << residentBytes.load() << " resident_hits=" << residentHits.exchange(0)
        << " resident_read_bytes=" << residentBytesRead.exchange(0)
        << '\n';
}

void runWorker(const fs::path& game)
{
    const auto conf = game / L"modules\\config\\ff10-file-loader.ini";
    const auto log = game / L"ffx-texture-prefetch.log";
    const auto hotPath = game / L"modules\\config\\FFXTexturePrefetch.hot";
    const auto slowPath = game / L"ffx-texture-slow-reads.log";
    const auto roots = getRoots(conf, game);
    residentTextures.reserve(maxResidentFiles);
    wake = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!wake) return;
    const auto ioEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    QueryPerformanceFrequency(&frequency);
    HMODULE loader = nullptr;
    for (int attempt = 0; attempt < 500 && !loader; ++attempt) {
        loader = GetModuleHandleW(L"ff10-file-loader.dll");
        if (!loader) Sleep(20);
    }
    if (!loader) {
        std::ofstream(log, std::ios::app) << "External File Loader was not detected.\n";
        if (ioEvent) CloseHandle(ioEvent);
        CloseHandle(wake);
        return;
    }
    const auto exe = GetModuleHandleW(nullptr);
    const bool close = replaceImport(exe, "CloseHandle", originalClose, closeHook);
    const bool read = close && replaceImport(exe, "ReadFile", originalRead, readHook);
    const bool attrs = replaceImport(loader, "GetFileAttributesExW", originalAttributes, attributesHook);
    const bool opens = replaceImport(loader, "CreateFileW", originalOpen, openHook);


    SetThreadPriority(GetCurrentThread(), THREAD_MODE_BACKGROUND_BEGIN);
    std::vector<BYTE> buffer(512 * 1024);
    std::deque<Job> jobs;
    auto hotSet = readHotSet(hotPath);
    auto battleHot = hotJob(roots, hotSet, true);
    if (!battleHot.textures.empty()) jobs.push_back(std::move(battleHot));
    auto prioritized = hotJob(roots, hotSet, false);
    if (!prioritized.textures.empty()) jobs.push_back(std::move(prioritized));
    bool learned = false;
    auto lastSave = GetTickCount64();
    const auto addStartup = [&](const wchar_t* path, Bytes budget, bool largestFirst) {
        Job job{{}, budget};
        for (const auto& root : roots) addTree(job, root, path);
        if (largestFirst) {
            std::sort(job.textures.begin(), job.textures.end(),
                [](const auto& a, const auto& b) { return a.size > b.size; });
        }
        if (!job.textures.empty()) jobs.push_back(std::move(job));
    };
    addStartup(L"chr\\pc", 192 * mib, true);
    addStartup(L"yonishi_data\\dat_et", 64 * mib, false);
    addStartup(L"magic", 128 * mib, false);
    addStartup(L"chr\\mon", 384 * mib, true);

    std::unordered_set<std::wstring> planned, cached;
    Bytes warmed = 0;
    unsigned files = 0;
    auto lastReport = GetTickCount64();
    report(log, warmed, files, opens, attrs, read);

    for (;;) {
        Observation request{};
        for (unsigned i = 0; i < 16; ++i) {
            AcquireSRWLockExclusive(&queueLock);
            if (readIndex == writeIndex) { ReleaseSRWLockExclusive(&queueLock); break; }
            request = queue[readIndex];
            readIndex = (readIndex + 1) % queue.size();
            ReleaseSRWLockExclusive(&queueLock);
            const auto asset = assetPath(request.path);
            if (asset.empty()) continue;
            if (request.readUs) {
                auto& score = hotSet[asset];
                if (score < request.readUs) {
                    score = request.readUs;
                    learned = true;
                }
                SYSTEMTIME now{};
                GetLocalTime(&now);
                std::wofstream slow(slowPath, std::ios::app);
                slow << now.wHour << L":" << now.wMinute << L":" << now.wSecond
                     << L" uptime_ms=" << GetTickCount64()
                     << L" elapsed_us=" << request.readUs << L" bytes=" << request.readBytes
                     << L" end_qpc=" << request.readQpc << L" thread=" << request.thread
                     << L" asset=" << asset << L"\n";
                continue;
            }
            if (request.opened) cached.insert(asset);
            addAreaJobs(jobs, roots, asset, planned, hotSet);
        }
        const auto now = GetTickCount64();
        if (learned && now - lastSave >= 2000) {
            saveHotSet(hotPath, hotSet);
            learned = false;
            lastSave = now;
        }
        const auto lastRead = lastTextureRead.load(std::memory_order_relaxed);
        if (!jobs.empty() && now >= lastRead && now - lastRead >= 350) {
            auto& job = jobs.front();
            if (job.textures.empty() || !job.remaining || (!job.retain && warmed >= sessionBudget)) {
                jobs.pop_front();
                continue;
            }
            MEMORYSTATUSEX status{};
            status.dwLength = sizeof status;
            if (GlobalMemoryStatusEx(&status) && status.ullAvailPhys < 3 * 1024 * mib) {
                WaitForSingleObject(wake, 1000);
            } else {
                const auto texture = std::move(job.textures.front());
                job.textures.pop_front();
                const auto available = job.retain ? job.remaining : std::min(job.remaining, sessionBudget - warmed);
                const auto key = texture.relative.native();
                if (texture.size <= available && (job.retain || cached.insert(key).second)) {
                    for (const auto& root : roots) {
                        const auto path = root / texture.relative;
                        std::error_code error;
                        if (!fs::is_regular_file(path, error)) continue;
                        auto total = job.retain && ioEvent ? loadResident(path, available, ioEvent) : 0;
                        if (!total && warmed < sessionBudget)
                            total = warmFile(path, std::min(available, sessionBudget - warmed), buffer);
                        if (total) {
                            ++files;
                            warmed += total;
                            job.remaining -= total;
                        }
                        break;
                    }
                    // Each job yields between files to give the game's foreground reads priority.
                    Sleep(2);
                }
            }
        } else {
            WaitForSingleObject(wake, 250);
        }

        if (GetTickCount64() - lastReport >= 5000) {
            report(log, warmed, files, opens, attrs, read);
            lastReport = GetTickCount64();
        }
    }
}
[[maybe_unused]] DWORD WINAPI worker(void*)
{
    try {
        wchar_t exe[32768]{};
        const auto count = GetModuleFileNameW(nullptr, exe, DWORD(std::size(exe)));
        if (count && count < std::size(exe) && _wcsicmp(fs::path(exe).filename().c_str(), L"FFX.exe") == 0)
            runWorker(fs::path(exe).parent_path());
    } catch (...) {
        // Failure leaves the installed loader's normal opens and reads available.
    }
    return 0;
}
}

#ifndef FFX_PREFETCH_TEST
struct ModuleVersion { int major, minor, patch; };
extern "C" __declspec(dllexport) const char* FF10HgetName() { return "FFX Texture Prefetch Beta"; }
extern "C" __declspec(dllexport) ModuleVersion FF10HgetVer() { return {0, 5, 0}; }
BOOL WINAPI DllMain(HMODULE module, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(module);
        HMODULE pinned = nullptr;
        if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
            reinterpret_cast<LPCWSTR>(&worker), &pinned)) {
            if (HANDLE thread = CreateThread(nullptr, 0, worker, nullptr, 0, nullptr))
                CloseHandle(thread);
        }
    }
    return TRUE;
}
#else
int wmain(int argc, wchar_t** argv)
{
    if (argc != 2 && argc != 3) return 1;
    if (assetPath(L"DATA/mods/FFX_Data/Gamedata/PS3Data/map/KINO/KINO07/test.dds.phyre")
        != L"map\\kino\\kino07\\test.dds.phyre") return 2;
    if (!assetPath(L"ffx_data/gamedata/ps3data/../test.dds.phyre").empty()) return 3;
    if (isTexture(L"test.dds.phyre.old", wcslen(L"test.dds.phyre.old"))) return 4;
    if (!isTexture(L"test.DDS.PHYRE", wcslen(L"test.DDS.PHYRE"))) return 5;
    const fs::path root(argv[1]);
    std::deque<Job> jobs;
    std::unordered_set<std::wstring> seen;
    addAreaJobs(jobs, {root}, L"map\\kino\\kino07\\test.dds.phyre", seen, {});
    if (jobs.size() != 1 || jobs.front().textures.empty()) return 6;
    addAreaJobs(jobs, {root}, L"map\\kino\\kino07\\test.dds.phyre", seen, {});
    if (jobs.size() != 1) return 7;
    const auto& sample = jobs.front().textures.front();
    std::vector<BYTE> buffer(32 * 1024);
    if (warmFile(root / sample.relative, sample.size - 1, buffer) != 0) return 8;
    if (warmFile(root / sample.relative, sample.size, buffer) != sample.size) return 9;
    wake = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!wake || !QueryPerformanceFrequency(&frequency)) return 10;
    const auto exe = GetModuleHandleW(nullptr);
    if (!replaceImport(exe, "CloseHandle", originalClose, closeHook) ||
        !replaceImport(exe, "ReadFile", originalRead, readHook) ||
        !replaceImport(exe, "CreateFileW", originalOpen, openHook))
        return 11;
    const auto path = root / sample.relative;
    residentTextures.reserve(maxResidentFiles);
    const auto ioEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!ioEvent || loadResident(path, sample.size, ioEvent) != sample.size) return 12;
    const auto file = CreateFileW(path.c_str(), FILE_READ_DATA, FILE_SHARE_READ,
                                  nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (file == INVALID_HANDLE_VALUE) return 13;
    DWORD count = 0;
    std::array<BYTE, 128> expected{};
    if (!originalRead(file, expected.data(), DWORD(expected.size()), &count, nullptr) || count != expected.size())
        return 14;
    if (SetFilePointer(file, 0, nullptr, FILE_BEGIN) != 0) return 15;
    if (!ReadFile(file, buffer.data(), DWORD(expected.size()), &count, nullptr) || count != expected.size() ||
        residentHits.load() != 1 || bytesRead.load() != expected.size() ||
        std::memcmp(expected.data(), buffer.data(), expected.size()) != 0) return 16;
    LARGE_INTEGER pointer{};
    if (!SetFilePointerEx(file, {}, &pointer, FILE_CURRENT) || pointer.QuadPart != expected.size()) return 17;

    // The native request still handles partial reads and EOF with the correct offset.
    if (SetFilePointer(file, -32, nullptr, FILE_END) == INVALID_SET_FILE_POINTER ||
        !ReadFile(file, buffer.data(), 64, &count, nullptr) || count != 32 || residentHits.load() != 1)
        return 18;
    if (!CloseHandle(file) || handleSlot(file).load()) return 19;
    if (argc == 3) {
        const auto hot = hotJob({root}, readHotSet(fs::path(argv[2])), true);
        if (hot.textures.empty()) return 20;
        unsigned files = 0;
        Bytes loaded = 0;
        for (const auto& texture : hot.textures) {
            if (texture.size > residentBudget - loaded) continue;
            const auto size = loadResident(root / texture.relative, residentBudget - loaded, ioEvent);
            if (size != texture.size) return 21;
            loaded += size;
            ++files;
        }
        if (residentBytes.load() > residentBudget) return 22;
        std::printf("Loaded %u learned battle textures (%llu bytes) via overlapped I/O.\n",
                    files, static_cast<unsigned long long>(loaded));
        for (const auto& texture : hot.textures) {
            if (texture.size < buffer.size()) continue;
            const auto target = CreateFileW((root / texture.relative).c_str(), FILE_READ_DATA, FILE_SHARE_READ,
                                            nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
            if (target == INVALID_HANDLE_VALUE) return 23;
            const auto hits = residentHits.load();
            const auto result = ReadFile(target, buffer.data(), static_cast<DWORD>(buffer.size()), &count, nullptr);
            CloseHandle(target);
            if (!result || count != buffer.size()) return 24;
            if (residentHits.load() == hits) return 25;
            break;
        }
    }
    CloseHandle(ioEvent);
    CloseHandle(wake);
    std::printf("Background read, byte-identical cache hit on a FILE_READ_DATA handle, offset, partial-read fallback, and map mapping passed.\n");
    return 0;
}
#endif
