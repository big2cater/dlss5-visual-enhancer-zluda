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

// The same gate as room_for_another, asked as a question about the machine
// rather than about the next spawn: a compile process that stops accumulating
// CPU under memory pressure is usually being paged out, not hung.
bool memory_pressure_low() {
    return free_physical_mb() < kReserveMb + kHeadroomMb;
}

// TerminateProcess returns before the target has actually exited, and the
// handles it owned (its module_*.bin in particular) stay open until it does.
// Closing the handle and deleting the file straight away therefore races the
// child's teardown and leaves the temporary directory behind. Wait first.
void terminate_and_reap(HANDLE process, DWORD wait_ms = 5000) {
    if (!process || process == INVALID_HANDLE_VALUE) return;
    TerminateProcess(process, 1);
    WaitForSingleObject(process, wait_ms);
    CloseHandle(process);
}

// A child that makes no CPU progress at all for this many 60 s slices is a
// hang. The window is doubled when the machine is short of memory, because a
// process being paged out looks identical to one that is stuck.
constexpr int kStallSlices = 6;
constexpr int kStallSlicesLowMemory = 12;

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

std::wstring get_absolute_path(const std::wstring &path) {
    if (path.empty()) return {};
    wchar_t full[MAX_PATH] = {};
    DWORD len = GetFullPathNameW(path.c_str(), MAX_PATH, full, nullptr);
    if (len > 0 && len < MAX_PATH) return std::wstring(full, len);
    return path;
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

std::string to_utf8(const std::wstring &text) {
    if (text.empty()) return {};
    int bytes = WideCharToMultiByte(CP_UTF8, 0, text.c_str(), (int)text.size(),
                                    nullptr, 0, nullptr, nullptr);
    if (bytes <= 0) return {};
    std::string out(bytes, '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.c_str(), (int)text.size(), out.data(), bytes,
                        nullptr, nullptr);
    return out;
}

void write_precompile_stamp(const std::wstring &library, const std::wstring &driver) {
    const std::wstring directory = cache_directory();
    if (directory.empty()) return;
    // No database observed means nothing was verified about where the cache
    // lives; stamping here would let a cold machine skip precompile forever.
    const unsigned long long db_bytes = cache_database_bytes(directory);
    if (db_bytes == 0) return;
    // The compiled kernels are keyed to the chip they were translated for, so
    // the stamp names the GPU too: a machine that swapped graphics cards must
    // not skip precompile on the word of a stamp written for the old one.
    const std::wstring gpu = dlssnr::detected_gpu_identity();
    if (gpu.empty()) return;
    const std::string gpu_utf8 = to_utf8(gpu);
    if (gpu_utf8.empty()) return;
    FileStamp snippet{};
    FileStamp cuda{};
    if (!file_stamp(library, snippet) || !file_stamp(driver, cuda)) return;
    CreateDirectoryW(directory.c_str(), nullptr);
    std::ofstream out(directory + kStampName);
    if (!out) return;
    out << "v2\n"
        << "gpu " << gpu_utf8 << "\n"
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

int compile_one(const std::wstring &module_file, const std::wstring &driver_in) {
    dlssnr::auto_configure_gpu_environment();
    const std::wstring driver = get_absolute_path(driver_in);
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

bool precompile(const std::wstring &library_in, const std::wstring &driver_in, unsigned jobs,
                const std::function<void(const Progress &)> &report, std::string &error) {
    const std::wstring library = get_absolute_path(library_in);
    const std::wstring driver = get_absolute_path(driver_in);
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
        if (report) report(progress);
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
    if (report) report(progress);

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
    std::vector<size_t> slots;      // which module each running child is translating
    size_t next = 0;
    int failures = 0;

    // Two guards, because they catch different children.
    //
    // `stalled` above catches a child that is parked: no CPU progress at all. It
    // cannot catch one that keeps burning CPU while making no visible progress, and
    // that is not hypothetical -- with the precompile stamp removed, a run finished
    // fourteen modules in seconds and was still waiting on the fifteenth after three
    // minutes, and the parent would have waited indefinitely, GUI included.
    //
    // So there is also a deadline on "nothing has finished at all". When it fires,
    // the CPU monitor's own verdict splits the children: one that has stopped
    // accumulating CPU time is killed and counted as failed; one that is still
    // burning CPU is left running and only named in a warning. The split exists
    // because the largest module legitimately takes tens of minutes on a slow
    // machine (see the comment in image_processor.cpp) -- a budget short enough to
    // kill that one murders a healthy translation exactly where the machine is
    // slowest. Overridable for a machine that needs a different budget. Kept
    // separate from `error` because the tail of this function overwrites `error`
    // with a count, and a count is exactly what is not useful here.
    std::string straggler_detail;
    // How many deadlines in a row have fired with children still burning CPU. Reset
    // whenever a module finishes, so it counts the same tail surviving, not the
    // elapsed time: a slow machine that keeps finishing modules never reaches it.
    constexpr int kDeadlineFiringsBeforeKill = 3;
    int deadline_firings = 0;
    int no_progress_minutes = 60;
    {
        char forced[16] = {};
        if (GetEnvironmentVariableA("DLSSNR_PRECOMPILE_NO_PROGRESS_MINUTES", forced, sizeof forced) > 0 &&
            atoi(forced) > 0)
            no_progress_minutes = atoi(forced);
    }
    ULONGLONG last_completion = GetTickCount64();

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
                slots.push_back(next);
            } else {
                ++failures;
                ++progress.done;
            }
            ++next;
        }
        if (running.empty()) break;

        // Wait in 60 s slices: a translation that makes no CPU progress at
        // all across the stall window is a hung child, and killing it beats
        // letting it stall the whole pipeline (and the GUI) forever.
        const DWORD which = WaitForMultipleObjects((DWORD)running.size(), running.data(), FALSE,
                                                   60000);
        if (which == WAIT_FAILED) {
            DWORD err = GetLastError();
            // Kept in straggler_detail as well: the tail of this function overwrites
            // `error` with a count, which is exactly what made this detail vanish from
            // the message a user gets to read.
            straggler_detail = "WaitForMultipleObjects failed with error " + std::to_string(err);
            error = straggler_detail;
            for (HANDLE h : running) terminate_and_reap(h);
            failures += (unsigned)running.size() + (unsigned)(files.size() - next);
            running.clear();
            cpu.clear();
            stalled.clear();
            slots.clear();
            break;
        }
        if (which == WAIT_TIMEOUT) {
            for (size_t j = 0; j < running.size(); ++j) {
                DWORD exit_code = STILL_ACTIVE;
                if (!GetExitCodeProcess(running[j], &exit_code) || exit_code != STILL_ACTIVE) {
                    // Child exited or vanished during this wait slice. Either way the
                    // tail changed, so the deadline counts from here: without this a
                    // vanished child left last_completion stale and the deadline could
                    // fire on the strength of a child that no longer exists. A handle
                    // whose exit code could not be read leaves STILL_ACTIVE in place,
                    // which is deliberately counted as a failure.
                    if (exit_code != 0) {
                        ++failures;
                    }
                    last_completion = GetTickCount64();
                    ++progress.done;
                    CloseHandle(running[j]);
                    running.erase(running.begin() + j);
                    cpu.erase(cpu.begin() + j);
                    stalled.erase(stalled.begin() + j);
                    slots.erase(slots.begin() + j);
                    --j;
                    continue;
                }

                // Child is STILL_ACTIVE: monitor CPU usage
                FILETIME created{}, exited{}, kernel{}, user{};
                if (GetProcessTimes(running[j], &created, &exited, &kernel, &user)) {
                    ULONGLONG cur = (((ULONGLONG)kernel.dwHighDateTime << 32) | kernel.dwLowDateTime) +
                                    (((ULONGLONG)user.dwHighDateTime << 32) | user.dwLowDateTime);
                    if (cur == cpu[j]) {
                        const int stall_limit = memory_pressure_low() ? kStallSlicesLowMemory
                                                                     : kStallSlices;
                        if (++stalled[j] >= stall_limit) {
                            terminate_and_reap(running[j]);
                            ++failures;
                            ++progress.done;
                            running.erase(running.begin() + j);
                            cpu.erase(cpu.begin() + j);
                            stalled.erase(stalled.begin() + j);
                            slots.erase(slots.begin() + j);
                            --j; // vector shrank under us
                        }
                    } else {
                        cpu[j] = cur;
                        stalled[j] = 0;
                    }
                }
            }
            // Nothing has finished for the whole budget while children are
            // still running: the tail is not making visible progress. Name the
            // outstanding modules -- the progress line counts finished modules
            // and never says which one is missing, which is what turned
            // localising the straggler into a log archaeology pass. Then split
            // them the way the CPU monitor above already does: a child with no
            // CPU progress is parked and killed; one still accumulating CPU
            // time is a slow machine doing real work, and killing it discards
            // a translation that only needed more time than the budget.
            if (!running.empty() &&
                GetTickCount64() - last_completion > (ULONGLONG)no_progress_minutes * 60000ull) {
                std::string working, killed;
                for (size_t j = 0; j < running.size(); ++j) {
                    const size_t slot = j < slots.size() ? slots[j] : files.size();
                    std::wstring leaf = slot < files.size() ? files[slot] : L"(unknown module)";
                    const size_t slash = leaf.find_last_of(L"\\/");
                    if (slash != std::wstring::npos) leaf = leaf.substr(slash + 1);
                    std::string &sink = (j < stalled.size() && stalled[j] > 0) ? killed : working;
                    if (!sink.empty()) sink += ", ";
                    sink += to_utf8(leaf);
                }
                for (size_t j = 0; j < running.size(); ++j) {
                    if (j < stalled.size() && stalled[j] > 0) {
                        terminate_and_reap(running[j]);
                        ++failures;
                        ++progress.done;
                        running.erase(running.begin() + j);
                        cpu.erase(cpu.begin() + j);
                        stalled.erase(stalled.begin() + j);
                        slots.erase(slots.begin() + j);
                        --j; // vector shrank under us
                    }
                }
                // The survivors are the ones still accumulating CPU time. Once
                // is a slow machine doing real work, and the note above is right
                // that killing it throws away a translation that only needed
                // more time. But children that keep burning CPU while deadline
                // after deadline passes with nothing finishing are no longer
                // "slow", they are a tail that will never end -- and waiting for
                // it forever is the hang this whole guard exists to prevent. So
                // count the firings and take them down on the third. The count
                // resets whenever a module does finish, and scales with the
                // budget on purpose: three times sixty minutes by default, three
                // minutes if someone overrode the budget to one.
                std::string escalated;
                if (!running.empty() && ++deadline_firings >= kDeadlineFiringsBeforeKill) {
                    for (size_t j = 0; j < running.size(); ++j) {
                        const size_t slot = j < slots.size() ? slots[j] : files.size();
                        std::wstring leaf = slot < files.size() ? files[slot] : L"(unknown module)";
                        const size_t slash = leaf.find_last_of(L"\\/");
                        if (slash != std::wstring::npos) leaf = leaf.substr(slash + 1);
                        if (!escalated.empty()) escalated += ", ";
                        escalated += to_utf8(leaf);
                        terminate_and_reap(running[j]);
                        ++failures;
                        ++progress.done;
                    }
                    running.clear();
                    cpu.clear();
                    stalled.clear();
                    slots.clear();
                    deadline_firings = 0;
                }
                if (straggler_detail.empty()) {
                    straggler_detail = "no module finished for " +
                                       std::to_string(no_progress_minutes) + " minutes";
                    if (!killed.empty())
                        straggler_detail += "; killed (no CPU progress): " + killed;
                    if (!working.empty() && escalated.empty())
                        straggler_detail += "; still translating (left running): " + working;
                    if (!escalated.empty())
                        straggler_detail += "; killed after " +
                                            std::to_string(kDeadlineFiringsBeforeKill) +
                                            " deadlines with nothing finishing: " + escalated;
                }
                // A survivor that is still burning CPU gets a visible warning and
                // keeps going, and the deadline re-fires on every later silent
                // slice -- but only kDeadlineFiringsBeforeKill times before it is
                // taken down as well.
                progress.message = straggler_detail;
                if (report) report(progress);
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
        if (index < slots.size()) slots.erase(slots.begin() + index);
        last_completion = GetTickCount64();
        deadline_firings = 0; // something finished: this is not a stuck tail

        ++progress.done;
        {
            char buffer[192];
            snprintf(buffer, sizeof buffer, "translated %d of %d, %zu running, %llu MB free",
                     progress.done, progress.total, running.size(), free_physical_mb());
            progress.message = buffer;
        }
        if (report) report(progress);
    }

    for (HANDLE h : running) terminate_and_reap(h);
    running.clear();

    for (const std::wstring &path : files) {
        if (!DeleteFileW(path.c_str())) {
            // A just-reaped child can still be releasing the file; one retry
            // covers the teardown window without masking a real failure.
            Sleep(50);
            DeleteFileW(path.c_str());
        }
    }
    RemoveDirectoryW(directory.c_str());

    if (failures) {
        error = std::to_string(failures) + " of " + std::to_string(progress.total) +
                " modules could not be translated";
        // The count alone leaves the reader with no idea which module to look at,
        // and finding out meant going through the run logs by hand.
        if (!straggler_detail.empty()) error += "; " + straggler_detail;
        return false;
    }
    write_precompile_stamp(library, driver);
    return true;
}

bool precompile_cache_is_warm(const std::wstring &library_in, const std::wstring &driver_in) {
    const std::wstring library = get_absolute_path(library_in);
    const std::wstring driver = get_absolute_path(driver_in);
    const std::wstring directory = cache_directory();
    if (directory.empty()) return false;
    std::ifstream in(directory + kStampName);
    if (!in) return false;
    std::string line;
    if (!std::getline(in, line)) return false;
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (line != "v2") return false;
    // The GPU the stamp was written for must still be the GPU present: the
    // cache entries it vouches for are keyed to that chip. An environment
    // without an identifiable GPU answers not warm rather than guessing.
    std::wstring gpu = dlssnr::detected_gpu_identity();
    if (gpu.empty()) return false;
    const std::string gpu_utf8 = to_utf8(gpu);
    unsigned long long snippet_size = 0, snippet_mtime = 0;
    unsigned long long driver_size = 0, driver_mtime = 0;
    unsigned long long db_bytes = 0;
    std::string gpu_line;
    bool gpu_seen = false;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        std::istringstream fields(line);
        std::string kind;
        fields >> kind;
        if (kind == "gpu") {
            gpu_line = line.size() > 4 ? line.substr(4) : std::string();
            gpu_seen = true;
        }
        else if (kind == "snippet") fields >> snippet_size >> snippet_mtime;
        else if (kind == "driver") fields >> driver_size >> driver_mtime;
        else if (kind == "db") fields >> db_bytes;
    }
    if (!gpu_seen || gpu_line != gpu_utf8) return false;
    if (db_bytes == 0) return false;
    FileStamp snippet{};
    FileStamp cuda{};
    if (!file_stamp(library, snippet) || !file_stamp(driver, cuda)) return false;

    auto mtime_matches = [](unsigned long long a, unsigned long long b) {
        // FAT32, zip extraction, and integer text serialization can truncate
        // sub-second timestamp fractions. Allow up to 2 seconds (20,000,000 FILETIME ticks).
        constexpr unsigned long long kTwoSeconds = 20000000ULL;
        return (a >= b ? a - b : b - a) <= kTwoSeconds;
    };

    if (snippet.size != snippet_size || !mtime_matches(snippet.mtime, snippet_mtime)) return false;
    if (cuda.size != driver_size || !mtime_matches(cuda.mtime, driver_mtime)) return false;
    // The databases only grow in normal use; smaller than recorded means the
    // cache was reset or rotated and the entries behind the stamp are gone.
    return cache_database_bytes(directory) >= db_bytes;
}

} // namespace enhancer
