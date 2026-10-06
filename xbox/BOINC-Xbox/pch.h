#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>
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

// C++/CX compatibility for the integration sources.
// Import only Uri instead of the whole Windows::Foundation namespace;
// importing the namespace makes EventRegistrationToken ambiguous in event.h.
namespace Windows { namespace Foundation {} }
using Windows::Foundation::Uri;

// "generic" is a C++/CX contextual keyword. Older integration code uses it
// as a local variable, so keep this compatibility alias.
#define generic generic_url
