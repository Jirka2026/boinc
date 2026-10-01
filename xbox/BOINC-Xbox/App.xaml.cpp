#include "pch.h"
#include "App.xaml.h"
#include "xbox_platform.h"

using namespace BOINC_Xbox;
using namespace Platform;
using namespace concurrency;
using namespace Windows::ApplicationModel::Activation;
using namespace Windows::Foundation;
using namespace Windows::Networking;
using namespace Windows::Networking::Connectivity;
using namespace Windows::Networking::Sockets;
using namespace Windows::Storage;
using namespace Windows::System;
using namespace Windows::UI;
using namespace Windows::UI::Xaml;
using namespace Windows::UI::Xaml::Controls;
using namespace Windows::UI::Xaml::Media;
using namespace Windows::Web::Http;
using namespace Windows::Web::Http::Filters;

namespace
{
    String^ ToPlatformString(const std::wstring& value)
    {
        return ref new String(value.c_str());
    }

    std::wstring NormalizeProjectUrl(String^ value)
    {
        std::wstring url = value ? value->Data() : L"";

        while (!url.empty() && iswspace(url.front()))
        {
            url.erase(url.begin());
        }

        while (!url.empty() && iswspace(url.back()))
        {
            url.pop_back();
        }

        // Correct common controller/onscreen-keyboard URL typos.
        if (url.rfind(L"https//", 0) == 0)
        {
            url.replace(0, 7, L"https://");
        }
        else if (url.rfind(L"http//", 0) == 0)
        {
            url.replace(0, 6, L"http://");
        }
        else if (url.rfind(L"https:/", 0) == 0 && url.rfind(L"https://", 0) != 0)
        {
            url.replace(0, 7, L"https://");
        }
        else if (url.rfind(L"http:/", 0) == 0 && url.rfind(L"http://", 0) != 0)
        {
            url.replace(0, 6, L"http://");
        }
        else if (!url.empty() && url.find(L"://") == std::wstring::npos)
        {
            url = L"https://" + url;
        }

        while (!url.empty() && url.back() == L'/')
        {
            url.pop_back();
        }

        return url;
    }

    std::wstring ExtractFirstTag(const std::wstring& xml, const std::wstring& tag)
    {
        const std::wstring open = L"<" + tag + L">";
        const std::wstring close = L"</" + tag + L">";
        const size_t startTag = xml.find(open);
        if (startTag == std::wstring::npos)
        {
            return L"";
        }

        const size_t valueStart = startTag + open.size();
        const size_t valueEnd = xml.find(close, valueStart);
        if (valueEnd == std::wstring::npos)
        {
            return L"";
        }

        return xml.substr(valueStart, valueEnd - valueStart);
    }

    std::vector<std::wstring> ExtractAllTags(const std::wstring& xml, const std::wstring& tag)
    {
        std::vector<std::wstring> values;
        const std::wstring open = L"<" + tag + L">";
        const std::wstring close = L"</" + tag + L">";
        size_t cursor = 0;

        while (true)
        {
            const size_t startTag = xml.find(open, cursor);
            if (startTag == std::wstring::npos)
            {
                break;
            }

            const size_t valueStart = startTag + open.size();
            const size_t valueEnd = xml.find(close, valueStart);
            if (valueEnd == std::wstring::npos)
            {
                break;
            }

            values.push_back(xml.substr(valueStart, valueEnd - valueStart));
            cursor = valueEnd + close.size();
        }

        return values;
    }

    bool ContainsPlatform(const std::vector<std::wstring>& platforms, const std::wstring& wanted)
    {
        for (const auto& value : platforms)
        {
            if (value == wanted)
            {
                return true;
            }
        }

        return false;
    }

    String^ GetNetworkState()
    {
        try
        {
            auto profile = NetworkInformation::GetInternetConnectionProfile();
            if (profile == nullptr)
            {
                return ref new String(L"Offline");
            }

            auto level = profile->GetNetworkConnectivityLevel();
            if (level == NetworkConnectivityLevel::InternetAccess)
            {
                return ref new String(L"Online");
            }

            if (level == NetworkConnectivityLevel::ConstrainedInternetAccess)
            {
                return ref new String(L"Constrained");
            }

            if (level == NetworkConnectivityLevel::LocalAccess)
            {
                return ref new String(L"Local only");
            }
        }
        catch (Exception^)
        {
        }

        return ref new String(L"Offline");
    }
}

App::App()
{
    InitializeComponent();
}

void App::OnLaunched(LaunchActivatedEventArgs^)
{
    boinc_xbox_platform_init();

    auto root = ref new Grid();
    root->Background = ref new SolidColorBrush(ColorHelper::FromArgb(255, 16, 24, 32));

    auto scroll = ref new ScrollViewer();
    scroll->HorizontalScrollMode = ScrollMode::Disabled;
    scroll->VerticalScrollMode = ScrollMode::Auto;
    scroll->VerticalScrollBarVisibility = ScrollBarVisibility::Auto;

    auto panel = ref new StackPanel();
    panel->HorizontalAlignment = HorizontalAlignment::Stretch;
    panel->VerticalAlignment = VerticalAlignment::Top;
    panel->MaxWidth = 960;
    panel->Margin = Thickness(140, 64, 140, 64);
    panel->Spacing = 14;

    auto title = ref new TextBlock();
    title->Text = ref new String(L"BOINC Xbox");
    title->FontSize = 52;
    title->HorizontalAlignment = HorizontalAlignment::Center;

    auto subtitle = ref new TextBlock();
    subtitle->Text = ref new String(L"v0.3 - Project parser");
    subtitle->FontSize = 22;
    subtitle->Opacity = 0.8;
    subtitle->HorizontalAlignment = HorizontalAlignment::Center;

    auto statusBox = ref new Border();
    statusBox->Margin = Thickness(0, 18, 0, 8);
    statusBox->Padding = Thickness(22);
    statusBox->Background = ref new SolidColorBrush(ColorHelper::FromArgb(255, 32, 42, 51));

    auto statusPanel = ref new StackPanel();
    statusPanel->Spacing = 7;

    const char* platformName = boinc_xbox_platform_name();
    std::string platformAscii(platformName ? platformName : "unknown");
    std::wstring platformWide(platformAscii.begin(), platformAscii.end());

    unsigned int cpuThreads = std::thread::hardware_concurrency();
    unsigned long long memoryLimitMb = 0;

    try
    {
        memoryLimitMb = MemoryManager::AppMemoryUsageLimit / (1024ULL * 1024ULL);
    }
    catch (Exception^)
    {
    }

    auto platformText = ref new TextBlock();
    platformText->Text = ToPlatformString(L"Platform: " + platformWide);
    platformText->FontSize = 18;

    auto cpuText = ref new TextBlock();
    cpuText->Text = ToPlatformString(L"CPU threads visible to app: " + std::to_wstring(cpuThreads));
    cpuText->FontSize = 18;

    auto memoryText = ref new TextBlock();
    memoryText->Text = ToPlatformString(L"App memory limit: " + std::to_wstring(memoryLimitMb) + L" MB");
    memoryText->FontSize = 18;

    auto resourcePolicyText = ref new TextBlock();
    unsigned long long plannedMemoryMb = (memoryLimitMb * 90ULL) / 100ULL;
    resourcePolicyText->Text = ToPlatformString(
        L"Compute policy: max runtime resources; CPU workers=" +
        std::to_wstring(cpuThreads) +
        L", memory high-water=" +
        std::to_wstring(plannedMemoryMb) +
        L" MB"
    );
    resourcePolicyText->FontSize = 16;
    resourcePolicyText->TextWrapping = TextWrapping::Wrap;
    resourcePolicyText->Opacity = 0.85;

    auto networkText = ref new TextBlock();
    std::wstring networkLine = L"Network: ";
    networkLine += GetNetworkState()->Data();
    networkText->Text = ToPlatformString(networkLine);
    networkText->FontSize = 18;

    auto storageText = ref new TextBlock();
    std::wstring storageLine = L"Data folder: ";
    storageLine += ApplicationData::Current->LocalFolder->Path->Data();
    storageText->Text = ToPlatformString(storageLine);
    storageText->FontSize = 16;
    storageText->TextWrapping = TextWrapping::Wrap;
    storageText->Opacity = 0.8;

    statusPanel->Children->Append(platformText);
    statusPanel->Children->Append(cpuText);
    statusPanel->Children->Append(memoryText);
    statusPanel->Children->Append(resourcePolicyText);
    statusPanel->Children->Append(networkText);
    statusPanel->Children->Append(storageText);
    statusBox->Child = statusPanel;

    auto projectTitle = ref new TextBlock();
    projectTitle->Text = ref new String(L"BOINC project");
    projectTitle->FontSize = 24;
    projectTitle->Margin = Thickness(0, 16, 0, 0);

    auto urlBox = ref new TextBox();
    urlBox->Header = ref new String(L"Project URL");
    urlBox->PlaceholderText = ref new String(L"https://project.example.org/");
    urlBox->FontSize = 20;
    urlBox->MinHeight = 50;

    try
    {
        auto values = ApplicationData::Current->LocalSettings->Values;
        auto projectUrlKey = ref new String(L"ProjectUrl");
        if (values->HasKey(projectUrlKey))
        {
            auto saved = dynamic_cast<String^>(values->Lookup(projectUrlKey));
            if (saved != nullptr)
            {
                urlBox->Text = saved;
            }
        }
    }
    catch (Exception^)
    {
    }

    auto testButton = ref new Button();
    testButton->Content = ref new String(L"Connect / Test project");
    testButton->FontSize = 20;
    testButton->Padding = Thickness(24, 12, 24, 12);
    testButton->HorizontalAlignment = HorizontalAlignment::Left;

    auto diagnosticButton = ref new Button();
    diagnosticButton->Content = ref new String(L"Run network diagnostics");
    diagnosticButton->FontSize = 18;
    diagnosticButton->Padding = Thickness(22, 10, 22, 10);
    diagnosticButton->HorizontalAlignment = HorizontalAlignment::Left;

    auto endpointText = ref new TextBlock();
    endpointText->Text = ref new String(L"Endpoint: get_project_config.php");
    endpointText->FontSize = 15;
    endpointText->Opacity = 0.65;

    auto summaryTitle = ref new TextBlock();
    summaryTitle->Text = ref new String(L"Project summary");
    summaryTitle->FontSize = 24;
    summaryTitle->Margin = Thickness(0, 18, 0, 0);

    auto summaryBorder = ref new Border();
    summaryBorder->Padding = Thickness(18);
    summaryBorder->Background = ref new SolidColorBrush(ColorHelper::FromArgb(255, 32, 42, 51));

    auto summaryText = ref new TextBlock();
    summaryText->Text = ref new String(L"No project loaded.");
    summaryText->FontSize = 17;
    summaryText->TextWrapping = TextWrapping::Wrap;
    summaryBorder->Child = summaryText;

    auto logTitle = ref new TextBlock();
    logTitle->Text = ref new String(L"Log");
    logTitle->FontSize = 24;
    logTitle->Margin = Thickness(0, 18, 0, 0);

    auto logBorder = ref new Border();
    logBorder->Padding = Thickness(18);
    logBorder->Background = ref new SolidColorBrush(ColorHelper::FromArgb(255, 24, 31, 38));
    logBorder->MinHeight = 210;

    auto logText = ref new TextBlock();
    logText->Text = ref new String(L"Ready. Enter a BOINC project URL and run the network test.\n");
    logText->FontSize = 16;
    logText->TextWrapping = TextWrapping::Wrap;
    logBorder->Child = logText;

    auto appendLog = [logText](String^ message)
    {
        std::wstring combined = logText->Text ? logText->Text->Data() : L"";
        if (message != nullptr)
        {
            combined += message->Data();
        }
        combined += L"\n";
        logText->Text = ToPlatformString(combined);
    };

    diagnosticButton->Click += ref new RoutedEventHandler(
        [diagnosticButton, logText, appendLog](Object^, RoutedEventArgs^)
        {
            logText->Text = ref new String(L"");
            appendLog(ref new String(L"Network diagnostics"));
            std::wstring profileLine = L"1) Network profile: ";
            profileLine += GetNetworkState()->Data();
            appendLog(ToPlatformString(profileLine));
            diagnosticButton->IsEnabled = false;

            auto ipSocket = ref new StreamSocket();
            auto ipHost = ref new HostName(ref new String(L"1.1.1.1"));

            appendLog(ref new String(L"2) Direct IP TCP test: 1.1.1.1:443"));

            create_task(ipSocket->ConnectAsync(
                ipHost,
                ref new String(L"443"),
                SocketProtectionLevel::PlainSocket
            ))
            .then(
                [appendLog, diagnosticButton](task<void> ipTask)
                {
                    bool ipOk = false;
                    try
                    {
                        ipTask.get();
                        ipOk = true;
                        appendLog(ref new String(L"   PASS: outbound TCP by IP works."));
                    }
                    catch (Exception^ ex)
                    {
                        std::wstring msg = L"   FAIL: direct IP connection failed. HRESULT=0x";
                        wchar_t hexBuf[16] = {};
                        swprintf_s(hexBuf, L"%08X", static_cast<unsigned int>(ex->HResult));
                        msg += hexBuf;
                        msg += L" ";
                        msg += ex->Message->Data();
                        appendLog(ToPlatformString(msg));
                    }

                    if (!ipOk)
                    {
                        appendLog(ref new String(L"Result: outbound internet is blocked or unavailable for this app."));
                        diagnosticButton->IsEnabled = true;
                        return task_from_result();
                    }

                    appendLog(ref new String(L"3) DNS/TCP test: one.one.one.one:443"));

                    auto dnsSocket = ref new StreamSocket();
                    auto dnsHost = ref new HostName(ref new String(L"one.one.one.one"));

                    return create_task(dnsSocket->ConnectAsync(
                        dnsHost,
                        ref new String(L"443"),
                        SocketProtectionLevel::PlainSocket
                    )).then(
                        [appendLog, diagnosticButton](task<void> dnsTask)
                        {
                            try
                            {
                                dnsTask.get();
                                appendLog(ref new String(L"   PASS: hostname resolution and outbound TCP work."));
                                appendLog(ref new String(L"Result: network stack is OK; retry the BOINC project test."));
                            }
                            catch (Exception^ ex)
                            {
                                std::wstring msg = L"   FAIL: hostname/DNS test failed. HRESULT=0x";
                                wchar_t hexBuf[16] = {};
                                swprintf_s(hexBuf, L"%08X", static_cast<unsigned int>(ex->HResult));
                                msg += hexBuf;
                                msg += L" ";
                                msg += ex->Message->Data();
                                appendLog(ToPlatformString(msg));
                                appendLog(ref new String(L"Result: direct Internet works, but DNS/name resolution does not."));
                            }

                            diagnosticButton->IsEnabled = true;
                        },
                        task_continuation_context::use_current()
                    );
                },
                task_continuation_context::use_current()
            );
        }
    );

    testButton->Click += ref new RoutedEventHandler(
        [urlBox, testButton, logText, summaryText, appendLog](Object^, RoutedEventArgs^)
        {
            std::wstring baseUrl = NormalizeProjectUrl(urlBox->Text);
            if (baseUrl.empty())
            {
                appendLog(ref new String(L"ERROR: Project URL is empty."));
                return;
            }

            std::wstring endpoint = baseUrl + L"/get_project_config.php";
            auto endpointString = ToPlatformString(endpoint);

            try
            {
                ApplicationData::Current->LocalSettings->Values->Insert(
                    ref new String(L"ProjectUrl"),
                    ToPlatformString(baseUrl)
                );
            }
            catch (Exception^ ex)
            {
                std::wstring message = L"Warning: Could not save Project URL: ";
                message += ex->Message->Data();
                appendLog(ToPlatformString(message));
            }

            Uri^ uri = nullptr;
            try
            {
                uri = ref new Uri(endpointString);
            }
            catch (Exception^ ex)
            {
                std::wstring message = L"ERROR: Invalid URL: ";
                message += ex->Message->Data();
                appendLog(ToPlatformString(message));
                return;
            }

            logText->Text = ref new String(L"");
            appendLog(ref new String(L"Testing BOINC project..."));
            appendLog(ToPlatformString(L"GET " + endpoint));
            testButton->IsEnabled = false;

            auto filter = ref new HttpBaseProtocolFilter();
            filter->UseProxy = false;

            auto client = ref new HttpClient(filter);
            appendLog(ref new String(L"HTTP transport: direct connection (proxy disabled)"));

            create_task(client->GetAsync(uri))
                .then(
                    [](HttpResponseMessage^ response)
                    {
                        if (!response->IsSuccessStatusCode)
                        {
                            std::wstring message =
                                L"HTTP request failed, status " +
                                std::to_wstring(static_cast<unsigned int>(response->StatusCode));
                            throw ref new FailureException(ToPlatformString(message));
                        }

                        return create_task(response->Content->ReadAsStringAsync());
                    }
                )
                .then(
                    [appendLog, summaryText](String^ body)
                    {
                        std::wstring xml = body ? body->Data() : L"";
                        bool looksLikeBoinc =
                            xml.find(L"<project_config") != std::wstring::npos ||
                            xml.find(L"<project>") != std::wstring::npos;

                        appendLog(ref new String(L"HTTP: OK"));

                        if (looksLikeBoinc)
                        {
                            appendLog(ref new String(L"BOINC project configuration detected."));

                            const std::wstring projectName = ExtractFirstTag(xml, L"name");
                            const std::wstring masterUrl = ExtractFirstTag(xml, L"master_url");
                            const std::wstring serverVersion = ExtractFirstTag(xml, L"server_version");
                            const auto platforms = ExtractAllTags(xml, L"platform_name");
                            const bool xboxNative = ContainsPlatform(platforms, L"x86_64-pc-xbox-uwp");

                            std::wstring summary;
                            summary += L"Project: " + (projectName.empty() ? L"(unknown)" : projectName) + L"\n";
                            summary += L"Master URL: " + (masterUrl.empty() ? L"(not reported)" : masterUrl) + L"\n";
                            summary += L"Server version: " + (serverVersion.empty() ? L"(not reported)" : serverVersion) + L"\n";
                            summary += L"Platforms advertised: " + std::to_wstring(platforms.size()) + L"\n";
                            summary += L"Native Xbox platform: ";
                            summary += xboxNative ? L"YES" : L"NO";
                            summary += L"\n";
                            summary += L"Xbox platform ID: x86_64-pc-xbox-uwp";

                            summaryText->Text = ToPlatformString(summary);

                            try
                            {
                                auto values = ApplicationData::Current->LocalSettings->Values;
                                values->Insert(ref new String(L"ProjectName"), ToPlatformString(projectName));
                                values->Insert(ref new String(L"MasterUrl"), ToPlatformString(masterUrl));
                                values->Insert(ref new String(L"ServerVersion"), ToPlatformString(serverVersion));
                            }
                            catch (Exception^)
                            {
                            }

                            appendLog(ToPlatformString(
                                L"Parsed project: " +
                                (projectName.empty() ? L"(unknown)" : projectName) +
                                L"; platforms=" +
                                std::to_wstring(platforms.size())
                            ));
                        }
                        else
                        {
                            summaryText->Text = ref new String(L"Response is not a recognized BOINC project configuration.");
                            appendLog(ref new String(L"Response received, but BOINC project XML was not recognized."));
                        }

                        appendLog(ref new String(L"Project configuration parsed."));
                    },
                    task_continuation_context::use_current()
                )
                .then(
                    [appendLog, testButton](task<void> previousTask)
                    {
                        try
                        {
                            previousTask.get();
                        }
                        catch (Exception^ ex)
                        {
                            std::wstring message = L"ERROR HRESULT=0x";
                            wchar_t hexBuf[16] = {};
                            swprintf_s(hexBuf, L"%08X", static_cast<unsigned int>(ex->HResult));
                            message += hexBuf;
                            message += L" ";
                            message += ex->Message->Data();
                            appendLog(ToPlatformString(message));
                        }
                        catch (...)
                        {
                            appendLog(ref new String(L"ERROR: Unknown network failure."));
                        }

                        testButton->IsEnabled = true;
                    },
                    task_continuation_context::use_current()
                );
        }
    );

    panel->Children->Append(title);
    panel->Children->Append(subtitle);
    panel->Children->Append(statusBox);
    panel->Children->Append(projectTitle);
    panel->Children->Append(urlBox);
    panel->Children->Append(testButton);
    panel->Children->Append(diagnosticButton);
    panel->Children->Append(endpointText);
    panel->Children->Append(summaryTitle);
    panel->Children->Append(summaryBorder);
    panel->Children->Append(logTitle);
    panel->Children->Append(logBorder);

    scroll->Content = panel;
    root->Children->Append(scroll);

    Window::Current->Content = root;
    Window::Current->Activate();
}
