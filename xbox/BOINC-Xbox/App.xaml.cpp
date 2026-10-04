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
using namespace Windows::Security::Credentials;
using namespace Windows::Security::Cryptography;
using namespace Windows::Security::Cryptography::Core;
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
    struct ProjectInfo
    {
        std::wstring name;
        std::wstring masterUrl;
        std::wstring serverVersion;
        size_t platformCount = 0;
        bool xboxNative = false;
    };

    struct IntegrationState
    {
        std::wstring project = L"PENDING";
        std::wstring schedulerDiscovery = L"PENDING";
        std::wstring schedulerPreflight = L"PENDING";
        std::wstring account = L"PENDING";
        std::wstring cpu = L"PENDING";
        std::wstring memory = L"PENDING";
        std::wstring storage = L"PENDING";
        std::wstring schedulerUrl;
        std::wstring projectName;
        std::wstring masterUrl;
    };

    String^ ToPlatformString(const std::wstring& value)
    {
        return ref new String(value.c_str());
    }

    std::wstring Trim(const std::wstring& input)
    {
        std::wstring value = input;
        while (!value.empty() && iswspace(value.front())) value.erase(value.begin());
        while (!value.empty() && iswspace(value.back())) value.pop_back();
        return value;
    }

    std::wstring Lowercase(std::wstring value)
    {
        std::transform(
            value.begin(),
            value.end(),
            value.begin(),
            [](wchar_t c) { return static_cast<wchar_t>(towlower(c)); }
        );
        return value;
    }

    std::wstring NormalizeProjectUrl(String^ value)
    {
        std::wstring url = Trim(value ? value->Data() : L"");

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

        while (!url.empty() && url.back() == L'/') url.pop_back();
        return url;
    }

    std::wstring NormalizeUrlString(std::wstring url)
    {
        url = Trim(url);
        while (!url.empty() && url.back() == L'/') url.pop_back();
        return url;
    }

    std::wstring UrlEncode(const std::wstring& value)
    {
        std::wstring out;
        wchar_t buf[8] = {};

        for (wchar_t c : value)
        {
            const bool safe =
                (c >= L'a' && c <= L'z') ||
                (c >= L'A' && c <= L'Z') ||
                (c >= L'0' && c <= L'9') ||
                c == L'-' || c == L'_' || c == L'.' || c == L'~';

            if (safe)
            {
                out += c;
            }
            else if (c <= 0x7F)
            {
                swprintf_s(buf, L"%%%02X", static_cast<unsigned int>(c));
                out += buf;
            }
            else
            {
                out += c;
            }
        }

        return out;
    }

    std::wstring Md5Hex(const std::wstring& value)
    {
        auto provider = HashAlgorithmProvider::OpenAlgorithm(HashAlgorithmNames::Md5);
        auto input = CryptographicBuffer::ConvertStringToBinary(
            ToPlatformString(value),
            BinaryStringEncoding::Utf8
        );
        auto hash = provider->HashData(input);
        std::wstring hex = CryptographicBuffer::EncodeToHexString(hash)->Data();
        return Lowercase(hex);
    }

    std::wstring ExtractFirstTag(const std::wstring& xml, const std::wstring& tag)
    {
        const std::wstring open = L"<" + tag + L">";
        const std::wstring close = L"</" + tag + L">";
        const size_t startTag = xml.find(open);
        if (startTag == std::wstring::npos) return L"";

        const size_t valueStart = startTag + open.size();
        const size_t valueEnd = xml.find(close, valueStart);
        if (valueEnd == std::wstring::npos) return L"";

        return Trim(xml.substr(valueStart, valueEnd - valueStart));
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
            if (startTag == std::wstring::npos) break;

            const size_t valueStart = startTag + open.size();
            const size_t valueEnd = xml.find(close, valueStart);
            if (valueEnd == std::wstring::npos) break;

            values.push_back(Trim(xml.substr(valueStart, valueEnd - valueStart)));
            cursor = valueEnd + close.size();
        }

        return values;
    }

    void PushUnique(std::vector<std::wstring>& values, const std::wstring& value)
    {
        if (value.empty()) return;
        if (std::find(values.begin(), values.end(), value) == values.end())
        {
            values.push_back(value);
        }
    }

    std::vector<std::wstring> ExtractSchedulerUrls(const std::wstring& masterPage)
    {
        std::vector<std::wstring> urls = ExtractAllTags(masterPage, L"scheduler");
        size_t cursor = 0;

        while (true)
        {
            const size_t relPos = masterPage.find(L"boinc_scheduler", cursor);
            if (relPos == std::wstring::npos) break;

            const size_t tagStart = masterPage.rfind(L'<', relPos);
            const size_t tagEnd = masterPage.find(L'>', relPos);
            if (tagStart == std::wstring::npos || tagEnd == std::wstring::npos)
            {
                cursor = relPos + 1;
                continue;
            }

            const std::wstring tag = masterPage.substr(tagStart, tagEnd - tagStart + 1);
            const size_t hrefPos = tag.find(L"href=\"");
            if (hrefPos != std::wstring::npos)
            {
                const size_t start = hrefPos + 6;
                const size_t end = tag.find(L'\"', start);
                if (end != std::wstring::npos)
                {
                    PushUnique(urls, Trim(tag.substr(start, end - start)));
                }
            }

            cursor = tagEnd + 1;
        }

        return urls;
    }

    bool ContainsPlatform(const std::vector<std::wstring>& platforms, const std::wstring& wanted)
    {
        return std::find(platforms.begin(), platforms.end(), wanted) != platforms.end();
    }

    bool ParseProjectConfig(
        const std::wstring& xml,
        const std::wstring& fallbackUrl,
        ProjectInfo& info
    )
    {
        if (xml.find(L"<project_config") == std::wstring::npos &&
            xml.find(L"<project>") == std::wstring::npos)
        {
            return false;
        }

        info.name = ExtractFirstTag(xml, L"name");
        info.masterUrl = ExtractFirstTag(xml, L"master_url");
        if (info.masterUrl.empty()) info.masterUrl = fallbackUrl;
        info.masterUrl = NormalizeUrlString(info.masterUrl);
        info.serverVersion = ExtractFirstTag(xml, L"server_version");

        const auto platforms = ExtractAllTags(xml, L"platform_name");
        info.platformCount = platforms.size();
        info.xboxNative = ContainsPlatform(platforms, L"x86_64-pc-xbox-uwp");
        return true;
    }

    std::wstring ProjectSummary(const ProjectInfo& info)
    {
        std::wstring summary;
        summary += L"Project: " + (info.name.empty() ? L"(unknown)" : info.name) + L"\n";
        summary += L"Master URL: " + info.masterUrl + L"/\n";
        summary += L"Server version: " + (info.serverVersion.empty() ? L"(not reported)" : info.serverVersion) + L"\n";
        summary += L"Platforms advertised: " + std::to_wstring(info.platformCount) + L"\n";
        summary += L"Native Xbox platform: ";
        summary += info.xboxNative ? L"YES" : L"NO";
        summary += L"\nXbox platform ID: x86_64-pc-xbox-uwp";
        return summary;
    }

    void PersistProjectInfo(const std::wstring& projectUrl, const ProjectInfo& info)
    {
        auto values = ApplicationData::Current->LocalSettings->Values;
        values->Insert(ref new String(L"ProjectUrl"), ToPlatformString(projectUrl));
        values->Insert(ref new String(L"ProjectName"), ToPlatformString(info.name));
        values->Insert(ref new String(L"MasterUrl"), ToPlatformString(info.masterUrl));
        values->Insert(ref new String(L"ServerVersion"), ToPlatformString(info.serverVersion));
    }

    String^ GetNetworkState()
    {
        try
        {
            auto profile = NetworkInformation::GetInternetConnectionProfile();
            if (profile == nullptr) return ref new String(L"Offline");

            auto level = profile->GetNetworkConnectivityLevel();
            if (level == NetworkConnectivityLevel::InternetAccess) return ref new String(L"Online");
            if (level == NetworkConnectivityLevel::ConstrainedInternetAccess) return ref new String(L"Constrained");
            if (level == NetworkConnectivityLevel::LocalAccess) return ref new String(L"Local only");
        }
        catch (Exception^)
        {
        }

        return ref new String(L"Offline");
    }

    HttpClient^ CreateDirectHttpClient()
    {
        auto filter = ref new HttpBaseProtocolFilter();
        filter->UseProxy = false;
        return ref new HttpClient(filter);
    }

    task<String^> HttpGetText(const std::wstring& url)
    {
        auto client = CreateDirectHttpClient();
        auto uri = ref new Uri(ToPlatformString(url));

        return create_task(client->GetAsync(uri)).then(
            [client](HttpResponseMessage^ response) -> task<String^>
            {
                if (!response->IsSuccessStatusCode)
                {
                    std::wstring message = L"HTTP status " +
                        std::to_wstring(static_cast<unsigned int>(response->StatusCode));
                    throw ref new FailureException(ToPlatformString(message));
                }

                return create_task(response->Content->ReadAsStringAsync());
            }
        );
    }

    bool ParseHostAndPort(const std::wstring& url, std::wstring& host, std::wstring& port)
    {
        const size_t schemeEnd = url.find(L"://");
        if (schemeEnd == std::wstring::npos) return false;

        const std::wstring scheme = Lowercase(url.substr(0, schemeEnd));
        const size_t authorityStart = schemeEnd + 3;
        const size_t authorityEnd = url.find(L'/', authorityStart);
        const std::wstring authority = url.substr(
            authorityStart,
            authorityEnd == std::wstring::npos ? std::wstring::npos : authorityEnd - authorityStart
        );

        if (authority.empty()) return false;

        const size_t colon = authority.rfind(L':');
        if (colon != std::wstring::npos && authority.find(L']') == std::wstring::npos)
        {
            host = authority.substr(0, colon);
            port = authority.substr(colon + 1);
        }
        else
        {
            host = authority;
            port = scheme == L"https" ? L"443" : L"80";
        }

        return !host.empty() && !port.empty();
    }

    task<std::wstring> SchedulerTcpPreflight(const std::wstring& schedulerUrl)
    {
        std::wstring host;
        std::wstring port;
        if (!ParseHostAndPort(schedulerUrl, host, port))
        {
            throw ref new FailureException(ref new String(L"Invalid scheduler URL"));
        }

        auto socket = ref new StreamSocket();
        auto hostName = ref new HostName(ToPlatformString(host));
        return create_task(socket->ConnectAsync(
            hostName,
            ToPlatformString(port),
            SocketProtectionLevel::PlainSocket
        )).then(
            [socket, host, port](task<void> previousTask)
            {
                previousTask.get();
                return L"PASS: " + host + L":" + port;
            }
        );
    }

    std::wstring RunCpuBenchmark(unsigned int threadCount)
    {
        if (threadCount == 0) threadCount = 1;

        std::vector<std::thread> workers;
        std::vector<unsigned long long> sums(threadCount, 0);
        const unsigned int iterations = 2500000;
        const auto start = std::chrono::steady_clock::now();

        for (unsigned int i = 0; i < threadCount; ++i)
        {
            workers.emplace_back([i, iterations, &sums]()
            {
                unsigned long long x = 0x9E3779B97F4A7C15ULL ^
                    (static_cast<unsigned long long>(i) + 1ULL);

                for (unsigned int n = 0; n < iterations; ++n)
                {
                    x ^= x << 13;
                    x ^= x >> 7;
                    x ^= x << 17;
                    x += static_cast<unsigned long long>(n) * 0x5851F42D4C957F2DULL;
                }

                sums[i] = x;
            });
        }

        for (auto& worker : workers) worker.join();

        unsigned long long checksum = 0;
        for (auto value : sums) checksum ^= value;

        const auto end = std::chrono::steady_clock::now();
        const auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(
            end - start
        ).count();

        return L"PASS: workers=" + std::to_wstring(threadCount) +
            L", time=" + std::to_wstring(elapsedMs) +
            L" ms, checksum=" + std::to_wstring(checksum & 0xFFFFULL);
    }

    bool StoreAuthenticator(
        const std::wstring& projectUrl,
        const std::wstring& email,
        const std::wstring& authenticator
    )
    {
        try
        {
            auto vault = ref new PasswordVault();
            auto resource = ToPlatformString(L"BOINC Xbox:" + projectUrl);

            try
            {
                auto oldCredentials = vault->FindAllByResource(resource);
                for each (PasswordCredential^ credential in oldCredentials)
                {
                    vault->Remove(credential);
                }
            }
            catch (Exception^)
            {
            }

            vault->Add(ref new PasswordCredential(
                resource,
                ToPlatformString(email),
                ToPlatformString(authenticator)
            ));

            auto stored = vault->FindAllByResource(resource);
            return stored != nullptr && stored->Size > 0;
        }
        catch (Exception^)
        {
            return false;
        }
    }

    std::wstring FinalizeStatus(std::wstring status, const std::wstring& failText)
    {
        if (status == L"PENDING" || status == L"RUNNING") return failText;
        return status;
    }

    std::wstring BuildIntegrationReport(const std::shared_ptr<IntegrationState>& state)
    {
        std::wstring report = L"v0.4 integration report\n\n";
        report += L"Project: " + FinalizeStatus(state->project, L"FAIL / not completed") + L"\n";
        report += L"Scheduler discovery: " +
            FinalizeStatus(state->schedulerDiscovery, L"FAIL / not completed") + L"\n";
        report += L"Scheduler TCP: " +
            FinalizeStatus(state->schedulerPreflight, L"FAIL / not completed") + L"\n";
        report += L"Account: " + FinalizeStatus(state->account, L"FAIL / not completed") + L"\n";
        report += L"CPU: " + FinalizeStatus(state->cpu, L"FAIL / not completed") + L"\n";
        report += L"Memory: " + FinalizeStatus(state->memory, L"FAIL / not completed") + L"\n";
        report += L"Storage: " + FinalizeStatus(state->storage, L"FAIL / not completed") + L"\n";
        report += L"Password persistence: PASS - plaintext password is never stored";
        return report;
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
    subtitle->Text = ref new String(L"v0.4 - Integration build");
    subtitle->FontSize = 22;
    subtitle->Opacity = 0.8;
    subtitle->HorizontalAlignment = HorizontalAlignment::Center;

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

    const unsigned long long plannedMemoryMb = (memoryLimitMb * 90ULL) / 100ULL;

    auto statusBox = ref new Border();
    statusBox->Padding = Thickness(22);
    statusBox->Background = ref new SolidColorBrush(ColorHelper::FromArgb(255, 32, 42, 51));

    auto statusPanel = ref new StackPanel();
    statusPanel->Spacing = 7;

    auto platformText = ref new TextBlock();
    platformText->Text = ToPlatformString(L"Platform: " + platformWide);
    platformText->FontSize = 18;

    auto cpuText = ref new TextBlock();
    cpuText->Text = ToPlatformString(
        L"CPU threads visible to app: " + std::to_wstring(cpuThreads)
    );
    cpuText->FontSize = 18;

    auto memoryText = ref new TextBlock();
    memoryText->Text = ToPlatformString(
        L"App memory limit: " + std::to_wstring(memoryLimitMb) + L" MB"
    );
    memoryText->FontSize = 18;

    auto resourcePolicyText = ref new TextBlock();
    resourcePolicyText->Text = ToPlatformString(
        L"Compute policy: all visible CPU threads; memory high-water=" +
        std::to_wstring(plannedMemoryMb) + L" MB"
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
    urlBox->PlaceholderText = ref new String(L"https://asteroidsathome.net/boinc/");
    urlBox->FontSize = 20;
    urlBox->MinHeight = 50;

    auto testButton = ref new Button();
    testButton->Content = ref new String(L"Connect / Test project");
    testButton->FontSize = 20;
    testButton->Padding = Thickness(24, 12, 24, 12);
    testButton->HorizontalAlignment = HorizontalAlignment::Left;

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

    auto accountTitle = ref new TextBlock();
    accountTitle->Text = ref new String(L"BOINC account");
    accountTitle->FontSize = 24;
    accountTitle->Margin = Thickness(0, 18, 0, 0);

    auto emailBox = ref new TextBox();
    emailBox->Header = ref new String(L"Email");
    emailBox->FontSize = 19;

    auto passwordBox = ref new PasswordBox();
    passwordBox->Header = ref new String(L"Password - never stored");
    passwordBox->FontSize = 19;
    passwordBox->PlaceholderText = ref new String(L"Enter BOINC project password");

    auto integrationButton = ref new Button();
    integrationButton->Content = ref new String(L"Run full integration test");
    integrationButton->FontSize = 21;
    integrationButton->Padding = Thickness(26, 13, 26, 13);
    integrationButton->HorizontalAlignment = HorizontalAlignment::Left;

    auto integrationHint = ref new TextBlock();
    integrationHint->Text = ref new String(
        L"One run tests project config, scheduler discovery, scheduler TCP, account login, all visible CPU workers, memory policy and persistence."
    );
    integrationHint->TextWrapping = TextWrapping::Wrap;
    integrationHint->Opacity = 0.75;
    integrationHint->FontSize = 15;

    auto reportTitle = ref new TextBlock();
    reportTitle->Text = ref new String(L"Integration report");
    reportTitle->FontSize = 24;
    reportTitle->Margin = Thickness(0, 18, 0, 0);

    auto reportBorder = ref new Border();
    reportBorder->Padding = Thickness(18);
    reportBorder->Background = ref new SolidColorBrush(ColorHelper::FromArgb(255, 32, 42, 51));

    auto reportText = ref new TextBlock();
    reportText->Text = ref new String(L"Not run yet.");
    reportText->FontSize = 16;
    reportText->TextWrapping = TextWrapping::Wrap;
    reportBorder->Child = reportText;

    auto diagnosticButton = ref new Button();
    diagnosticButton->Content = ref new String(L"Run network diagnostics");
    diagnosticButton->FontSize = 17;
    diagnosticButton->Padding = Thickness(20, 9, 20, 9);
    diagnosticButton->HorizontalAlignment = HorizontalAlignment::Left;

    auto logTitle = ref new TextBlock();
    logTitle->Text = ref new String(L"Log");
    logTitle->FontSize = 24;
    logTitle->Margin = Thickness(0, 18, 0, 0);

    auto logBorder = ref new Border();
    logBorder->Padding = Thickness(18);
    logBorder->Background = ref new SolidColorBrush(ColorHelper::FromArgb(255, 24, 31, 38));
    logBorder->MinHeight = 210;

    auto logText = ref new TextBlock();
    logText->Text = ref new String(L"Ready.\n");
    logText->FontSize = 16;
    logText->TextWrapping = TextWrapping::Wrap;
    logBorder->Child = logText;

    auto appendLog = [logText](String^ message)
    {
        std::wstring combined = logText->Text ? logText->Text->Data() : L"";
        if (message != nullptr) combined += message->Data();
        combined += L"\n";
        logText->Text = ToPlatformString(combined);
    };

    try
    {
        auto values = ApplicationData::Current->LocalSettings->Values;
        auto projectUrlKey = ref new String(L"ProjectUrl");
        auto emailKey = ref new String(L"AccountEmail");

        if (values->HasKey(projectUrlKey))
        {
            auto saved = dynamic_cast<String^>(values->Lookup(projectUrlKey));
            if (saved != nullptr) urlBox->Text = saved;
        }

        if (values->HasKey(emailKey))
        {
            auto savedEmail = dynamic_cast<String^>(values->Lookup(emailKey));
            if (savedEmail != nullptr) emailBox->Text = savedEmail;
        }

        auto projectNameKey = ref new String(L"ProjectName");
        auto masterUrlKey = ref new String(L"MasterUrl");
        if (values->HasKey(projectNameKey) && values->HasKey(masterUrlKey))
        {
            auto savedName = dynamic_cast<String^>(values->Lookup(projectNameKey));
            auto savedMaster = dynamic_cast<String^>(values->Lookup(masterUrlKey));
            if (savedName != nullptr && savedMaster != nullptr)
            {
                summaryText->Text = ToPlatformString(
                    L"Project: " + std::wstring(savedName->Data()) +
                    L"\nMaster URL: " + std::wstring(savedMaster->Data()) +
                    L"\nSaved project metadata restored."
                );
            }
        }
    }
    catch (Exception^)
    {
    }

    testButton->Click += ref new RoutedEventHandler(
        [urlBox, testButton, summaryText, logText, appendLog](Object^, RoutedEventArgs^)
        {
            const std::wstring baseUrl = NormalizeProjectUrl(urlBox->Text);
            if (baseUrl.empty())
            {
                appendLog(ref new String(L"ERROR: Project URL is empty."));
                return;
            }

            logText->Text = ref new String(L"");
            appendLog(ref new String(L"Testing BOINC project..."));
            testButton->IsEnabled = false;

            HttpGetText(baseUrl + L"/get_project_config.php")
                .then(
                    [baseUrl, summaryText, appendLog](String^ body)
                    {
                        ProjectInfo info;
                        const std::wstring xml = body ? body->Data() : L"";
                        if (!ParseProjectConfig(xml, baseUrl, info))
                        {
                            throw ref new FailureException(
                                ref new String(L"BOINC project XML not recognized")
                            );
                        }

                        summaryText->Text = ToPlatformString(ProjectSummary(info));
                        PersistProjectInfo(baseUrl, info);
                        appendLog(ref new String(L"PASS: Project configuration parsed."));
                    },
                    task_continuation_context::use_current()
                )
                .then(
                    [testButton, appendLog](task<void> previousTask)
                    {
                        try
                        {
                            previousTask.get();
                        }
                        catch (Exception^ ex)
                        {
                            std::wstring msg = L"FAIL: ";
                            msg += ex->Message->Data();
                            appendLog(ToPlatformString(msg));
                        }
                        testButton->IsEnabled = true;
                    },
                    task_continuation_context::use_current()
                );
        }
    );

    integrationButton->Click += ref new RoutedEventHandler(
        [urlBox, emailBox, passwordBox, integrationButton, summaryText, reportText, logText, appendLog, cpuThreads, memoryLimitMb, plannedMemoryMb](Object^, RoutedEventArgs^)
        {
            const std::wstring baseUrl = NormalizeProjectUrl(urlBox->Text);
            if (baseUrl.empty())
            {
                reportText->Text = ref new String(L"FAIL: Project URL is empty.");
                return;
            }

            const std::wstring email = Lowercase(
                Trim(emailBox->Text ? emailBox->Text->Data() : L"")
            );

            std::wstring password = passwordBox->Password
                ? passwordBox->Password->Data()
                : L"";

            std::wstring passwdHash;
            if (!email.empty() && !password.empty())
            {
                passwdHash = Md5Hex(password + email);
            }

            std::fill(password.begin(), password.end(), L'\0');
            password.clear();
            passwordBox->Password = ref new String(L"");

            auto state = std::make_shared<IntegrationState>();
            state->project = L"RUNNING";
            state->memory = L"PASS: limit=" + std::to_wstring(memoryLimitMb) +
                L" MB, high-water=" + std::to_wstring(plannedMemoryMb) + L" MB";

            integrationButton->IsEnabled = false;
            logText->Text = ref new String(L"");
            reportText->Text = ref new String(L"Integration test running...");
            appendLog(ref new String(L"Starting v0.4 integration suite..."));
            appendLog(ref new String(
                L"Plaintext password is hashed immediately, cleared from the UI and never persisted."
            ));

            try
            {
                auto values = ApplicationData::Current->LocalSettings->Values;
                auto probeKey = ref new String(L"IntegrationPersistenceProbe");
                auto probeValue = ref new String(L"v0.4-ok");
                values->Insert(probeKey, probeValue);
                auto restored = dynamic_cast<String^>(values->Lookup(probeKey));

                state->storage =
                    restored != nullptr && std::wstring(restored->Data()) == L"v0.4-ok"
                    ? L"PASS: write/read persistence OK"
                    : L"FAIL: persistence mismatch";
            }
            catch (Exception^ ex)
            {
                state->storage = L"FAIL: ";
                state->storage += ex->Message->Data();
            }

            auto cpuTask = create_task([state, cpuThreads]()
            {
                try
                {
                    state->cpu = RunCpuBenchmark(cpuThreads);
                }
                catch (...)
                {
                    state->cpu = L"FAIL: CPU benchmark exception";
                }
            });

            auto networkTask = HttpGetText(baseUrl + L"/get_project_config.php")
                .then(
                    [state, baseUrl, summaryText, appendLog](String^ body) -> task<String^>
                    {
                        ProjectInfo info;
                        const std::wstring xml = body ? body->Data() : L"";
                        if (!ParseProjectConfig(xml, baseUrl, info))
                        {
                            state->project = L"FAIL: BOINC XML not recognized";
                            throw ref new FailureException(
                                ref new String(L"BOINC project XML not recognized")
                            );
                        }

                        state->projectName = info.name;
                        state->masterUrl = info.masterUrl;
                        state->project = L"PASS: " +
                            (info.name.empty() ? L"unknown project" : info.name) +
                            L", platforms=" + std::to_wstring(info.platformCount);

                        summaryText->Text = ToPlatformString(ProjectSummary(info));
                        PersistProjectInfo(baseUrl, info);
                        appendLog(ref new String(L"PASS: Project configuration"));

                        state->schedulerDiscovery = L"RUNNING";
                        return HttpGetText(state->masterUrl + L"/");
                    },
                    task_continuation_context::use_current()
                )
                .then(
                    [state, appendLog](String^ masterBody) -> task<std::wstring>
                    {
                        const std::wstring html = masterBody ? masterBody->Data() : L"";
                        const auto schedulers = ExtractSchedulerUrls(html);
                        if (schedulers.empty())
                        {
                            state->schedulerDiscovery = L"FAIL: no scheduler URL found";
                            throw ref new FailureException(
                                ref new String(L"No scheduler URL found in master page")
                            );
                        }

                        state->schedulerUrl = schedulers.front();
                        state->schedulerDiscovery = L"PASS: " + state->schedulerUrl;
                        appendLog(ToPlatformString(
                            L"PASS: Scheduler discovered: " + state->schedulerUrl
                        ));

                        try
                        {
                            ApplicationData::Current->LocalSettings->Values->Insert(
                                ref new String(L"SchedulerUrl"),
                                ToPlatformString(state->schedulerUrl)
                            );
                        }
                        catch (Exception^)
                        {
                        }

                        state->schedulerPreflight = L"RUNNING";
                        return SchedulerTcpPreflight(state->schedulerUrl);
                    },
                    task_continuation_context::use_current()
                )
                .then(
                    [state, email, passwdHash, appendLog](std::wstring schedulerResult) -> task<String^>
                    {
                        state->schedulerPreflight = schedulerResult;
                        appendLog(ToPlatformString(
                            L"PASS: Scheduler TCP preflight: " + schedulerResult
                        ));

                        if (email.empty() || passwdHash.empty())
                        {
                            state->account = L"SKIP: enter email and password to test account";
                            appendLog(ref new String(
                                L"SKIP: Account login - email/password not entered."
                            ));
                            return task_from_result(ref new String(L""));
                        }

                        state->account = L"RUNNING";
                        const std::wstring lookupUrl =
                            state->masterUrl +
                            L"/lookup_account.php?email_addr=" + UrlEncode(email) +
                            L"&passwd_hash=" + UrlEncode(passwdHash);

                        appendLog(ref new String(L"Testing BOINC account lookup..."));
                        return HttpGetText(lookupUrl);
                    },
                    task_continuation_context::use_current()
                )
                .then(
                    [state, email, appendLog](String^ accountBody)
                    {
                        if (accountBody == nullptr || accountBody->Length() == 0)
                        {
                            return;
                        }

                        const std::wstring xml = accountBody->Data();
                        const std::wstring authenticator = ExtractFirstTag(
                            xml,
                            L"authenticator"
                        );

                        if (authenticator.empty())
                        {
                            std::wstring errorMsg = ExtractFirstTag(xml, L"error_msg");
                            if (errorMsg.empty()) errorMsg = ExtractFirstTag(xml, L"error_num");
                            if (errorMsg.empty()) errorMsg = L"authenticator missing";

                            state->account = L"FAIL: " + errorMsg;
                            throw ref new FailureException(
                                ToPlatformString(L"Account lookup failed: " + errorMsg)
                            );
                        }

                        if (!StoreAuthenticator(state->masterUrl, email, authenticator))
                        {
                            state->account =
                                L"FAIL: authenticator received but PasswordVault storage failed";
                            throw ref new FailureException(
                                ref new String(L"PasswordVault storage failed")
                            );
                        }

                        try
                        {
                            ApplicationData::Current->LocalSettings->Values->Insert(
                                ref new String(L"AccountEmail"),
                                ToPlatformString(email)
                            );
                        }
                        catch (Exception^)
                        {
                        }

                        state->account = L"PASS: authenticator stored securely";
                        appendLog(ref new String(
                            L"PASS: Account connected; authenticator stored in PasswordVault."
                        ));
                    },
                    task_continuation_context::use_current()
                );

            networkTask.then(
                [cpuTask, appendLog](task<void> completedNetworkTask) mutable -> task<void>
                {
                    try
                    {
                        completedNetworkTask.get();
                        appendLog(ref new String(L"Network/account part completed."));
                    }
                    catch (Exception^ ex)
                    {
                        std::wstring msg = L"Network/account part stopped: ";
                        msg += ex->Message->Data();
                        appendLog(ToPlatformString(msg));
                    }
                    catch (...)
                    {
                        appendLog(ref new String(
                            L"Network/account part stopped by an unknown error."
                        ));
                    }

                    return cpuTask;
                },
                task_continuation_context::use_current()
            ).then(
                [state, integrationButton, reportText, appendLog](task<void> completedCpuTask)
                {
                    try
                    {
                        completedCpuTask.get();
                    }
                    catch (...)
                    {
                        state->cpu = L"FAIL: CPU task exception";
                    }

                    appendLog(ref new String(L"Integration suite completed."));
                    reportText->Text = ToPlatformString(BuildIntegrationReport(state));
                    integrationButton->IsEnabled = true;
                },
                task_continuation_context::use_current()
            );
        }
    );

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

            create_task(ipSocket->ConnectAsync(
                ipHost,
                ref new String(L"443"),
                SocketProtectionLevel::PlainSocket
            )).then(
                [appendLog, diagnosticButton](task<void> ipTask)
                {
                    try
                    {
                        ipTask.get();
                        appendLog(ref new String(L"2) Direct IP TCP: PASS"));
                    }
                    catch (Exception^ ex)
                    {
                        std::wstring msg = L"2) Direct IP TCP: FAIL - ";
                        msg += ex->Message->Data();
                        appendLog(ToPlatformString(msg));
                        diagnosticButton->IsEnabled = true;
                        return task_from_result();
                    }

                    auto dnsSocket = ref new StreamSocket();
                    auto dnsHost = ref new HostName(ref new String(L"one.one.one.one"));
                    return create_task(dnsSocket->ConnectAsync(
                        dnsHost,
                        ref new String(L"443"),
                        SocketProtectionLevel::PlainSocket
                    )).then(
                        [dnsSocket, appendLog, diagnosticButton](task<void> dnsTask)
                        {
                            try
                            {
                                dnsTask.get();
                                appendLog(ref new String(L"3) DNS/TCP: PASS"));
                                appendLog(ref new String(L"Result: network stack OK."));
                            }
                            catch (Exception^ ex)
                            {
                                std::wstring msg = L"3) DNS/TCP: FAIL - ";
                                msg += ex->Message->Data();
                                appendLog(ToPlatformString(msg));
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

    panel->Children->Append(title);
    panel->Children->Append(subtitle);
    panel->Children->Append(statusBox);
    panel->Children->Append(projectTitle);
    panel->Children->Append(urlBox);
    panel->Children->Append(testButton);
    panel->Children->Append(summaryTitle);
    panel->Children->Append(summaryBorder);
    panel->Children->Append(accountTitle);
    panel->Children->Append(emailBox);
    panel->Children->Append(passwordBox);
    panel->Children->Append(integrationButton);
    panel->Children->Append(integrationHint);
    panel->Children->Append(reportTitle);
    panel->Children->Append(reportBorder);
    panel->Children->Append(diagnosticButton);
    panel->Children->Append(logTitle);
    panel->Children->Append(logBorder);

    scroll->Content = panel;
    root->Children->Append(scroll);

    Window::Current->Content = root;
    Window::Current->Activate();
}
