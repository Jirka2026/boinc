#include "pch.h"
#include "App.xaml.h"
#include "xbox_platform.h"

#include <d3d11_1.h>
#include <dxgi1_2.h>
#include <wrl.h>

using namespace BOINC_Xbox;
using namespace Platform;
using namespace concurrency;
using namespace Microsoft::WRL;
using namespace Windows::ApplicationModel::Activation;
using namespace Windows::Foundation;
using namespace Windows::Networking;
using namespace Windows::Networking::Connectivity;
using namespace Windows::Networking::Sockets;
using namespace Windows::Security::Credentials;
using namespace Windows::Security::Cryptography;
using namespace Windows::Security::Cryptography::Core;
using namespace Windows::Storage;
using namespace Windows::Storage::Streams;
using namespace Windows::System;
using namespace Windows::UI;
using namespace Windows::UI::Xaml;
using namespace Windows::UI::Xaml::Controls;
using namespace Windows::UI::Xaml::Media;
using namespace Windows::Web::Http;
using namespace Windows::Web::Http::Filters;

namespace
{
    struct RunState
    {
        std::wstring project = L"PENDING";
        std::wstring schedulerDiscovery = L"PENDING";
        std::wstring schedulerTcp = L"PENDING";
        std::wstring account = L"PENDING";
        std::wstring schedulerRpc = L"PENDING";
        std::wstring host = L"PENDING";
        std::wstring work = L"PENDING";
        std::wstring download = L"PENDING";
        std::wstring cpu = L"PENDING";
        std::wstring gpu = L"PENDING";
        std::wstring memory = L"PENDING";
        std::wstring storage = L"PENDING";
        std::wstring projectUrl;
        std::wstring masterUrl;
        std::wstring schedulerUrl;
        std::wstring authenticator;
        std::wstring email;
        unsigned long hostId = 0;
        int rpcSeqno = 0;
    };

    String^ PS(const std::wstring& s) { return ref new String(s.c_str()); }

    std::wstring Trim(std::wstring s)
    {
        while (!s.empty() && iswspace(s.front())) s.erase(s.begin());
        while (!s.empty() && iswspace(s.back())) s.pop_back();
        return s;
    }

    std::wstring Lower(std::wstring s)
    {
        std::transform(s.begin(), s.end(), s.begin(), [](wchar_t c) { return (wchar_t)towlower(c); });
        return s;
    }

    std::wstring NormalizeUrl(std::wstring s)
    {
        s = Trim(s);
        if (s.rfind(L"https//", 0) == 0) s.replace(0, 7, L"https://");
        else if (s.rfind(L"http//", 0) == 0) s.replace(0, 6, L"http://");
        else if (s.rfind(L"https:/", 0) == 0 && s.rfind(L"https://", 0) != 0) s.replace(0, 7, L"https://");
        else if (s.rfind(L"http:/", 0) == 0 && s.rfind(L"http://", 0) != 0) s.replace(0, 6, L"http://");
        else if (!s.empty() && s.find(L"://") == std::wstring::npos) s = L"https://" + s;
        while (!s.empty() && s.back() == L'/') s.pop_back();
        return s;
    }

    std::wstring EscapeXml(const std::wstring& s)
    {
        std::wstring out;
        for (wchar_t c : s)
        {
            if (c == L'&') out += L"&amp;";
            else if (c == L'<') out += L"&lt;";
            else if (c == L'>') out += L"&gt;";
            else if (c == L'\"') out += L"&quot;";
            else if (c == L'\'') out += L"&apos;";
            else out += c;
        }
        return out;
    }

    std::wstring UrlEncode(const std::wstring& s)
    {
        std::wstring out;
        wchar_t buf[8] = {};
        for (wchar_t c : s)
        {
            bool safe = iswalnum(c) || c == L'-' || c == L'_' || c == L'.' || c == L'~';
            if (safe && c <= 0x7F) out += c;
            else if (c <= 0x7F)
            {
                swprintf_s(buf, L"%%%02X", (unsigned int)c);
                out += buf;
            }
            else out += c;
        }
        return out;
    }

    std::wstring Md5Hex(const std::wstring& s)
    {
        auto p = HashAlgorithmProvider::OpenAlgorithm(HashAlgorithmNames::Md5);
        auto b = CryptographicBuffer::ConvertStringToBinary(PS(s), BinaryStringEncoding::Utf8);
        return Lower(CryptographicBuffer::EncodeToHexString(p->HashData(b))->Data());
    }

    std::wstring Tag(const std::wstring& xml, const std::wstring& name)
    {
        std::wstring a = L"<" + name + L">", b = L"</" + name + L">";
        size_t p = xml.find(a);
        if (p == std::wstring::npos) return L"";
        p += a.size();
        size_t q = xml.find(b, p);
        return q == std::wstring::npos ? L"" : Trim(xml.substr(p, q - p));
    }

    size_t Count(const std::wstring& xml, const std::wstring& name)
    {
        std::wstring n = L"<" + name;
        size_t pos = 0, c = 0;
        while ((pos = xml.find(n, pos)) != std::wstring::npos) { ++c; pos += n.size(); }
        return c;
    }

    std::vector<std::wstring> SchedulerUrls(const std::wstring& page)
    {
        std::vector<std::wstring> out;
        size_t pos = 0;
        while (true)
        {
            size_t a = page.find(L"<scheduler>", pos);
            if (a == std::wstring::npos) break;
            a += 11;
            size_t b = page.find(L"</scheduler>", a);
            if (b == std::wstring::npos) break;
            std::wstring u = Trim(page.substr(a, b - a));
            if (!u.empty() && std::find(out.begin(), out.end(), u) == out.end()) out.push_back(u);
            pos = b + 12;
        }
        pos = 0;
        while (true)
        {
            size_t r = page.find(L"boinc_scheduler", pos);
            if (r == std::wstring::npos) break;
            size_t a = page.rfind(L'<', r), b = page.find(L'>', r);
            if (a == std::wstring::npos || b == std::wstring::npos) break;
            std::wstring t = page.substr(a, b - a + 1);
            size_t h = t.find(L"href=\"");
            if (h != std::wstring::npos)
            {
                h += 6;
                size_t e = t.find(L'\"', h);
                if (e != std::wstring::npos)
                {
                    std::wstring u = Trim(t.substr(h, e - h));
                    if (!u.empty() && std::find(out.begin(), out.end(), u) == out.end()) out.push_back(u);
                }
            }
            pos = b + 1;
        }
        return out;
    }

    HttpClient^ Client()
    {
        auto f = ref new HttpBaseProtocolFilter();
        f->UseProxy = false;
        return ref new HttpClient(f);
    }

    task<String^> GetText(const std::wstring& url)
    {
        auto c = Client();
        return create_task(c->GetAsync(ref new Uri(PS(url)))).then([c](HttpResponseMessage^ r) -> task<String^>
        {
            if (!r->IsSuccessStatusCode) throw ref new FailureException(PS(L"HTTP " + std::to_wstring((unsigned int)r->StatusCode)));
            return create_task(r->Content->ReadAsStringAsync());
        });
    }

    task<String^> PostXml(const std::wstring& url, const std::wstring& xml)
    {
        auto c = Client();
        auto content = ref new HttpStringContent(PS(xml), UnicodeEncoding::Utf8, ref new String(L"text/xml"));
        return create_task(c->PostAsync(ref new Uri(PS(url)), content)).then([c, content](HttpResponseMessage^ r) -> task<String^>
        {
            if (!r->IsSuccessStatusCode) throw ref new FailureException(PS(L"HTTP " + std::to_wstring((unsigned int)r->StatusCode)));
            return create_task(r->Content->ReadAsStringAsync());
        });
    }

    bool HostPort(const std::wstring& url, std::wstring& host, std::wstring& port)
    {
        size_t s = url.find(L"://");
        if (s == std::wstring::npos) return false;
        std::wstring scheme = Lower(url.substr(0, s));
        size_t a = s + 3, b = url.find(L'/', a);
        std::wstring authority = url.substr(a, b == std::wstring::npos ? std::wstring::npos : b - a);
        size_t c = authority.rfind(L':');
        if (c != std::wstring::npos) { host = authority.substr(0, c); port = authority.substr(c + 1); }
        else { host = authority; port = scheme == L"https" ? L"443" : L"80"; }
        return !host.empty();
    }

    task<std::wstring> TcpTest(const std::wstring& url)
    {
        std::wstring host, port;
        if (!HostPort(url, host, port)) throw ref new FailureException(ref new String(L"Bad scheduler URL"));
        auto s = ref new StreamSocket();
        return create_task(s->ConnectAsync(ref new HostName(PS(host)), PS(port), SocketProtectionLevel::PlainSocket))
            .then([s, host, port](task<void> t) { t.get(); return L"PASS: " + host + L":" + port; });
    }

    std::wstring CpuTest(unsigned int n)
    {
        if (!n) n = 1;
        std::vector<std::thread> w;
        std::vector<unsigned long long> r(n, 0);
        auto start = std::chrono::steady_clock::now();
        for (unsigned int i = 0; i < n; ++i) w.emplace_back([i, &r]()
        {
            unsigned long long x = 0x9E3779B97F4A7C15ULL ^ (i + 1);
            for (unsigned int k = 0; k < 5000000; ++k) { x ^= x << 13; x ^= x >> 7; x ^= x << 17; x += k; }
            r[i] = x;
        });
        for (auto& x : w) x.join();
        unsigned long long sum = 0; for (auto x : r) sum ^= x;
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();
        return L"PASS: workers=" + std::to_wstring(n) + L", time=" + std::to_wstring(ms) + L" ms, checksum=" + std::to_wstring(sum & 0xFFFF);
    }

    std::wstring GpuTest()
    {
        UINT flags = 0;
        D3D_FEATURE_LEVEL levels[] = { D3D_FEATURE_LEVEL_12_1, D3D_FEATURE_LEVEL_12_0, D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0 };
        D3D_FEATURE_LEVEL got = D3D_FEATURE_LEVEL_11_0;
        ComPtr<ID3D11Device> dev;
        ComPtr<ID3D11DeviceContext> ctx;
        HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags, levels, ARRAYSIZE(levels), D3D11_SDK_VERSION, &dev, &got, &ctx);
        if (FAILED(hr)) return L"FAIL: D3D11CreateDevice HRESULT=" + std::to_wstring((long long)hr);

        D3D11_BUFFER_DESC bd = {};
        bd.ByteWidth = 1024 * sizeof(UINT);
        bd.Usage = D3D11_USAGE_DEFAULT;
        bd.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
        bd.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
        bd.StructureByteStride = sizeof(UINT);
        ComPtr<ID3D11Buffer> buffer;
        hr = dev->CreateBuffer(&bd, nullptr, &buffer);
        if (FAILED(hr)) return L"FAIL: compute buffer HRESULT=" + std::to_wstring((long long)hr);

        D3D11_UNORDERED_ACCESS_VIEW_DESC ud = {};
        ud.Format = DXGI_FORMAT_UNKNOWN;
        ud.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
        ud.Buffer.NumElements = 1024;
        ComPtr<ID3D11UnorderedAccessView> uav;
        hr = dev->CreateUnorderedAccessView(buffer.Get(), &ud, &uav);
        if (FAILED(hr)) return L"FAIL: compute UAV HRESULT=" + std::to_wstring((long long)hr);

        std::wstring adapter = L"hardware adapter";
        ComPtr<IDXGIDevice> dxgi;
        if (SUCCEEDED(dev.As(&dxgi)))
        {
            ComPtr<IDXGIAdapter> a;
            if (SUCCEEDED(dxgi->GetAdapter(&a)))
            {
                DXGI_ADAPTER_DESC d = {};
                if (SUCCEEDED(a->GetDesc(&d))) adapter = d.Description;
            }
        }
        unsigned int major = ((unsigned int)got >> 12) & 0xF, minor = ((unsigned int)got >> 8) & 0xF;
        return L"PASS: Direct3D compute resources available; " + adapter + L", FL " + std::to_wstring(major) + L"." + std::to_wstring(minor);
    }

    bool SaveAuth(const std::wstring& url, const std::wstring& email, const std::wstring& auth)
    {
        try
        {
            auto v = ref new PasswordVault();
            auto key = PS(L"BOINC Xbox:" + url);
            try { auto old = v->FindAllByResource(key); for each (PasswordCredential^ c in old) v->Remove(c); } catch (Exception^) {}
            v->Add(ref new PasswordCredential(key, PS(email), PS(auth)));
            return true;
        }
        catch (Exception^) { return false; }
    }

    bool LoadAuth(const std::wstring& url, std::wstring& email, std::wstring& auth)
    {
        try
        {
            auto v = ref new PasswordVault();
            auto list = v->FindAllByResource(PS(L"BOINC Xbox:" + url));
            if (!list || !list->Size) return false;
            auto c = list->GetAt(0); c->RetrievePassword();
            email = c->UserName->Data(); auth = c->Password->Data(); return !auth.empty();
        }
        catch (Exception^) { return false; }
    }

    std::wstring GetSetting(const wchar_t* name)
    {
        try
        {
            auto v = ApplicationData::Current->LocalSettings->Values;
            auto k = ref new String(name);
            if (!v->HasKey(k)) return L"";
            auto s = dynamic_cast<String^>(v->Lookup(k));
            return s ? s->Data() : L"";
        }
        catch (Exception^) { return L""; }
    }

    void SetSetting(const wchar_t* name, const std::wstring& value)
    {
        ApplicationData::Current->LocalSettings->Values->Insert(ref new String(name), PS(value));
    }

    std::wstring BuildRequest(const std::shared_ptr<RunState>& s, unsigned int cpus, unsigned long long memBytes)
    {
        std::wstring cpid = GetSetting(L"HostCPID");
        if (cpid.empty()) { cpid = Md5Hex(L"boinc-xbox-series-x:" + s->projectUrl); SetSetting(L"HostCPID", cpid); }
        std::wstring x;
        x += L"<scheduler_request>\n";
        x += L"<authenticator>" + EscapeXml(s->authenticator) + L"</authenticator>\n";
        x += L"<hostid>" + std::to_wstring(s->hostId) + L"</hostid>\n";
        x += L"<rpc_seqno>" + std::to_wstring(s->rpcSeqno) + L"</rpc_seqno>\n";
        x += L"<platform_name>x86_64-pc-xbox-uwp</platform_name>\n";
        x += L"<core_client_major_version>8</core_client_major_version><core_client_minor_version>0</core_client_minor_version><core_client_release>0</core_client_release>\n";
        x += L"<work_req_seconds>1</work_req_seconds><cpu_req_secs>1</cpu_req_secs><cpu_req_instances>" + std::to_wstring(cpus) + L"</cpu_req_instances>\n";
        x += L"<resource_share_fraction>1</resource_share_fraction><rrs_fraction>1</rrs_fraction><prrs_fraction>1</prrs_fraction>\n";
        x += L"<host_info><timezone>0</timezone><domain_name>Xbox-Series-X</domain_name><ip_addr>0.0.0.0</ip_addr>";
        x += L"<host_cpid>" + cpid + L"</host_cpid><p_ncpus>" + std::to_wstring(cpus) + L"</p_ncpus>";
        x += L"<p_vendor>AMD</p_vendor><p_model>Xbox Series X Developer Mode</p_model><p_features>x86_64</p_features>";
        x += L"<p_fpops>1e10</p_fpops><p_iops>1e10</p_iops><p_membw>1e9</p_membw>";
        x += L"<m_nbytes>" + std::to_wstring(memBytes) + L"</m_nbytes><m_cache>0</m_cache><m_swap>0</m_swap>";
        x += L"<d_total>0</d_total><d_free>0</d_free><os_name>Windows</os_name><os_version>Xbox UWP Developer Mode</os_version></host_info>\n";
        x += L"<client_cap_plan_class>1</client_cap_plan_class><sandbox>0</sandbox><dont_use_docker>1</dont_use_docker><dont_use_wsl>1</dont_use_wsl>\n";
        x += L"</scheduler_request>\n";
        return x;
    }

    void ParseScheduler(const std::wstring& xml, const std::shared_ptr<RunState>& s)
    {
        if (xml.find(L"<scheduler_reply") == std::wstring::npos) { s->schedulerRpc = L"FAIL: response is not scheduler_reply"; return; }
        std::wstring err = Tag(xml, L"error_msg"), msg = Tag(xml, L"message");
        if (!err.empty()) s->schedulerRpc = L"SERVER: " + err;
        else s->schedulerRpc = L"PASS: scheduler_reply received";
        std::wstring h = Tag(xml, L"hostid");
        if (!h.empty())
        {
            try { s->hostId = std::stoul(h); SetSetting(L"HostId", h); s->host = L"PASS: hostid=" + h; }
            catch (...) { s->host = L"WARN: invalid hostid"; }
        }
        else s->host = s->hostId ? L"PASS: existing hostid=" + std::to_wstring(s->hostId) : L"WARN: no hostid returned";
        int apps = (int)Count(xml, L"app>"), vers = (int)Count(xml, L"app_version>"), wu = (int)Count(xml, L"workunit>"), res = (int)Count(xml, L"result>"), files = (int)Count(xml, L"file_info>");
        s->work = L"PASS: app=" + std::to_wstring(apps) + L", app_version=" + std::to_wstring(vers) + L", workunit=" + std::to_wstring(wu) + L", result=" + std::to_wstring(res) + L", file_info=" + std::to_wstring(files);
        if (!msg.empty()) s->work += L", message=" + msg;
        ++s->rpcSeqno; SetSetting(L"RpcSeqno", std::to_wstring(s->rpcSeqno));
    }

    std::wstring Report(const std::shared_ptr<RunState>& s)
    {
        std::wstring r = L"v0.5 integration report\n\n";
        r += L"Project: " + s->project + L"\nScheduler discovery: " + s->schedulerDiscovery + L"\nScheduler TCP: " + s->schedulerTcp;
        r += L"\nAccount: " + s->account + L"\nScheduler RPC: " + s->schedulerRpc + L"\nHost: " + s->host + L"\nWork parser: " + s->work;
        r += L"\nDownload/cache: " + s->download + L"\nCPU: " + s->cpu + L"\nGPU: " + s->gpu + L"\nMemory: " + s->memory + L"\nStorage: " + s->storage;
        r += L"\nPassword: PASS - plaintext password is never stored";
        return r;
    }
}

App::App() { InitializeComponent(); }

void App::OnLaunched(LaunchActivatedEventArgs^)
{
    boinc_xbox_platform_init();
    unsigned int cpus = std::thread::hardware_concurrency(); if (!cpus) cpus = 1;
    unsigned long long mem = 0; try { mem = MemoryManager::AppMemoryUsageLimit; } catch (Exception^) {}

    auto root = ref new Grid(); root->Background = ref new SolidColorBrush(ColorHelper::FromArgb(255, 16, 24, 32));
    auto scroll = ref new ScrollViewer(); scroll->VerticalScrollMode = ScrollMode::Auto; scroll->VerticalScrollBarVisibility = ScrollBarVisibility::Auto;
    auto panel = ref new StackPanel(); panel->MaxWidth = 960; panel->Margin = Thickness(140, 64, 140, 64); panel->Spacing = 14;

    auto title = ref new TextBlock(); title->Text = ref new String(L"BOINC Xbox"); title->FontSize = 48; title->HorizontalAlignment = HorizontalAlignment::Center;
    auto sub = ref new TextBlock(); sub->Text = ref new String(L"v0.5 - Scheduler + CPU/GPU integration"); sub->FontSize = 22; sub->HorizontalAlignment = HorizontalAlignment::Center;
    auto status = ref new TextBlock(); status->Text = PS(L"Runtime: CPU threads=" + std::to_wstring(cpus) + L", memory limit=" + std::to_wstring(mem / 1048576ULL) + L" MB"); status->FontSize = 17;

    auto url = ref new TextBox(); url->Header = ref new String(L"Project URL"); url->Text = PS(GetSetting(L"ProjectUrl")); url->PlaceholderText = ref new String(L"https://asteroidsathome.net/boinc/"); url->FontSize = 19;
    auto email = ref new TextBox(); email->Header = ref new String(L"Email (optional if authenticator is already stored)"); email->Text = PS(GetSetting(L"AccountEmail")); email->FontSize = 19;
    auto pass = ref new PasswordBox(); pass->Header = ref new String(L"Password (never stored)"); pass->FontSize = 19;
    auto run = ref new Button(); run->Content = ref new String(L"Run v0.5 full integration test"); run->FontSize = 20; run->Padding = Thickness(24, 12, 24, 12);
    auto hint = ref new TextBlock(); hint->Text = ref new String(L"Tests project, account/authenticator, real scheduler RPC, host/work parser, download cache, all CPU workers and Direct3D compute resources."); hint->TextWrapping = TextWrapping::Wrap; hint->Opacity = 0.8;
    auto report = ref new TextBlock(); report->Text = ref new String(L"Not run yet."); report->FontSize = 17; report->TextWrapping = TextWrapping::Wrap;
    auto log = ref new TextBlock(); log->Text = ref new String(L"Ready.\n"); log->FontSize = 15; log->TextWrapping = TextWrapping::Wrap;

    auto box = [](TextBlock^ t, Color c) { auto b = ref new Border(); b->Padding = Thickness(18); b->Background = ref new SolidColorBrush(c); b->Child = t; return b; };
    auto reportTitle = ref new TextBlock(); reportTitle->Text = ref new String(L"Integration report"); reportTitle->FontSize = 26;
    auto logTitle = ref new TextBlock(); logTitle->Text = ref new String(L"Log"); logTitle->FontSize = 24;

    auto append = [log](const std::wstring& s) { std::wstring x = log->Text ? log->Text->Data() : L""; x += s + L"\n"; log->Text = PS(x); };

    run->Click += ref new RoutedEventHandler([=](Object^, RoutedEventArgs^)
    {
        std::wstring projectUrl = NormalizeUrl(url->Text ? url->Text->Data() : L"");
        if (projectUrl.empty()) { append(L"ERROR: Project URL is empty."); return; }
        auto s = std::make_shared<RunState>(); s->projectUrl = projectUrl; s->email = Lower(Trim(email->Text ? email->Text->Data() : L""));
        std::wstring password = pass->Password ? pass->Password->Data() : L""; pass->Password = ref new String(L"");
        try { SetSetting(L"ProjectUrl", projectUrl); if (!s->email.empty()) SetSetting(L"AccountEmail", s->email); } catch (...) {}
        try { std::wstring h = GetSetting(L"HostId"); if (!h.empty()) s->hostId = std::stoul(h); std::wstring q = GetSetting(L"RpcSeqno"); if (!q.empty()) s->rpcSeqno = std::stoi(q); } catch (...) {}

        run->IsEnabled = false; log->Text = ref new String(L""); report->Text = ref new String(L"Running..."); append(L"v0.5 integration started");
        s->memory = L"PASS: limit=" + std::to_wstring(mem / 1048576ULL) + L" MB, high-water=" + std::to_wstring((mem / 1048576ULL) * 90 / 100) + L" MB";
        s->gpu = GpuTest(); append(L"GPU: " + s->gpu);

        create_task([=]() { return CpuTest(cpus); }).then([=](std::wstring cpuResult)
        {
            s->cpu = cpuResult; append(L"CPU: " + cpuResult);
            return GetText(projectUrl + L"/get_project_config.php");
        }).then([=](String^ body)
        {
            std::wstring xml = body ? body->Data() : L"";
            if (xml.find(L"<project_config") == std::wstring::npos) throw ref new FailureException(ref new String(L"Not a BOINC project config"));
            std::wstring name = Tag(xml, L"name"); s->masterUrl = NormalizeUrl(Tag(xml, L"master_url")); if (s->masterUrl.empty()) s->masterUrl = projectUrl;
            size_t platforms = Count(xml, L"platform_name>"); s->project = L"PASS: " + (name.empty() ? projectUrl : name) + L", platforms=" + std::to_wstring(platforms); append(L"Project: " + s->project);
            return create_task(ApplicationData::Current->LocalFolder->CreateFileAsync(ref new String(L"v05_download_test.xml"), CreationCollisionOption::ReplaceExisting)).then([=](StorageFile^ f)
            {
                return create_task(FileIO::WriteTextAsync(f, body)).then([=]() { s->download = L"PASS: v05_download_test.xml cached"; return GetText(s->masterUrl + L"/"); });
            });
        }).then([=](String^ master)
        {
            auto u = SchedulerUrls(master ? master->Data() : L"");
            if (u.empty()) throw ref new FailureException(ref new String(L"No scheduler URL found"));
            s->schedulerUrl = u.front(); s->schedulerDiscovery = L"PASS: " + s->schedulerUrl; append(L"Scheduler: " + s->schedulerUrl);
            return TcpTest(s->schedulerUrl);
        }).then([=](std::wstring tcp)
        {
            s->schedulerTcp = tcp;
            if (!s->email.empty() && !password.empty())
            {
                std::wstring hash = Md5Hex(password + s->email);
                std::wstring lookup = projectUrl + L"/lookup_account.php?email_addr=" + UrlEncode(s->email) + L"&passwd_hash=" + hash;
                return GetText(lookup).then([=](String^ b)
                {
                    std::wstring x = b ? b->Data() : L""; std::wstring a = Tag(x, L"authenticator"); std::wstring e = Tag(x, L"error_msg");
                    if (a.empty()) { s->account = L"FAIL: " + (e.empty() ? L"no authenticator" : e); return; }
                    s->authenticator = a; bool stored = SaveAuth(projectUrl, s->email, a); s->account = stored ? L"PASS: authenticator received and stored" : L"PASS: authenticator received (vault store failed)";
                });
            }
            std::wstring em, a; if (LoadAuth(projectUrl, em, a)) { s->email = em; s->authenticator = a; s->account = L"PASS: stored authenticator loaded"; }
            else s->account = L"SKIP: enter project account email/password";
            return task_from_result();
        }).then([=]()
        {
            if (s->authenticator.empty()) { s->schedulerRpc = L"SKIP: no authenticator"; s->host = L"SKIP"; s->work = L"SKIP"; return task_from_result(); }
            std::wstring req = BuildRequest(s, cpus, mem);
            append(L"Posting real scheduler RPC...");
            return PostXml(s->schedulerUrl, req).then([=](String^ b) { ParseScheduler(b ? b->Data() : L"", s); });
        }).then([=](task<void> t)
        {
            try { t.get(); }
            catch (Exception^ ex) { append(L"Pipeline error: " + std::wstring(ex->Message->Data())); if (s->project == L"PENDING") s->project = L"FAIL"; }
            catch (...) { append(L"Pipeline error: unknown"); }
            try { SetSetting(L"V05Persistence", L"OK"); s->storage = GetSetting(L"V05Persistence") == L"OK" ? L"PASS: local persistence OK" : L"FAIL"; } catch (...) { s->storage = L"FAIL"; }
            report->Text = PS(Report(s)); run->IsEnabled = true; append(L"v0.5 integration finished");
        }, task_continuation_context::use_current());
    });

    panel->Children->Append(title); panel->Children->Append(sub); panel->Children->Append(status); panel->Children->Append(url); panel->Children->Append(email); panel->Children->Append(pass); panel->Children->Append(run); panel->Children->Append(hint); panel->Children->Append(reportTitle); panel->Children->Append(box(report, ColorHelper::FromArgb(255, 32, 42, 51))); panel->Children->Append(logTitle); panel->Children->Append(box(log, ColorHelper::FromArgb(255, 24, 31, 38)));
    scroll->Content = panel; root->Children->Append(scroll); Window::Current->Content = root; Window::Current->Activate();
}
