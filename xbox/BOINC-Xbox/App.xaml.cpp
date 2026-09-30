#include "pch.h"
#include "App.xaml.h"
#include "xbox_platform.h"

using namespace BOINC_Xbox;
using namespace Platform;
using namespace Windows::ApplicationModel::Activation;
using namespace Windows::UI;
using namespace Windows::UI::Xaml;
using namespace Windows::UI::Xaml::Controls;
using namespace Windows::UI::Xaml::Media;

App::App()
{
    InitializeComponent();
}

void App::OnLaunched(LaunchActivatedEventArgs^)
{
    boinc_xbox_platform_init();

    auto root = ref new Grid();
    root->Background = ref new SolidColorBrush(ColorHelper::FromArgb(255, 16, 24, 32));

    auto panel = ref new StackPanel();
    panel->HorizontalAlignment = HorizontalAlignment::Center;
    panel->VerticalAlignment = VerticalAlignment::Center;
    panel->Spacing = 18;

    auto title = ref new TextBlock();
    title->Text = ref new String(L"BOINC Xbox");
    title->FontSize = 56;
    title->HorizontalAlignment = HorizontalAlignment::Center;

    auto subtitle = ref new TextBlock();
    subtitle->Text = ref new String(L"Xbox Series X Developer Mode port");
    subtitle->FontSize = 24;
    subtitle->HorizontalAlignment = HorizontalAlignment::Center;

    auto platformLabel = ref new TextBlock();
    platformLabel->Text = ref new String(L"Platform layer:");
    platformLabel->FontSize = 20;
    platformLabel->Margin = Thickness(0, 26, 0, 0);

    const char* platformName = boinc_xbox_platform_name();
    std::string platformAscii(platformName ? platformName : "unknown");
    std::wstring platformWide(platformAscii.begin(), platformAscii.end());

    auto platformText = ref new TextBlock();
    platformText->Text = ref new String(platformWide.c_str());
    platformText->FontSize = 18;

    auto statusText = ref new TextBlock();
    statusText->Text = ref new String(L"Xbox platform scaffold initialized");
    statusText->FontSize = 18;

    panel->Children->Append(title);
    panel->Children->Append(subtitle);
    panel->Children->Append(platformLabel);
    panel->Children->Append(platformText);
    panel->Children->Append(statusText);

    root->Children->Append(panel);

    Window::Current->Content = root;
    Window::Current->Activate();
}
