#include "pch.h"
#include "App.xaml.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <functional>
#include <iomanip>
#include <numeric>
#include <sstream>
#include <thread>
#include <vector>

// Reuse the validated v1.0.2 protocol and science engine. v1.3 adds
// server-aware scheduler autotuning, corrected end-to-end telemetry,
// rate-limit diagnostics and the existing long-run production guards.
namespace BOINC_Xbox_V10
{
    class App
    {
    public:
        App();
        void OnLaunched(Windows::ApplicationModel::Activation::LaunchActivatedEventArgs^ e);
    private:
        void InitializeComponent() {}
    };
}

#define BOINC_Xbox BOINC_Xbox_V10
#include "AppV10.cpp"
#undef BOINC_Xbox

using namespace BOINC_Xbox;
using namespace Platform;
using namespace concurrency;
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
    struct V12Lifetime
    {
        unsigned long long sessions = 0;
        unsigned long long no_work = 0;
        unsigned long long memory_guards = 0;
        unsigned long long disk_guards = 0;
        unsigned long long slow_wu_alerts = 0;
        unsigned long long peak_memory_mb = 0;
        double best_wu_seconds = 0.0;
        double worst_wu_seconds = 0.0;
        double best_e2e_wuh = 0.0;
        double best_compute_duty = 0.0;
        unsigned long long scheduler_rate_limits = 0;
        double scheduler_rate_limit_wait_s = 0.0;
        double best_scheduler_efficiency = 0.0;
    };

    struct V12Session
    {
        long long started_ms = 0;
        long long stage_started_ms = 0;
        long long cycle_started_ms = 0;
        std::wstring tracked_stage;
        unsigned long long peak_memory_mb = 0;
        int no_work = 0;
        int no_work_streak = 0;
        int hard_failures = 0;
        int consecutive_failures = 0;
        int success_delay_s = 0;
        int server_delay_hint_s = 11;
        int scheduler_safety_margin_s = 0;
        int rate_limit_events = 0;
        int successful_scheduler_streak = 0;
        double scheduler_wait_s = 0.0;
        double rate_limit_wait_s = 0.0;
        bool memory_guard_triggered = false;
        bool disk_guard_triggered = false;
        bool slow_alert_current = false;
        double validation_s = 0.0;
        double network_s = 0.0;
        double science_stage_s = 0.0;
        double other_stage_s = 0.0;
        std::vector<double> solve_times;
        std::vector<double> e2e_times;
        std::vector<std::wstring> recent;
    };

    V12Lifetime LoadV12Lifetime()
    {
        V12Lifetime s;
        s.sessions = ParseU64(GetSetting(L"V12Sessions"));
        s.no_work = ParseU64(GetSetting(L"V12NoWork"));
        s.memory_guards = ParseU64(GetSetting(L"V12MemoryGuards"));
        s.disk_guards = ParseU64(GetSetting(L"V12DiskGuards"));
        s.slow_wu_alerts = ParseU64(GetSetting(L"V12SlowWuAlerts"));
        s.peak_memory_mb = ParseU64(GetSetting(L"V12PeakMemoryMb"));
        s.best_wu_seconds = ParseDouble10(GetSetting(L"V12BestWuSeconds"));
        s.worst_wu_seconds = ParseDouble10(GetSetting(L"V12WorstWuSeconds"));
        s.best_e2e_wuh = ParseDouble10(GetSetting(L"V12BestE2EWuh"));
        s.best_compute_duty = ParseDouble10(GetSetting(L"V12BestComputeDuty"));
        s.scheduler_rate_limits = ParseU64(GetSetting(L"V13SchedulerRateLimits"));
        s.scheduler_rate_limit_wait_s = ParseDouble10(GetSetting(L"V13SchedulerRateLimitWait"));
        s.best_scheduler_efficiency = ParseDouble10(GetSetting(L"V13BestSchedulerEfficiency"));
        return s;
    }

    void SaveV12Lifetime(const V12Lifetime& s)
    {
        SetSetting(L"V12Sessions", std::to_wstring(s.sessions));
        SetSetting(L"V12NoWork", std::to_wstring(s.no_work));
        SetSetting(L"V12MemoryGuards", std::to_wstring(s.memory_guards));
        SetSetting(L"V12DiskGuards", std::to_wstring(s.disk_guards));
        SetSetting(L"V12SlowWuAlerts", std::to_wstring(s.slow_wu_alerts));
        SetSetting(L"V12PeakMemoryMb", std::to_wstring(s.peak_memory_mb));
        SetSetting(L"V12BestWuSeconds", std::to_wstring(s.best_wu_seconds));
        SetSetting(L"V12WorstWuSeconds", std::to_wstring(s.worst_wu_seconds));
        SetSetting(L"V12BestE2EWuh", std::to_wstring(s.best_e2e_wuh));
        SetSetting(L"V12BestComputeDuty", std::to_wstring(s.best_compute_duty));
        SetSetting(L"V13SchedulerRateLimits", std::to_wstring(s.scheduler_rate_limits));
        SetSetting(L"V13SchedulerRateLimitWait", std::to_wstring(s.scheduler_rate_limit_wait_s));
        SetSetting(L"V13BestSchedulerEfficiency", std::to_wstring(s.best_scheduler_efficiency));
    }

    std::wstring Fixed1V12(double v)
    {
        std::wostringstream o;
        o << std::fixed << std::setprecision(1) << v;
        return o.str();
    }

    double SumV12(const std::vector<double>& v)
    {
        return std::accumulate(v.begin(), v.end(), 0.0);
    }

    double AvgV12(const std::vector<double>& v)
    {
        return v.empty() ? 0.0 : SumV12(v) / static_cast<double>(v.size());
    }

    double MinV12(const std::vector<double>& v)
    {
        return v.empty() ? 0.0 : *std::min_element(v.begin(), v.end());
    }

    double MaxV12(const std::vector<double>& v)
    {
        return v.empty() ? 0.0 : *std::max_element(v.begin(), v.end());
    }

    double StdDevV12(const std::vector<double>& v)
    {
        if (v.size() < 2) return 0.0;
        const double avg = AvgV12(v);
        double sum = 0.0;
        for (double x : v)
        {
            const double d = x - avg;
            sum += d * d;
        }
        return std::sqrt(sum / static_cast<double>(v.size()));
    }

    double PercentileV12(const std::vector<double>& input, double p)
    {
        if (input.empty()) return 0.0;
        std::vector<double> v = input;
        std::sort(v.begin(), v.end());
        const double pos = p * static_cast<double>(v.size() - 1);
        const size_t lo = static_cast<size_t>(std::floor(pos));
        const size_t hi = static_cast<size_t>(std::ceil(pos));
        if (lo == hi) return v[lo];
        const double f = pos - static_cast<double>(lo);
        return v[lo] * (1.0 - f) + v[hi] * f;
    }

    bool ContainsV12(const std::wstring& s, const wchar_t* token)
    {
        return s.find(token) != std::wstring::npos;
    }

    enum class StageBucketV12 { Validation, Network, Science, Other };

    StageBucketV12 BucketForV12(const std::wstring& stage)
    {
        if (ContainsV12(stage, L"SCIENCE")) return StageBucketV12::Science;
        if (ContainsV12(stage, L"VALIDAT")) return StageBucketV12::Validation;
        if (ContainsV12(stage, L"DOWNLOAD") || ContainsV12(stage, L"UPLOAD") ||
            ContainsV12(stage, L"REPORT") || ContainsV12(stage, L"SCHEDUL") ||
            ContainsV12(stage, L"ACCOUNT") || ContainsV12(stage, L"PROJECT") ||
            ContainsV12(stage, L"CONNECT") || ContainsV12(stage, L"REQUEST"))
            return StageBucketV12::Network;
        return StageBucketV12::Other;
    }

    void AccumulateStageV12(V12Session& s, long long now_ms)
    {
        if (s.stage_started_ms <= 0 || s.tracked_stage.empty())
        {
            s.stage_started_ms = now_ms;
            return;
        }
        const double elapsed = (std::max)(0LL, now_ms - s.stage_started_ms) / 1000.0;
        switch (BucketForV12(s.tracked_stage))
        {
        case StageBucketV12::Validation: s.validation_s += elapsed; break;
        case StageBucketV12::Network: s.network_s += elapsed; break;
        case StageBucketV12::Science: s.science_stage_s += elapsed; break;
        default: s.other_stage_s += elapsed; break;
        }
        s.stage_started_ms = now_ms;
    }

    void TrackStageV12(V12Session& s, const std::wstring& stage, long long now_ms)
    {
        if (s.tracked_stage == stage) return;
        AccumulateStageV12(s, now_ms);
        s.tracked_stage = stage;
        s.stage_started_ms = now_ms;
    }

    int TargetFromModeV12(int selected)
    {
        switch (selected)
        {
        case 0: return 3;
        case 1: return 10;
        case 2: return 25;
        case 3: return 50;
        default: return -1;
        }
    }

    int SuccessDelayV12(int selected)
    {
        switch (selected)
        {
        case 0: return 0;
        case 1: return 3;
        default: return 8;
        }
    }

    int NoWorkDelayV12(int streak)
    {
        int delay = 15;
        const int steps = (std::min)(3, (std::max)(0, streak - 1));
        for (int i = 0; i < steps; ++i) delay *= 2;
        return (std::min)(120, delay);
    }

    int ParseWaitSecondsV13(const std::wstring& text)
    {
        const std::wstring token = L"Please wait ";
        const size_t p = text.find(token);
        if (p == std::wstring::npos) return 0;
        size_t i = p + token.size();
        size_t j = i;
        while (j < text.size() && text[j] >= L'0' && text[j] <= L'9') ++j;
        if (j == i) return 0;
        try { return std::stoi(text.substr(i, j - i)); }
        catch (...) { return 0; }
    }

    int ParseDelayTagV13(const std::wstring& text)
    {
        const std::wstring token = L"delay=";
        const size_t p = text.find(token);
        if (p == std::wstring::npos) return 0;
        size_t i = p + token.size();
        size_t j = i;
        while (j < text.size())
        {
            const wchar_t ch = text[j];
            if (!((ch >= L'0' && ch <= L'9') || ch == L'.' || ch == L'-')) break;
            ++j;
        }
        if (j == i) return 0;
        try
        {
            const double value = std::stod(text.substr(i, j - i));
            return value > 0.0 ? static_cast<int>(std::ceil(value)) : 0;
        }
        catch (...) { return 0; }
    }

    int ServerDelayHintV13(const V10CycleResult& r)
    {
        int hint = 0;
        hint = (std::max)(hint, ParseWaitSecondsV13(r.error));
        hint = (std::max)(hint, ParseDelayTagV13(r.error));
        if (r.state)
        {
            hint = (std::max)(hint, ParseWaitSecondsV13(r.state->diagnostics));
            hint = (std::max)(hint, ParseDelayTagV13(r.state->diagnostics));
        }
        return hint;
    }

    bool IsRateLimitV13(const V10CycleResult& r)
    {
        return ContainsV12(r.error, L"Last request too recent") ||
               ContainsV12(r.error, L"Please wait ");
    }

    void AddRecentV12(V12Session& s, const std::wstring& line)
    {
        s.recent.push_back(line);
        if (s.recent.size() > 12) s.recent.erase(s.recent.begin());
        std::wstring all;
        for (const auto& x : s.recent) all += x + L"\n";
        SetSetting(L"V12RecentHistory", all);
    }

    std::wstring RecentTextV12(const V12Session& s)
    {
        if (s.recent.empty())
        {
            const std::wstring saved = GetSetting(L"V12RecentHistory");
            return saved.empty() ? L"No v1.3 workunit history yet." : saved;
        }
        std::wstring out;
        for (const auto& x : s.recent) out += x + L"\n";
        return out;
    }

    int HealthScoreV12(const V12Session& s, const V10Engine& engine, unsigned long long mem_limit_mb)
    {
        int score = 100;
        if (s.hard_failures > 0) score -= (std::min)(40, s.hard_failures * 20);
        if (s.no_work > s.rate_limit_events) score -= (std::min)(10, (s.no_work - s.rate_limit_events) * 2);
        if (s.rate_limit_events >= 3) score -= 5;
        if (engine.session_attempts >= 3 && s.hard_failures > 0 && engine.session_success * 100 < engine.session_attempts * 90) score -= 15;
        if (mem_limit_mb > 0 && s.peak_memory_mb * 100 > mem_limit_mb * 80) score -= 10;
        return (std::max)(0, score);
    }

    std::wstring PerfTextV12(
        const V12Session& session,
        const V12Lifetime& lifetime,
        const V10Stats& core,
        const V10Engine& engine,
        unsigned long long mem_limit_mb)
    {
        const long long now = MonotonicMs10();
        const double wall = session.started_ms > 0 ? (now - session.started_ms) / 1000.0 : 0.0;
        const double science_sum = SumV12(session.solve_times);
        const double avg = AvgV12(session.solve_times);
        const double measured_cycle_avg = AvgV12(session.e2e_times);
        const double true_e2e_avg = engine.session_success > 0 ? wall / static_cast<double>(engine.session_success) : 0.0;
        const double science_wuh = avg > 0.0 ? 3600.0 / avg : 0.0;
        const double e2e_wuh = wall > 0.0 ? 3600.0 * static_cast<double>(engine.session_success) / wall : 0.0;
        const double duty = wall > 0.0 ? 100.0 * science_sum / wall : 0.0;
        const double overhead = engine.session_success > 0 ?
            (std::max)(0.0, wall - science_sum) / static_cast<double>(engine.session_success) : 0.0;
        const double wu_reliability = (engine.session_success + session.hard_failures) > 0 ?
            100.0 * static_cast<double>(engine.session_success) /
            static_cast<double>(engine.session_success + session.hard_failures) : 0.0;
        const int scheduler_decisions = engine.session_success + session.no_work;
        const double scheduler_efficiency = scheduler_decisions > 0 ?
            100.0 * static_cast<double>(engine.session_success) / static_cast<double>(scheduler_decisions) : 0.0;
        const int health = HealthScoreV12(session, engine, mem_limit_mb);
        const int remaining = engine.target_success > 0 ? (std::max)(0, engine.target_success - engine.session_success) : 0;
        const double eta = remaining > 0 && true_e2e_avg > 0.0 ? remaining * true_e2e_avg : 0.0;

        double validation = session.validation_s;
        double network = session.network_s;
        double science_stage = session.science_stage_s;
        double other = session.other_stage_s;
        if (session.stage_started_ms > 0 && !session.tracked_stage.empty())
        {
            const double active = (std::max)(0LL, now - session.stage_started_ms) / 1000.0;
            switch (BucketForV12(session.tracked_stage))
            {
            case StageBucketV12::Validation: validation += active; break;
            case StageBucketV12::Network: network += active; break;
            case StageBucketV12::Science: science_stage += active; break;
            default: other += active; break;
            }
        }

        std::wostringstream o;
        o << L"Session performance and health\n"
          << L"Completed=" << engine.session_success << L"  attempts=" << engine.session_attempts
          << L"  WU reliability=" << std::fixed << std::setprecision(1) << wu_reliability << L" %"
          << L"  scheduler efficiency=" << scheduler_efficiency << L" %"
          << L"  health=" << health << L"/100\n"
          << L"No-work=" << session.no_work << L"  rate-limits=" << session.rate_limit_events
          << L"  hard-failures=" << session.hard_failures << L"\n"
          << L"WU science avg/min/p50/p90/max=" << Fixed1V12(avg) << L" / " << Fixed1V12(MinV12(session.solve_times))
          << L" / " << Fixed1V12(PercentileV12(session.solve_times, 0.50))
          << L" / " << Fixed1V12(PercentileV12(session.solve_times, 0.90))
          << L" / " << Fixed1V12(MaxV12(session.solve_times)) << L" s"
          << L"  stdev=" << Fixed1V12(StdDevV12(session.solve_times)) << L" s\n"
          << L"Science throughput=" << Fixed1V12(science_wuh) << L" WU/h"
          << L"  end-to-end=" << Fixed1V12(e2e_wuh) << L" WU/h"
          << L"  projected/24h=" << Fixed1V12(e2e_wuh * 24.0) << L" WU\n"
          << L"Compute duty=" << Fixed1V12(duty) << L" %"
          << L"  overhead/WU=" << Fixed1V12(overhead) << L" s"
          << L"  true e2e avg=" << Fixed1V12(true_e2e_avg) << L" s"
          << L"  measured successful-cycle avg=" << Fixed1V12(measured_cycle_avg) << L" s"
          << L"  peak RAM=" << session.peak_memory_mb << L"/" << mem_limit_mb << L" MB";
        if (eta > 0.0) o << L"  ETA=" << Fixed1V12(eta) << L" s";

        o << L"\nStage telemetry: validation=" << Fixed1V12(validation)
          << L" s  network/protocol=" << Fixed1V12(network)
          << L" s  science-stage=" << Fixed1V12(science_stage)
          << L" s  other/wait=" << Fixed1V12(other) << L" s\n"
          << L"Scheduler pacing: server hint=" << session.server_delay_hint_s
          << L" s  policy extra=" << session.success_delay_s
          << L" s  autotune margin=" << session.scheduler_safety_margin_s
          << L" s  no-work streak=" << session.no_work_streak << L"\n"
          << L"Intentional scheduler wait=" << Fixed1V12(session.scheduler_wait_s)
          << L" s  rate-limit wait=" << Fixed1V12(session.rate_limit_wait_s)
          << L" s  session wall=" << Fixed1V12(wall) << L" s\n\n"
          << L"Lifetime v1.3 scheduler diagnostics\n"
          << L"Sessions=" << lifetime.sessions << L"  no-work=" << lifetime.no_work
          << L"  memory guards=" << lifetime.memory_guards << L"  disk guards=" << lifetime.disk_guards
          << L"  slow-WU alerts=" << lifetime.slow_wu_alerts
          << L"  rate-limits=" << lifetime.scheduler_rate_limits
          << L"  rate-limit wait=" << Fixed1V12(lifetime.scheduler_rate_limit_wait_s) << L" s\n"
          << L"Best/Worst science WU=" << Fixed1V12(lifetime.best_wu_seconds) << L" / " << Fixed1V12(lifetime.worst_wu_seconds)
          << L" s  best e2e throughput=" << Fixed1V12(lifetime.best_e2e_wuh)
          << L" WU/h  best duty=" << Fixed1V12(lifetime.best_compute_duty)
          << L" %  best scheduler efficiency=" << Fixed1V12(lifetime.best_scheduler_efficiency) << L" %\n"
          << L"All production WUs=" << core.completed << L"  uploaded+ACK=" << core.uploaded
          << L"  failed attempts=" << core.failed;
        return o.str();
    }

    std::wstring ReportV12(
        const V10CycleResult& r,
        const V10Stats& stats,
        const V10Engine& engine,
        const V12Session& session)
    {
        std::wstring out = V10Report(r, stats, engine);
        const std::wstring old_head = L"BOINC Xbox v1.0.2 production-test report";
        const size_t p = out.find(old_head);
        if (p != std::wstring::npos)
            out.replace(p, old_head.size(), L"BOINC Xbox v1.3 server-aware production-suite report");

        const double wall = session.started_ms > 0 ? (MonotonicMs10() - session.started_ms) / 1000.0 : 0.0;
        const double science = SumV12(session.solve_times);
        const double e2e_wuh = wall > 0.0 ? 3600.0 * static_cast<double>(engine.session_success) / wall : 0.0;
        const double duty = wall > 0.0 ? 100.0 * science / wall : 0.0;

        out += L"\n\nAccelerated performance snapshot";
        out += L"\nScience avg/min/max: " + Fixed1V12(AvgV12(session.solve_times)) + L" / " +
            Fixed1V12(MinV12(session.solve_times)) + L" / " + Fixed1V12(MaxV12(session.solve_times)) + L" s";
        out += L"\nEnd-to-end throughput: " + Fixed1V12(e2e_wuh) + L" WU/h; compute duty=" + Fixed1V12(duty) + L" %";
        out += L"\nPeak memory: " + std::to_wstring(session.peak_memory_mb) + L" MB";
        out += L"\nServer-aware pacing: hint=" + std::to_wstring(session.server_delay_hint_s) +
            L" s, policy extra=" + std::to_wstring(session.success_delay_s) +
            L" s, autotune margin=" + std::to_wstring(session.scheduler_safety_margin_s) +
            L" s, rate-limits=" + std::to_wstring(session.rate_limit_events);
        out += L"\nTrue end-to-end average: " +
            Fixed1V12(engine.session_success > 0 ? wall / static_cast<double>(engine.session_success) : 0.0) + L" s/WU";
        out += L"\nScheduler intentional wait: " + Fixed1V12(session.scheduler_wait_s) +
            L" s; rate-limit wait=" + Fixed1V12(session.rate_limit_wait_s) + L" s";
        out += L"\nParallel PeriodSearch remains disabled because the upstream solver uses process-global state.";
        out += L"\nPassword plaintext is never persisted.";
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
        const unsigned long long mem_limit_mb = mem_limit / 1048576ULL;
        const DiskInfo startup_disk = QueryDiskInfo();

        auto engine = std::make_shared<V10Engine>();
        auto stats = std::make_shared<V10Stats>(LoadV10Stats());
        auto lifetime = std::make_shared<V12Lifetime>(LoadV12Lifetime());
        auto session = std::make_shared<V12Session>();

        auto root = ref new Grid();
        root->Background = ref new SolidColorBrush(ColorHelper::FromArgb(255,16,24,32));
        auto scroll = ref new ScrollViewer();
        scroll->VerticalScrollMode = ScrollMode::Auto;
        scroll->VerticalScrollBarVisibility = ScrollBarVisibility::Auto;
        auto panel = ref new StackPanel();
        panel->MaxWidth = 1180;
        panel->Margin = Thickness(70,36,70,36);
        panel->Spacing = 12;

        auto title = ref new TextBlock();
        title->Text = ref new String(L"BOINC Xbox");
        title->FontSize = 48;
        title->HorizontalAlignment = HorizontalAlignment::Center;

        auto sub = ref new TextBlock();
        sub->Text = ref new String(L"v1.3 - server-aware scheduler autotuning, corrected E2E telemetry and production soak suite");
        sub->FontSize = 21;
        sub->HorizontalAlignment = HorizontalAlignment::Center;
        sub->TextWrapping = TextWrapping::Wrap;

        const bool interrupted = GetSetting(L"V12WasRunning") == L"1";
        auto capability = ref new TextBlock();
        capability->Text = PS(
            L"CPU threads=" + std::to_wstring(cpus) +
            L", RAM limit=" + std::to_wstring(mem_limit_mb) + L" MB, disk free=" +
            std::to_wstring(startup_disk.free / 1048576ULL) + L" MB\n" +
            CpuFeatureSummary() + L"\n" + GpuProbeSummary() +
            L"\nv1.3 bundle: server-delay discovery, rate-limit classification, scheduler autotuning, true E2E metrics, "
            L"WU reliability vs scheduler efficiency, phase telemetry, p50/p90 latency, health score, disk/memory/failure guards, "
            L"50-WU soak mode, persistent recent history, diagnostics export and recovery markers." +
            (interrupted ? L"\nRecovery marker detected: the validated v1.0.2 engine will reuse a persisted active workunit when possible." : L""));
        capability->FontSize = 15;
        capability->TextWrapping = TextWrapping::Wrap;

        std::wstring initial = GetSetting(L"ProjectUrl");
        if (initial.empty()) initial = L"https://asteroidsathome.net/boinc/";
        auto url = ref new TextBox();
        url->Header = ref new String(L"Project URL");
        url->Text = PS(initial);
        url->FontSize = 17;

        auto email = ref new TextBox();
        email->Header = ref new String(L"Email (needed only if no authenticator is stored)");
        email->Text = PS(GetSetting(L"AccountEmail").empty() ? L"jirkasalek@yahoo.com" : GetSetting(L"AccountEmail"));
        email->FontSize = 17;

        auto pass = ref new PasswordBox();
        pass->Header = ref new String(L"Password (never stored)");
        pass->FontSize = 17;

        auto selectors = ref new StackPanel();
        selectors->Orientation = Orientation::Horizontal;
        selectors->Spacing = 18;

        auto mode = ref new ComboBox();
        mode->Header = ref new String(L"Suite mode");
        mode->FontSize = 17;
        mode->MinWidth = 360;
        mode->Items->Append(ref new String(L"3 workunits - quick integration"));
        mode->Items->Append(ref new String(L"10 workunits - performance suite"));
        mode->Items->Append(ref new String(L"25 workunits - scheduler autotune/endurance"));
        mode->Items->Append(ref new String(L"50 workunits - long soak/health test"));
        mode->Items->Append(ref new String(L"Continuous production - until Stop"));
        mode->SelectedIndex = 1;

        auto cadence = ref new ComboBox();
        cadence->Header = ref new String(L"Scheduler policy");
        cadence->FontSize = 17;
        cadence->MinWidth = 300;
        cadence->Items->Append(ref new String(L"Auto optimal - obey server delay"));
        cadence->Items->Append(ref new String(L"Safe - server delay + 3 s"));
        cadence->Items->Append(ref new String(L"Conservative - server delay + 8 s"));
        cadence->SelectedIndex = 0;

        selectors->Children->Append(mode);
        selectors->Children->Append(cadence);

        auto row1 = ref new StackPanel();
        row1->Orientation = Orientation::Horizontal;
        row1->Spacing = 10;

        auto start = ref new Button();
        start->Content = ref new String(L"Start v1.3 mega suite");
        start->FontSize = 18;
        start->Padding = Thickness(18,12,18,12);

        auto stop = ref new Button();
        stop->Content = ref new String(L"Stop after current WU");
        stop->FontSize = 17;
        stop->Padding = Thickness(16,12,16,12);
        stop->IsEnabled = false;

        auto network = ref new Button();
        network->Content = ref new String(L"Network: ON");
        network->FontSize = 17;
        network->Padding = Thickness(16,12,16,12);

        row1->Children->Append(start);
        row1->Children->Append(stop);
        row1->Children->Append(network);

        auto row2 = ref new StackPanel();
        row2->Orientation = Orientation::Horizontal;
        row2->Spacing = 10;

        auto saveDiag = ref new Button();
        saveDiag->Content = ref new String(L"Save diagnostics snapshot");
        saveDiag->FontSize = 16;
        saveDiag->Padding = Thickness(14,10,14,10);

        auto reset = ref new Button();
        reset->Content = ref new String(L"Reset statistics");
        reset->FontSize = 16;
        reset->Padding = Thickness(14,10,14,10);

        row2->Children->Append(saveDiag);
        row2->Children->Append(reset);

        auto statusTitle = ref new TextBlock();
        statusTitle->Text = ref new String(L"Live engine status");
        statusTitle->FontSize = 27;
        auto status = ref new TextBlock();
        status->FontSize = 17;
        status->TextWrapping = TextWrapping::Wrap;
        auto statusBox = ref new Border();
        statusBox->Padding = Thickness(18);
        statusBox->Background = ref new SolidColorBrush(ColorHelper::FromArgb(255,31,46,58));
        statusBox->Child = status;

        auto perfTitle = ref new TextBlock();
        perfTitle->Text = ref new String(L"Performance, health and phase telemetry");
        perfTitle->FontSize = 25;
        auto perfText = ref new TextBlock();
        perfText->Text = ref new String(L"Start v1.2 to collect production telemetry.");
        perfText->FontSize = 15;
        perfText->TextWrapping = TextWrapping::Wrap;
        auto perfBox = ref new Border();
        perfBox->Padding = Thickness(18);
        perfBox->Background = ref new SolidColorBrush(ColorHelper::FromArgb(255,28,40,49));
        perfBox->Child = perfText;

        auto statsTitle = ref new TextBlock();
        statsTitle->Text = ref new String(L"Persistent production statistics");
        statsTitle->FontSize = 25;
        auto statsText = ref new TextBlock();
        statsText->Text = PS(StatsText(*stats));
        statsText->FontSize = 16;
        statsText->TextWrapping = TextWrapping::Wrap;
        auto statsBox = ref new Border();
        statsBox->Padding = Thickness(18);
        statsBox->Background = ref new SolidColorBrush(ColorHelper::FromArgb(255,32,42,51));
        statsBox->Child = statsText;

        auto recentTitle = ref new TextBlock();
        recentTitle->Text = ref new String(L"Recent workunits");
        recentTitle->FontSize = 24;
        auto recentText = ref new TextBlock();
        recentText->Text = PS(RecentTextV12(*session));
        recentText->FontSize = 14;
        recentText->TextWrapping = TextWrapping::Wrap;
        auto recentBox = ref new Border();
        recentBox->Padding = Thickness(16);
        recentBox->Background = ref new SolidColorBrush(ColorHelper::FromArgb(255,25,36,44));
        recentBox->Child = recentText;

        auto reportTitle = ref new TextBlock();
        reportTitle->Text = ref new String(L"Last cycle report");
        reportTitle->FontSize = 25;
        auto report = ref new TextBlock();
        report->Text = ref new String(L"No v1.3 cycle has run yet.");
        report->FontSize = 14;
        report->TextWrapping = TextWrapping::Wrap;
        auto reportBox = ref new Border();
        reportBox->Padding = Thickness(18);
        reportBox->Background = ref new SolidColorBrush(ColorHelper::FromArgb(255,32,42,51));
        reportBox->Child = report;

        auto logTitle = ref new TextBlock();
        logTitle->Text = ref new String(L"Session log");
        logTitle->FontSize = 24;
        auto log = ref new TextBlock();
        log->Text = ref new String(interrupted ? L"Recovery marker detected. Start the suite to resume/reuse validated active state when possible.\n" : L"Ready.\n");
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
            if (x.size() > 22000) x.erase(0, x.size() - 17000);
            log->Text = PS(x);
        };

        auto set_stage = [=](const std::wstring& st, const std::wstring& result_name)
        {
            const long long now = MonotonicMs10();
            const bool same_science = engine->stage == L"SCIENCE COMPUTE" && st == L"SCIENCE COMPUTE" && engine->current_result == result_name;
            const bool same_stage = engine->stage == st && engine->current_result == result_name;
            if (!same_stage) TrackStageV12(*session, st, now);
            engine->stage = st;
            engine->current_result = result_name;
            if (st == L"SCIENCE COMPUTE")
            {
                if (!same_science) engine->science_started_ms.store(now);
            }
            else engine->science_started_ms.store(0);
        };

        auto update_all = [=]()
        {
            statsText->Text = PS(StatsText(*stats));
            perfText->Text = PS(PerfTextV12(*session, *lifetime, *stats, *engine, mem_limit_mb));
            recentText->Text = PS(RecentTextV12(*session));
        };

        auto finish_session = [=](const std::wstring& reason)
        {
            const long long now = MonotonicMs10();
            AccumulateStageV12(*session, now);
            engine->running.store(false);
            engine->science_started_ms.store(0);
            engine->stage = reason;
            SetSetting(L"V12WasRunning", L"0");

            const double wall = session->started_ms > 0 ? (now - session->started_ms) / 1000.0 : 0.0;
            const double science = SumV12(session->solve_times);
            const double e2e_wuh = wall > 0.0 ? 3600.0 * static_cast<double>(engine->session_success) / wall : 0.0;
            const double duty = wall > 0.0 ? 100.0 * science / wall : 0.0;
            if (e2e_wuh > lifetime->best_e2e_wuh) lifetime->best_e2e_wuh = e2e_wuh;
            if (duty > lifetime->best_compute_duty) lifetime->best_compute_duty = duty;
            const int scheduler_decisions = engine->session_success + session->no_work;
            const double scheduler_efficiency = scheduler_decisions > 0 ?
                100.0 * static_cast<double>(engine->session_success) / static_cast<double>(scheduler_decisions) : 0.0;
            if (scheduler_efficiency > lifetime->best_scheduler_efficiency)
                lifetime->best_scheduler_efficiency = scheduler_efficiency;
            SaveV12Lifetime(*lifetime);

            start->IsEnabled = true;
            stop->IsEnabled = false;
            stop->Content = ref new String(L"Stop after current WU");
            reset->IsEnabled = true;
            mode->IsEnabled = true;
            cadence->IsEnabled = true;
            append(L"Session finished: " + reason);
            update_all();
        };

        const auto ui = task_continuation_context::use_current();

        auto timer = ref new DispatcherTimer();
        TimeSpan interval;
        interval.Duration = 10000000LL;
        timer->Interval = interval;
        timer->Tick += ref new EventHandler<Object^>([=](Object^, Object^)
        {
            unsigned long long usage = 0;
            try { usage = MemoryManager::AppMemoryUsage; } catch (Exception^) {}
            const unsigned long long usage_mb = usage / 1048576ULL;
            session->peak_memory_mb = (std::max)(session->peak_memory_mb, usage_mb);
            lifetime->peak_memory_mb = (std::max)(lifetime->peak_memory_mb, usage_mb);

            if (engine->running.load() && mem_limit > 0 && usage > (mem_limit * 90ULL) / 100ULL && !session->memory_guard_triggered)
            {
                session->memory_guard_triggered = true;
                ++lifetime->memory_guards;
                SaveV12Lifetime(*lifetime);
                engine->stop_requested.store(true);
                append(L"MEMORY GUARD: application memory exceeded 90% of the UWP limit. Current WU will finish, then the suite will stop.");
            }

            const long long now = MonotonicMs10();
            const long long science_start = engine->science_started_ms.load();
            const bool science_running = engine->stage == L"SCIENCE COMPUTE" && science_start > 0;
            const long long science_seconds = science_running ? (std::max)(0LL, (now - science_start) / 1000LL) : 0LL;
            const long long stage_seconds = session->stage_started_ms > 0 ? (std::max)(0LL, (now - session->stage_started_ms) / 1000LL) : 0LL;
            const double progress = periodsearch_progress() * 100.0;

            if (science_running && science_seconds >= 900 && !session->slow_alert_current)
            {
                session->slow_alert_current = true;
                ++lifetime->slow_wu_alerts;
                SaveV12Lifetime(*lifetime);
                append(L"SLOW WU ALERT: science compute has exceeded 15 minutes. It is allowed to continue; no result is discarded.");
            }

            std::wostringstream o;
            o << L"State: " << engine->stage
              << L"   stage time=" << stage_seconds << L" s"
              << L"   network=" << (engine->network_enabled.load() ? L"ON" : L"PAUSED")
              << L"   stop=" << (engine->stop_requested.load() ? L"requested" : L"no")
              << L"\nSession completed: " << engine->session_success;
            if (engine->target_success > 0) o << L" / " << engine->target_success;
            else o << L" / continuous";
            o << L"   attempts=" << engine->session_attempts;
            if (!engine->current_result.empty()) o << L"\nCurrent result: " << engine->current_result;
            if (science_running)
            {
                o << L"\nScience running: " << science_seconds << L" s";
                if (progress > 0.05) o << L"   progress=" << std::fixed << std::setprecision(1) << progress << L" %";
                else o << L"   progress=n/a (stable CPU path)";
                o << L"   recovery=stage-persistent";
            }
            else o << L"\nScience: idle / waiting for compute stage";
            o << L"   app memory=" << usage_mb << L" / " << mem_limit_mb << L" MB"
              << L"   peak=" << session->peak_memory_mb << L" MB";
            status->Text = PS(o.str());
            update_all();
        });
        timer->Start();

        network->Click += ref new RoutedEventHandler([=](Object^, RoutedEventArgs^)
        {
            const bool new_value = !engine->network_enabled.load();
            engine->network_enabled.store(new_value);
            network->Content = ref new String(new_value ? L"Network: ON" : L"Network: PAUSED");
            append(new_value ? L"Network enabled; pending suite can continue." : L"Network paused. Current science compute may finish; new network cycles wait safely.");
        });

        stop->Click += ref new RoutedEventHandler([=](Object^, RoutedEventArgs^)
        {
            engine->stop_requested.store(true);
            stop->Content = ref new String(L"Stopping after current WU...");
            append(L"Stop requested. Active workunit will finish safely before the suite stops.");
        });

        reset->Click += ref new RoutedEventHandler([=](Object^, RoutedEventArgs^)
        {
            if (engine->running.load()) return;
            *stats = V10Stats();
            SaveV10Stats(*stats);
            *lifetime = V12Lifetime();
            SaveV12Lifetime(*lifetime);
            SetSetting(L"V12RecentHistory", L"");
            SetSetting(L"V12SmokeValidated", L"");
            SetSetting(L"V12WasRunning", L"0");
            SetSetting(L"V13SchedulerRateLimits", L"0");
            SetSetting(L"V13SchedulerRateLimitWait", L"0");
            SetSetting(L"V13BestSchedulerEfficiency", L"0");
            SetSetting(L"V13ServerDelayHint", L"11");
            *session = V12Session();
            report->Text = ref new String(L"No v1.3 cycle has run yet.");
            update_all();
            append(L"Persistent production and v1.3 scheduler/diagnostic statistics reset.");
        });

        saveDiag->Click += ref new RoutedEventHandler([=](Object^, RoutedEventArgs^)
        {
            std::wstring snapshot = L"BOINC Xbox v1.3 diagnostics snapshot\n\n";
            snapshot += L"STATUS\n" + std::wstring(status->Text ? status->Text->Data() : L"") + L"\n\n";
            snapshot += L"PERFORMANCE\n" + std::wstring(perfText->Text ? perfText->Text->Data() : L"") + L"\n\n";
            snapshot += L"PRODUCTION STATS\n" + std::wstring(statsText->Text ? statsText->Text->Data() : L"") + L"\n\n";
            snapshot += L"LAST REPORT\n" + std::wstring(report->Text ? report->Text->Data() : L"") + L"\n\n";
            snapshot += L"RECENT WORKUNITS\n" + RecentTextV12(*session);
            SaveText(L"v13_diagnostics.txt", snapshot).then([=](task<void> t)
            {
                try
                {
                    t.get();
                    append(L"Diagnostics saved to LocalState\\v13_diagnostics.txt");
                }
                catch (Exception^ ex)
                {
                    append(L"Diagnostics save failed: " + std::wstring(ex->Message ? ex->Message->Data() : L"unknown error"));
                }
            }, ui);
        });

        start->Click += ref new RoutedEventHandler([=](Object^, RoutedEventArgs^)
        {
            if (engine->running.load()) return;
            const std::wstring project_url = NormalizeUrl(url->Text ? url->Text->Data() : L"");
            if (project_url.empty()) { append(L"ERROR: Project URL is empty."); return; }

            auto session_email = std::make_shared<std::wstring>(Lower(Trim(email->Text ? email->Text->Data() : L"")));
            auto session_password = std::make_shared<std::wstring>(pass->Password ? pass->Password->Data() : L"");
            pass->Password = ref new String(L"");

            SetSetting(L"ProjectUrl", project_url);
            if (!session_email->empty()) SetSetting(L"AccountEmail", *session_email);

            engine->target_success = TargetFromModeV12(mode->SelectedIndex);
            engine->max_attempts = engine->target_success > 0 ? engine->target_success * 3 + 10 : 1000000;
            engine->session_success = 0;
            engine->session_attempts = 0;
            engine->smoke_needed = GetSetting(L"V12SmokeValidated") != L"1";
            engine->stop_requested.store(false);
            engine->science_started_ms.store(0);
            engine->network_enabled.store(true);
            network->Content = ref new String(L"Network: ON");
            engine->running.store(true);
            engine->stage = L"STARTING";
            engine->current_result.clear();

            *session = V12Session();
            session->started_ms = MonotonicMs10();
            session->stage_started_ms = session->started_ms;
            session->tracked_stage = L"STARTING";
            session->success_delay_s = SuccessDelayV12(cadence->SelectedIndex);
            {
                const unsigned long long saved_hint = ParseU64(GetSetting(L"V13ServerDelayHint"));
                session->server_delay_hint_s = saved_hint > 0 && saved_hint < 300 ? static_cast<int>(saved_hint) : 11;
            }
            session->scheduler_safety_margin_s = 0;
            ++lifetime->sessions;
            SaveV12Lifetime(*lifetime);

            SetSetting(L"V12WasRunning", L"1");
            SetSetting(L"V12LastTarget", std::to_wstring(engine->target_success));
            SetSetting(L"V12LastSessionSuccess", L"0");
            SetSetting(L"V12LastCadenceDelay", std::to_wstring(session->success_delay_s));

            start->IsEnabled = false;
            stop->IsEnabled = true;
            stop->Content = ref new String(L"Stop after current WU");
            reset->IsEnabled = false;
            mode->IsEnabled = false;
            cadence->IsEnabled = false;
            log->Text = ref new String(L"");

            append(L"BOINC Xbox v1.3 mega suite started.");
            append(L"v1.3 bundle: server-aware scheduler autotuning + corrected E2E telemetry + production soak testing + safety guards.");
            append(L"Stable CPU science path remains single-instance for numerical safety; scheduler cadence is optimized independently.");
            append(L"Scheduler policy extra=" + std::to_wstring(session->success_delay_s) +
                L" s; initial server hint=" + std::to_wstring(session->server_delay_hint_s) +
                L" s; generic no-work uses exponential backoff.");
            append(L"CPU: " + CpuFeatureSummary());
            append(L"GPU probe: " + GpuProbeSummary());

            auto next = std::make_shared<std::function<void()>>();
            *next = [=]()
            {
                if (!engine->running.load()) return;
                if (engine->stop_requested.load()) { finish_session(L"STOPPED BY USER/GUARD"); return; }

                if (!engine->network_enabled.load())
                {
                    set_stage(L"PAUSED - NETWORK OFF", L"");
                    create_task([]() { std::this_thread::sleep_for(std::chrono::seconds(2)); })
                        .then([=]() { (*next)(); }, ui);
                    return;
                }

                if (engine->target_success > 0 && engine->session_success >= engine->target_success)
                {
                    finish_session(L"TEST TARGET COMPLETE");
                    return;
                }
                if (engine->session_attempts >= engine->max_attempts)
                {
                    finish_session(L"ATTEMPT LIMIT REACHED");
                    return;
                }

                const DiskInfo disk_now = QueryDiskInfo();
                if (disk_now.free > 0 && disk_now.free < 256ULL * 1024ULL * 1024ULL)
                {
                    session->disk_guard_triggered = true;
                    ++lifetime->disk_guards;
                    SaveV12Lifetime(*lifetime);
                    append(L"DISK GUARD: less than 256 MB free. Suite stopped before requesting another workunit.");
                    finish_session(L"DISK GUARD");
                    return;
                }

                ++engine->session_attempts;
                session->cycle_started_ms = MonotonicMs10();
                session->slow_alert_current = false;
                append(L"--- cycle attempt " + std::to_wstring(engine->session_attempts) + L" ---");
                const bool smoke = engine->smoke_needed;

                RunOneV10Cycle(project_url, session_email, session_password, cpus, mem_limit, disk_now, smoke, engine, append, set_stage, ui)
                .then([=](V10CycleResult result)
                {
                    const int learned_server_hint = ServerDelayHintV13(result);
                    if (learned_server_hint > 0)
                    {
                        session->server_delay_hint_s = learned_server_hint;
                        SetSetting(L"V13ServerDelayHint", std::to_wstring(learned_server_hint));
                    }
                    const bool rate_limited = result.no_work && IsRateLimitV13(result);

                    const long long done_ms = MonotonicMs10();
                    TrackStageV12(*session, L"POST CYCLE", done_ms);
                    const double e2e = session->cycle_started_ms > 0 ? (done_ms - session->cycle_started_ms) / 1000.0 : 0.0;

                    if (result.state && result.state->smoke.rfind(L"PASS", 0) == 0)
                    {
                        engine->smoke_needed = false;
                        SetSetting(L"V12SmokeValidated", L"1");
                    }

                    if (result.success)
                    {
                        ++engine->session_success;
                        session->consecutive_failures = 0;
                        session->no_work_streak = 0;
                        ++session->successful_scheduler_streak;
                        if (session->successful_scheduler_streak >= 5 && session->scheduler_safety_margin_s > 0)
                        {
                            --session->scheduler_safety_margin_s;
                            session->successful_scheduler_streak = 0;
                            append(L"Scheduler autotune: five successful acquisitions; safety margin reduced by 1 s.");
                        }
                        ++stats->completed;
                        ++stats->uploaded;
                        const double elapsed = result.state ? result.state->elapsed : 0.0;
                        stats->total_solve_seconds += elapsed;
                        stats->checkpoints += periodsearch_checkpoint_count();
                        if (elapsed > 0.0)
                        {
                            session->solve_times.push_back(elapsed);
                            if (lifetime->best_wu_seconds <= 0.0 || elapsed < lifetime->best_wu_seconds) lifetime->best_wu_seconds = elapsed;
                            if (elapsed > lifetime->worst_wu_seconds) lifetime->worst_wu_seconds = elapsed;
                        }
                        if (e2e > 0.0) session->e2e_times.push_back(e2e);
                        if (result.state)
                        {
                            stats->last_result = result.state->package.result_name;
                            stats->last_credit = result.state->credit;
                        }
                        SaveV10Stats(*stats);
                        SaveV12Lifetime(*lifetime);
                        SetSetting(L"V12LastSessionSuccess", std::to_wstring(engine->session_success));

                        const std::wstring h = L"ACK PASS  " + stats->last_result + L"  science=" + Fixed1V12(elapsed) +
                            L" s  e2e=" + Fixed1V12(e2e) + L" s";
                        AddRecentV12(*session, h);
                        append(h);
                    }
                    else if (result.no_work)
                    {
                        ++session->no_work;
                        ++session->no_work_streak;
                        ++lifetime->no_work;
                        session->successful_scheduler_streak = 0;
                        if (rate_limited)
                        {
                            ++session->rate_limit_events;
                            ++lifetime->scheduler_rate_limits;
                            session->scheduler_safety_margin_s = (std::min)(5, session->scheduler_safety_margin_s + 1);
                            append(L"Scheduler rate-limit detected. Server delay will be obeyed and autotune safety margin increased.");
                        }
                        else
                        {
                            append(L"Server returned no work for a non-rate-limit reason; exponential backoff will be used.");
                        }
                        SaveV12Lifetime(*lifetime);
                    }
                    else
                    {
                        ++stats->failed;
                        ++session->hard_failures;
                        ++session->consecutive_failures;
                        SaveV10Stats(*stats);
                        if (session->consecutive_failures >= 2)
                        {
                            engine->stop_requested.store(true);
                            append(L"FAILURE GUARD: two consecutive hard failures. Suite will stop after this cycle for review.");
                        }
                    }

                    update_all();
                    report->Text = PS(ReportV12(result, *stats, *engine, *session));

                    if (engine->stop_requested.load()) { finish_session(L"STOPPED AFTER CURRENT WU / GUARD"); return; }
                    if (engine->target_success > 0 && engine->session_success >= engine->target_success) { finish_session(L"TEST TARGET COMPLETE"); return; }
                    if (engine->session_attempts >= engine->max_attempts) { finish_session(L"ATTEMPT LIMIT REACHED"); return; }

                    int delay_seconds = 15;
                    if (result.success)
                    {
                        delay_seconds = (std::max)(1, session->server_delay_hint_s) +
                            session->success_delay_s + session->scheduler_safety_margin_s;
                    }
                    else if (result.no_work && rate_limited)
                    {
                        const int hinted = (std::max)(session->server_delay_hint_s, learned_server_hint);
                        delay_seconds = (std::max)(1, hinted) + session->scheduler_safety_margin_s;
                    }
                    else if (result.no_work)
                    {
                        delay_seconds = NoWorkDelayV12(session->no_work_streak);
                    }
                    else
                    {
                        delay_seconds = (std::max)(15, session->server_delay_hint_s + session->scheduler_safety_margin_s);
                    }

                    session->scheduler_wait_s += static_cast<double>(delay_seconds);
                    if (rate_limited)
                    {
                        session->rate_limit_wait_s += static_cast<double>(delay_seconds);
                        lifetime->scheduler_rate_limit_wait_s += static_cast<double>(delay_seconds);
                    }
                    const int scheduler_decisions = engine->session_success + session->no_work;
                    const double scheduler_efficiency = scheduler_decisions > 0 ?
                        100.0 * static_cast<double>(engine->session_success) / static_cast<double>(scheduler_decisions) : 0.0;
                    if (scheduler_efficiency > lifetime->best_scheduler_efficiency)
                        lifetime->best_scheduler_efficiency = scheduler_efficiency;
                    SaveV12Lifetime(*lifetime);

                    set_stage(L"SERVER-AWARE WAIT " + std::to_wstring(delay_seconds) + L" s BEFORE NEXT CYCLE", L"");
                    create_task([delay_seconds]() { std::this_thread::sleep_for(std::chrono::seconds(delay_seconds)); })
                        .then([=]() { (*next)(); }, ui);
                }, ui);
            };
            (*next)();
        });

        Application::Current->Suspending += ref new SuspendingEventHandler([=](Object^, SuspendingEventArgs^)
        {
            engine->stop_requested.store(true);
            SetSetting(L"V12WasRunning", engine->running.load() ? L"1" : L"0");
            SetSetting(L"V12LastLifecycle", L"suspended");
            SaveV12Lifetime(*lifetime);
        });

        Application::Current->Resuming += ref new EventHandler<Object^>([=](Object^, Object^)
        {
            SetSetting(L"V12LastLifecycle", L"resumed");
            append(L"Lifecycle: application resumed. Active persisted BOINC state remains protected.");
        });

        panel->Children->Append(title);
        panel->Children->Append(sub);
        panel->Children->Append(capability);
        panel->Children->Append(url);
        panel->Children->Append(email);
        panel->Children->Append(pass);
        panel->Children->Append(selectors);
        panel->Children->Append(row1);
        panel->Children->Append(row2);
        panel->Children->Append(statusTitle);
        panel->Children->Append(statusBox);
        panel->Children->Append(perfTitle);
        panel->Children->Append(perfBox);
        panel->Children->Append(statsTitle);
        panel->Children->Append(statsBox);
        panel->Children->Append(recentTitle);
        panel->Children->Append(recentBox);
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
