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
        if (file_)
        {
            fclose(file_);
            file_ = nullptr;
        }
    }

    int open(const char* path, const char* mode)
    {
        if (file_)
        {
            fclose(file_);
            file_ = nullptr;
        }

        file_ = periodsearch_open_file(path, mode);
        return file_ ? 0 : (errno ? errno : 1);
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
