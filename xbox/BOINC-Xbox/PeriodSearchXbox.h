#pragma once

#include <string>

struct PeriodSearchRunResult
{
    int exit_code = -1;
    double elapsed_seconds = 0.0;
    double fraction_done = 0.0;
    std::wstring output;
    std::wstring error;
};

PeriodSearchRunResult periodsearch_run(const std::wstring& work_directory, bool fresh_start);
double periodsearch_progress();
