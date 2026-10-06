#include "pch.h"
#include "App.xaml.h"

#include <atomic>
#include <functional>
#include <intrin.h>
#include <iomanip>
#include <sstream>
#include <d3d11_1.h>
#include <wrl.h>

// Reuse the already validated v0.9.1 BOINC protocol implementation in this
// translation unit. Only the legacy App class is renamed; all anonymous-namespace
// protocol helpers remain available to the v1.0 engine below.
namespace BOINC_Xbox
{
    class LegacyAppV09
    {
    public:
        LegacyAppV09();
        void OnLaunched(Windows::ApplicationModel::Activation::LaunchActivatedEventArgs^ e);
    private:
        void InitializeComponent() {}
    };
}

#define App LegacyAppV09
#include "AppV09.cpp"
#undef App

using namespace BOINC_Xbox;
using namespace Platform;
using namespace concurrency;
using namespace Microsoft::WRL;
using namespace Windows::ApplicationModel;
using namespace Windows::ApplicationModel::Activation;
using namespace Windows::Foundation;
using namespace Windows::Storage;
using namespace Windows::System;
using namespace Windows::UI;
using namespace Windows::UI::Xaml;
using namespace Windows::UI::Xaml::Controls;
using namespace Windows::UI::Xaml::Media;

namespace
{
    struct V10Stats
    {
        unsigned long long completed = 0;
        unsigned long long failed = 0;
        unsigned long long uploaded = 0;
        unsigned long long checkpoints = 0;
        double total_solve_seconds = 0.0;
        std::wstring last_result;
        std::wstring last_credit;
    };

    struct V10Engine
    {
        std::atomic<bool> running{ false };
        std::atomic<bool> stop_requested{ false };
        std::atomic<bool> network_enabled{ true };
        int target_success = 3;
        int session_success = 0;
        int session_attempts = 0;
        int max_attempts = 7;
        bool smoke_needed = true;
        std::wstring stage = L"IDLE";
        std::wstring current_result;
    };

    struct V10CycleResult
    {
        bool success = false;
        bool no_work = false;
        std::wstring error;
        std::shared_ptr<CycleState> state;
    };

    unsigned long long ParseU64(const std::wstring& s)
    {
        try { return s.empty() ? 0ULL : std::stoull(s); }
        catch (...) { return 0ULL; }
    }

    double ParseDouble10(const std::wstring& s)
    {
        try { return s.empty() ? 0.0 : std::stod(s); }
        catch (...) { return 0.0; }
    }

    V10Stats LoadV10Stats()
    {
        V10Stats s;
        s.completed = ParseU64(GetSetting(L"V10Completed"));
        s.failed = ParseU64(GetSetting(L"V10Failed"));
        s.uploaded = ParseU64(GetSetting(L"V10Uploaded"));
        s.checkpoints = ParseU64(GetSetting(L"V10Checkpoints"));
        s.total_solve_seconds = ParseDouble10(GetSetting(L"V10TotalSolveSeconds"));
        s.last_result = GetSetting(L"V10LastResult");
        s.last_credit = GetSetting(L"V10LastCredit");
        return s;
    }

    void SaveV10Stats(const V10Stats& s)
    {
        SetSetting(L"V10Completed", std::to_wstring(s.completed));
        SetSetting(L"V10Failed", std::to_wstring(s.failed));
        SetSetting(L"V10Uploaded", std::to_wstring(s.uploaded));
        SetSetting(L"V10Checkpoints", std::to_wstring(s.checkpoints));
        SetSetting(L"V10TotalSolveSeconds", std::to_wstring(s.total_solve_seconds));
        SetSetting(L"V10LastResult", s.last_result);
        SetSetting(L"V10LastCredit", s.last_credit);
    }

    std::wstring StatsText(const V10Stats& s)
    {
        const double average = s.completed ? s.total_solve_seconds / static_cast<double>(s.completed) : 0.0;
        std::wostringstream o;
        o << L"Lifetime completed: " << s.completed
          << L"   uploaded+ACK: " << s.uploaded
          << L"   failed attempts: " << s.failed
          << L"\nTotal science time: " << std::fixed << std::setprecision(1) << s.total_solve_seconds
          << L" s   average/WU: " << average << L" s"
          << L"   checkpoints: " << s.checkpoints;
        if (!s.last_result.empty()) o << L"\nLast ACK result: " << s.last_result;
        if (!s.last_credit.empty()) o << L"   credit snapshot: " << s.last_credit;
        return o.str();
    }

    std::wstring CpuFeatureSummary()
    {
        int r[4] = { 0,0,0,0 };
        __cpuid(r, 0);
        const int max_id = r[0];
        bool sse2 = false, avx = false, fma = false, avx2 = false;
        if (max_id >= 1)
        {
            __cpuid(r, 1);
            sse2 = (r[3] & (1 << 26)) != 0;
            avx = (r[2] & (1 << 28)) != 0;
            fma = (r[2] & (1 << 12)) != 0;
        }
        if (max_id >= 7)
        {
            __cpuidex(r, 7, 0);
            avx2 = (r[1] & (1 << 5)) != 0;
        }
        return L"SSE2=" + std::wstring(sse2 ? L"yes" : L"no") +
            L", AVX=" + (avx ? L"yes" : L"no") +
            L", AVX2=" + (avx2 ? L"yes" : L"no") +
            L", FMA=" + (fma ? L"yes" : L"no");
    }

    std::wstring GpuProbeSummary()
    {
        D3D_FEATURE_LEVEL levels[] = {
            D3D_FEATURE_LEVEL_12_1, D3D_FEATURE_LEVEL_12_0,
            D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0
        };
        D3D_FEATURE_LEVEL got = D3D_FEATURE_LEVEL_11_0;
        ComPtr<ID3D11Device> dev;
        ComPtr<ID3D11DeviceContext> ctx;
        HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0,
            levels, ARRAYSIZE(levels), D3D11_SDK_VERSION, &dev, &got, &ctx);
        if (FAILED(hr)) return L"D3D11 hardware compute device: FAIL";
        const unsigned int major = ((unsigned int)got >> 12) & 0xF;
        const unsigned int minor = ((unsigned int)got >> 8) & 0xF;
        return L"D3D11 hardware compute device: PASS, FL " + std::to_wstring(major) + L"." + std::to_wstring(minor) +
            L" (science backend remains CPU in v1.0)";
    }

    task<bool> LocalFileExists10(const std::wstring& name)
    {
        return create_task(ApplicationData::Current->LocalFolder->GetFileAsync(PS(name))).then([](task<StorageFile^> t)
        {
            try { t.get(); return true; }
            catch (...) { return false; }
        });
    }

    task<String^> LoadV10CachedWorkXml(const std::wstring& project_url)
    {
        // Only a true active-state file may be resumed. Old scheduler replies can
        // contain already reported jobs, so they are deliberately ignored here.
        return ReadLocalTextIfExists(L"v10_active_work.xml").then([project_url](String^ active) -> task<String^>
        {
            const std::wstring a = active ? active->Data() : L"";
            if (ParseWorkPackage(a, project_url).valid) return task_from_result<String^>(active);
            // One-time migration path from v0.9.1: only its active file, never its
            // historical scheduler-work cache.
            return ReadLocalTextIfExists(L"v09_active_work.xml");
        }).then([project_url](String^ legacy) -> String^
        {
            const std::wstring l = legacy ? legacy->Data() : L"";
            if (ParseWorkPackage(l, project_url).valid) return legacy;
            return ref new String(L"");
        });
    }

    std::wstring PhysicalOutputName(const WorkPackage& p)
    {
        std::wstring physical;
        for (const auto& fr : p.outputs)
        {
            if (fr.open_name == L"period_search_out" || physical.empty()) physical = fr.file_name;
            if (fr.open_name == L"period_search_out") break;
        }
        return physical;
    }

    task<void> SolveAndPrepareV10(const std::shared_ptr<CycleState>& s, bool resumed)
    {
        const std::wstring physical = PhysicalOutputName(s->package);
        if (physical.empty()) throw ref new FailureException(ref new String(L"Physical result filename missing"));

        const std::wstring active_result = GetSetting(L"V10ActiveResult");
        const std::wstring active_stage = GetSetting(L"V10ActiveStage");
        const bool same = active_result == s->package.result_name;

        auto run_solver = [=]() -> task<void>
        {
            return LocalFileExists10(L"period_search_state").then([=](bool checkpoint_exists) -> task<void>
            {
                // A checkpoint may only be reused for the exact result that created it.
                const bool checkpoint_matches = GetSetting(L"V10CheckpointResult") == s->package.result_name;
                const bool fresh_start = !(resumed && checkpoint_exists && checkpoint_matches);
                SetSetting(L"V10CheckpointResult", s->package.result_name);
                const std::wstring dir = ApplicationData::Current->LocalFolder->Path->Data();
                periodsearch_set_checkpoint_interval(45);
                return create_task([dir, fresh_start]() { return periodsearch_run(dir, fresh_start); }).then([=](PeriodSearchRunResult r) -> task<void>
                {
                    if (r.exit_code != 0 || r.output.empty())
                        throw ref new FailureException(PS(L"PeriodSearch failed: " + r.error));
                    s->elapsed = r.elapsed_seconds;
                    s->solve = L"PASS: elapsed=" + std::to_wstring(r.elapsed_seconds) + L" s, checkpoints=" + std::to_wstring(r.checkpoints) +
                        L", mode=" + (fresh_start ? L"fresh" : L"checkpoint-resume") + L", sha256=" + Sha256Hex(r.output).substr(0,20) + L"...";
                    SetSetting(L"V10LastRunCheckpoints", std::to_wstring(r.checkpoints));
                    return CopyLocalFile(L"period_search_out", physical).then([=]()
                    {
                        SetSetting(L"V10ActiveResult", s->package.result_name);
                        SetSetting(L"V10ActiveStage", L"solved");
                        SetSetting(L"V10ActiveElapsed", std::to_wstring(s->elapsed));
                        SetSetting(L"V10ActivePhysical", physical);
                    });
                });
            });
        };

        if (same && (active_stage == L"solved" || active_stage == L"uploaded"))
        {
            return LocalFileExists10(physical).then([=](bool exists) -> task<void>
            {
                if (!exists) return run_solver();
                s->elapsed = ParseDouble10(GetSetting(L"V10ActiveElapsed"));
                s->solve = L"PASS: reused persisted solved output, elapsed=" + std::to_wstring(s->elapsed) + L" s";
                return task_from_result();
            });
        }
        return run_solver();
    }

    task<void> UploadPreparedV10(const std::shared_ptr<CycleState>& s)
    {
        const std::wstring physical = PhysicalOutputName(s->package);
        const FileRecord* ptr = FindFile(s->package, physical);
        if (!ptr) throw ref new FailureException(ref new String(L"Output file_info missing"));
        const FileRecord rec = *ptr;
        const std::wstring stage = GetSetting(L"V10ActiveStage");
        const bool same = GetSetting(L"V10ActiveResult") == s->package.result_name;

        if (same && stage == L"uploaded")
        {
            s->upload_info.ok = true;
            s->upload_info.nbytes = static_cast<unsigned int>(ParseU64(GetSetting(L"V10ActiveBytes")));
            s->upload_info.md5 = GetSetting(L"V10ActiveMd5");
            s->upload_info.certificate_present = !rec.xml_signature.empty();
            s->upload = L"PASS: reused previously uploaded file, bytes=" + std::to_wstring(s->upload_info.nbytes) + L", md5=" + s->upload_info.md5;
            return task_from_result();
        }

        return UploadBoincFile(rec, physical).then([=](UploadInfo u)
        {
            s->upload_info = u;
            if (!u.ok)
            {
                s->upload = L"FAIL: " + u.error;
                throw ref new FailureException(PS(s->upload));
            }
            s->upload = L"PASS: " + std::to_wstring(u.nbytes) + L" bytes, md5=" + u.md5 +
                (u.certificate_present ? L", certificate present" : L", server accepted unsigned upload");
            SetSetting(L"V10ActiveStage", L"uploaded");
            SetSetting(L"V10ActiveBytes", std::to_wstring(u.nbytes));
            SetSetting(L"V10ActiveMd5", u.md5);
        });
    }

    task<V10CycleResult> RunOneV10Cycle(
        const std::wstring& project_url,
        const std::shared_ptr<std::wstring>& email,
        const std::shared_ptr<std::wstring>& password,
        unsigned int cpus,
        unsigned long long mem,
        const DiskInfo& disk,
        bool run_smoke,
        const std::shared_ptr<V10Engine>& engine,
        const std::function<void(const std::wstring&)>& append,
        const std::function<void(const std::wstring&, const std::wstring&)>& set_stage,
        task_continuation_context ui)
    {
        auto s = std::make_shared<CycleState>();
        s->project_url = project_url;
        s->email = *email;
        // Keep the existing BOINC host identity and monotonically increasing RPC
        // sequence. Sending hostid=0/rpc_seqno=0 repeatedly can make a scheduler
        // treat this as a reattached client and disturb in-progress results.
        try
        {
            const std::wstring h = GetSetting(L"HostId");
            if (!h.empty()) s->host_id = std::stoul(h);
            const std::wstring q = GetSetting(L"RpcSeqno");
            if (!q.empty()) s->rpc_seqno = std::stoi(q);
        }
        catch (...) {}
        auto resumed = std::make_shared<bool>(false);

        task<void> start = task_from_result();
        if (run_smoke)
        {
            set_stage(L"VALIDATING", L"");
            start = create_task(Package::Current->InstalledLocation->GetFileAsync(ref new String(L"PeriodSearchSampleIn.txt")))
                .then([](StorageFile^ f) { return create_task(FileIO::ReadTextAsync(f)); })
                .then([=](String^ raw) { return SaveText(L"period_search_in", SmokeInput(raw ? raw->Data() : L"")); })
                .then([=]()
                {
                    const std::wstring dir = ApplicationData::Current->LocalFolder->Path->Data();
                    periodsearch_set_checkpoint_interval(45);
                    return create_task([dir]() { return periodsearch_run(dir, true); });
                })
                .then([=](PeriodSearchRunResult r)
                {
                    s->smoke = (r.exit_code == 0 && SmokeMatches(r.output)) ? L"PASS: numerical reference matched" : L"FAIL: reference mismatch";
                    if (s->smoke.rfind(L"PASS",0) != 0) throw ref new FailureException(ref new String(L"Smoke validation failed"));
                });
        }
        else s->smoke = L"SKIP: already passed in this session";

        auto chain = start
        .then([=]() -> task<String^>
        {
            if (!engine->network_enabled.load()) throw ref new FailureException(ref new String(L"Network paused"));
            set_stage(L"PROJECT CONFIG", L"");
            return GetText(project_url + L"/get_project_config.php");
        }, ui)
        .then([=](String^ cfg) -> task<String^>
        {
            const std::wstring xml = cfg ? cfg->Data() : L"";
            if (xml.find(L"<project_config") == std::wstring::npos) throw ref new FailureException(ref new String(L"Project config invalid"));
            s->project = L"PASS: " + Tag(xml,L"name");
            s->master_url = NormalizeUrl(Tag(xml,L"master_url"));
            if (s->master_url.empty()) s->master_url = project_url;
            return GetText(s->master_url + L"/");
        }, ui)
        .then([=](String^ master) -> task<void>
        {
            auto urls = SchedulerUrls(master ? master->Data() : L"");
            if (urls.empty()) throw ref new FailureException(ref new String(L"Scheduler URL not found"));
            s->scheduler_url = urls.front();
            s->scheduler = L"PASS: " + s->scheduler_url;
            set_stage(L"ACCOUNT / SCHEDULER", L"");

            std::wstring em, au;
            if (LoadAuth(project_url, em, au))
            {
                *email = em;
                s->email = em;
                s->authenticator = au;
                s->account = L"PASS: stored authenticator loaded";
                password->clear();
                return task_from_result();
            }
            if (email->empty() || password->empty())
                throw ref new FailureException(ref new String(L"Email/password required once"));

            const std::wstring lookup = project_url + L"/lookup_account.php?email_addr=" + UrlEncode(*email) + L"&passwd_hash=" + Md5Hex(*password + *email);
            return GetText(lookup).then([=](String^ body)
            {
                const std::wstring x = body ? body->Data() : L"";
                s->authenticator = Tag(x,L"authenticator");
                if (s->authenticator.empty()) throw ref new FailureException(ref new String(L"Account lookup failed"));
                SaveAuth(project_url,*email,s->authenticator);
                s->account = L"PASS: authenticator received and stored";
                password->clear();
            });
        }, ui)
        .then([=]() -> task<String^>
        {
            set_stage(L"WORK ACQUISITION", L"");
            return LoadV10CachedWorkXml(project_url);
        }, ui)
        .then([=](String^ cached) -> task<String^>
        {
            const std::wstring xml = cached ? cached->Data() : L"";
            WorkPackage p = ParseWorkPackage(xml, project_url);
            const std::wstring last = GetSetting(L"LastReportedResult");
            if (p.valid && p.result_name != last)
            {
                *resumed = true;
                s->package = p;
                s->work_source = L"PASS: resumed pending workunit";
                set_stage(L"RESUMING", p.result_name);
                append(L"Resuming pending result: " + p.result_name);
                return task_from_result<String^>(cached);
            }
            s->work_source = L"PASS: requesting one new workunit";
            append(L"Requesting one new workunit");
            return PostXml(s->scheduler_url, BuildRequest(s,cpus,mem,disk,true,L""));
        }, ui)
        .then([=](String^ work_reply) -> task<void>
        {
            const std::wstring xml = work_reply ? work_reply->Data() : L"";
            if (!s->package.valid)
            {
                ++s->rpc_seqno;
                SetSetting(L"RpcSeqno", std::to_wstring(s->rpc_seqno));
                const std::wstring host = Tag(xml,L"hostid");
                if (!host.empty()) { try { s->host_id = std::stoul(host); SetSetting(L"HostId",host); } catch (...) {} }
                s->package = ParseWorkPackage(xml,project_url);
                if (!s->package.valid)
                {
                    s->diagnostics = ServerDiagnostics(xml);
                    throw ref new FailureException(PS(L"No workunit: " + s->diagnostics));
                }
                SetSetting(L"V10ActiveResult", s->package.result_name);
                SetSetting(L"V10ActiveStage", L"downloaded");
                return SaveText(L"v10_active_work.xml",xml).then([=]()
                {
                    s->work = L"PASS: " + s->package.result_name;
                    s->diagnostics = ServerDiagnostics(xml);
                    set_stage(L"DOWNLOADING", s->package.result_name);
                    return DownloadInputs(s->package).then([=]() { s->download = L"PASS: input file(s) ready"; });
                });
            }
            s->work = L"PASS: " + s->package.result_name;
            set_stage(L"DOWNLOADING / RESUME", s->package.result_name);
            return DownloadInputs(s->package).then([=]() { s->download = L"PASS: input file(s) ready"; });
        }, ui)
        .then([=]() -> task<void>
        {
            set_stage(L"SCIENCE COMPUTE", s->package.result_name);
            append(L"PeriodSearch compute: " + s->package.result_name);
            return SolveAndPrepareV10(s,*resumed);
        }, ui)
        .then([=]() -> task<void>
        {
            if (!engine->network_enabled.load()) throw ref new FailureException(ref new String(L"Network paused after compute; output persisted"));
            set_stage(L"UPLOADING", s->package.result_name);
            return UploadPreparedV10(s);
        }, ui)
        .then([=]() -> task<String^>
        {
            const std::wstring physical = PhysicalOutputName(s->package);
            const FileRecord* rec = FindFile(s->package,physical);
            if (!rec) throw ref new FailureException(ref new String(L"Output metadata missing before report"));
            set_stage(L"REPORTING", s->package.result_name);
            return PostXml(s->scheduler_url, BuildRequest(s,cpus,mem,disk,false,BuildResultXml(s,*rec)));
        }, ui)
        .then([=](String^ reply) -> task<void>
        {
            const std::wstring xml = reply ? reply->Data() : L"";
            ++s->rpc_seqno;
            SetSetting(L"RpcSeqno",std::to_wstring(s->rpc_seqno));
            s->report = xml.find(L"<scheduler_reply") != std::wstring::npos ? L"PASS: scheduler_reply received" : L"FAIL: scheduler_reply missing";
            const bool ack = ResultAcked(xml,s->package.result_name);
            s->ack = ack ? L"PASS: server acknowledged result" : L"FAIL: no result_ack";
            s->diagnostics = ServerDiagnostics(xml);
            const std::wstring credit = Tag(xml,L"user_total_credit");
            s->credit = credit.empty() ? L"not returned; validator asynchronous" : L"user_total_credit=" + credit;
            return SaveText(L"v10_scheduler_report.xml",xml).then([=]()
            {
                if (!ack) throw ref new FailureException(ref new String(L"Scheduler did not ACK result"));
                SetSetting(L"LastReportedResult",s->package.result_name);
                SetSetting(L"V10ActiveStage",L"");
                SetSetting(L"V10ActiveResult",L"");
                SetSetting(L"V10ActiveElapsed",L"");
                SetSetting(L"V10ActivePhysical",L"");
                SetSetting(L"V10ActiveBytes",L"");
                SetSetting(L"V10ActiveMd5",L"");
                SetSetting(L"V10CheckpointResult",L"");
                return SaveText(L"v10_active_work.xml",L"").then([=]()
                {
                    return SaveText(L"v09_active_work.xml",L"");
                });
            });
        }, ui);

        return chain.then([=](task<void> finished)
        {
            V10CycleResult out;
            out.state = s;
            try
            {
                finished.get();
                out.success = s->ack.rfind(L"PASS",0) == 0;
                set_stage(out.success ? L"ACKNOWLEDGED" : L"FAILED", s->package.result_name);
            }
            catch (Exception^ ex)
            {
                out.error = ex->Message ? ex->Message->Data() : L"Platform exception";
                out.no_work = out.error.find(L"No workunit:") != std::wstring::npos;
                append(L"Cycle error: " + out.error);
                set_stage(out.no_work ? L"NO WORK" : L"RECOVERABLE ERROR", s->package.result_name);
            }
            catch (...)
            {
                out.error = L"Unknown native exception";
                append(L"Cycle error: unknown native exception");
                set_stage(L"RECOVERABLE ERROR", s->package.result_name);
            }
            return out;
        }, ui);
    }

    std::wstring V10Report(const V10CycleResult& r, const V10Stats& stats, const V10Engine& engine)
    {
        std::wstring out = L"BOINC Xbox v1.0 production-test report\n\n";
        if (r.state)
        {
            out += L"Smoke: " + r.state->smoke;
            out += L"\nProject: " + r.state->project;
            out += L"\nAccount: " + r.state->account;
            out += L"\nScheduler: " + r.state->scheduler;
            out += L"\nWork source: " + r.state->work_source;
            out += L"\nWork: " + r.state->work;
            out += L"\nDownload: " + r.state->download;
            out += L"\nScience: " + r.state->solve;
            out += L"\nUpload: " + r.state->upload;
            out += L"\nReport: " + r.state->report;
            out += L"\nACK: " + r.state->ack;
            out += L"\nCredit: " + r.state->credit;
            out += L"\nServer: " + r.state->diagnostics;
        }
        if (!r.error.empty()) out += L"\nLast error: " + r.error;
        out += L"\n\nSession: " + std::to_wstring(engine.session_success) + L" completed, " + std::to_wstring(engine.session_attempts) + L" attempts";
        out += L"\n" + StatsText(stats);
        return out;
    }
}

namespace BOINC_Xbox
{
    App::App()
    {
        InitializeComponent();
    }

    void App::OnLaunched(LaunchActivatedEventArgs^)
    {
        boinc_xbox_platform_init();
        periodsearch_set_checkpoint_interval(45);

        unsigned int cpus = std::thread::hardware_concurrency();
        if (!cpus) cpus = 1;
        unsigned long long mem_limit = 0;
        try { mem_limit = MemoryManager::AppMemoryUsageLimit; } catch (Exception^) {}
        const DiskInfo disk = QueryDiskInfo();

        auto engine = std::make_shared<V10Engine>();
        auto stats = std::make_shared<V10Stats>(LoadV10Stats());

        auto root = ref new Grid();
        root->Background = ref new SolidColorBrush(ColorHelper::FromArgb(255,16,24,32));
        auto scroll = ref new ScrollViewer();
        scroll->VerticalScrollMode = ScrollMode::Auto;
        scroll->VerticalScrollBarVisibility = ScrollBarVisibility::Auto;
        auto panel = ref new StackPanel();
        panel->MaxWidth = 1120;
        panel->Margin = Thickness(90,46,90,46);
        panel->Spacing = 13;

        auto title = ref new TextBlock();
        title->Text = ref new String(L"BOINC Xbox");
        title->FontSize = 48;
        title->HorizontalAlignment = HorizontalAlignment::Center;
        auto sub = ref new TextBlock();
        sub->Text = ref new String(L"v1.0 - automated Asteroids@home production test");
        sub->FontSize = 23;
        sub->HorizontalAlignment = HorizontalAlignment::Center;

        const unsigned long long mem_mb = mem_limit / 1048576ULL;
        const unsigned int memory_slots = mem_mb ? (unsigned int)(mem_mb / 220ULL) : 1U;
        const unsigned int hardware_slots = (std::min)(cpus, (std::max)(1U,memory_slots));
        auto capability = ref new TextBlock();
        capability->Text = PS(
            L"CPU threads=" + std::to_wstring(cpus) +
            L", RAM limit=" + std::to_wstring(mem_mb) + L" MB, disk free=" + std::to_wstring(disk.free/1048576ULL) + L" MB\n" +
            CpuFeatureSummary() + L"\n" + GpuProbeSummary() +
            L"\nEstimated hardware task slots=" + std::to_wstring(hardware_slots) +
            L"; active PeriodSearch concurrency=1 because the upstream solver uses process-global state.");
        capability->FontSize = 16;
        capability->TextWrapping = TextWrapping::Wrap;

        std::wstring initial = GetSetting(L"ProjectUrl");
        if (initial.empty()) initial = L"https://asteroidsathome.net/boinc/";
        auto url = ref new TextBox();
        url->Header = ref new String(L"Project URL");
        url->Text = PS(initial);
        url->FontSize = 18;
        auto email = ref new TextBox();
        email->Header = ref new String(L"Email (only needed once if authenticator is not stored)");
        email->Text = PS(GetSetting(L"AccountEmail"));
        email->FontSize = 18;
        auto pass = ref new PasswordBox();
        pass->Header = ref new String(L"Password (never stored)");
        pass->FontSize = 18;

        auto mode = ref new ComboBox();
        mode->Header = ref new String(L"Test mode");
        mode->FontSize = 18;
        mode->Items->Append(ref new String(L"3 workunits - integration test"));
        mode->Items->Append(ref new String(L"5 workunits - stress test"));
        mode->Items->Append(ref new String(L"Continuous - run until Stop"));
        mode->SelectedIndex = 0;

        auto buttons = ref new StackPanel();
        buttons->Orientation = Orientation::Horizontal;
        buttons->Spacing = 12;
        auto start = ref new Button();
        start->Content = ref new String(L"Start v1.0 engine");
        start->FontSize = 19;
        start->Padding = Thickness(22,12,22,12);
        auto stop = ref new Button();
        stop->Content = ref new String(L"Stop after current WU");
        stop->FontSize = 17;
        stop->Padding = Thickness(18,12,18,12);
        stop->IsEnabled = false;
        auto network = ref new Button();
        network->Content = ref new String(L"Network: ON");
        network->FontSize = 17;
        network->Padding = Thickness(18,12,18,12);
        auto reset = ref new Button();
        reset->Content = ref new String(L"Reset statistics");
        reset->FontSize = 17;
        reset->Padding = Thickness(18,12,18,12);
        buttons->Children->Append(start);
        buttons->Children->Append(stop);
        buttons->Children->Append(network);
        buttons->Children->Append(reset);

        auto statusTitle = ref new TextBlock();
        statusTitle->Text = ref new String(L"Live engine status");
        statusTitle->FontSize = 27;
        auto status = ref new TextBlock();
        status->FontSize = 18;
        status->TextWrapping = TextWrapping::Wrap;
        auto statusBox = ref new Border();
        statusBox->Padding = Thickness(20);
        statusBox->Background = ref new SolidColorBrush(ColorHelper::FromArgb(255,31,46,58));
        statusBox->Child = status;

        auto statsTitle = ref new TextBlock();
        statsTitle->Text = ref new String(L"Persistent statistics");
        statsTitle->FontSize = 25;
        auto statsText = ref new TextBlock();
        statsText->Text = PS(StatsText(*stats));
        statsText->FontSize = 17;
        statsText->TextWrapping = TextWrapping::Wrap;
        auto statsBox = ref new Border();
        statsBox->Padding = Thickness(18);
        statsBox->Background = ref new SolidColorBrush(ColorHelper::FromArgb(255,32,42,51));
        statsBox->Child = statsText;

        auto reportTitle = ref new TextBlock();
        reportTitle->Text = ref new String(L"Last cycle report");
        reportTitle->FontSize = 25;
        auto report = ref new TextBlock();
        report->Text = ref new String(L"No v1.0 cycle has run yet.");
        report->FontSize = 15;
        report->TextWrapping = TextWrapping::Wrap;
        auto reportBox = ref new Border();
        reportBox->Padding = Thickness(18);
        reportBox->Background = ref new SolidColorBrush(ColorHelper::FromArgb(255,32,42,51));
        reportBox->Child = report;

        auto logTitle = ref new TextBlock();
        logTitle->Text = ref new String(L"Session log");
        logTitle->FontSize = 24;
        auto log = ref new TextBlock();
        log->Text = ref new String(L"Ready. Pending work from v0.9/v1.0 will be resumed automatically.\n");
        log->FontSize = 14;
        log->TextWrapping = TextWrapping::Wrap;
        auto logBox = ref new Border();
        logBox->Padding = Thickness(18);
        logBox->Background = ref new SolidColorBrush(ColorHelper::FromArgb(255,24,31,38));
        logBox->Child = log;

        auto append = [log](const std::wstring& m)
        {
            std::wstring x = log->Text ? log->Text->Data() : L"";
            x += m + L"\n";
            if (x.size() > 14000) x.erase(0, x.size() - 11000);
            log->Text = PS(x);
        };

        auto set_stage = [=](const std::wstring& st, const std::wstring& result_name)
        {
            engine->stage = st;
            engine->current_result = result_name;
        };

        auto update_stats = [=]()
        {
            statsText->Text = PS(StatsText(*stats));
        };

        auto finish_session = [=](const std::wstring& reason)
        {
            engine->running.store(false);
            engine->stage = reason;
            start->IsEnabled = true;
            stop->IsEnabled = false;
            reset->IsEnabled = true;
            mode->IsEnabled = true;
            append(L"Session finished: " + reason);
        };

        auto timer = ref new DispatcherTimer();
        TimeSpan interval;
        interval.Duration = 10000000LL;
        timer->Interval = interval;
        timer->Tick += ref new EventHandler<Object^>([=](Object^, Object^)
        {
            unsigned long long usage = 0;
            try { usage = MemoryManager::AppMemoryUsage; } catch (Exception^) {}
            const double progress = periodsearch_progress() * 100.0;
            std::wostringstream o;
            o << L"State: " << engine->stage
              << L"   network=" << (engine->network_enabled.load() ? L"ON" : L"PAUSED")
              << L"   stop=" << (engine->stop_requested.load() ? L"requested" : L"no")
              << L"\nSession completed: " << engine->session_success;
            if (engine->target_success > 0) o << L" / " << engine->target_success;
            else o << L" / continuous";
            o << L"   attempts=" << engine->session_attempts;
            if (!engine->current_result.empty()) o << L"\nCurrent result: " << engine->current_result;
            o << L"\nScience progress: " << std::fixed << std::setprecision(1) << progress << L" %"
              << L"   checkpoints this run=" << periodsearch_checkpoint_count()
              << L"   app memory=" << (usage/1048576ULL) << L" / " << mem_mb << L" MB";
            status->Text = PS(o.str());
        });
        timer->Start();

        network->Click += ref new RoutedEventHandler([=](Object^, RoutedEventArgs^)
        {
            const bool new_value = !engine->network_enabled.load();
            engine->network_enabled.store(new_value);
            network->Content = ref new String(new_value ? L"Network: ON" : L"Network: PAUSED");
            append(new_value ? L"Network enabled." : L"Network pause requested. Current science task may finish; new network stages will stop.");
        });

        stop->Click += ref new RoutedEventHandler([=](Object^, RoutedEventArgs^)
        {
            engine->stop_requested.store(true);
            stop->Content = ref new String(L"Stopping after current WU...");
            append(L"Stop requested. The active workunit will finish safely.");
        });

        reset->Click += ref new RoutedEventHandler([=](Object^, RoutedEventArgs^)
        {
            if (engine->running.load()) return;
            *stats = V10Stats();
            SaveV10Stats(*stats);
            update_stats();
            append(L"Persistent v1.0 statistics reset.");
        });

        start->Click += ref new RoutedEventHandler([=](Object^, RoutedEventArgs^)
        {
            if (engine->running.load()) return;
            const std::wstring project_url = NormalizeUrl(url->Text ? url->Text->Data() : L"");
            if (project_url.empty()) { append(L"ERROR: Project URL is empty."); return; }

            auto session_email = std::make_shared<std::wstring>(Lower(Trim(email->Text ? email->Text->Data() : L"")));
            auto session_password = std::make_shared<std::wstring>(pass->Password ? pass->Password->Data() : L"");
            pass->Password = ref new String(L"");
            try
            {
                SetSetting(L"ProjectUrl", project_url);
                if (!session_email->empty()) SetSetting(L"AccountEmail", *session_email);
            }
            catch (...) {}

            const int selected = mode->SelectedIndex;
            engine->target_success = selected == 0 ? 3 : (selected == 1 ? 5 : -1);
            engine->max_attempts = engine->target_success > 0 ? engine->target_success + 4 : 1000000;
            engine->session_success = 0;
            engine->session_attempts = 0;
            engine->smoke_needed = true;
            engine->stop_requested.store(false);
            engine->running.store(true);
            engine->stage = L"STARTING";
            engine->current_result.clear();
            start->IsEnabled = false;
            stop->IsEnabled = true;
            stop->Content = ref new String(L"Stop after current WU");
            reset->IsEnabled = false;
            mode->IsEnabled = false;
            log->Text = ref new String(L"");
            append(L"BOINC Xbox v1.0 session started.");
            append(L"Checkpoint interval: 45 s. Recovery state is persistent.");

            const auto ui = task_continuation_context::use_current();
            auto next = std::make_shared<std::function<void()>>();
            *next = [=]()
            {
                if (!engine->running.load()) return;
                if (engine->stop_requested.load()) { finish_session(L"STOPPED BY USER"); return; }
                if (!engine->network_enabled.load()) { finish_session(L"PAUSED - NETWORK OFF"); return; }
                if (engine->target_success > 0 && engine->session_success >= engine->target_success) { finish_session(L"TEST TARGET COMPLETE"); return; }
                if (engine->session_attempts >= engine->max_attempts) { finish_session(L"ATTEMPT LIMIT REACHED"); return; }

                ++engine->session_attempts;
                append(L"--- cycle attempt " + std::to_wstring(engine->session_attempts) + L" ---");
                const bool smoke = engine->smoke_needed;

                RunOneV10Cycle(project_url,session_email,session_password,cpus,mem_limit,disk,smoke,engine,append,set_stage,ui)
                .then([=](V10CycleResult result)
                {
                    if (result.state && result.state->smoke.rfind(L"PASS",0) == 0) engine->smoke_needed = false;

                    if (result.success)
                    {
                        ++engine->session_success;
                        ++stats->completed;
                        ++stats->uploaded;
                        stats->total_solve_seconds += result.state ? result.state->elapsed : 0.0;
                        stats->checkpoints += periodsearch_checkpoint_count();
                        if (result.state)
                        {
                            stats->last_result = result.state->package.result_name;
                            stats->last_credit = result.state->credit;
                        }
                        SaveV10Stats(*stats);
                        append(L"ACK PASS: " + stats->last_result);
                    }
                    else if (!result.no_work)
                    {
                        ++stats->failed;
                        SaveV10Stats(*stats);
                    }
                    else append(L"Server returned no work; this is not counted as a failure.");

                    update_stats();
                    report->Text = PS(V10Report(result,*stats,*engine));

                    if (engine->stop_requested.load()) { finish_session(L"STOPPED AFTER CURRENT WU"); return; }
                    if (!engine->network_enabled.load()) { finish_session(L"PAUSED - NETWORK OFF"); return; }
                    if (engine->target_success > 0 && engine->session_success >= engine->target_success) { finish_session(L"TEST TARGET COMPLETE"); return; }
                    if (engine->session_attempts >= engine->max_attempts) { finish_session(L"ATTEMPT LIMIT REACHED"); return; }

                    const int delay_seconds = result.no_work ? 60 : (result.success ? 12 : 15);
                    engine->stage = L"WAITING " + std::to_wstring(delay_seconds) + L" s BEFORE NEXT CYCLE";
                    create_task([delay_seconds]() { std::this_thread::sleep_for(std::chrono::seconds(delay_seconds)); })
                        .then([=]() { (*next)(); }, ui);
                }, ui);
            };
            (*next)();
        });

        Application::Current->Suspending += ref new SuspendingEventHandler([=](Object^, SuspendingEventArgs^)
        {
            engine->stop_requested.store(true);
            SetSetting(L"V10LastLifecycle", L"suspended");
        });

        panel->Children->Append(title);
        panel->Children->Append(sub);
        panel->Children->Append(capability);
        panel->Children->Append(url);
        panel->Children->Append(email);
        panel->Children->Append(pass);
        panel->Children->Append(mode);
        panel->Children->Append(buttons);
        panel->Children->Append(statusTitle);
        panel->Children->Append(statusBox);
        panel->Children->Append(statsTitle);
        panel->Children->Append(statsBox);
        panel->Children->Append(reportTitle);
        panel->Children->Append(reportBox);
        panel->Children->Append(logTitle);
        panel->Children->Append(logBox);

        scroll->Content = panel;
        root->Children->Append(scroll);
        Window::Current->Content = root;
        Window::Current->Activate();
    }
}
