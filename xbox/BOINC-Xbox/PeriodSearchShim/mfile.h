#pragma once

#include <cstdio>
#include <cstdarg>
#include <cerrno>

FILE* periodsearch_open_file(const char* path, const char* mode);

class MFILE
{
public:
    MFILE() : file_(nullptr) {}
    ~MFILE()
    {
        close();
    }

    int open(const char* path, const char* mode)
    {
        close();

        file_ = periodsearch_open_file(path, mode);
        return file_ ? 0 : (errno ? errno : 1);
    }

    int close()
    {
        if (!file_) return 0;

        int result = fflush(file_);
        if (fclose(file_) != 0 && result == 0)
        {
            result = errno ? errno : 1;
        }

        file_ = nullptr;
        return result;
    }

    int flush()
    {
        return file_ ? fflush(file_) : 0;
    }

    int printf(const char* format, ...)
    {
        if (!file_) return -1;

        va_list args;
        va_start(args, format);
        const int result = vfprintf(file_, format, args);
        va_end(args);
        return result;
    }

private:
    FILE* file_;
};
