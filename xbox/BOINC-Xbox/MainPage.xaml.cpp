#include "pch.h"
#include "MainPage.xaml.h"
#include "xbox_platform.h"

using namespace BOINC_Xbox;
using namespace Platform;

MainPage::MainPage()
{
    InitializeComponent();

    boinc_xbox_platform_init();

    const char* platformName = boinc_xbox_platform_name();
    std::string platformAscii(platformName ? platformName : "unknown");
    std::wstring platformWide(platformAscii.begin(), platformAscii.end());

    PlatformText->Text = ref new String(platformWide.c_str());
    StatusText->Text = ref new String(L"Xbox platform scaffold initialized");
}
