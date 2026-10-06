#include "pch.h"
#include "PeriodSearchXbox.h"

#include "PeriodSearchShim/boinc_win.h"
#include "PeriodSearchShim/str_util.h"
#include "PeriodSearchShim/util.h"
#include "PeriodSearchShim/filesys.h"
#include "PeriodSearchShim/boinc_api.h"
#include "PeriodSearchShim/mfile.h"
#include "PeriodSearchShim/graphics2.h"

#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <ctime>
#include <exception>
#include <string>
#include <vector>

namespace
{
    std::wstring g_work_directory;
    std::atomic<double> g_fraction_done(0.0);
    FILE* g_input_file = nullptr;

    struct PeriodSearchExit
    {
        explicit PeriodSearchExit(int value) : code(value) {}
        int code;
    };

    std::wstring widen_ascii(const char* value)
    {
        std::wstring out;
        if (!value) return out;
        while (*value)
        {
            out.push_back(static_cast<unsigned char>(*value));
            ++value;
        }
        return out;
    }

    std::wstring join_work_path(const char* path)
    {
        std::wstring p = widen_ascii(path);
        if (p.size() > 1 && p[1] == L':') return p;
        if (!p.empty() && (p[0] == L'\\' || p[0] == L'/')) return p;

        std::wstring result = g_work_directory;
        if (!result.empty() && result.back() != L'\\' && result.back() != L'/') result += L'\\';
        result += p;
        return result;
    }

    void close_input_file()
    {
        if (g_input_file)
        {
            fclose(g_input_file);
            g_input_file = nullptr;
        }
    }

    bool is_period_search_input(const char* path, const char* mode)
    {
        if (!path || !mode || !strchr(mode, 'r')) return false;
        const char* base = path;
        for (const char* p = path; *p; ++p)
        {
            if (*p == '\\' || *p == '/') base = p + 1;
        }
        return strcmp(base, "period_search_in") == 0;
    }

    void remove_work_file(const wchar_t* name)
    {
        if (g_work_directory.empty()) return;
        std::wstring path = g_work_directory;
        if (path.back() != L'\\' && path.back() != L'/') path += L'\\';
        path += name;
        _wremove(path.c_str());
    }

    std::wstring read_work_file(const wchar_t* name)
    {
        std::wstring path = g_work_directory;
        if (!path.empty() && path.back() != L'\\' && path.back() != L'/') path += L'\\';
        path += name;

        FILE* file = nullptr;
        if (_wfopen_s(&file, path.c_str(), L"rb") != 0 || !file) return L"";

        std::vector<char> bytes;
        char block[4096];
        for (;;)
        {
            const size_t count = fread(block, 1, sizeof(block), file);
            if (count) bytes.insert(bytes.end(), block, block + count);
            if (count < sizeof(block)) break;
        }
        fclose(file);

        std::wstring out;
        out.reserve(bytes.size());
        for (char c : bytes) out.push_back(static_cast<unsigned char>(c));
        return out;
    }

    int periodsearch_system(const char*) { return 0; }

    LPSTR periodsearch_get_command_line()
    {
        static char command_line[] = "period_search";
        return command_line;
    }
}

FILE* periodsearch_open_file(const char* path, const char* mode)
{
    const std::wstring full_path = join_work_path(path);
    const std::wstring wide_mode = widen_ascii(mode ? mode : "r");
    FILE* file = nullptr;
    if (_wfopen_s(&file, full_path.c_str(), wide_mode.c_str()) != 0) return nullptr;

    // The upstream PeriodSearch source does not close period_search_in before returning.
    // Track that one handle explicitly so consecutive validation runs can replace the file.
    if (is_period_search_input(path, mode))
    {
        close_input_file();
        g_input_file = file;
    }

    return file;
}

int boinc_resolve_filename(const char* logical_name, char* physical_name, int len)
{
    if (!logical_name || !physical_name || len <= 0) return EINVAL;
    strncpy_s(physical_name, static_cast<size_t>(len), logical_name, _TRUNCATE);
    return 0;
}

int boinc_resolve_filename_s(const char* logical_name, std::string& physical_name)
{
    physical_name = logical_name ? logical_name : "";
    return 0;
}

FILE* boinc_fopen(const char* path, const char* mode) { return periodsearch_open_file(path, mode); }

int boinc_rename(const char* old_name, const char* new_name)
{
    const std::wstring old_path = join_work_path(old_name);
    const std::wstring new_path = join_work_path(new_name);
    _wremove(new_path.c_str());
    return _wrename(old_path.c_str(), new_path.c_str());
}

int parse_command_line(char*, char** argv)
{
    static char app_name[] = "period_search";
    if (argv) argv[0] = app_name;
    return 1;
}

int boinc_init() { g_fraction_done.store(0.0); return 0; }
int boinc_is_standalone() { return 0; }
int boinc_time_to_checkpoint() { return 0; }
void boinc_checkpoint_completed() {}

void boinc_fraction_done(double fraction)
{
    if (fraction < 0.0) fraction = 0.0;
    if (fraction > 1.0) fraction = 1.0;
    g_fraction_done.store(fraction);
}

double boinc_get_fraction_done() { return g_fraction_done.load(); }
double boinc_worker_thread_cpu_time() { return static_cast<double>(clock()) / CLOCKS_PER_SEC; }
void boinc_finish(int status) { throw PeriodSearchExit(status); }

char* boinc_msg_prefix(char* buffer, int length)
{
    if (buffer && length > 0) buffer[0] = '\0';
    return buffer;
}

double dtime() { return static_cast<double>(clock()) / CLOCKS_PER_SEC; }

#ifdef GetCommandLine
#undef GetCommandLine
#endif

#define main periodsearch_original_main
#define WinMain periodsearch_unused_winmain
#define GetCommandLine periodsearch_get_command_line
#define system periodsearch_system
#define exit(code) throw PeriodSearchExit(static_cast<int>(code))

#include "..\\third_party\\PeriodSearch\\period_search_pure_cpu_win\\period_search\\period_search_BOINC.cpp"

#undef exit
#undef system
#undef GetCommandLine
#undef WinMain
#undef main

#include "..\\third_party\\PeriodSearch\\period_search_pure_cpu_win\\period_search\\memory.c"
#include "..\\third_party\\PeriodSearch\\period_search_pure_cpu_win\\period_search\\areanorm.c"
#include "..\\third_party\\PeriodSearch\\period_search_pure_cpu_win\\period_search\\trifac.c"
#include "..\\third_party\\PeriodSearch\\period_search_pure_cpu_win\\period_search\\sphfunc.c"
#include "..\\third_party\\PeriodSearch\\period_search_pure_cpu_win\\period_search\\ludcmp.c"
#include "..\\third_party\\PeriodSearch\\period_search_pure_cpu_win\\period_search\\lubksb.c"
#include "..\\third_party\\PeriodSearch\\period_search_pure_cpu_win\\period_search\\ellfit.c"
#include "..\\third_party\\PeriodSearch\\period_search_pure_cpu_win\\period_search\\mrqmin.c"
#include "..\\third_party\\PeriodSearch\\period_search_pure_cpu_win\\period_search\\mrqcof.c"
#include "..\\third_party\\PeriodSearch\\period_search_pure_cpu_win\\period_search\\bright.c"
#include "..\\third_party\\PeriodSearch\\period_search_pure_cpu_win\\period_search\\conv.c"
#include "..\\third_party\\PeriodSearch\\period_search_pure_cpu_win\\period_search\\covsrt.c"
#include "..\\third_party\\PeriodSearch\\period_search_pure_cpu_win\\period_search\\host_dot_product.c"
#include "..\\third_party\\PeriodSearch\\period_search_pure_cpu_win\\period_search\\gauss_errc.c"
#include "..\\third_party\\PeriodSearch\\period_search_pure_cpu_win\\period_search\\blmatrix.c"
#include "..\\third_party\\PeriodSearch\\period_search_pure_cpu_win\\period_search\\curv.c"
#include "..\\third_party\\PeriodSearch\\period_search_pure_cpu_win\\period_search\\matrix.c"
#include "..\\third_party\\PeriodSearch\\period_search_pure_cpu_win\\period_search\\phasec.c"

PeriodSearchRunResult periodsearch_run(const std::wstring& work_directory, bool fresh_start)
{
    PeriodSearchRunResult result;
    g_work_directory = work_directory;
    g_fraction_done.store(0.0);

    // Defensive cleanup in case a previous solver invocation exited through an exception.
    close_input_file();

    if (fresh_start)
    {
        remove_work_file(L"period_search_out");
        remove_work_file(L"period_search_state");
        remove_work_file(L"temp");
    }

    const auto started = std::chrono::steady_clock::now();
    try
    {
        char app_name[] = "period_search";
        char* argv[] = { app_name, nullptr };
        result.exit_code = periodsearch_original_main(1, argv);
    }
    catch (const PeriodSearchExit& e)
    {
        result.exit_code = e.code;
    }
    catch (const std::exception& e)
    {
        result.exit_code = -1000;
        result.error = widen_ascii(e.what());
    }
    catch (...)
    {
        result.exit_code = -1001;
        result.error = L"Unknown native PeriodSearch exception";
    }

    // Upstream period_search_BOINC.cpp leaves its input FILE* open.
    // Close it before the next v0.8 validation overwrites period_search_in.
    close_input_file();

    result.elapsed_seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
    result.fraction_done = g_fraction_done.load();
    result.output = read_work_file(L"period_search_out");

    if (result.exit_code == 0 && result.output.empty() && result.error.empty())
        result.error = L"PeriodSearch returned success but period_search_out is empty";

    return result;
}

double periodsearch_progress() { return g_fraction_done.load(); }