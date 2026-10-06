#include "pch.h"
#include "App.xaml.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <iomanip>
#include <limits>
#include <numeric>
#include <sstream>
#include <vector>

// Import the validated v1.0.2 engine and protocol helpers into this translation
// unit under a legacy namespace. The real BOINC_Xbox::App below provides the
// v1.1 UI, orchestration and performance telemetry.
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
    struct V11Lifetime
    {
        unsigned long long sessions = 0;
        unsigned long long no_work = 0;
        unsigned long long memory_guard_events = 0;
        double best_wu_seconds = 0.0;
        double worst_wu_seconds = 0.0;
        unsigned long long peak_memory_mb = 0;
    };

    struct V11Session
    {
        long long started_ms = 0;
        long long stage_started_ms = 0;
        unsigned long long peak_memory_mb = 0;
        int no_work = 0;
        int hard_failures = 0;
        int consecutive_failures = 0;
        bool memory_guard_triggered = false;
        std::vector<double> solve_times;
    };

    V11Lifetime LoadV11Lifetime()
    {
        V11Lifetime s;
        s.sessions = ParseU64(GetSetting(L"V11Sessions"));
        s.no_work = ParseU64(GetSetting(L"V11NoWork"));
        s.memory_guard_events = ParseU64(GetSetting(L"V11MemoryGuardEvents"));
        s.best_wu_seconds = ParseDouble10(GetSetting(L"V11BestWuSeconds"));
        s.worst_wu_seconds = ParseDouble10(GetSetting(L"V11WorstWuSeconds"));
        s.peak_memory_mb = ParseU64(GetSetting(L"V11PeakMemoryMb"));
        return s;
    }

    void SaveV11Lifetime(const V11Lifetime& s)
    {
        SetSetting(L"V11Sessions", std::to_wstring(s.sessions));
        SetSetting(L"V11NoWork", std::to_wstring(s.no_work));
        SetSetting(L"V11MemoryGuardEvents", std::to_wstring(s.memory_guard_events));
        SetSetting(L"V11BestWuSeconds", std::to_wstring(s.best_wu_seconds));
        SetSetting(L"V11WorstWuSeconds", std::to_wstring(s.worst_wu_seconds));
        SetSetting(L"V11PeakMemoryMb", std::to_wstring(s.peak_memory_mb));
    }

    std::wstring Fixed1(double v)
    {
        std::wostringstream o;
        o << std::fixed << std::setprecision(1) << v;
        return o.str();
    }

    double SessionAverage(const V11Session& s)
    {
        if (s.solve_times.empty()) return 0.0;
        return std::accumulate(s.solve_times.begin(), s.solve_times.end(), 0.0) /
            static_cast<double>(s.solve_times.size());
    }

    double SessionMin(const V11Session& s)
    {
        return s.solve_times.empty() ? 0.0 : *std::min_element(s.solve_times.begin(), s.solve_times.end());
    }

    double SessionMax(const V11Session& s)
    {
        return s.solve_times.empty() ? 0.0 : *std::max_element(s.solve_times.begin(), s.solve_times.end());
    }

    double SessionStdDev(const V11Session& s)
    {
        if (s.solve_times.size() < 2) return 0.0;
        const double avg = SessionAverage(s);
        double sum = 0.0;
        for (double v : s.solve_times)
        {
            const double d = v - avg;
            sum += d * d;
        }
        return std::sqrt(sum / static_cast<double>(s.solve_times.size()));
    }

    std::wstring V11PerfText(
        const V11Session& session,
        const V11Lifetime& lifetime,
        const V10Stats& core,
        const V10Engine& engine)
    {
        const long long now = MonotonicMs10();
        const double wall = session.started_ms > 0 ? (now - session.started_ms) / 1000.0 : 0.0;
        const double avg = SessionAverage(session);
        const double minv = SessionMin(session);
        const double maxv = SessionMax(session);
        const double sd = SessionStdDev(session);
        const double science_sum = std::accumulate(session.solve_times.begin(), session.solve_times.end(), 0.0);
        const double science_wuh = avg > 0.0 ? 3600.0 / avg : 0.0;
        const double e2e_wuh = wall > 0.0 ? 3600.0 * static_cast<double>(engine.session_success) / wall : 0.0;
        const double duty = wall > 0.0 ? 100.0 * science_sum / wall : 0.0;
        const int remaining = engine.target_success > 0 ? (std::max)(0, engine.target_success - engine.session_success) : 0;
        const double eta = remaining > 0 && avg > 0.0 ? remaining * (avg + 12.0) : 0.0;
        const double success_rate = engine.session_attempts > 0 ?
            100.0 * static_cast<double>(engine.session_success) / static_cast<double>(engine.session_attempts) : 0.0;

        std::wostringstream o;
        o << L"Session performance\n"
          << L"Completed=" << engine.session_success << L"  attempts=" << engine.session_attempts
          << L"  success=" << std::fixed << std::setprecision(1) << success_rate << L" %"
          << L"  no-work=" << session.no_work << L"  hard-failures=" << session.hard_failures << L"\n"
          << L"WU time avg/min/max=" << Fixed1(avg) << L" / " << Fixed1(minv) << L" / " << Fixed1(maxv)
          << L" s  stdev=" << Fixed1(sd) << L" s\n"
          << L"Science throughput=" << Fixed1(science_wuh) << L" WU/h  end-to-end=" << Fixed1(e2e_wuh)
          << L" WU/h  compute duty=" << Fixed1(duty) << L" %\n"
          << L"Session wall=" << Fixed1(wall) << L" s  peak memory=" << session.peak_memory_mb << L" MB";
        if (eta > 0.0) o << L"  ETA=" << Fixed1(eta) << L" s";

        o << L"\n\nLifetime v1.1 diagnostics\n"
          << L"Sessions=" << lifetime.sessions << L"  no-work replies=" << lifetime.no_work
          << L"  memory guards=" << lifetime.memory_guard_events << L"\n"
          << L"Best/Worst WU=" << Fixed1(lifetime.best_wu_seconds) << L" / " << Fixed1(lifetime.worst_wu_seconds)
          << L" s  peak memory=" << lifetime.peak_memory_mb << L" MB\n"
          << L"All production WUs=" << core.completed << L"  uploaded+ACK=" << core.uploaded
          << L"  failed attempts=" << core.failed;
        return o.str();
    }

    std::wstring V11Report(const V10CycleResult& r, const V10Stats& stats, const V10Engine& engine, const V11Session& session)
    {
        std::wstring out = V10Report(r, stats, engine);
        const std::wstring old_head = L"BOINC Xbox v1.0.2 production-test report";
        const size_t p = out.find(old_head);
        if (p != std::wstring::npos)
            out.replace(p, old_head.size(), L"BOINC Xbox v1.1 performance-suite report");

        out += L"\n\nPerformance snapshot";
        out += L"\nSession WU avg/min/max: " + Fixed1(SessionAverage(session)) + L" / " + Fixed1(SessionMin(session)) + L" / " + Fixed1(SessionMax(session)) + L" s";
        out += L"\nSession peak memory: " + std::to_wstring(session.peak_memory_mb) + L" MB";
        out += L"\nHard failures: " + std::to_wstring(session.hard_failures) + L", no-work replies: " + std::to_wstring(session.no_work);
        out += L"\nParallel PeriodSearch: intentionally disabled; upstream solver uses process-global state.";
        return out;
    }

    int TargetFromMode(int selected)
    {
        switch (selected)
        {
        case 0: return 3;
        case 1: return 5;
        case 2: return 10;
        case 3: return 20;
        default: return -1;
        }
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
        const unsigned long long mem_mb = mem_limit / 1048576ULL;

        auto engine = std::make_shared<V10Engine>();
        auto stats = std::make_shared<V10Stats>(LoadV10Stats());
        auto lifetime = std::make_shared<V11Lifetime>(LoadV11Lifetime());
        auto session = std::make_shared<V11Session>();

        auto root = ref new Grid();
        root->Background = ref new SolidColorBrush(ColorHelper::FromArgb(255,16,24,32));
        auto scroll = ref new ScrollViewer();
        scroll->VerticalScrollMode = ScrollMode::Auto;
        scroll->VerticalScrollBarVisibility = ScrollBarVisibility::Auto;
        auto panel = ref new StackPanel();
        panel->MaxWidth = 1160;
        panel->Margin = Thickness(80,40,80,40);
        panel->Spacing = 12;

        auto title = ref new TextBlock();
        title->Text = ref new String(L"BOINC Xbox");
        title->FontSize = 48;
        title->HorizontalAlignment = HorizontalAlignment::Center;

        auto sub = ref new TextBlock();
        sub->Text = ref new String(L"v1.1 - performance, endurance and production validation suite");
        sub->FontSize = 23;
        sub->HorizontalAlignment = HorizontalAlignment::Center;

        const unsigned int memory_slots = mem_mb ? static_cast<unsigned int>(mem_mb / 220ULL) : 1U;
        const unsigned int hardware_slots = (std::min)(cpus, (std::max)(1U, memory_slots));
        auto capability = ref new TextBlock();
        capability->Text = PS(
            L"CPU threads=" + std::to_wstring(cpus) +
            L", RAM limit=" + std::to_wstring(mem_mb) + L" MB, disk free=" + std::to_wstring(disk.free/1048576ULL) + L" MB\n" +
            CpuFeatureSummary() + L"\n" + GpuProbeSummary() +
            L"\nEstimated hardware task slots=" + std::to_wstring(hardware_slots) +
            L". v1.1 keeps PeriodSearch concurrency at 1 to preserve numerical correctness; throughput, memory and reliability are measured automatically.");
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
        email->Text = PS(GetSetting(L"AccountEmail").empty() ? L"jirkasalek@yahoo.com" : GetSetting(L"AccountEmail"));
        email->FontSize = 18;

        auto pass = ref new PasswordBox();
        pass->Header = ref new String(L"Password (never stored)");
        pass->FontSize = 18;

        auto mode = ref new ComboBox();
        mode->Header = ref new String(L"Test mode");
        mode->FontSize = 18;
        mode->Items->Append(ref new String(L"3 workunits - integration test"));
        mode->Items->Append(ref new String(L"5 workunits - stress test"));
        mode->Items->Append(ref new String(L"10 workunits - performance/endurance suite"));
        mode->Items->Append(ref new String(L"20 workunits - long endurance suite"));
        mode->Items->Append(ref new String(L"Continuous - run until Stop"));
        mode->SelectedIndex = 2;

        auto buttons = ref new StackPanel();
        buttons->Orientation = Orientation::Horizontal;
        buttons->Spacing = 10;

        auto start = ref new Button();
        start->Content = ref new String(L"Start v1.1 suite");
        start->FontSize = 19;
        start->Padding = Thickness(20,12,20,12);

        auto stop = ref new Button();
        stop->Content = ref new String(L"Stop after current WU");
        stop->FontSize = 17;
        stop->Padding = Thickness(16,12,16,12);
        stop->IsEnabled = false;

        auto network = ref new Button();
        network->Content = ref new String(L"Network: ON");
        network->FontSize = 17;
        network->Padding = Thickness(16,12,16,12);

        auto reset = ref new Button();
        reset->Content = ref new String(L"Reset statistics");
        reset->FontSize = 17;
        reset->Padding = Thickness(16,12,16,12);

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

        auto perfTitle = ref new TextBlock();
        perfTitle->Text = ref new String(L"Performance and health");
        perfTitle->FontSize = 25;
        auto perfText = ref new TextBlock();
        perfText->Text = ref new String(L"Start a suite to collect throughput and reliability data.");
        perfText->FontSize = 16;
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
        report->Text = ref new String(L"No v1.1 cycle has run yet.");
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
        log->Text = ref new String(L"Ready. Pending v1.0.x work is resumed automatically.\n");
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
            if (x.size() > 18000) x.erase(0, x.size() - 14000);
            log->Text = PS(x);
        };

        auto set_stage = [=](const std::wstring& st, const std::wstring& result_name)
        {
            const bool same_science = engine->stage == L"SCIENCE COMPUTE" && st == L"SCIENCE COMPUTE" &&
                engine->current_result == result_name;
            const bool same_stage = engine->stage == st && engine->current_result == result_name;
            engine->stage = st;
            engine->current_result = result_name;
            if (!same_stage) session->stage_started_ms = MonotonicMs10();
            if (st == L"SCIENCE COMPUTE")
            {
                if (!same_science) engine->science_started_ms.store(MonotonicMs10());
            }
            else engine->science_started_ms.store(0);
        };

        auto update_all_stats = [=]()
        {
            statsText->Text = PS(StatsText(*stats));
            perfText->Text = PS(V11PerfText(*session, *lifetime, *stats, *engine));
        };

        auto finish_session = [=](const std::wstring& reason)
        {
            engine->running.store(false);
            engine->science_started_ms.store(0);
            engine->stage = reason;
            start->IsEnabled = true;
            stop->IsEnabled = false;
            reset->IsEnabled = true;
            mode->IsEnabled = true;
            append(L"Session finished: " + reason);
            update_all_stats();
        };

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

            if (engine->running.load() && mem_limit > 0 && usage > (mem_limit * 88ULL) / 100ULL && !session->memory_guard_triggered)
            {
                session->memory_guard_triggered = true;
                ++lifetime->memory_guard_events;
                SaveV11Lifetime(*lifetime);
                engine->stop_requested.store(true);
                append(L"MEMORY GUARD: app memory exceeded 88% of the UWP limit. The current WU will finish and the suite will stop.");
            }

            const long long now = MonotonicMs10();
            const long long science_start = engine->science_started_ms.load();
            const bool science_running = engine->stage == L"SCIENCE COMPUTE" && science_start > 0;
            const long long science_seconds = science_running ? (std::max)(0LL, (now - science_start) / 1000LL) : 0LL;
            const long long stage_seconds = session->stage_started_ms > 0 ? (std::max)(0LL, (now - session->stage_started_ms) / 1000LL) : 0LL;
            const double progress = periodsearch_progress() * 100.0;

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
            o << L"   app memory=" << usage_mb << L" / " << mem_mb << L" MB"
              << L"   peak=" << session->peak_memory_mb << L" MB";
            status->Text = PS(o.str());

            update_all_stats();
        });
        timer->Start();

        network->Click += ref new RoutedEventHandler([=](Object^, RoutedEventArgs^)
        {
            const bool new_value = !engine->network_enabled.load();
            engine->network_enabled.store(new_value);
            network->Content = ref new String(new_value ? L"Network: ON" : L"Network: PAUSED");
            append(new_value ? L"Network enabled." : L"Network pause requested. Current science task may finish safely.");
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
            *lifetime = V11Lifetime();
            SaveV11Lifetime(*lifetime);
            update_all_stats();
            append(L"Persistent production and v1.1 diagnostic statistics reset.");
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

            engine->target_success = TargetFromMode(mode->SelectedIndex);
            engine->max_attempts = engine->target_success > 0 ? engine->target_success + (std::max)(4, engine->target_success / 2) : 1000000;
            engine->session_success = 0;
            engine->session_attempts = 0;
            engine->smoke_needed = true;
            engine->stop_requested.store(false);
            engine->science_started_ms.store(0);
            engine->running.store(true);
            engine->stage = L"STARTING";
            engine->current_result.clear();

            *session = V11Session();
            session->started_ms = MonotonicMs10();
            session->stage_started_ms = session->started_ms;
            ++lifetime->sessions;
            SaveV11Lifetime(*lifetime);

            start->IsEnabled = false;
            stop->IsEnabled = true;
            stop->Content = ref new String(L"Stop after current WU");
            reset->IsEnabled = false;
            mode->IsEnabled = false;
            log->Text = ref new String(L"");

            append(L"BOINC Xbox v1.1 suite started.");
            append(L"Plan: integration + production cycles + throughput telemetry + memory guard + failure guard.");
            append(L"Stable CPU science path active; PeriodSearch concurrency remains 1 for numerical safety.");
            append(L"CPU: " + CpuFeatureSummary());
            append(L"GPU probe: " + GpuProbeSummary());

            const auto ui = task_continuation_context::use_current();
            auto next = std::make_shared<std::function<void()>>();
            *next = [=]()
            {
                if (!engine->running.load()) return;
                if (engine->stop_requested.load()) { finish_session(L"STOPPED BY USER/GUARD"); return; }
                if (!engine->network_enabled.load()) { finish_session(L"PAUSED - NETWORK OFF"); return; }
                if (engine->target_success > 0 && engine->session_success >= engine->target_success) { finish_session(L"TEST TARGET COMPLETE"); return; }
                if (engine->session_attempts >= engine->max_attempts) { finish_session(L"ATTEMPT LIMIT REACHED"); return; }

                ++engine->session_attempts;
                append(L"--- cycle attempt " + std::to_wstring(engine->session_attempts) + L" ---");
                const bool smoke = engine->smoke_needed;

                RunOneV10Cycle(project_url, session_email, session_password, cpus, mem_limit, disk, smoke, engine, append, set_stage, ui)
                .then([=](V10CycleResult result)
                {
                    if (result.state && result.state->smoke.rfind(L"PASS",0) == 0) engine->smoke_needed = false;

                    if (result.success)
                    {
                        engine->session_success++;
                        session->consecutive_failures = 0;
                        stats->completed++;
                        stats->uploaded++;
                        const double elapsed = result.state ? result.state->elapsed : 0.0;
                        stats->total_solve_seconds += elapsed;
                        stats->checkpoints += periodsearch_checkpoint_count();
                        if (elapsed > 0.0)
                        {
                            session->solve_times.push_back(elapsed);
                            if (lifetime->best_wu_seconds <= 0.0 || elapsed < lifetime->best_wu_seconds) lifetime->best_wu_seconds = elapsed;
                            if (elapsed > lifetime->worst_wu_seconds) lifetime->worst_wu_seconds = elapsed;
                        }
                        if (result.state)
                        {
                            stats->last_result = result.state->package.result_name;
                            stats->last_credit = result.state->credit;
                        }
                        SaveV10Stats(*stats);
                        SaveV11Lifetime(*lifetime);
                        append(L"ACK PASS: " + stats->last_result + L"  science=" + Fixed1(elapsed) + L" s");
                    }
                    else if (result.no_work)
                    {
                        ++session->no_work;
                        ++lifetime->no_work;
                        SaveV11Lifetime(*lifetime);
                        append(L"Server returned no work; not counted as a hard failure.");
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
                            append(L"FAILURE GUARD: two consecutive hard failures. The suite will stop for review.");
                        }
                    }

                    update_all_stats();
                    report->Text = PS(V11Report(result, *stats, *engine, *session));

                    if (engine->stop_requested.load()) { finish_session(L"STOPPED AFTER CURRENT WU / GUARD"); return; }
                    if (!engine->network_enabled.load()) { finish_session(L"PAUSED - NETWORK OFF"); return; }
                    if (engine->target_success > 0 && engine->session_success >= engine->target_success) { finish_session(L"TEST TARGET COMPLETE"); return; }
                    if (engine->session_attempts >= engine->max_attempts) { finish_session(L"ATTEMPT LIMIT REACHED"); return; }

                    const int delay_seconds = result.no_work ? 60 : (result.success ? 12 : 15);
                    engine->stage = L"WAITING " + std::to_wstring(delay_seconds) + L" s BEFORE NEXT CYCLE";
                    engine->current_result.clear();
                    engine->science_started_ms.store(0);
                    session->stage_started_ms = MonotonicMs10();
                    create_task([delay_seconds]() { std::this_thread::sleep_for(std::chrono::seconds(delay_seconds)); })
                        .then([=]() { (*next)(); }, ui);
                }, ui);
            };
            (*next)();
        });

        Application::Current->Suspending += ref new SuspendingEventHandler([=](Object^, SuspendingEventArgs^)
        {
            engine->stop_requested.store(true);
            SetSetting(L"V11LastLifecycle", L"suspended");
            SaveV11Lifetime(*lifetime);
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
        panel->Children->Append(perfTitle);
        panel->Children->Append(perfBox);
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
