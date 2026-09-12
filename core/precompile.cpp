#include "precompile.h"
#include "gpu_detection.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <psapi.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>

#pragma comment(lib, "psapi.lib")

namespace enhancer {
namespace {

// How much room to keep free for one more translation before starting it.
//
// A guess, and deliberately a modest one, because it is no longer the thing
// that decides the answer: the number of translations running at once is
// reconsidered every time one finishes, against the memory actually free at
// that moment. A constant that is too small only means the count ramps up in
// steps; one that is too large would leave the machine idle, which is what the
// earlier fixed estimate did.
constexpr unsigned long long kHeadroomMb = 1200;

// What to leave for everything else. Someone may well be using the machine.
constexpr unsigned long long kReserveMb = 3000;

unsigned long long free_physical_mb() {
    MEMORYSTATUSEX status{};
    status.dwLength = sizeof status;
    if (!GlobalMemoryStatusEx(&status)) return 0;
    return status.ullAvailPhys / (1024 * 1024);
}

unsigned logical_processors() {
    SYSTEM_INFO info{};
    GetSystemInfo(&info);
    return info.dwNumberOfProcessors ? info.dwNumberOfProcessors : 1;
}

// Whether there is room to start one more right now.
bool room_for_another(size_t running) {
    if (running == 0) return true; // always make progress, whatever the machine says
    const unsigned long long free_mb = free_physical_mb();
    return free_mb > kReserveMb + kHeadroomMb;
}

std::wstring own_path() {
    wchar_t buffer[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, buffer, MAX_PATH);
    return buffer;
}

std::wstring temporary_directory() {
    wchar_t base[MAX_PATH] = {};
    GetTempPathW(MAX_PATH, base);
    std::wstring directory = std::wstring(base) + L"dlss5-precompile";
    CreateDirectoryW(directory.c_str(), nullptr);
    return directory;
}

// ---- the warm-start stamp -------------------------------------------------
//
// precompile() cannot cheaply ask ZLUDA whether its cache already holds a
// module: the answer lives behind the same driver load and context creation
// that makes the child processes expensive in the first place. So the last
// fully successful precompile signs the cache instead, with a small text
// stamp written into ZLUDA's own cache directory. It records the identity of
// the network library and of the driver build -- either changing means the
// cached keys no longer describe what is on disk -- and the total size of the
// cache databases at the time. The databases only grow in normal use, so a
// smaller total than recorded means the cache was reset or rotated and the
// entries are gone. Because the stamp lives inside the cache directory,
// wiping the directory takes the stamp with it.
//
// The safe direction for every failure is "not warm": an unreadable stamp, a
// missing cache directory or an environment without a discoverable one all
// simply send the caller through the full precompile, as if the stamp were
// not a thing.

constexpr wchar_t kStampName[] = L"\\dlssnr-precompile.stamp";

struct FileStamp {
    unsigned long long size = 0;
    unsigned long long mtime = 0;
};

bool file_stamp(const std::wstring &path, FileStamp &out) {
    WIN32_FILE_ATTRIBUTE_DATA data{};
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &data)) return false;
    out.size = ((unsigned long long)data.nFileSizeHigh << 32) | data.nFileSizeLow;
    out.mtime = ((unsigned long long)data.ftLastWriteTime.dwHighDateTime << 32) |
                data.ftLastWriteTime.dwLowDateTime;
    return true;
}

std::wstring env_wstring(const wchar_t *name) {
    DWORD needed = GetEnvironmentVariableW(name, nullptr, 0);
    if (needed == 0) return {};
    std::wstring value(needed, L'\0');
    DWORD written = GetEnvironmentVariableW(name, value.data(), needed);
    if (written == 0 || written >= needed) return {};
    value.resize(written);
    return value;
}

std::wstring cache_directory() {
    // ZLUDA_CACHE_DIR moves the whole cache somewhere else (see zluda_cache),
    // and a stamp written to the default directory would then vouch for a
    // cache nobody is using -- possibly a stale one with plausible-looking
    // databases still in it. The override is the whole directory, nothing
    // appended.
    if (std::wstring override_dir = env_wstring(L"ZLUDA_CACHE_DIR"); !override_dir.empty())
        return override_dir;
    std::wstring base = env_wstring(L"LOCALAPPDATA");
    if (base.empty()) return {};
    return base + L"\\zluda\\ComputeCache";
}

unsigned long long cache_database_bytes(const std::wstring &directory) {
    WIN32_FIND_DATAW found{};
    HANDLE search = FindFirstFileW((directory + L"\\*.db").c_str(), &found);
    if (search == INVALID_HANDLE_VALUE) return 0;
    unsigned long long total = 0;
    do {
        if (!(found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
            total += ((unsigned long long)found.nFileSizeHigh << 32) | found.nFileSizeLow;
    } while (FindNextFileW(search, &found));
    FindClose(search);
    return total;
}

void write_precompile_stamp(const std::wstring &library, const std::wstring &driver) {
    const std::wstring directory = cache_directory();
    if (directory.empty()) return;
    // No database observed means nothing was verified about where the cache
    // lives; stamping here would let a cold machine skip precompile forever.
    const unsigned long long db_bytes = cache_database_bytes(directory);
    if (db_bytes == 0) return;
    FileStamp snippet{};
    FileStamp cuda{};
    if (!file_stamp(library, snippet) || !file_stamp(driver, cuda)) return;
    CreateDirectoryW(directory.c_str(), nullptr);
    std::ofstream out(directory + kStampName);
    if (!out) return;
    out << "v1\n"
        << "snippet " << snippet.size << " " << snippet.mtime << "\n"
        << "driver " << cuda.size << " " << cuda.mtime << "\n"
        << "db " << db_bytes << "\n";
}

} // namespace

std::vector<std::vector<unsigned char>> extract_modules(const std::wstring &library,
                                                        std::string &error) {
    std::vector<std::vector<unsigned char>> modules;

    std::ifstream file(library, std::ios::binary);
    if (!file) {
        error = "the network library could not be opened";
        return modules;
    }
    const std::vector<unsigned char> bytes((std::istreambuf_iterator<char>(file)),
                                           std::istreambuf_iterator<char>());
    if (bytes.size() < 16) {
        error = "the network library is empty";
        return modules;
    }

    // A module begins with a fixed magic, and its header says how long the whole
    // thing is: two bytes of header size at +6, eight of body size at +8.
    const unsigned char magic[4] = {0x50, 0xED, 0x55, 0xBA};
    for (size_t i = 0; i + 16 < bytes.size(); ++i) {
        if (std::memcmp(&bytes[i], magic, 4) != 0) continue;
        unsigned short header_size = 0;
        unsigned long long body_size = 0;
        std::memcpy(&header_size, &bytes[i + 6], sizeof header_size);
        std::memcpy(&body_size, &bytes[i + 8], sizeof body_size);
        const unsigned long long total = (unsigned long long)header_size + body_size;
        // A plausible module: it fits, and it is neither empty nor absurd.
        if (header_size < 16 || total < 64 || total > 64ull * 1024 * 1024 ||
            i + total > bytes.size())
            continue;
        modules.emplace_back(bytes.begin() + i, bytes.begin() + i + (size_t)total);
        i += (size_t)total - 1;
    }

    if (modules.empty()) error = "no code modules were found in the network library";
    return modules;
}

int compile_one(const std::wstring &module_file, const std::wstring &driver) {
    dlssnr::auto_configure_gpu_environment();
    const size_t slash = driver.find_last_of(L"/\\");
    if (slash != std::wstring::npos) {
        SetDllDirectoryW(driver.substr(0, slash).c_str());
    }
    HMODULE cuda = LoadLibraryW(driver.c_str());
    if (!cuda) return 1;

    auto cuInit = (int (*)(unsigned))GetProcAddress(cuda, "cuInit");
    auto cuDeviceGet = (int (*)(int *, int))GetProcAddress(cuda, "cuDeviceGet");
    auto cuCtxCreate = (int (*)(void **, unsigned, int))GetProcAddress(cuda, "cuCtxCreate_v2");
    if (!cuCtxCreate) cuCtxCreate = (int (*)(void **, unsigned, int))GetProcAddress(cuda, "cuCtxCreate");
    auto cuModuleLoadData = (int (*)(void **, const void *))GetProcAddress(cuda, "cuModuleLoadData");
    if (!cuInit || !cuDeviceGet || !cuCtxCreate || !cuModuleLoadData) return 2;

    std::ifstream file(module_file, std::ios::binary);
    if (!file) return 3;
    std::vector<unsigned char> image((std::istreambuf_iterator<char>(file)),
                                     std::istreambuf_iterator<char>());
    // One trailing zero: a module held as text is read as a C string, and a file
    // on disk carries no terminator of its own.
    image.push_back(0);

    if (cuInit(0) != 0) return 4;
    int device = 0;
    void *context = nullptr;
    if (cuDeviceGet(&device, 0) != 0 || cuCtxCreate(&context, 0, device) != 0) return 5;

    void *module = nullptr;
    return cuModuleLoadData(&module, image.data()) == 0 ? 0 : 6;
}

bool precompile(const std::wstring &library, const std::wstring &driver, unsigned jobs,
                const std::function<void(const Progress &)> &report, std::string &error) {
    // The stamp check lives here and not only in the callers, because the
    // --precompile entry points -- the prewarm child that --precompile-wait
    // spawns, the manual command, the GUI's own -- dispatch every module
    // unconditionally. Against a cache that already holds everything, each of
    // the fifteen children still pays a full driver load and context creation
    // on its way to a cache hit: minutes of pure overhead, paid again for
    // every image in a batch. The callers that check the stamp themselves
    // simply never reach this line when it answers warm.
    if (precompile_cache_is_warm(library, driver)) {
        Progress progress;
        progress.message = "every module is already in the cache, nothing to translate";
        report(progress);
        return true;
    }
    const std::vector<std::vector<unsigned char>> modules = extract_modules(library, error);
    if (modules.empty()) return false;

    // Zero means "as many as the machine turns out to allow", which is decided
    // again every time one finishes rather than once at the start. The
    // DLSSNR_PRECOMPILE_JOBS environment variable forces a fixed parallelism;
    // otherwise the core count is the ceiling and free memory is the real
    // limit. Sixteen concurrent children all writing ZLUDA's cache database
    // was once suspected of corrupting entries, which is why this used to be
    // pinned to four -- but zluda_cache now runs SQLite in WAL mode with a
    // long busy_timeout, so the writes take care of themselves and the memory
    // gate below is what keeps the machine from being swamped.
    unsigned ceiling = jobs;
    if (ceiling == 0) {
        char forced[16] = {};
        if (GetEnvironmentVariableA("DLSSNR_PRECOMPILE_JOBS", forced, sizeof forced) > 0 &&
            atoi(forced) > 0)
            ceiling = (unsigned)atoi(forced);
    }
    if (ceiling == 0) ceiling = logical_processors();
    if (ceiling > 32) ceiling = 32;
    const bool adaptive = jobs == 0;

    Progress progress;
    progress.total = (int)modules.size();
    {
        char buffer[192];
        snprintf(buffer, sizeof buffer, "%d modules, up to %u at a time%s", progress.total,
                 ceiling, adaptive ? " as memory allows" : "");
        progress.message = buffer;
    }
    report(progress);

    // Written out because the work happens in separate processes: each one is
    // a whole compiler pipeline over a large module, and running them in
    // threads of one process would share a driver context that is not built for
    // it.
    const std::wstring directory = temporary_directory();
    std::vector<std::wstring> files;
    for (size_t i = 0; i < modules.size(); ++i) {
        wchar_t name[64];
        swprintf(name, 64, L"\\module_%03zu.bin", i);
        const std::wstring path = directory + name;
        std::ofstream out(path, std::ios::binary);
        if (!out) {
            error = "the extracted modules could not be written to a temporary directory";
            return false;
        }
        out.write((const char *)modules[i].data(), (std::streamsize)modules[i].size());
        files.push_back(path);
    }

    // Largest first: the long poles then start immediately instead of being
    // picked up last, which is the difference between finishing in one wave and
    // waiting on a single straggler.
    std::sort(files.begin(), files.end(), [&modules, &files](const std::wstring &a,
                                                             const std::wstring &b) {
        auto size_of = [](const std::wstring &path) -> unsigned long long {
            WIN32_FILE_ATTRIBUTE_DATA data{};
            if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &data)) return 0;
            return ((unsigned long long)data.nFileSizeHigh << 32) | data.nFileSizeLow;
        };
        return size_of(a) > size_of(b);
    });

    const std::wstring self = own_path();
    std::vector<HANDLE> running;
    std::vector<ULONGLONG> cpu;     // last observed CPU time per child
    std::vector<int> stalled;       // consecutive 60s slices without CPU progress
    size_t next = 0;
    int failures = 0;

    while (next < files.size() || !running.empty()) {
        while (next < files.size() && running.size() < ceiling &&
               (!adaptive || room_for_another(running.size()))) {
            std::wstring command = L"\"" + self + L"\" --compile-one \"" + files[next] + L"\" \"" +
                                   driver + L"\"";
            STARTUPINFOW startup{};
            startup.cb = sizeof startup;
            PROCESS_INFORMATION process{};
            if (CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
                               nullptr, nullptr, &startup, &process)) {
                CloseHandle(process.hThread);
                running.push_back(process.hProcess);
                cpu.push_back(0);
                stalled.push_back(0);
            } else {
                ++failures;
                ++progress.done;
            }
            ++next;
        }
        if (running.empty()) break;

        // Wait in 60 s slices: a translation that makes no CPU progress at
        // all for three slices is a hung child, and killing it beats letting
        // it stall the whole pipeline (and the GUI) forever.
        const DWORD which = WaitForMultipleObjects((DWORD)running.size(), running.data(), FALSE,
                                                   60000);
        if (which == WAIT_FAILED) {
            DWORD err = GetLastError();
            error = "WaitForMultipleObjects failed with error " + std::to_string(err);
            for (HANDLE h : running) {
                TerminateProcess(h, 1);
                CloseHandle(h);
            }
            failures += (unsigned)running.size() + (unsigned)(files.size() - next);
            running.clear();
            break;
        }
        if (which == WAIT_TIMEOUT) {
            for (size_t j = 0; j < running.size(); ++j) {
                DWORD exit_code = STILL_ACTIVE;
                if (!GetExitCodeProcess(running[j], &exit_code) || exit_code != STILL_ACTIVE) {
                    // Child exited or vanished during this wait slice
                    if (exit_code != 0) {
                        ++failures;
                    }
                    ++progress.done;
                    CloseHandle(running[j]);
                    running.erase(running.begin() + j);
                    cpu.erase(cpu.begin() + j);
                    stalled.erase(stalled.begin() + j);
                    --j;
                    continue;
                }

                // Child is STILL_ACTIVE: monitor CPU usage
                FILETIME created{}, exited{}, kernel{}, user{};
                if (GetProcessTimes(running[j], &created, &exited, &kernel, &user)) {
                    ULONGLONG cur = (((ULONGLONG)kernel.dwHighDateTime << 32) | kernel.dwLowDateTime) +
                                    (((ULONGLONG)user.dwHighDateTime << 32) | user.dwLowDateTime);
                    if (cur == cpu[j]) {
                        if (++stalled[j] >= 3) {
                            TerminateProcess(running[j], 1);
                            ++failures;
                            ++progress.done;
                            CloseHandle(running[j]);
                            running.erase(running.begin() + j);
                            cpu.erase(cpu.begin() + j);
                            stalled.erase(stalled.begin() + j);
                            --j; // vector shrank under us
                        }
                    } else {
                        cpu[j] = cur;
                        stalled[j] = 0;
                    }
                }
            }
            continue; // keep waiting on the survivors
        }
        const size_t index = (size_t)(which - WAIT_OBJECT_0);
        if (index >= running.size()) break;
        DWORD code = 1;
        GetExitCodeProcess(running[index], &code);
        if (code != 0) ++failures;
        CloseHandle(running[index]);
        running.erase(running.begin() + index);
        cpu.erase(cpu.begin() + index);
        stalled.erase(stalled.begin() + index);

        ++progress.done;
        {
            char buffer[192];
            snprintf(buffer, sizeof buffer, "translated %d of %d, %zu running, %llu MB free",
                     progress.done, progress.total, running.size(), free_physical_mb());
            progress.message = buffer;
        }
        report(progress);
    }

    for (HANDLE h : running) {
        TerminateProcess(h, 1);
        CloseHandle(h);
    }
    running.clear();

    for (const std::wstring &path : files) DeleteFileW(path.c_str());
    RemoveDirectoryW(directory.c_str());

    if (failures) {
        error = std::to_string(failures) + " of " + std::to_string(progress.total) +
                " modules could not be translated";
        return false;
    }
    write_precompile_stamp(library, driver);
    return true;
}

bool precompile_cache_is_warm(const std::wstring &library, const std::wstring &driver) {
    const std::wstring directory = cache_directory();
    if (directory.empty()) return false;
    std::ifstream in(directory + kStampName);
    if (!in) return false;
    std::string line;
    if (!std::getline(in, line) || line != "v1") return false;
    unsigned long long snippet_size = 0, snippet_mtime = 0;
    unsigned long long driver_size = 0, driver_mtime = 0;
    unsigned long long db_bytes = 0;
    while (std::getline(in, line)) {
        std::istringstream fields(line);
        std::string kind;
        fields >> kind;
        if (kind == "snippet") fields >> snippet_size >> snippet_mtime;
        else if (kind == "driver") fields >> driver_size >> driver_mtime;
        else if (kind == "db") fields >> db_bytes;
    }
    if (db_bytes == 0) return false;
    FileStamp snippet{};
    FileStamp cuda{};
    if (!file_stamp(library, snippet) || !file_stamp(driver, cuda)) return false;
    if (snippet.size != snippet_size || snippet.mtime != snippet_mtime) return false;
    if (cuda.size != driver_size || cuda.mtime != driver_mtime) return false;
    // The databases only grow in normal use; smaller than recorded means the
    // cache was reset or rotated and the entries behind the stamp are gone.
    return cache_database_bytes(directory) >= db_bytes;
}

} // namespace enhancer
