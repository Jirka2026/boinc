#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <collection.h>
#include <ppltasks.h>
#include <string>
#include <cstring>
#include <cwctype>
#include <cmath>
#include <thread>
#include <vector>
#include <algorithm>
#include <chrono>
#include <memory>

// C++/CX compatibility for the v0.8 integration source.
// Uri is declared in Windows::Foundation, while AppV08 uses it unqualified.
namespace Windows { namespace Foundation {} }
using namespace Windows::Foundation;

// "generic" is a C++/CX contextual keyword. AppV08 uses it as a local variable.
#define generic generic_url

// Windows::System exposes Diagnostics; rename the local AppV08 helper token.
#define Diagnostics ServerDiagnostics
