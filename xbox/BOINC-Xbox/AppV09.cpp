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
        std::wstring xml_signature;
        double max_nbytes = 0.0;
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
        std::wstring platform;
        int version_num = 10222;
        std::vector<FileRefInfo> inputs;
        std::vector<FileRefInfo> outputs;
        std::vector<FileRecord> files;
    };

    struct DiskInfo
    {
        unsigned long long total = 0;
        unsigned long long free = 0;
        bool measured = false;
    };

    struct UploadInfo
    {
        bool ok = false;
        unsigned int nbytes = 0;
        std::wstring md5;
        std::wstring response;
        std::wstring error;
    };

    struct CycleState
    {
        std::wstring smoke = L"PENDING";
        std::wstring project = L"PENDING";
        std::wstring account = L"PENDING";
        std::wstring scheduler = L"PENDING";
        std::wstring work_source = L"PENDING";
        std::wstring work = L"PENDING";
        std::wstring download = L"PENDING";
        std::wstring solve = L"PENDING";
        std::wstring upload = L"PENDING";
        std::wstring report = L"PENDING";
        std::wstring ack = L"PENDING";
        std::wstring credit = L"PENDING";
        std::wstring diagnostics = L"PENDING";
        std::wstring project_url;
        std::wstring master_url;
        std::wstring scheduler_url;
        std::wstring email;
        std::wstring authenticator;
        unsigned long host_id = 0;
        int rpc_seqno = 0;
        double elapsed = 0.0;
        WorkPackage package;
        UploadInfo upload_info;
    };

    String^ PS(const std::wstring& s) { return ref new String(s.c_str()); }

    std::wstring Trim(std::wstring s)
    {
        while (!s.empty() && iswspace(s.front())) s.erase(s.begin());
        while (!s.empty() && iswspace(s.back())) s.pop_back();
        return s;
    }

    std::wstring StripWhitespace(const std::wstring& s)
    {
        std::wstring out;
        for (wchar_t c : s) if (!iswspace(c)) out += c;
        return out;
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
        Pair pairs[] = {
            {L"&amp;", L"&"}, {L"&lt;", L"<"}, {L"&gt;", L">"},
            {L"&quot;", L"\""}, {L"&apos;", L"'"}
        };
        for (auto& x : pairs)
        {
            size_t p = 0;
            while ((p = s.find(x.a, p)) != std::wstring::npos)
            {
                s.replace(p, wcslen(x.a), x.b);
                p += wcslen(x.b);
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

    std::wstring HashText(const std::wstring& value, String^ algorithm)
    {
        auto p = HashAlgorithmProvider::OpenAlgorithm(algorithm);
        auto b = CryptographicBuffer::ConvertStringToBinary(PS(value), BinaryStringEncoding::Utf8);
        return Lower(CryptographicBuffer::EncodeToHexString(p->HashData(b))->Data());
    }

    std::wstring HashBuffer(IBuffer^ buffer, String^ algorithm)
    {
        auto p = HashAlgorithmProvider::OpenAlgorithm(algorithm);
        return Lower(CryptographicBuffer::EncodeToHexString(p->HashData(buffer))->Data());
    }

    std::wstring Md5Hex(const std::wstring& value) { return HashText(value, HashAlgorithmNames::Md5); }
    std::wstring Sha256Hex(const std::wstring& value) { return HashText(value, HashAlgorithmNames::Sha256); }

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

    std::wstring ServerDiagnostics(const std::wstring& xml)
    {
        std::wstring out;
        for (const auto& b : Blocks(xml, L"message")) if (!Trim(b).empty()) out += L"MSG: " + Trim(b) + L" | ";
        for (const auto& b : Blocks(xml, L"error_msg")) if (!Trim(b).empty()) out += L"ERROR: " + Trim(b) + L" | ";
        const std::wstring delay = Tag(xml, L"request_delay");
        if (!delay.empty()) out += L"delay=" + delay + L" | ";
        if (out.empty()) out = L"No server diagnostic message";
        if (out.size() > 600) { out.resize(600); out += L"..."; }
        return out;
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

    DiskInfo QueryDiskInfo()
    {
        DiskInfo d;
        try
        {
            const std::wstring path = ApplicationData::Current->LocalFolder->Path->Data();
            ULARGE_INTEGER free_available = {};
            ULARGE_INTEGER total_bytes = {};
            ULARGE_INTEGER total_free = {};
            if (GetDiskFreeSpaceExW(path.c_str(), &free_available, &total_bytes, &total_free))
            {
                d.total = total_bytes.QuadPart;
                d.free = free_available.QuadPart;
                d.measured = d.total > 0 && d.free > 0;
            }
        }
        catch (...) {}
        if (!d.measured)
        {
            d.total = 1024ULL * 1024ULL * 1024ULL;
            d.free = 512ULL * 1024ULL * 1024ULL;
        }
        return d;
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

    task<String^> ReadLocalTextIfExists(const std::wstring& name)
    {
        return create_task(ApplicationData::Current->LocalFolder->GetFileAsync(PS(name))).then([](task<StorageFile^> t) -> task<String^>
        {
            try
            {
                StorageFile^ f = t.get();
                return create_task(FileIO::ReadTextAsync(f));
            }
            catch (...) { return task_from_result<String^>(ref new String(L"")); }
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
            if (!u.empty()) out.push_back(u);
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
                if (e != std::wstring::npos) out.push_back(Trim(t.substr(h, e - h)));
            }
            pos = b + 1;
        }
        return out;
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
                const std::wstring candidate = AbsoluteUrl(project_url, Tag(block, L"url"));
                if (candidate.find(L"file_upload_handler") != std::wstring::npos) r.upload_url = candidate;
            }
            r.xml_signature = StripWhitespace(Tag(block, L"xml_signature"));
            try { r.max_nbytes = std::stod(Tag(block, L"max_nbytes")); } catch (...) { r.max_nbytes = 0.0; }
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
        p.platform = Tag(result, L"platform");
        try { p.version_num = std::stoi(Tag(result, L"version_num")); } catch (...) { p.version_num = 10222; }
        if (p.platform.empty()) p.platform = L"windows_x86_64";
        p.inputs = ParseFileRefs(wu);
        p.outputs = ParseFileRefs(result);
        p.valid = !p.workunit_name.empty() && !p.result_name.empty() && !p.inputs.empty() && !p.outputs.empty();
        return p;
    }

    std::wstring BuildRequest(const std::shared_ptr<CycleState>& s, unsigned int cpus, unsigned long long mem_bytes, const DiskInfo& disk, bool request_work, const std::wstring& result_xml)
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
        if (request_work)
            x += L"<work_req_seconds>120</work_req_seconds><cpu_req_secs>120</cpu_req_secs><cpu_req_instances>1</cpu_req_instances>\n";
        else
            x += L"<work_req_seconds>0</work_req_seconds><cpu_req_secs>0</cpu_req_secs><cpu_req_instances>0</cpu_req_instances><dont_send_work>1</dont_send_work>\n";
        x += L"<client_cap_plan_class>1</client_cap_plan_class><sandbox>0</sandbox><dont_use_docker>1</dont_use_docker><dont_use_wsl>1</dont_use_wsl>\n";
        x += L"<host_info><timezone>0</timezone><domain_name>Xbox-Series-X</domain_name><ip_addr>0.0.0.0</ip_addr>";
        x += L"<host_cpid>" + cpid + L"</host_cpid><p_ncpus>" + std::to_wstring(cpus) + L"</p_ncpus>";
        x += L"<p_vendor>AMD</p_vendor><p_model>Xbox Series X Developer Mode</p_model><p_features>x86_64</p_features>";
        x += L"<p_fpops>1e10</p_fpops><p_iops>1e10</p_iops><p_membw>1e9</p_membw>";
        x += L"<m_nbytes>" + std::to_wstring(mem_bytes) + L"</m_nbytes><m_cache>0</m_cache><m_swap>0</m_swap>";
        x += L"<d_total>" + std::to_wstring(disk.total) + L"</d_total><d_free>" + std::to_wstring(disk.free) + L"</d_free><os_name>Windows</os_name><os_version>Xbox UWP Developer Mode</os_version></host_info>\n";
        x += L"<app_versions><app_version><app_name>period_search</app_name><platform>windows_x86_64</platform><version_num>10222</version_num><avg_ncpus>1</avg_ncpus><flops>1e10</flops></app_version></app_versions>\n";
        if (!result_xml.empty()) x += result_xml;
        x += L"</scheduler_request>\n";
        return x;
    }

    std::wstring BuildResultXml(const std::shared_ptr<CycleState>& s, const FileRecord& rec)
    {
        const double max_bytes = rec.max_nbytes > 0 ? rec.max_nbytes : (double)s->upload_info.nbytes * 2.0 + 1024.0;
        std::wostringstream x;
        x << std::fixed << std::setprecision(6);
        x << L"<result>\n";
        x << L"<name>" << EscapeXml(s->package.result_name) << L"</name>\n";
        x << L"<final_cpu_time>" << s->elapsed << L"</final_cpu_time>\n";
        x << L"<final_elapsed_time>" << s->elapsed << L"</final_elapsed_time>\n";
        x << L"<exit_status>0</exit_status>\n";
        x << L"<state>5</state>\n";
        x << L"<platform>" << EscapeXml(s->package.platform) << L"</platform>\n";
        x << L"<version_num>" << s->package.version_num << L"</version_num>\n";
        x << L"<app_version_num>" << s->package.version_num << L"</app_version_num>\n";
        x << L"<stderr_out><core_client_version>8.0.0</core_client_version>\nXbox Series X Developer Mode PeriodSearch port v0.9\n</stderr_out>\n";
        x << L"<file_info>\n";
        x << L"<name>" << EscapeXml(rec.name) << L"</name>\n";
        x << L"<nbytes>" << s->upload_info.nbytes << L"</nbytes>\n";
        x << L"<max_nbytes>" << std::setprecision(0) << max_bytes << L"</max_nbytes>\n";
        x << L"<md5_cksum>" << s->upload_info.md5 << L"</md5_cksum>\n";
        x << L"<upload_url>" << EscapeXml(rec.upload_url) << L"</upload_url>\n";
        x << L"</file_info>\n";
        x << L"</result>\n";
        return x.str();
    }

    bool ResultAcked(const std::wstring& xml, const std::wstring& result_name)
    {
        for (const auto& b : Blocks(xml, L"result_ack"))
            if (XmlDecode(Tag(b, L"name")) == result_name) return true;
        return false;
    }

    task<UploadInfo> UploadBoincFile(const FileRecord& rec, const std::wstring& local_name)
    {
        return create_task(ApplicationData::Current->LocalFolder->GetFileAsync(PS(local_name))).then([rec](StorageFile^ f) -> task<IBuffer^>
        {
            return create_task(FileIO::ReadBufferAsync(f));
        }).then([rec](IBuffer^ file_buffer) -> task<UploadInfo>
        {
            UploadInfo info;
            info.nbytes = file_buffer ? file_buffer->Length : 0;
            info.md5 = file_buffer ? HashBuffer(file_buffer, HashAlgorithmNames::Md5) : L"";
            if (!file_buffer || !info.nbytes) { info.error = L"output file is empty"; return task_from_result(info); }
            if (rec.upload_url.empty()) { info.error = L"upload URL missing"; return task_from_result(info); }
            if (rec.xml_signature.empty()) { info.error = L"upload certificate signature missing"; return task_from_result(info); }
            if (rec.max_nbytes <= 0) { info.error = L"max_nbytes missing"; return task_from_result(info); }

            std::wostringstream hs;
            hs << std::fixed << std::setprecision(0);
            hs << L"<data_server_request>\n";
            hs << L"<core_client_major_version>8</core_client_major_version>\n<core_client_minor_version>0</core_client_minor_version>\n<core_client_release>0</core_client_release>\n";
            hs << L"<file_upload>\n<file_info>\n<name>" << EscapeXml(rec.name) << L"</name>\n";
            hs << L"<xml_signature>\n" << rec.xml_signature << L"\n</xml_signature>\n";
            hs << L"<max_nbytes>" << rec.max_nbytes << L"</max_nbytes>\n</file_info>\n";
            hs << L"<nbytes>" << info.nbytes << L"</nbytes>\n";
            hs << L"<md5_cksum>" << info.md5 << L"</md5_cksum>\n<offset>0</offset>\n<data>\n";

            IBuffer^ header_buffer = CryptographicBuffer::ConvertStringToBinary(PS(hs.str()), BinaryStringEncoding::Utf8);
            auto header_bytes = ref new Array<unsigned char>(header_buffer->Length);
            auto file_bytes = ref new Array<unsigned char>(file_buffer->Length);
            DataReader::FromBuffer(header_buffer)->ReadBytes(header_bytes);
            DataReader::FromBuffer(file_buffer)->ReadBytes(file_bytes);
            auto combined = ref new Array<unsigned char>(header_bytes->Length + file_bytes->Length);
            for (unsigned int i = 0; i < header_bytes->Length; ++i) combined[i] = header_bytes[i];
            for (unsigned int i = 0; i < file_bytes->Length; ++i) combined[header_bytes->Length + i] = file_bytes[i];
            IBuffer^ body = CryptographicBuffer::CreateFromByteArray(combined);

            auto c = Client();
            auto content = ref new HttpBufferContent(body);
            content->Headers->ContentType = ref new Windows::Web::Http::Headers::HttpMediaTypeHeaderValue(ref new String(L"application/x-www-form-urlencoded"));
            return create_task(c->PostAsync(ref new Uri(PS(rec.upload_url)), content)).then([c, content, info](HttpResponseMessage^ r) mutable -> task<String^>
            {
                if (!r->IsSuccessStatusCode)
                {
                    UploadInfo failed = info;
                    failed.error = L"upload HTTP " + std::to_wstring((unsigned int)r->StatusCode);
                    return task_from_result<String^>(PS(L"__UPLOAD_ERROR__" + failed.error));
                }
                return create_task(r->Content->ReadAsStringAsync());
            }).then([info](String^ response) mutable
            {
                UploadInfo out = info;
                out.response = response ? response->Data() : L"";
                if (out.response.rfind(L"__UPLOAD_ERROR__", 0) == 0)
                {
                    out.error = out.response.substr(18);
                    return out;
                }
                const std::wstring status = Tag(out.response, L"status");
                out.ok = status == L"0";
                if (!out.ok) out.error = L"file upload handler status=" + (status.empty() ? L"missing" : status) + L"; " + ServerDiagnostics(out.response);
                return out;
            });
        });
    }

    task<void> DownloadInputs(const WorkPackage& p)
    {
        task<void> chain = task_from_result();
        for (const auto& ref : p.inputs)
        {
            const FileRecord* rec = FindFile(p, ref.file_name);
            if (!rec || rec->download_url.empty()) continue;
            if (rec->gzipped) throw ref new FailureException(ref new String(L"Gzipped input is not supported in v0.9"));
            const std::wstring local = ref.open_name.empty() ? ref.file_name : ref.open_name;
            const std::wstring remote = rec->download_url;
            chain = chain.then([remote, local]() { return DownloadFile(remote, local); });
        }
        return chain;
    }

    std::wstring SmokeInput(const std::wstring& raw)
    {
        std::vector<std::wstring> lines;
        std::wistringstream in(raw);
        std::wstring line;
        while (std::getline(in, line)) { if (!line.empty() && line.back() == L'\r') line.pop_back(); lines.push_back(line); }
        if (lines.size() < 11) return raw;
        std::wistringstream first(lines[0]);
        double start = 0, step = 0, end = 0; int fixed = 1;
        if (first >> start >> step >> end >> fixed)
        {
            std::wostringstream r; r << std::setprecision(12) << start << L" " << step << L" " << start << L" " << fixed << L" period_start period_step period_end fixed/free";
            lines[0] = r.str();
        }
        lines[10] = L"2                iteration stop condition";
        std::wostringstream out;
        for (size_t i = 0; i < lines.size(); ++i) { out << lines[i]; if (i + 1 < lines.size()) out << L"\n"; }
        return out.str();
    }

    bool SmokeMatches(const std::wstring& output)
    {
        std::wistringstream lines(output); std::wstring line;
        while (std::getline(lines, line)) if (!Trim(line).empty()) break;
        std::wistringstream ss(line);
        double a=0,b=0,c=0,d=0,e=0,f=0;
        if (!(ss>>a>>b>>c>>d>>e>>f)) return false;
        return fabs(a-17.20820800)<1e-6 && fabs(b-0.257266)<1e-6 && fabs(c-7.809903)<1e-6 && fabs(d-0.1)<1e-6 && fabs(e)<1e-9 && fabs(f)<1e-9;
    }

    std::wstring CycleReport(const std::shared_ptr<CycleState>& s)
    {
        std::wstring r = L"v0.9 complete BOINC cycle report\n\n";
        r += L"Smoke validation: " + s->smoke;
        r += L"\nProject: " + s->project;
        r += L"\nAccount: " + s->account;
        r += L"\nScheduler: " + s->scheduler;
        r += L"\nWork source: " + s->work_source;
        r += L"\nWork: " + s->work;
        r += L"\nInput download: " + s->download;
        r += L"\nScience solve: " + s->solve;
        r += L"\nUpload: " + s->upload;
        r += L"\nScheduler report: " + s->report;
        r += L"\nResult ACK: " + s->ack;
        r += L"\nCredit snapshot: " + s->credit;
        r += L"\nServer diagnostics: " + s->diagnostics;
        r += L"\nSafety: one workunit at a time; password plaintext is never stored";
        return r;
    }
}

App::App() { InitializeComponent(); }

void App::OnLaunched(LaunchActivatedEventArgs^)
{
    boinc_xbox_platform_init();
    unsigned int cpus = std::thread::hardware_concurrency(); if (!cpus) cpus = 1;
    unsigned long long mem = 0; try { mem = MemoryManager::AppMemoryUsageLimit; } catch (Exception^) {}
    const DiskInfo disk = QueryDiskInfo();

    auto root = ref new Grid(); root->Background = ref new SolidColorBrush(ColorHelper::FromArgb(255,16,24,32));
    auto scroll = ref new ScrollViewer(); scroll->VerticalScrollMode = ScrollMode::Auto; scroll->VerticalScrollBarVisibility = ScrollBarVisibility::Auto;
    auto panel = ref new StackPanel(); panel->MaxWidth = 1060; panel->Margin = Thickness(110,54,110,54); panel->Spacing = 14;
    auto title = ref new TextBlock(); title->Text = ref new String(L"BOINC Xbox"); title->FontSize = 48; title->HorizontalAlignment = HorizontalAlignment::Center;
    auto sub = ref new TextBlock(); sub->Text = ref new String(L"v0.9 - Complete upload + scheduler report cycle"); sub->FontSize = 22; sub->HorizontalAlignment = HorizontalAlignment::Center;
    auto runtime = ref new TextBlock(); runtime->Text = PS(L"CPU threads=" + std::to_wstring(cpus) + L", memory=" + std::to_wstring(mem/1048576ULL) + L" MB, disk free=" + std::to_wstring(disk.free/1048576ULL) + L" MB"); runtime->FontSize = 17;

    std::wstring initial = GetSetting(L"ProjectUrl"); if (initial.empty()) initial = L"https://asteroidsathome.net/boinc/";
    auto url = ref new TextBox(); url->Header = ref new String(L"Project URL"); url->Text = PS(initial); url->FontSize = 19;
    auto email = ref new TextBox(); email->Header = ref new String(L"Email (only needed if no authenticator is stored)"); email->Text = PS(GetSetting(L"AccountEmail")); email->FontSize = 19;
    auto pass = ref new PasswordBox(); pass->Header = ref new String(L"Password (never stored)"); pass->FontSize = 19;
    auto run = ref new Button(); run->Content = ref new String(L"Run one complete BOINC workunit cycle"); run->FontSize = 21; run->Padding = Thickness(26,14,26,14);
    auto hint = ref new TextBlock(); hint->Text = ref new String(L"v0.9 resumes the unreported v0.8 workunit when available, recomputes it for an accurate runtime, uploads the result through the BOINC file upload handler, reports completion to the scheduler and requires a result ACK. It never requests more than one workunit at a time."); hint->TextWrapping = TextWrapping::Wrap; hint->FontSize = 16; hint->Opacity = 0.84;
    auto reportTitle = ref new TextBlock(); reportTitle->Text = ref new String(L"Complete-cycle report"); reportTitle->FontSize = 28;
    auto report = ref new TextBlock(); report->Text = ref new String(L"Not run yet."); report->FontSize = 16; report->TextWrapping = TextWrapping::Wrap;
    auto reportBox = ref new Border(); reportBox->Padding = Thickness(20); reportBox->Background = ref new SolidColorBrush(ColorHelper::FromArgb(255,32,42,51)); reportBox->Child = report;
    auto logTitle = ref new TextBlock(); logTitle->Text = ref new String(L"Log"); logTitle->FontSize = 24;
    auto log = ref new TextBlock(); log->Text = ref new String(L"Ready.\n"); log->FontSize = 15; log->TextWrapping = TextWrapping::Wrap;
    auto logBox = ref new Border(); logBox->Padding = Thickness(20); logBox->Background = ref new SolidColorBrush(ColorHelper::FromArgb(255,24,31,38)); logBox->Child = log;

    auto append = [log](const std::wstring& m) { std::wstring x = log->Text ? log->Text->Data() : L""; x += m + L"\n"; log->Text = PS(x); };

    run->Click += ref new RoutedEventHandler([=](Object^, RoutedEventArgs^)
    {
        const std::wstring project_url = NormalizeUrl(url->Text ? url->Text->Data() : L"");
        if (project_url.empty()) { append(L"ERROR: Project URL is empty"); return; }
        auto s = std::make_shared<CycleState>(); s->project_url = project_url; s->email = Lower(Trim(email->Text ? email->Text->Data() : L""));
        const std::wstring password = pass->Password ? pass->Password->Data() : L""; pass->Password = ref new String(L"");
        try
        {
            SetSetting(L"ProjectUrl", project_url); if (!s->email.empty()) SetSetting(L"AccountEmail", s->email);
            std::wstring h = GetSetting(L"HostId"); if (!h.empty()) s->host_id = std::stoul(h);
            std::wstring q = GetSetting(L"RpcSeqno"); if (!q.empty()) s->rpc_seqno = std::stoi(q);
        }
        catch (...) {}

        run->IsEnabled = false; report->Text = ref new String(L"Running v0.9 complete cycle..."); log->Text = ref new String(L""); append(L"v0.9 cycle started");
        const auto ui = task_continuation_context::use_current();

        create_task(Package::Current->InstalledLocation->GetFileAsync(ref new String(L"PeriodSearchSampleIn.txt")))
        .then([](StorageFile^ f){ return create_task(FileIO::ReadTextAsync(f)); })
        .then([=](String^ raw){ return SaveText(L"period_search_in", SmokeInput(raw ? raw->Data() : L"")); }, ui)
        .then([=](){ const std::wstring dir = ApplicationData::Current->LocalFolder->Path->Data(); return create_task([dir](){ return periodsearch_run(dir,true); }); }, ui)
        .then([=](PeriodSearchRunResult r) -> task<String^>
        {
            s->smoke = (r.exit_code==0 && SmokeMatches(r.output)) ? L"PASS: numerical reference matched" : L"FAIL: reference mismatch";
            if (s->smoke.rfind(L"PASS",0)!=0) throw ref new FailureException(ref new String(L"Smoke validation failed; upload blocked"));
            append(L"Smoke validation PASS");
            return GetText(project_url + L"/get_project_config.php");
        }, ui)
        .then([=](String^ cfg) -> task<String^>
        {
            std::wstring xml = cfg ? cfg->Data() : L""; if (xml.find(L"<project_config")==std::wstring::npos) throw ref new FailureException(ref new String(L"Project config invalid"));
            s->project = L"PASS: " + Tag(xml,L"name"); s->master_url = NormalizeUrl(Tag(xml,L"master_url")); if (s->master_url.empty()) s->master_url=project_url;
            return GetText(s->master_url + L"/");
        }, ui)
        .then([=](String^ master) -> task<void>
        {
            auto urls = SchedulerUrls(master ? master->Data() : L""); if (urls.empty()) throw ref new FailureException(ref new String(L"Scheduler URL not found"));
            s->scheduler_url = urls.front(); s->scheduler = L"PASS: " + s->scheduler_url;
            std::wstring em, au;
            if (LoadAuth(project_url,em,au)) { s->email=em; s->authenticator=au; s->account=L"PASS: stored authenticator loaded"; return task_from_result(); }
            if (s->email.empty() || password.empty()) { s->account=L"FAIL: email/password required once"; throw ref new FailureException(ref new String(L"No authenticator")); }
            std::wstring lookup = project_url + L"/lookup_account.php?email_addr=" + UrlEncode(s->email) + L"&passwd_hash=" + Md5Hex(password+s->email);
            return GetText(lookup).then([=](String^ body)
            {
                std::wstring x=body?body->Data():L""; s->authenticator=Tag(x,L"authenticator"); if (s->authenticator.empty()) throw ref new FailureException(ref new String(L"Account lookup failed"));
                SaveAuth(project_url,s->email,s->authenticator); s->account=L"PASS: authenticator received and stored";
            });
        }, ui)
        .then([=]() -> task<String^> { return ReadLocalTextIfExists(L"v08_scheduler_anonymous.xml"); }, ui)
        .then([=](String^ cached) -> task<String^>
        {
            std::wstring xml = cached ? cached->Data() : L"";
            WorkPackage p = ParseWorkPackage(xml,project_url);
            const std::wstring last = GetSetting(L"LastReportedResult");
            if (p.valid && p.result_name != last)
            {
                s->package=p; s->work_source=L"PASS: resumed unreported v0.8 workunit"; append(L"Resuming cached v0.8 workunit: "+p.result_name); return task_from_result<String^>(cached);
            }
            s->work_source=L"PASS: requesting one new workunit"; append(L"Requesting one new workunit");
            return PostXml(s->scheduler_url,BuildRequest(s,cpus,mem,disk,true,L""));
        }, ui)
        .then([=](String^ work_reply) -> task<void>
        {
            std::wstring xml=work_reply?work_reply->Data():L"";
            if (!s->package.valid)
            {
                ++s->rpc_seqno; SetSetting(L"RpcSeqno",std::to_wstring(s->rpc_seqno));
                std::wstring host=Tag(xml,L"hostid"); if(!host.empty()){ try{s->host_id=std::stoul(host);SetSetting(L"HostId",host);}catch(...){} }
                s->package=ParseWorkPackage(xml,project_url); SaveText(L"v09_scheduler_work.xml",xml);
            }
            if (!s->package.valid) { s->diagnostics=ServerDiagnostics(xml); throw ref new FailureException(PS(L"No workunit: "+s->diagnostics)); }
            s->work=L"PASS: "+s->package.result_name; s->diagnostics=ServerDiagnostics(xml);
            return DownloadInputs(s->package).then([=](){ s->download=L"PASS: input file(s) ready"; });
        }, ui)
        .then([=]()
        {
            append(L"Running real PeriodSearch workunit..."); const std::wstring dir=ApplicationData::Current->LocalFolder->Path->Data(); return create_task([dir](){ return periodsearch_run(dir,true); });
        }, ui)
        .then([=](PeriodSearchRunResult r) -> task<void>
        {
            if(r.exit_code!=0 || r.output.empty()) throw ref new FailureException(ref new String(L"PeriodSearch workunit failed"));
            s->elapsed=r.elapsed_seconds; s->solve=L"PASS: elapsed="+std::to_wstring(r.elapsed_seconds)+L" s, sha256="+Sha256Hex(r.output).substr(0,20)+L"...";
            std::wstring physical;
            for(const auto& fr:s->package.outputs){ if(fr.open_name==L"period_search_out"||physical.empty())physical=fr.file_name; if(fr.open_name==L"period_search_out")break; }
            if(physical.empty()) throw ref new FailureException(ref new String(L"Physical result filename missing"));
            return CopyLocalFile(L"period_search_out",physical).then([=]()
            {
                const FileRecord* rec=FindFile(s->package,physical); if(!rec) throw ref new FailureException(ref new String(L"Output file_info missing"));
                append(L"Uploading result through BOINC file upload handler...");
                return UploadBoincFile(*rec,physical).then([=](UploadInfo ui2)
                {
                    s->upload_info=ui2; if(!ui2.ok){s->upload=L"FAIL: "+ui2.error; throw ref new FailureException(PS(s->upload));}
                    s->upload=L"PASS: "+std::to_wstring(ui2.nbytes)+L" bytes, md5="+ui2.md5; return task_from_result();
                });
            });
        }, ui)
        .then([=]() -> task<String^>
        {
            std::wstring physical; for(const auto& fr:s->package.outputs){ if(fr.open_name==L"period_search_out"||physical.empty())physical=fr.file_name; if(fr.open_name==L"period_search_out")break; }
            const FileRecord* rec=FindFile(s->package,physical); if(!rec) throw ref new FailureException(ref new String(L"Output metadata lost before report"));
            const std::wstring result_xml=BuildResultXml(s,*rec); append(L"Reporting completed result to scheduler...");
            return PostXml(s->scheduler_url,BuildRequest(s,cpus,mem,disk,false,result_xml));
        }, ui)
        .then([=](String^ reply) -> task<void>
        {
            std::wstring xml=reply?reply->Data():L""; ++s->rpc_seqno; SetSetting(L"RpcSeqno",std::to_wstring(s->rpc_seqno));
            s->report=xml.find(L"<scheduler_reply")!=std::wstring::npos?L"PASS: scheduler_reply received":L"FAIL: scheduler_reply missing";
            const bool ack=ResultAcked(xml,s->package.result_name); s->ack=ack?L"PASS: server acknowledged result":L"FAIL: no result_ack";
            s->diagnostics=ServerDiagnostics(xml);
            std::wstring credit=Tag(xml,L"user_total_credit"); s->credit=credit.empty()?L"not returned; validator is asynchronous":L"user_total_credit="+credit+L" (validation may still be pending)";
            SaveText(L"v09_scheduler_report.xml",xml);
            if(!ack) throw ref new FailureException(ref new String(L"Scheduler did not ACK result"));
            SetSetting(L"LastReportedResult",s->package.result_name);
            return task_from_result();
        }, ui)
        .then([=](task<void> finished)
        {
            try { finished.get(); }
            catch(Exception^ ex){ append(L"Cycle error: "+std::wstring(ex->Message->Data())); if(s->report==L"PENDING"&&s->upload.rfind(L"PASS",0)==0)s->report=L"FAIL before ACK"; }
            catch(...){ append(L"Cycle error: unknown exception"); }
            report->Text=PS(CycleReport(s)); run->IsEnabled=true; append(L"v0.9 cycle finished");
        }, ui);
    });

    panel->Children->Append(title); panel->Children->Append(sub); panel->Children->Append(runtime); panel->Children->Append(url); panel->Children->Append(email); panel->Children->Append(pass); panel->Children->Append(run); panel->Children->Append(hint); panel->Children->Append(reportTitle); panel->Children->Append(reportBox); panel->Children->Append(logTitle); panel->Children->Append(logBox);
    scroll->Content=panel; root->Children->Append(scroll); Window::Current->Content=root; Window::Current->Activate();
}
