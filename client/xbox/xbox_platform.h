// This file is part of BOINC.
// Xbox Series X Developer Mode platform adaptation.

#pragma once

#ifdef BOINC_XBOX

// Initialize the Xbox-specific platform layer.
// Keep this layer isolated from the normal Win32 client.
void boinc_xbox_platform_init();

// Return a BOINC platform identifier for Xbox builds.
const char* boinc_xbox_platform_name();

#endif
