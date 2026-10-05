#pragma once

#include <cstdio>
#include <string>

int boinc_resolve_filename(const char* logical_name, char* physical_name, int len);
int boinc_resolve_filename_s(const char* logical_name, std::string& physical_name);
FILE* boinc_fopen(const char* path, const char* mode);
int boinc_rename(const char* old_name, const char* new_name);
