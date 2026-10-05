#include "pch.h"
#include "App.xaml.h"
#include "xbox_platform.h"
#include "PeriodSearchXbox.h"

#include <iomanip>
#include <sstream>

using namespace BOINC_Xbox;
using namespace Platform;
using namespace concurrency;
using namespace Windows::ApplicationModel;
using namespace Windows::ApplicationModel::Activation;
using namespace Windows::Networking;
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
    struct FileRecord
    {
        std::wstring name;
        std::wstring download_url;
        std::wstring upload_url;
        bool gzipped = false;
    };

    struct FileRefInfo
    {
        std::wstring file_name;
        std::wstring open_name;
    };

    struct WorkPackage
    {
        bool valid = false;
        std::wstring app_name;
        std::wstring workunit_name;
        std::wstring result_name;
        std::vector<FileRefInfo> inputs;
        std::vector<FileRefInfo> outputs;
        std::vector<FileRecord> files;
    };

    struct SuiteState
    {
        std::wstring smoke = L"PENDING";
        std::wstring extended = L"PENDING";
        std::wstring project = L"PENDING";
        std::wstring account = L"PENDING";
        std::wstring scheduler = L"PENDING";
        std::wstring anonymous_rpc = L"PENDING";
        std::wstring work = L"PENDING";
        std::wstring download = L"PENDING";
        std::wstring solve = L"PENDING";
        std::wstring output = L"PENDING";
        std::wstring upload_gate = L"BLOCKED: validation gate active";
        std::wstring diagnostics = L"PENDING";
        std::wstring project_url;
        std::wstring master_url;
        std::wstring scheduler_url;
        std::wstring email;
        std::wstring authenticator;
        unsigned long host_id = 0;
        int rpc_seqno = 0;
        WorkPackage package;
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

    std::wstring XmlDecode(std::wstring s)
    {
        struct Pair { const wchar_t* a; const wchar_t* b; };
        Pair p[] = {
            { L"&amp;", L"&" }, { L"&lt;", L"<" }, { L"&gt;", L">" },
            { L"&quot;", L"\"" }, { L"&apos;", L"'" }
        };
        for (auto& x : p)
        {
            size_t pos = 0;
            while ((pos = s.find(x.a, pos)) != std::wstring::npos)
            {
                s.replace(pos, wcslen(x.a), x.b);
                pos += wcslen(x.b);
            }
        }
        return s;
    }

    std::wstring UrlEncode(const std::wstring& s)
    {
        std::wstring out;
        wchar_t buf[8] = {};
        for (wchar_t c : s)
        {
            const bool safe = iswalnum(c) || c == L'-' || c == L'_' || c == L'.' || c == L'~';
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

    std::wstring HashHex(const std::wstring& value, String^ algorithm)
    {
        auto p = HashAlgorithmProvider::OpenAlgorithm(algorithm);
        auto b = CryptographicBuffer::ConvertStringToBinary(PS(value), BinaryStringEncoding::Utf8);
        return Lower(CryptographicBuffer::EncodeToHexString(p->HashData(b))->Data());
    }

    std::wstring Md5Hex(const std::wstring& value) { return HashHex(value, HashAlgorithmNames::Md5); }
    std::wstring Sha256Hex(const std::wstring& value) { return HashHex(value, HashAlgorithmNames::Sha256); }

    std::wstring Tag(const std::wstring& xml, const std::wstring& name)
    {
        const std::wstring a = L"<" + name + L">";
        const std::wstring b = L"</" + name + L">";
        size_t p = xml.find(a);
        if (p == std::wstring::npos) return L"";
        p += a.size();
        const size_t q = xml.find(b, p);
        return q == std::wstring::npos ? L"" : Trim(xml.substr(p, q - p));
    }

    std::vector<std::wstring> Blocks(const std::wstring& xml, const std::wstring& name)
    {
        std::vector<std::wstring> out;
        const std::wstring open = L"<" + name;
        const std::wstring close = L"</" + name + L">";
        size_t pos = 0;
        while (true)
        {
            const size_t a = xml.find(open, pos);
            if (a == std::wstring::npos) break;
            const size_t gt = xml.find(L'>', a + open.size());
            if (gt == std::wstring::npos) break;
            const size_t b = xml.find(close, gt + 1);
            if (b == std::wstring::npos) break;
            out.push_back(xml.substr(gt + 1, b - gt - 1));
            pos = b + close.size();
        }
        return out;
    }

    std::wstring Diagnostics(const std::wstring& xml)
    {
        std::wstring out;
        for (const auto& b : Blocks(xml, L"message")) if (!Trim(b).empty()) out += L"MSG: " + Trim(b) + L" | ";
        for (const auto& b : Blocks(xml, L"error_msg")) if (!Trim(b).empty()) out += L"ERROR: " + Trim(b) + L" | ";
        const std::wstring delay = Tag(xml, L"request_delay");
        if (!delay.empty()) out += L"delay=" + delay + L" | ";
        if (out.empty()) out = L"No server diagnostic message";
        if (out.size() > 500) { out.resize(500); out += L"..."; }
        return out;
    }

    size_t CountNonEmptyLines(const std::wstring& text)
    {
        std::wistringstream input(text);
        std::wstring line;
        size_t count = 0;
        while (std::getline(input, line))
        {
            if (!line.empty() && line.back() == L'\r') line.pop_back();
            if (!Trim(line).empty()) ++count;
        }
        return count;
    }

    std::wstring FirstNonEmptyLine(const std::wstring& text)
    {
        std::wistringstream input(text);
        std::wstring line;
        while (std::getline(input, line))
        {
            if (!line.empty() && line.back() == L'\r') line.pop_back();
            if (!Trim(line).empty()) return Trim(line);
        }
        return L"";
    }

    std::wstring MakeValidationInput(const std::wstring& raw, int periods, int iterations)
    {
        std::vector<std::wstring> lines;
        std::wistringstream input(raw);
        std::wstring line;
        while (std::getline(input, line))
        {
            if (!line.empty() && line.back() == L'\r') line.pop_back();
            lines.push_back(line);
        }
        if (lines.size() < 11) return raw;

        std::wistringstream first(lines[0]);
        double period_start = 0.0, period_step = 0.0, period_end = 0.0;
        int fixed_or_free = 1;
        if (first >> period_start >> period_step >> period_end >> fixed_or_free)
        {
            const double new_end = period_start + period_step * (periods > 0 ? periods - 1 : 0);
            std::wostringstream rebuilt;
            rebuilt << std::setprecision(12) << period_start << L" " << period_step << L" " << new_end << L" " << fixed_or_free
                    << L" period_start period_step period_end fixed/free";
            lines[0] = rebuilt.str();
        }
        lines[10] = std::to_wstring(iterations) + L"                iteration stop condition";

        std::wostringstream output;
        for (size_t i = 0; i < lines.size(); ++i)
        {
            output << lines[i];
            if (i + 1 < lines.size()) output << L"\n";
        }
        return output.str();
    }

    bool SmokeMatchesReference(const std::wstring& output)
    {
        const std::wstring line = FirstNonEmptyLine(output);
        std::wistringstream ss(line);
        double a = 0, b = 0, c = 0, d = 0, e = 0, f = 0;
        if (!(ss >> a >> b >> c >> d >> e >> f)) return false;
        return fabs(a - 17.20820800) < 1e-6
            && fabs(b - 0.257266) < 1e-6
            && fabs(c - 7.809903) < 1e-6
            && fabs(d - 0.1) < 1e-6
            && fabs(e) < 1e-9
            && fabs(f) < 1e-9;
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

    bool SaveAuth(const std::wstring& url, const std::wstring& email, const std::wstring& auth)
    {
        try
        {
            auto vault = ref new PasswordVault();
            auto key = PS(L"BOINC Xbox:" + url);
            try
            {
                auto old = vault->FindAllByResource(key);
                for each (PasswordCredential^ c in old) vault->Remove(c);
            }
            catch (Exception^) {}
            vault->Add(ref new PasswordCredential(key, PS(email), PS(auth)));
            return true;
        }
        catch (Exception^) { return false; }
    }

    bool LoadAuth(const std::wstring& url, std::wstring& email, std::wstring& auth)
    {
        try
        {
            auto vault = ref new PasswordVault();
            auto list = vault->FindAllByResource(PS(L"BOINC Xbox:" + url));
            if (!list || !list->Size) return false;
            auto c = list->GetAt(0);
            c->RetrievePassword();
            email = c->UserName->Data();
            auth = c->Password->Data();
            return !auth.empty();
        }
        catch (Exception^) { return false; }
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
            const std::wstring t = page.substr(a, b - a + 1);
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

    task<void> SaveText(const std::wstring& name, const std::wstring& text)
    {
        return create_task(ApplicationData::Current->LocalFolder->CreateFileAsync(PS(name), CreationCollisionOption::ReplaceExisting)).then([text](StorageFile^ f)
        {
            return create_task(FileIO::WriteTextAsync(f, PS(text)));
        });
    }

    task<void> DownloadFile(const std::wstring& url, const std::wstring& local_name)
    {
        auto c = Client();
        return create_task(c->GetAsync(ref new Uri(PS(url)))).then([c](HttpResponseMessage^ r) -> task<IBuffer^>
        {
            if (!r->IsSuccessStatusCode) throw ref new FailureException(PS(L"Download HTTP " + std::to_wstring((unsigned int)r->StatusCode)));
            return create_task(r->Content->ReadAsBufferAsync());
        }).then([local_name](IBuffer^ buffer) -> task<void>
        {
            return create_task(ApplicationData::Current->LocalFolder->CreateFileAsync(PS(local_name), CreationCollisionOption::ReplaceExisting)).then([buffer](StorageFile^ f)
            {
                return create_task(FileIO::WriteBufferAsync(f, buffer));
            });
        });
    }

    task<void> CopyLocalFile(const std::wstring& from, const std::wstring& to)
    {
        return create_task(ApplicationData::Current->LocalFolder->GetFileAsync(PS(from))).then([to](StorageFile^ f) -> task<StorageFile^>
        {
            return create_task(f->CopyAsync(ApplicationData::Current->LocalFolder, PS(to), NameCollisionOption::ReplaceExisting));
        }).then([](StorageFile^) {});
    }

    std::wstring AbsoluteUrl(const std::wstring& project_url, std::wstring url)
    {
        url = XmlDecode(Trim(url));
        if (url.find(L"://") != std::wstring::npos) return url;
        if (url.empty()) return url;
        if (url.front() == L'/')
        {
            size_t p = project_url.find(L"://");
            if (p == std::wstring::npos) return project_url + url;
            size_t slash = project_url.find(L'/', p + 3);
            return (slash == std::wstring::npos ? project_url : project_url.substr(0, slash)) + url;
        }
        return project_url + L"/" + url;
    }

    std::vector<FileRecord> ParseFileRecords(const std::wstring& xml, const std::wstring& project_url)
    {
        std::vector<FileRecord> out;
        for (const auto& block : Blocks(xml, L"file_info"))
        {
            FileRecord r;
            r.name = XmlDecode(Tag(block, L"name"));
            std::wstring u = Tag(block, L"download_url");
            if (u.empty()) u = Tag(block, L"url");
            std::wstring gz = Tag(block, L"gzipped_url");
            if (u.empty() && !gz.empty()) { u = gz; r.gzipped = true; }
            r.download_url = AbsoluteUrl(project_url, u);
            r.upload_url = AbsoluteUrl(project_url, Tag(block, L"upload_url"));
            if (r.upload_url.empty())
            {
                const std::wstring generic = AbsoluteUrl(project_url, Tag(block, L"url"));
                if (generic.find(L"file_upload_handler") != std::wstring::npos) r.upload_url = generic;
            }
            if (!r.name.empty()) out.push_back(r);
        }
        return out;
    }

    std::vector<FileRefInfo> ParseFileRefs(const std::wstring& xml)
    {
        std::vector<FileRefInfo> out;
        for (const auto& block : Blocks(xml, L"file_ref"))
        {
            FileRefInfo r;
            r.file_name = XmlDecode(Tag(block, L"file_name"));
            r.open_name = XmlDecode(Tag(block, L"open_name"));
            if (!r.file_name.empty()) out.push_back(r);
        }
        return out;
    }

    const FileRecord* FindFile(const WorkPackage& p, const std::wstring& name)
    {
        for (const auto& f : p.files) if (f.name == name) return &f;
        return nullptr;
    }

    WorkPackage ParseWorkPackage(const std::wstring& xml, const std::wstring& project_url)
    {
        WorkPackage p;
        p.files = ParseFileRecords(xml, project_url);
        auto wus = Blocks(xml, L"workunit");
        auto results = Blocks(xml, L"result");
        if (wus.empty() || results.empty()) return p;

        const std::wstring& wu = wus.front();
        const std::wstring& result = results.front();
        p.app_name = Tag(wu, L"app_name");
        p.workunit_name = Tag(wu, L"name");
        p.result_name = Tag(result, L"name");
        p.inputs = ParseFileRefs(wu);
        p.outputs = ParseFileRefs(result);
        p.valid = !p.workunit_name.empty() && !p.result_name.empty() && !p.inputs.empty();
        return p;
    }

    std::wstring BuildAnonymousRequest(const std::shared_ptr<SuiteState>& s, unsigned int cpus, unsigned long long mem_bytes)
    {
        std::wstring cpid = GetSetting(L"HostCPID");
        if (cpid.empty())
        {
            cpid = Md5Hex(L"boinc-xbox-series-x:" + s->project_url);
            SetSetting(L"HostCPID", cpid);
        }

        std::wstring x;
        x += L"<scheduler_request>\n";
        x += L"<authenticator>" + EscapeXml(s->authenticator) + L"</authenticator>\n";
        x += L"<hostid>" + std::to_wstring(s->host_id) + L"</hostid>\n";
        x += L"<rpc_seqno>" + std::to_wstring(s->rpc_seqno) + L"</rpc_seqno>\n";
        x += L"<platform_name>anonymous</platform_name>\n";
        x += L"<core_client_major_version>8</core_client_major_version><core_client_minor_version>0</core_client_minor_version><core_client_release>0</core_client_release>\n";
        x += L"<resource_share_fraction>1</resource_share_fraction><rrs_fraction>1</rrs_fraction><prrs_fraction>1</prrs_fraction>\n";
        x += L"<work_req_seconds>120</work_req_seconds><cpu_req_secs>120</cpu_req_secs><cpu_req_instances>1</cpu_req_instances>\n";
        x += L"<client_cap_plan_class>1</client_cap_plan_class><sandbox>0</sandbox><dont_use_docker>1</dont_use_docker><dont_use_wsl>1</dont_use_wsl>\n";
        x += L"<host_info><timezone>0</timezone><domain_name>Xbox-Series-X</domain_name><ip_addr>0.0.0.0</ip_addr>";
        x += L"<host_cpid>" + cpid + L"</host_cpid><p_ncpus>" + std::to_wstring(cpus) + L"</p_ncpus>";
        x += L"<p_vendor>AMD</p_vendor><p_model>Xbox Series X Developer Mode</p_model><p_features>x86_64</p_features>";
        x += L"<p_fpops>1e10</p_fpops><p_iops>1e10</p_iops><p_membw>1e9</p_membw>";
        x += L"<m_nbytes>" + std::to_wstring(mem_bytes) + L"</m_nbytes><m_cache>0</m_cache><m_swap>0</m_swap>";
        x += L"<d_total>0</d_total><d_free>0</d_free><os_name>Windows</os_name><os_version>Xbox UWP Developer Mode</os_version></host_info>\n";
        x += L"<app_versions>\n";
        x += L"<app_version><app_name>period_search</app_name><platform>windows_x86_64</platform><version_num>10222</version_num><avg_ncpus>1</avg_ncpus><flops>1e10</flops></app_version>\n";
        x += L"</app_versions>\n";
        x += L"</scheduler_request>\n";
        return x;
    }

    std::wstring Report(const std::shared_ptr<SuiteState>& s)
    {
        std::wstring r = L"v0.8 accelerated integration report\n\n";
        r += L"Smoke science validation: " + s->smoke;
        r += L"\nExtended science validation: " + s->extended;
        r += L"\nProject: " + s->project;
        r += L"\nAccount: " + s->account;
        r += L"\nScheduler: " + s->scheduler;
        r += L"\nAnonymous RPC: " + s->anonymous_rpc;
        r += L"\nWork: " + s->work;
        r += L"\nInput download: " + s->download;
        r += L"\nReal WU solve: " + s->solve;
        r += L"\nOutput preparation: " + s->output;
        r += L"\nUpload/report: " + s->upload_gate;
        r += L"\nServer diagnostics: " + s->diagnostics;
        r += L"\nPassword: PASS - plaintext password is never stored";
        return r;
    }
}

App::App() { InitializeComponent(); }

void App::OnLaunched(LaunchActivatedEventArgs^)
{
    boinc_xbox_platform_init();
    unsigned int cpus = std::thread::hardware_concurrency();
    if (!cpus) cpus = 1;
    unsigned long long mem = 0;
    try { mem = MemoryManager::AppMemoryUsageLimit; } catch (Exception^) {}

    auto root = ref new Grid();
    root->Background = ref new SolidColorBrush(ColorHelper::FromArgb(255, 16, 24, 32));
    auto scroll = ref new ScrollViewer();
    scroll->VerticalScrollMode = ScrollMode::Auto;
    scroll->VerticalScrollBarVisibility = ScrollBarVisibility::Auto;
    auto panel = ref new StackPanel();
    panel->MaxWidth = 1040;
    panel->Margin = Thickness(120, 56, 120, 56);
    panel->Spacing = 14;

    auto title = ref new TextBlock(); title->Text = ref new String(L"BOINC Xbox"); title->FontSize = 48; title->HorizontalAlignment = HorizontalAlignment::Center;
    auto subtitle = ref new TextBlock(); subtitle->Text = ref new String(L"v0.8 - Accelerated science + real work pipeline"); subtitle->FontSize = 22; subtitle->HorizontalAlignment = HorizontalAlignment::Center;
    auto runtime = ref new TextBlock(); runtime->Text = PS(L"Runtime: CPU threads=" + std::to_wstring(cpus) + L", app memory limit=" + std::to_wstring(mem / 1048576ULL) + L" MB"); runtime->FontSize = 17;

    std::wstring initial_url = GetSetting(L"ProjectUrl");
    if (initial_url.empty()) initial_url = L"https://asteroidsathome.net/boinc/";
    auto url = ref new TextBox(); url->Header = ref new String(L"Project URL"); url->Text = PS(initial_url); url->FontSize = 19;
    auto email = ref new TextBox(); email->Header = ref new String(L"Email (optional if authenticator is already stored)"); email->Text = PS(GetSetting(L"AccountEmail")); email->FontSize = 19;
    auto pass = ref new PasswordBox(); pass->Header = ref new String(L"Password (never stored)"); pass->FontSize = 19;

    auto run = ref new Button(); run->Content = ref new String(L"Run accelerated v0.8 suite"); run->FontSize = 21; run->Padding = Thickness(26, 14, 26, 14);
    auto full = ref new Button(); full->Content = ref new String(L"Run full official sample (slow validation)"); full->FontSize = 18; full->Padding = Thickness(22, 10, 22, 10);
    auto hint = ref new TextBlock();
    hint->Text = ref new String(L"One accelerated run validates the real PeriodSearch solver twice, contacts the BOINC scheduler as an anonymous-platform client, requests one real PeriodSearch task, downloads its input, solves it locally and prepares the output. Upload/report is intentionally blocked until the result is reviewed.");
    hint->TextWrapping = TextWrapping::Wrap; hint->Opacity = 0.82; hint->FontSize = 16;

    auto report_title = ref new TextBlock(); report_title->Text = ref new String(L"Integration report"); report_title->FontSize = 28;
    auto report = ref new TextBlock(); report->Text = ref new String(L"Not run yet."); report->FontSize = 16; report->TextWrapping = TextWrapping::Wrap;
    auto report_border = ref new Border(); report_border->Padding = Thickness(20); report_border->Background = ref new SolidColorBrush(ColorHelper::FromArgb(255, 32, 42, 51)); report_border->Child = report;
    auto full_report = ref new TextBlock(); full_report->Text = ref new String(L"Full sample validation: not run."); full_report->FontSize = 16; full_report->TextWrapping = TextWrapping::Wrap;
    auto full_border = ref new Border(); full_border->Padding = Thickness(18); full_border->Background = ref new SolidColorBrush(ColorHelper::FromArgb(255, 26, 35, 43)); full_border->Child = full_report;
    auto log_title = ref new TextBlock(); log_title->Text = ref new String(L"Log"); log_title->FontSize = 24;
    auto log = ref new TextBlock(); log->Text = ref new String(L"Ready.\n"); log->FontSize = 15; log->TextWrapping = TextWrapping::Wrap;
    auto log_border = ref new Border(); log_border->Padding = Thickness(20); log_border->Background = ref new SolidColorBrush(ColorHelper::FromArgb(255, 24, 31, 38)); log_border->Child = log;

    auto append = [log](const std::wstring& s)
    {
        std::wstring x = log->Text ? log->Text->Data() : L"";
        x += s + L"\n";
        log->Text = PS(x);
    };

    full->Click += ref new RoutedEventHandler([=](Object^, RoutedEventArgs^)
    {
        full->IsEnabled = false;
        full_report->Text = ref new String(L"Full official sample running...");
        const auto ui = task_continuation_context::use_current();
        create_task(Package::Current->InstalledLocation->GetFileAsync(ref new String(L"PeriodSearchSampleIn.txt")))
        .then([](StorageFile^ f) { return create_task(FileIO::ReadTextAsync(f)); })
        .then([=](String^ raw)
        {
            return create_task(ApplicationData::Current->LocalFolder->CreateFileAsync(ref new String(L"period_search_in"), CreationCollisionOption::ReplaceExisting)).then([raw](StorageFile^ f)
            {
                return create_task(FileIO::WriteTextAsync(f, raw));
            });
        }, ui)
        .then([=]()
        {
            const std::wstring dir = ApplicationData::Current->LocalFolder->Path->Data();
            return create_task([dir]() { return periodsearch_run(dir, true); });
        }, ui)
        .then([=](task<PeriodSearchRunResult> finished)
        {
            try
            {
                const auto r = finished.get();
                std::wostringstream s;
                s << L"Full sample validation: " << ((r.exit_code == 0 && !r.output.empty()) ? L"PASS" : L"FAIL")
                  << L"\nElapsed: " << std::fixed << std::setprecision(3) << r.elapsed_seconds << L" s"
                  << L"\nOutput lines: " << CountNonEmptyLines(r.output)
                  << L"\nSHA-256: " << Sha256Hex(r.output)
                  << L"\nFirst result: " << FirstNonEmptyLine(r.output);
                if (!r.error.empty()) s << L"\nError: " << r.error;
                full_report->Text = PS(s.str());
            }
            catch (Exception^ ex) { full_report->Text = PS(L"Full sample validation FAIL: " + std::wstring(ex->Message->Data())); }
            catch (...) { full_report->Text = ref new String(L"Full sample validation FAIL: unknown exception"); }
            full->IsEnabled = true;
        }, ui);
    });

    run->Click += ref new RoutedEventHandler([=](Object^, RoutedEventArgs^)
    {
        const std::wstring project_url = NormalizeUrl(url->Text ? url->Text->Data() : L"");
        if (project_url.empty()) { append(L"ERROR: Project URL is empty."); return; }

        auto s = std::make_shared<SuiteState>();
        s->project_url = project_url;
        s->email = Lower(Trim(email->Text ? email->Text->Data() : L""));
        const std::wstring password = pass->Password ? pass->Password->Data() : L"";
        pass->Password = ref new String(L"");
        try
        {
            SetSetting(L"ProjectUrl", project_url);
            if (!s->email.empty()) SetSetting(L"AccountEmail", s->email);
            const std::wstring h = GetSetting(L"HostId"); if (!h.empty()) s->host_id = std::stoul(h);
            const std::wstring q = GetSetting(L"RpcSeqno"); if (!q.empty()) s->rpc_seqno = std::stoi(q);
        }
        catch (...) {}

        run->IsEnabled = false;
        full->IsEnabled = false;
        report->Text = ref new String(L"Running accelerated v0.8 suite...");
        log->Text = ref new String(L"");
        append(L"v0.8 suite started");
        const auto ui = task_continuation_context::use_current();

        create_task(Package::Current->InstalledLocation->GetFileAsync(ref new String(L"PeriodSearchSampleIn.txt")))
        .then([](StorageFile^ f) { return create_task(FileIO::ReadTextAsync(f)); })
        .then([=](String^ raw) -> task<void>
        {
            const std::wstring smoke = MakeValidationInput(raw ? raw->Data() : L"", 1, 2);
            return SaveText(L"period_search_in", smoke);
        }, ui)
        .then([=]()
        {
            const std::wstring dir = ApplicationData::Current->LocalFolder->Path->Data();
            return create_task([dir]() { return periodsearch_run(dir, true); });
        }, ui)
        .then([=](PeriodSearchRunResult r) -> task<String^>
        {
            if (r.exit_code == 0 && SmokeMatchesReference(r.output))
                s->smoke = L"PASS: reference matched, " + Sha256Hex(r.output).substr(0, 16) + L"..., " + std::to_wstring((int)(r.elapsed_seconds * 1000.0)) + L" ms";
            else
                s->smoke = L"FAIL: numerical reference mismatch";
            append(L"Smoke science validation finished");
            return create_task(Package::Current->InstalledLocation->GetFileAsync(ref new String(L"PeriodSearchSampleIn.txt"))).then([](StorageFile^ f) { return create_task(FileIO::ReadTextAsync(f)); });
        }, ui)
        .then([=](String^ raw) -> task<void>
        {
            const std::wstring extended = MakeValidationInput(raw ? raw->Data() : L"", 3, 4);
            return SaveText(L"period_search_in", extended);
        }, ui)
        .then([=]()
        {
            const std::wstring dir = ApplicationData::Current->LocalFolder->Path->Data();
            return create_task([dir]() { return periodsearch_run(dir, true); });
        }, ui)
        .then([=](PeriodSearchRunResult r)
        {
            const size_t lines = CountNonEmptyLines(r.output);
            if (r.exit_code == 0 && lines >= 3)
                s->extended = L"PASS: 3-period solver path, lines=" + std::to_wstring(lines) + L", sha256=" + Sha256Hex(r.output).substr(0, 16) + L"...";
            else
                s->extended = L"FAIL: extended solver validation";
            append(L"Extended science validation finished");
            return GetText(project_url + L"/get_project_config.php");
        }, ui)
        .then([=](String^ config)
        {
            const std::wstring xml = config ? config->Data() : L"";
            if (xml.find(L"<project_config") == std::wstring::npos) throw ref new FailureException(ref new String(L"Not a BOINC project config"));
            const std::wstring name = Tag(xml, L"name");
            s->master_url = NormalizeUrl(Tag(xml, L"master_url"));
            if (s->master_url.empty()) s->master_url = project_url;
            s->project = L"PASS: " + (name.empty() ? project_url : name);
            append(L"Project config PASS");
            return GetText(s->master_url + L"/");
        }, ui)
        .then([=](String^ master)
        {
            auto urls = SchedulerUrls(master ? master->Data() : L"");
            if (urls.empty()) throw ref new FailureException(ref new String(L"No scheduler URL found"));
            s->scheduler_url = urls.front();
            s->scheduler = L"PASS: " + s->scheduler_url;

            std::wstring stored_email, stored_auth;
            if (LoadAuth(project_url, stored_email, stored_auth))
            {
                s->email = stored_email;
                s->authenticator = stored_auth;
                s->account = L"PASS: stored authenticator loaded";
                return task_from_result();
            }
            if (s->email.empty() || password.empty())
            {
                s->account = L"SKIP: enter email/password once";
                return task_from_result();
            }
            const std::wstring hash = Md5Hex(password + s->email);
            const std::wstring lookup = project_url + L"/lookup_account.php?email_addr=" + UrlEncode(s->email) + L"&passwd_hash=" + hash;
            return GetText(lookup).then([=](String^ body)
            {
                const std::wstring x = body ? body->Data() : L"";
                const std::wstring auth = Tag(x, L"authenticator");
                if (auth.empty())
                {
                    s->account = L"FAIL: " + Tag(x, L"error_msg");
                    return;
                }
                s->authenticator = auth;
                SaveAuth(project_url, s->email, auth);
                s->account = L"PASS: authenticator received and stored";
            });
        }, ui)
        .then([=]() -> task<String^>
        {
            if (s->authenticator.empty())
            {
                s->anonymous_rpc = L"SKIP: no authenticator";
                s->work = L"SKIP";
                return task_from_result<String^>(ref new String(L""));
            }
            append(L"Requesting one real PeriodSearch task via anonymous platform...");
            const std::wstring request = BuildAnonymousRequest(s, cpus, mem);
            return PostXml(s->scheduler_url, request);
        }, ui)
        .then([=](String^ reply) -> task<void>
        {
            const std::wstring xml = reply ? reply->Data() : L"";
            if (xml.empty()) return task_from_result();

            ++s->rpc_seqno;
            SetSetting(L"RpcSeqno", std::to_wstring(s->rpc_seqno));
            const std::wstring host = Tag(xml, L"hostid");
            if (!host.empty())
            {
                try { s->host_id = std::stoul(host); SetSetting(L"HostId", host); } catch (...) {}
            }
            s->diagnostics = Diagnostics(xml);
            s->anonymous_rpc = xml.find(L"<scheduler_reply") != std::wstring::npos ? L"PASS: scheduler_reply received" : L"FAIL: no scheduler_reply";
            return SaveText(L"v08_scheduler_anonymous.xml", xml).then([=]()
            {
                s->package = ParseWorkPackage(xml, project_url);
                if (!s->package.valid)
                {
                    s->work = L"NO WORK: " + s->diagnostics;
                    return task_from_result();
                }
                s->work = L"PASS: app=" + s->package.app_name + L", WU=" + s->package.workunit_name + L", result=" + s->package.result_name;

                task<void> chain = task_from_result();
                size_t downloaded = 0;
                for (const auto& ref : s->package.inputs)
                {
                    const FileRecord* rec = FindFile(s->package, ref.file_name);
                    if (!rec || rec->download_url.empty()) continue;
                    if (rec->gzipped)
                    {
                        s->download = L"FAIL: gzipped input encountered; decompressor not enabled yet";
                        continue;
                    }
                    const std::wstring local_name = ref.open_name.empty() ? ref.file_name : ref.open_name;
                    const std::wstring remote = rec->download_url;
                    ++downloaded;
                    chain = chain.then([remote, local_name]() { return DownloadFile(remote, local_name); });
                }
                if (!downloaded)
                {
                    s->download = L"FAIL: no downloadable input file found";
                    return task_from_result();
                }
                s->download = L"PASS: downloading " + std::to_wstring(downloaded) + L" input file(s)";
                return chain;
            });
        }, ui)
        .then([=]()
        {
            if (!s->package.valid || s->download.rfind(L"PASS", 0) != 0) return task_from_result<PeriodSearchRunResult>(PeriodSearchRunResult());
            append(L"Real WU input downloaded; starting Xbox PeriodSearch solver...");
            const std::wstring dir = ApplicationData::Current->LocalFolder->Path->Data();
            return create_task([dir]() { return periodsearch_run(dir, true); });
        }, ui)
        .then([=](PeriodSearchRunResult r) -> task<void>
        {
            if (!s->package.valid || s->download.rfind(L"PASS", 0) != 0) return task_from_result();
            if (r.exit_code != 0 || r.output.empty())
            {
                s->solve = L"FAIL: exit=" + std::to_wstring(r.exit_code) + (r.error.empty() ? L"" : L", " + r.error);
                return task_from_result();
            }
            const std::wstring hash = Sha256Hex(r.output);
            s->solve = L"PASS: elapsed=" + std::to_wstring(r.elapsed_seconds) + L" s, lines=" + std::to_wstring(CountNonEmptyLines(r.output)) + L", sha256=" + hash.substr(0, 20) + L"...";

            std::wstring physical_output;
            for (const auto& ref : s->package.outputs)
            {
                if (ref.open_name == L"period_search_out" || physical_output.empty()) physical_output = ref.file_name;
                if (ref.open_name == L"period_search_out") break;
            }
            if (physical_output.empty())
            {
                s->output = L"PASS: logical period_search_out ready; physical result filename not present in reply";
                return task_from_result();
            }
            return CopyLocalFile(L"period_search_out", physical_output).then([=]()
            {
                const FileRecord* rec = FindFile(s->package, physical_output);
                const bool upload_known = rec && !rec->upload_url.empty();
                s->output = L"PASS: prepared " + physical_output + (upload_known ? L", upload URL parsed" : L", upload URL not present");
            });
        }, ui)
        .then([=](task<void> finished)
        {
            try { finished.get(); }
            catch (Exception^ ex)
            {
                append(L"Pipeline error: " + std::wstring(ex->Message->Data()));
                if (s->project == L"PENDING") s->project = L"FAIL";
            }
            catch (...)
            {
                append(L"Pipeline error: unknown exception");
            }
            s->upload_gate = L"BLOCKED intentionally: result is not uploaded or reported in v0.8";
            report->Text = PS(Report(s));
            run->IsEnabled = true;
            full->IsEnabled = true;
            append(L"v0.8 suite finished");
        }, ui);
    });

    panel->Children->Append(title);
    panel->Children->Append(subtitle);
    panel->Children->Append(runtime);
    panel->Children->Append(url);
    panel->Children->Append(email);
    panel->Children->Append(pass);
    panel->Children->Append(run);
    panel->Children->Append(full);
    panel->Children->Append(hint);
    panel->Children->Append(report_title);
    panel->Children->Append(report_border);
    panel->Children->Append(full_border);
    panel->Children->Append(log_title);
    panel->Children->Append(log_border);
    scroll->Content = panel;
    root->Children->Append(scroll);
    Window::Current->Content = root;
    Window::Current->Activate();
}