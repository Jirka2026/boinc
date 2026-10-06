#pragma once

// Compatibility helpers for the C++/CX UWP compiler used by BOINC-Xbox v0.8.
// AppV08.cpp uses Uri unqualified, while Uri lives in Windows::Foundation.
namespace Windows { namespace Foundation {} }
using namespace Windows::Foundation;

// C++/CX treats "generic" as a language keyword. Rename the local identifier.
#define generic generic_url

// Windows::System also exposes Diagnostics, which makes the local helper ambiguous.
#define Diagnostics ServerDiagnostics
