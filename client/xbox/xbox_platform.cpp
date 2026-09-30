// This file is part of BOINC.
// Xbox Series X Developer Mode platform adaptation.

#ifdef BOINC_XBOX_UWP
#include "pch.h"
#endif

#include "xbox_platform.h"

#ifdef BOINC_XBOX

void boinc_xbox_platform_init() {
    // Initial scaffold. Xbox-specific lifecycle hooks will be added here.
}

const char* boinc_xbox_platform_name() {
    return "x86_64-pc-xbox-uwp";
}

#endif
