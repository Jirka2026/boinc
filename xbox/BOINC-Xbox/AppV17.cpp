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
#include <d3d11_1.h>
#include <dxgi1_2.h>
#include <wrl.h>

// Reuse the validated v1.0.2 protocol and science engine. v1.7 adds an optimization laboratory,
// deterministic multi-thread verification, CPU scaling, memory bandwidth and verified D3D11 compute
// benchmarking while retaining the numerically safe single-instance production PeriodSearch path.
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
using namespace Microsoft::WRL;
using namespace Windows::Security::Cryptography;
using namespace Windows::Storage::Streams;
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
        unsigned long long v14_preflight_runs = 0;
        unsigned long long v14_preflight_failures = 0;
        unsigned long long v14_pacing_violations = 0;
        unsigned long long v14_clean_sessions = 0;
        unsigned long long v14_verified_wus = 0;
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
        long long finished_ms = 0;
        int preflight_passes = 0;
        int preflight_warnings = 0;
        int preflight_failures = 0;
        std::wstring preflight_text;
        int pacing_violations = 0;
        long long wait_started_ms = 0;
        int planned_wait_s = 0;
        std::vector<double> actual_wait_times;
        std::vector<unsigned long long> memory_after_wu_mb;
        unsigned long long download_passes = 0;
        unsigned long long solve_passes = 0;
        unsigned long long upload_passes = 0;
        unsigned long long report_passes = 0;
        unsigned long long ack_passes = 0;
        unsigned long long duplicate_results = 0;
        std::vector<std::wstring> seen_results;
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
        s.v14_preflight_runs = ParseU64(GetSetting(L"V14PreflightRuns"));
        s.v14_preflight_failures = ParseU64(GetSetting(L"V14PreflightFailures"));
        s.v14_pacing_violations = ParseU64(GetSetting(L"V14PacingViolations"));
        s.v14_clean_sessions = ParseU64(GetSetting(L"V14CleanSessions"));
        s.v14_verified_wus = ParseU64(GetSetting(L"V14VerifiedWUs"));
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
        SetSetting(L"V14PreflightRuns", std::to_wstring(s.v14_preflight_runs));
        SetSetting(L"V14PreflightFailures", std::to_wstring(s.v14_preflight_failures));
        SetSetting(L"V14PacingViolations", std::to_wstring(s.v14_pacing_violations));
        SetSetting(L"V14CleanSessions", std::to_wstring(s.v14_clean_sessions));
        SetSetting(L"V14VerifiedWUs", std::to_wstring(s.v14_verified_wus));
    }

    std::wstring Fixed1V12(double v)
    {
        std::wostringstream o;
        o << std::fixed << std::setprecision(1) << v;
        return o.str();
    }

    std::wstring FormatDurationV17(double seconds)
    {
        if (!(seconds > 0.0) || !std::isfinite(seconds)) return L"n/a";
        const long long total = static_cast<long long>(std::ceil(seconds));
        const long long hours = total / 3600;
        const long long minutes = (total % 3600) / 60;
        const long long secs = total % 60;
        std::wostringstream o;
        if (hours > 0) o << hours << L"h ";
        if (hours > 0 || minutes > 0) o << minutes << L"m ";
        o << secs << L"s";
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


    bool IsAbsoluteHttpsUrlV16(const std::wstring& value)
    {
        if (value.rfind(L"https://", 0) != 0) return false;
        try
        {
            auto uri = ref new Windows::Foundation::Uri(PS(value));
            return uri != nullptr && uri->Host != nullptr && uri->Host->Length() > 0;
        }
        catch (Platform::Exception^)
        {
            return false;
        }
    }

    std::wstring NormalizeProjectUrlV16(const std::wstring& raw)
    {
        std::wstring value = Trim(raw);
        if (value.empty()) return L"";

        // Repair the exact persisted regression seen in v1.5.
        if (value.rfind(L"ttps://", 0) == 0)
            value = L"h" + value;
        else if (value.rfind(L"//", 0) == 0)
            value = L"https:" + value;
        else if (value.find(L"://") == std::wstring::npos)
            value = L"https://" + value;

        // BOINC project traffic is forced onto HTTPS.
        if (value.rfind(L"http://", 0) == 0)
            value.replace(0, 7, L"https://");

        while (value.size() > 8 && value.back() == L'/')
            value.pop_back();
        value += L"/";

        return IsAbsoluteHttpsUrlV16(value) ? value : L"";
    }

    std::wstring BuildModeV15()
    {
#ifdef NDEBUG
        return L"Release /O2 build; AVX2 code generation enabled";
#else
        return L"Debug build; functional validation is valid, but use Release x64 for final throughput measurements";
#endif
    }

    unsigned long long MixV15(unsigned long long x)
    {
        x ^= x >> 33;
        x *= 0xff51afd7ed558ccdULL;
        x ^= x >> 33;
        x *= 0xc4ceb9fe1a85ec53ULL;
        x ^= x >> 33;
        return x;
    }

    unsigned long long ChecksumRangeV15(unsigned int lane, unsigned int lanes, unsigned int items)
    {
        unsigned long long sum = 0;
        for (unsigned int i = lane; i < items; i += lanes)
            sum += MixV15(static_cast<unsigned long long>(i) + 0x9e3779b97f4a7c15ULL);
        return sum;
    }

    std::wstring ThreadCorrectnessV15(unsigned int visible_cpus)
    {
        const unsigned int lanes = (std::max)(1u, (std::min)(visible_cpus, 8u));
        const unsigned int items = 1500000u;
        const unsigned long long serial = ChecksumRangeV15(0, 1, items);

        std::vector<unsigned long long> partial(lanes, 0);
        std::vector<std::thread> workers;
        for (unsigned int i = 0; i < lanes; ++i)
            workers.emplace_back([i, lanes, items, &partial]() { partial[i] = ChecksumRangeV15(i, lanes, items); });
        for (auto& w : workers) w.join();

        unsigned long long parallel = 0;
        for (auto v : partial) parallel += v;
        return std::wstring(serial == parallel ? L"PASS" : L"FAIL") +
            L": deterministic integer checksum, 1T=" + std::to_wstring(serial) +
            L", " + std::to_wstring(lanes) + L"T=" + std::to_wstring(parallel);
    }

    std::wstring CpuScaleBenchmarkV15(unsigned int visible_cpus)
    {
        std::vector<unsigned int> candidates = { 1, 2, 4, 6, 8 };
        std::vector<unsigned int> tests;
        for (auto n : candidates) if (n <= (std::max)(1u, visible_cpus)) tests.push_back(n);
        if (tests.empty()) tests.push_back(1);

        const unsigned int loops = 7000000u;
        double base = 0.0;
        double best = 0.0;
        unsigned int best_threads = 1;
        std::wstring out = L"CPU worker scaling: ";

        for (size_t ti = 0; ti < tests.size(); ++ti)
        {
            const unsigned int n = tests[ti];
            std::vector<unsigned long long> result(n, 0);
            std::vector<std::thread> workers;
            const auto started = std::chrono::steady_clock::now();

            for (unsigned int i = 0; i < n; ++i)
            {
                workers.emplace_back([i, loops, &result]()
                {
                    unsigned long long x = 0x9e3779b97f4a7c15ULL ^ static_cast<unsigned long long>(i + 1);
                    for (unsigned int k = 0; k < loops; ++k)
                    {
                        x ^= x << 13;
                        x ^= x >> 7;
                        x ^= x << 17;
                        x += static_cast<unsigned long long>(k) + 0x632be59bd9b4e019ULL;
                    }
                    result[i] = x;
                });
            }
            for (auto& w : workers) w.join();

            const double seconds = (std::max)(
                0.001,
                std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count());
            const double miter = static_cast<double>(n) * static_cast<double>(loops) / seconds / 1.0e6;
            if (ti == 0) base = miter;
            if (miter > best) { best = miter; best_threads = n; }

            if (ti) out += L", ";
            out += std::to_wstring(n) + L"T=" + Fixed1V12(miter) + L" Miter/s";
        }

        const double speedup = base > 0.0 ? best / base : 0.0;
        out += L"; best=" + std::to_wstring(best_threads) + L"T, speedup=" + Fixed1V12(speedup) + L"x";
        return out;
    }

    std::wstring MemoryBandwidthBenchmarkV15()
    {
        const size_t words = 2u * 1024u * 1024u;
        std::vector<unsigned long long> data(words, 0);
        volatile unsigned long long sink = 0;
        const int passes = 4;
        const auto started = std::chrono::steady_clock::now();

        for (int p = 0; p < passes; ++p)
        {
            for (size_t i = 0; i < data.size(); ++i)
                data[i] = static_cast<unsigned long long>(i) * 0x9e3779b97f4a7c15ULL + static_cast<unsigned long long>(p);
            unsigned long long local = 0;
            for (size_t i = 0; i < data.size(); ++i) local += data[i];
            sink ^= local;
        }

        const double seconds = (std::max)(
            0.001,
            std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count());
        const double bytes = static_cast<double>(data.size() * sizeof(unsigned long long)) * 2.0 * passes;
        const double gbps = bytes / seconds / 1.0e9;
        return L"Memory bandwidth microtest: PASS, approx=" + Fixed1V12(gbps) +
            L" GB/s, working-set=16 MB, checksum=" + std::to_wstring(static_cast<unsigned long long>(sink));
    }

    task<std::wstring> GpuComputeBenchmarkV15()
    {
        return create_task(Package::Current->InstalledLocation->GetFileAsync(ref new String(L"ComputeBench.cso"))).then([](StorageFile^ f)
        {
            return create_task(FileIO::ReadBufferAsync(f));
        }).then([](IBuffer^ shaderBuffer) -> std::wstring
        {
            Array<byte>^ shaderBytes;
            CryptographicBuffer::CopyToByteArray(shaderBuffer, &shaderBytes);

            D3D_FEATURE_LEVEL levels[] = {
                D3D_FEATURE_LEVEL_12_1, D3D_FEATURE_LEVEL_12_0,
                D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0
            };
            D3D_FEATURE_LEVEL got = D3D_FEATURE_LEVEL_11_0;
            ComPtr<ID3D11Device> dev;
            ComPtr<ID3D11DeviceContext> ctx;
            HRESULT hr = D3D11CreateDevice(
                nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0,
                levels, ARRAYSIZE(levels), D3D11_SDK_VERSION, &dev, &got, &ctx);
            if (FAILED(hr)) return L"FAIL: D3D11CreateDevice HRESULT=" + std::to_wstring(static_cast<long long>(hr));

            ComPtr<ID3D11ComputeShader> shader;
            hr = dev->CreateComputeShader(shaderBytes->Data, shaderBytes->Length, nullptr, &shader);
            if (FAILED(hr)) return L"FAIL: CreateComputeShader HRESULT=" + std::to_wstring(static_cast<long long>(hr));

            const UINT elements = 262144;
            const UINT groups = elements / 256;
            const UINT repeats = 16;

            D3D11_BUFFER_DESC bd = {};
            bd.ByteWidth = elements * sizeof(UINT);
            bd.Usage = D3D11_USAGE_DEFAULT;
            bd.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
            bd.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
            bd.StructureByteStride = sizeof(UINT);

            ComPtr<ID3D11Buffer> buffer;
            hr = dev->CreateBuffer(&bd, nullptr, &buffer);
            if (FAILED(hr)) return L"FAIL: GPU buffer HRESULT=" + std::to_wstring(static_cast<long long>(hr));

            D3D11_UNORDERED_ACCESS_VIEW_DESC ud = {};
            ud.Format = DXGI_FORMAT_UNKNOWN;
            ud.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
            ud.Buffer.NumElements = elements;

            ComPtr<ID3D11UnorderedAccessView> uav;
            hr = dev->CreateUnorderedAccessView(buffer.Get(), &ud, &uav);
            if (FAILED(hr)) return L"FAIL: GPU UAV HRESULT=" + std::to_wstring(static_cast<long long>(hr));

            D3D11_BUFFER_DESC sd = bd;
            sd.Usage = D3D11_USAGE_STAGING;
            sd.BindFlags = 0;
            sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;

            ComPtr<ID3D11Buffer> staging;
            hr = dev->CreateBuffer(&sd, nullptr, &staging);
            if (FAILED(hr)) return L"FAIL: staging buffer HRESULT=" + std::to_wstring(static_cast<long long>(hr));

            ID3D11UnorderedAccessView* views[] = { uav.Get() };
            ctx->CSSetShader(shader.Get(), nullptr, 0);
            ctx->CSSetUnorderedAccessViews(0, 1, views, nullptr);

            const auto started = std::chrono::steady_clock::now();
            for (UINT i = 0; i < repeats; ++i) ctx->Dispatch(groups, 1, 1);
            ctx->CopyResource(staging.Get(), buffer.Get());

            D3D11_MAPPED_SUBRESOURCE mapped = {};
            hr = ctx->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped);
            const auto finished = std::chrono::steady_clock::now();
            if (FAILED(hr)) return L"FAIL: staging Map HRESULT=" + std::to_wstring(static_cast<long long>(hr));

            const UINT value = static_cast<UINT*>(mapped.pData)[0];
            ctx->Unmap(staging.Get(), 0);

            UINT expected = 1;
            for (UINT i = 0; i < 256; ++i) expected = expected * 1664525u + 1013904223u;
            if (value != expected) return L"FAIL: compute verification mismatch";

            const double seconds = (std::max)(
                0.001,
                std::chrono::duration<double>(finished - started).count());
            const double updates = static_cast<double>(elements) * 256.0 * repeats;
            const double gups = updates / seconds / 1.0e9;

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

            const unsigned int major = (static_cast<unsigned int>(got) >> 12) & 0xF;
            const unsigned int minor = (static_cast<unsigned int>(got) >> 8) & 0xF;
            return L"GPU compute microtest: PASS, shader verified, " + Fixed1V12(gups) +
                L" Gupdates/s, FL " + std::to_wstring(major) + L"." + std::to_wstring(minor) +
                L", " + adapter;
        });
    }

    std::wstring BackendPolicyV15()
    {
        const std::wstring cpu = CpuFeatureSummary();
        const bool avx2 = cpu.find(L"AVX2=yes") != std::wstring::npos;
        const bool fma = cpu.find(L"FMA=yes") != std::wstring::npos;
        return L"Science backend policy: validated upstream PeriodSearch remains single-instance CPU for numerical safety. "
            L"Release builds are compiled for AVX2; candidate CPU features: AVX2=" +
            std::wstring(avx2 ? L"yes" : L"no") + L", FMA=" + std::wstring(fma ? L"yes" : L"no") +
            L". GPU compute is benchmarked/verified separately and is not allowed to submit science results until numerical parity is proven.";
    }

    int TargetFromModeV12(int selected)
    {
        switch (selected)
        {
        case 0: return 1;
        case 1: return 3;
        case 2: return 10;
        case 3: return 25;
        case 4: return 50;
        case 5: return 100;
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
            return saved.empty() ? L"No v1.7 workunit history yet." : saved;
        }
        std::wstring out;
        for (const auto& x : s.recent) out += x + L"\n";
        return out;
    }

    struct V14Preflight
    {
        int passed = 0;
        int warnings = 0;
        int failures = 0;
        std::wstring text;
    };

    void AddPreflightV14(
        V14Preflight& r,
        const std::wstring& name,
        bool ok,
        bool hard_failure,
        const std::wstring& detail)
    {
        if (ok)
        {
            ++r.passed;
            r.text += L"PASS  " + name;
        }
        else if (hard_failure)
        {
            ++r.failures;
            r.text += L"FAIL  " + name;
        }
        else
        {
            ++r.warnings;
            r.text += L"WARN  " + name;
        }
        if (!detail.empty()) r.text += L": " + detail;
        r.text += L"\n";
    }

    V14Preflight RunPreflightV14(
        unsigned int cpus,
        unsigned long long mem_limit_mb,
        unsigned long long app_memory_mb,
        const DiskInfo& disk,
        const std::wstring& project_url,
        const V10Stats& stats)
    {
        V14Preflight r;
        const std::wstring cpu = CpuFeatureSummary();
        const std::wstring gpu = GpuProbeSummary();
        const unsigned long long disk_mb = disk.free / 1048576ULL;

        AddPreflightV14(r, L"CPU visibility", cpus >= 4, cpus == 0,
            L"threads=" + std::to_wstring(cpus));
        AddPreflightV14(r, L"x64 SSE2 path", cpu.find(L"SSE2=yes") != std::wstring::npos, true, cpu);
        AddPreflightV14(r, L"AVX2 optimization capability", cpu.find(L"AVX2=yes") != std::wstring::npos, false, cpu);
        AddPreflightV14(r, L"FMA optimization capability", cpu.find(L"FMA=yes") != std::wstring::npos, false, cpu);
        AddPreflightV14(r, L"UWP memory budget", mem_limit_mb >= 768, mem_limit_mb > 0 && mem_limit_mb < 384,
            L"limit=" + std::to_wstring(mem_limit_mb) + L" MB");
        AddPreflightV14(r, L"Current memory headroom",
            mem_limit_mb == 0 || app_memory_mb * 100ULL < mem_limit_mb * 80ULL,
            mem_limit_mb > 0 && app_memory_mb * 100ULL >= mem_limit_mb * 90ULL,
            L"usage=" + std::to_wstring(app_memory_mb) + L" MB");
        AddPreflightV14(r, L"LocalState disk headroom", disk_mb >= 1024, disk_mb < 256,
            L"free=" + std::to_wstring(disk_mb) + L" MB");
        AddPreflightV14(r, L"Absolute HTTPS project URL", IsAbsoluteHttpsUrlV16(project_url), true, project_url);
        AddPreflightV14(r, L"D3D11 compute probe", gpu.find(L"PASS") != std::wstring::npos, false, gpu);

        bool settings_ok = false;
        try
        {
            const std::wstring token = L"v14-roundtrip-ok";
            SetSetting(L"V14PreflightProbe", token);
            settings_ok = GetSetting(L"V14PreflightProbe") == token;
            SetSetting(L"V14PreflightProbe", L"");
        }
        catch (...) { settings_ok = false; }
        AddPreflightV14(r, L"Persistent settings roundtrip", settings_ok, true, L"LocalSettings write/read");

        const unsigned long long saved_hint = ParseU64(GetSetting(L"V13ServerDelayHint"));
        AddPreflightV14(r, L"Scheduler-delay state",
            saved_hint == 0 || (saved_hint >= 1 && saved_hint <= 300), false,
            saved_hint == 0 ? std::wstring(L"default 11 s will be used") :
                              L"saved hint=" + std::to_wstring(saved_hint) + L" s");

        AddPreflightV14(r, L"Persistent BOINC statistics consistency",
            stats.uploaded <= stats.completed, true,
            L"completed=" + std::to_wstring(stats.completed) + L", uploaded+ACK=" + std::to_wstring(stats.uploaded));

        {
            const std::wstring md5 = Md5Hex(L"abc");
            AddPreflightV14(r, L"MD5 cryptography self-test",
                md5 == L"900150983cd24fb0d6963f7d28e17f72", true, md5);
        }

        {
            const std::wstring normalized_text = Lower(Trim(L"  TEST@EXAMPLE.COM  "));
            AddPreflightV14(r, L"Text normalization self-test",
                normalized_text == L"test@example.com", true, normalized_text);
        }

        {
            const bool parser_ok =
                ParseWaitSecondsV13(L"MSG: Please wait 17 seconds.") == 17 &&
                ParseDelayTagV13(L"delay=11.000000") == 11;
            AddPreflightV14(r, L"Scheduler parser self-test", parser_ok, true,
                parser_ok ? std::wstring(L"wait=17 s, delay=11 s") : std::wstring(L"parser mismatch"));
        }

        {
            const std::vector<double> sample = { 1.0, 2.0, 3.0, 4.0, 5.0 };
            const bool stats_ok =
                std::fabs(AvgV12(sample) - 3.0) < 0.0001 &&
                std::fabs(PercentileV12(sample, 0.5) - 3.0) < 0.0001 &&
                std::fabs(MinV12(sample) - 1.0) < 0.0001 &&
                std::fabs(MaxV12(sample) - 5.0) < 0.0001;
            AddPreflightV14(r, L"Telemetry statistics self-test", stats_ok, true,
                stats_ok ? std::wstring(L"avg/p50/min/max verified") : std::wstring(L"statistics mismatch"));
        }

        {
            const long long t1 = MonotonicMs10();
            const long long t2 = MonotonicMs10();
            AddPreflightV14(r, L"Monotonic timer self-test", t2 >= t1, true,
                L"delta=" + std::to_wstring((std::max)(0LL, t2 - t1)) + L" ms");
        }

        {
            const std::wstring normalized_url = NormalizeProjectUrlV16(L"asteroidsathome.net/boinc");
            AddPreflightV14(r, L"Project URL normalization self-test",
                normalized_url == L"https://asteroidsathome.net/boinc/", true, normalized_url);
        }

        {
            const unsigned int items = 20000u;
            const unsigned long long serial = ChecksumRangeV15(0, 1, items);
            const unsigned long long parallel =
                ChecksumRangeV15(0, 2, items) + ChecksumRangeV15(1, 2, items);
            AddPreflightV14(r, L"Deterministic split-kernel self-test",
                serial == parallel, true,
                L"serial=" + std::to_wstring(serial) + L", split=" + std::to_wstring(parallel));
        }

        const std::wstring optimization_lab = GetSetting(L"V15OptimizationLab");
        AddPreflightV14(r, L"v1.7 optimization lab",
            !optimization_lab.empty(), false,
            optimization_lab.empty() ? std::wstring(L"not run yet; use Run optimization lab for CPU/GPU/thread tests") :
                                       std::wstring(L"previous result is available and persisted"));
#ifdef NDEBUG
        AddPreflightV14(r, L"Release optimization build", true, false, BuildModeV15());
#else
        AddPreflightV14(r, L"Release optimization build", false, false, BuildModeV15());
#endif

        r.text = L"v1.7 consolidated local preflight\nChecks=" +
            std::to_wstring(r.passed + r.warnings + r.failures) +
            L"  PASS=" + std::to_wstring(r.passed) +
            L"  WARN=" + std::to_wstring(r.warnings) +
            L"  FAIL=" + std::to_wstring(r.failures) + L"\n\n" + r.text;
        return r;
    }

    bool ProtocolPassV14(const std::wstring& value)
    {
        return value.rfind(L"PASS", 0) == 0;
    }

    std::wstring VerdictV14(const V12Session& s, const V10Engine& engine, bool final)
    {
        if (!final && engine.running.load()) return L"RUNNING";
        const unsigned long long ok = static_cast<unsigned long long>(engine.session_success);
        if (s.preflight_failures > 0 || s.hard_failures > 0 || s.memory_guard_triggered ||
            s.disk_guard_triggered || s.pacing_violations > 0 || s.duplicate_results > 0 ||
            s.download_passes < ok || s.solve_passes < ok || s.upload_passes < ok ||
            s.report_passes < ok || s.ack_passes < ok)
            return L"FAIL";
        if (engine.target_success > 0 && engine.session_success < engine.target_success)
            return L"INCOMPLETE";
        if (s.preflight_warnings > 0 || s.rate_limit_events > 0 || s.no_work > 0)
            return L"PASS WITH WARNINGS";
        return L"PASS";
    }

    std::wstring VerificationTextV14(const V12Session& s, const V10Engine& engine, bool final)
    {
        const double wait_avg = AvgV12(s.actual_wait_times);
        const double wait_min = MinV12(s.actual_wait_times);
        const double wait_max = MaxV12(s.actual_wait_times);
        long long memory_drift = 0;
        if (s.memory_after_wu_mb.size() >= 2)
            memory_drift = static_cast<long long>(s.memory_after_wu_mb.back()) -
                           static_cast<long long>(s.memory_after_wu_mb.front());

        std::wostringstream o;
        o << L"v1.7 validation matrix - verdict: " << VerdictV14(s, engine, final) << L"\n"
          << L"Local preflight: PASS=" << s.preflight_passes
          << L"  WARN=" << s.preflight_warnings
          << L"  FAIL=" << s.preflight_failures << L"\n"
          << L"Protocol-stage coverage: download=" << s.download_passes
          << L"  solve=" << s.solve_passes
          << L"  upload=" << s.upload_passes
          << L"  report=" << s.report_passes
          << L"  ACK=" << s.ack_passes
          << L"  successful WUs=" << engine.session_success << L"\n"
          << L"Result uniqueness: duplicates=" << s.duplicate_results
          << L"  unique=" << s.seen_results.size() << L"\n"
          << L"Scheduler wait verification: samples=" << s.actual_wait_times.size()
          << L"  actual min/avg/max=" << Fixed1V12(wait_min) << L" / "
          << Fixed1V12(wait_avg) << L" / " << Fixed1V12(wait_max)
          << L" s  under-wait violations=" << s.pacing_violations << L"\n"
          << L"Memory-after-WU samples=" << s.memory_after_wu_mb.size()
          << L"  drift(first->last)=" << memory_drift << L" MB\n"
          << L"Server behavior: no-work=" << s.no_work
          << L"  rate-limits=" << s.rate_limit_events
          << L"  hard-failures=" << s.hard_failures;
        return o.str();
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
        const long long now = session.finished_ms > 0 ? session.finished_ms : MonotonicMs10();
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
          << L"Lifetime v1.7 scheduler diagnostics\n"
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
          << L"  failed attempts=" << core.failed << L"\n"
          << L"v1.7 test lifetime: preflight runs=" << lifetime.v14_preflight_runs
          << L"  preflight failures=" << lifetime.v14_preflight_failures
          << L"  pacing violations=" << lifetime.v14_pacing_violations
          << L"  clean sessions=" << lifetime.v14_clean_sessions
          << L"  protocol-verified WUs=" << lifetime.v14_verified_wus << L"\n"
          << L"Live validation verdict=" << VerdictV14(session, engine, false);
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
            out.replace(p, old_head.size(), L"BOINC Xbox v1.7 consolidated validation-suite report");

        const long long report_now = session.finished_ms > 0 ? session.finished_ms : MonotonicMs10();
        const double wall = session.started_ms > 0 ? (report_now - session.started_ms) / 1000.0 : 0.0;
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
        out += L"\n\n" + VerificationTextV14(session, engine, false);
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
        sub->Text = ref new String(L"v1.7 - finish-today validation dashboard + 21 local tests + ETA + consolidated BOINC production validation");
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
            L"\nv1.7 bundle: graphical per-stage progress dashboard with WU/suite ETA, hardened absolute-HTTPS URI validation, 21-check local preflight, deterministic multi-thread correctness, 1/2/4/6/8-thread scaling, memory-bandwidth microtest, "
            L"verified D3D11 compute benchmark, AVX2 Release build path, backend-safety policy, server-delay verification, rate-limit classification, "
            L"protocol-stage coverage, uniqueness/memory-drift checks, persistent reports, p50/p90 telemetry, 100-WU certification, soak testing and diagnostics export." +
            (interrupted ? L"\nRecovery marker detected: the validated v1.0.2 engine will reuse a persisted active workunit when possible." : L""));
        capability->FontSize = 15;
        capability->TextWrapping = TextWrapping::Wrap;

        std::wstring initial = NormalizeProjectUrlV16(GetSetting(L"ProjectUrl"));
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
        mode->Items->Append(ref new String(L"1 workunit - protocol sanity (fastest)"));
        mode->Items->Append(ref new String(L"3 workunits - quick validation"));
        mode->Items->Append(ref new String(L"10 workunits - standard finish-today suite"));
        mode->Items->Append(ref new String(L"25 workunits - full validation"));
        mode->Items->Append(ref new String(L"50 workunits - long soak/health test"));
        mode->Items->Append(ref new String(L"100 workunits - certification/extended soak"));
        mode->Items->Append(ref new String(L"Continuous production - until Stop"));
        mode->SelectedIndex = 2;

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
        start->Content = ref new String(L"Start v1.7 mega validation");
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

        auto runLab = ref new Button();
        runLab->Content = ref new String(L"Run v1.7 optimization lab");
        runLab->FontSize = 16;
        runLab->Padding = Thickness(14,10,14,10);

        auto runLocal = ref new Button();
        runLocal->Content = ref new String(L"Run 21 local tests only");
        runLocal->FontSize = 16;
        runLocal->Padding = Thickness(14,10,14,10);

        row2->Children->Append(runLab);
        row2->Children->Append(runLocal);
        row2->Children->Append(saveDiag);
        row2->Children->Append(reset);

        auto dashboardTitle = ref new TextBlock();
        dashboardTitle->Text = ref new String(L"Live progress dashboard");
        dashboardTitle->FontSize = 30;

        auto dashboardPanel = ref new StackPanel();
        dashboardPanel->Spacing = 8;

        auto dashboardState = ref new TextBlock();
        dashboardState->Text = ref new String(L"Ready - start a validation suite.");
        dashboardState->FontSize = 18;
        dashboardState->TextWrapping = TextWrapping::Wrap;
        dashboardPanel->Children->Append(dashboardState);

        auto suiteProgressText = ref new TextBlock();
        suiteProgressText->Text = ref new String(L"Suite overall: 0.0 %");
        suiteProgressText->FontSize = 16;
        auto suiteProgress = ref new ProgressBar();
        suiteProgress->Minimum = 0.0;
        suiteProgress->Maximum = 100.0;
        suiteProgress->Value = 0.0;
        suiteProgress->Height = 18;
        dashboardPanel->Children->Append(suiteProgressText);
        dashboardPanel->Children->Append(suiteProgress);

        auto wuProgressText = ref new TextBlock();
        wuProgressText->Text = ref new String(L"Current workunit: 0.0 %");
        wuProgressText->FontSize = 16;
        auto wuProgress = ref new ProgressBar();
        wuProgress->Minimum = 0.0;
        wuProgress->Maximum = 100.0;
        wuProgress->Value = 0.0;
        wuProgress->Height = 18;
        dashboardPanel->Children->Append(wuProgressText);
        dashboardPanel->Children->Append(wuProgress);

        auto schedulerProgressText = ref new TextBlock();
        schedulerProgressText->Text = ref new String(L"1. Project / scheduler / work acquisition: 0.0 %");
        schedulerProgressText->FontSize = 15;
        auto schedulerProgress = ref new ProgressBar();
        schedulerProgress->Minimum = 0.0;
        schedulerProgress->Maximum = 100.0;
        schedulerProgress->Value = 0.0;
        schedulerProgress->Height = 15;
        dashboardPanel->Children->Append(schedulerProgressText);
        dashboardPanel->Children->Append(schedulerProgress);

        auto downloadProgressText = ref new TextBlock();
        downloadProgressText->Text = ref new String(L"2. Input download: 0.0 %");
        downloadProgressText->FontSize = 15;
        auto downloadProgress = ref new ProgressBar();
        downloadProgress->Minimum = 0.0;
        downloadProgress->Maximum = 100.0;
        downloadProgress->Value = 0.0;
        downloadProgress->Height = 15;
        dashboardPanel->Children->Append(downloadProgressText);
        dashboardPanel->Children->Append(downloadProgress);

        auto scienceProgressText = ref new TextBlock();
        scienceProgressText->Text = ref new String(L"3. PeriodSearch science: 0.0 %");
        scienceProgressText->FontSize = 15;
        auto scienceProgress = ref new ProgressBar();
        scienceProgress->Minimum = 0.0;
        scienceProgress->Maximum = 100.0;
        scienceProgress->Value = 0.0;
        scienceProgress->Height = 15;
        dashboardPanel->Children->Append(scienceProgressText);
        dashboardPanel->Children->Append(scienceProgress);

        auto uploadProgressText = ref new TextBlock();
        uploadProgressText->Text = ref new String(L"4. Result upload: 0.0 %");
        uploadProgressText->FontSize = 15;
        auto uploadProgress = ref new ProgressBar();
        uploadProgress->Minimum = 0.0;
        uploadProgress->Maximum = 100.0;
        uploadProgress->Value = 0.0;
        uploadProgress->Height = 15;
        dashboardPanel->Children->Append(uploadProgressText);
        dashboardPanel->Children->Append(uploadProgress);

        auto ackProgressText = ref new TextBlock();
        ackProgressText->Text = ref new String(L"5. Scheduler report / ACK: 0.0 %");
        ackProgressText->FontSize = 15;
        auto ackProgress = ref new ProgressBar();
        ackProgress->Minimum = 0.0;
        ackProgress->Maximum = 100.0;
        ackProgress->Value = 0.0;
        ackProgress->Height = 15;
        dashboardPanel->Children->Append(ackProgressText);
        dashboardPanel->Children->Append(ackProgress);

        auto dashboardBox = ref new Border();
        dashboardBox->Padding = Thickness(20);
        dashboardBox->Background = ref new SolidColorBrush(ColorHelper::FromArgb(255,23,45,60));
        dashboardBox->Child = dashboardPanel;

        auto optimizationTitle = ref new TextBlock();
        optimizationTitle->Text = ref new String(L"v1.7 optimization laboratory");
        optimizationTitle->FontSize = 25;
        auto optimizationText = ref new TextBlock();
        {
            const std::wstring saved = GetSetting(L"V15OptimizationLab");
            optimizationText->Text = PS(saved.empty()
                ? L"Not run yet. This lab tests deterministic multi-thread correctness, CPU scaling, memory bandwidth and verified D3D11 compute throughput. Production PeriodSearch remains on the validated CPU path."
                : saved);
        }
        optimizationText->FontSize = 14;
        optimizationText->TextWrapping = TextWrapping::Wrap;
        auto optimizationBox = ref new Border();
        optimizationBox->Padding = Thickness(18);
        optimizationBox->Background = ref new SolidColorBrush(ColorHelper::FromArgb(255,24,42,50));
        optimizationBox->Child = optimizationText;

        auto preflightTitle = ref new TextBlock();
        preflightTitle->Text = ref new String(L"Local preflight tests");
        preflightTitle->FontSize = 25;
        auto preflightText = ref new TextBlock();
        {
            const std::wstring saved = GetSetting(L"V14LastPreflight");
            preflightText->Text = PS(saved.empty() ? L"Preflight runs automatically when a v1.7 suite starts." : saved);
        }
        preflightText->FontSize = 14;
        preflightText->TextWrapping = TextWrapping::Wrap;
        auto preflightBox = ref new Border();
        preflightBox->Padding = Thickness(18);
        preflightBox->Background = ref new SolidColorBrush(ColorHelper::FromArgb(255,27,39,48));
        preflightBox->Child = preflightText;

        auto verifyTitle = ref new TextBlock();
        verifyTitle->Text = ref new String(L"Validation matrix");
        verifyTitle->FontSize = 25;
        auto verificationText = ref new TextBlock();
        {
            const std::wstring saved = GetSetting(L"V14LastVerification");
            verificationText->Text = PS(saved.empty() ? L"No v1.7 validation session has completed yet." : saved);
        }
        verificationText->FontSize = 14;
        verificationText->TextWrapping = TextWrapping::Wrap;
        auto verifyBox = ref new Border();
        verifyBox->Padding = Thickness(18);
        verifyBox->Background = ref new SolidColorBrush(ColorHelper::FromArgb(255,27,39,48));
        verifyBox->Child = verificationText;

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
        {
            const std::wstring saved = GetSetting(L"V14LastPerformance");
            perfText->Text = PS(saved.empty() ? L"Start v1.7 to collect the consolidated validation matrix and production telemetry." : saved);
        }
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
        {
            const std::wstring saved = GetSetting(L"V14LastReport");
            report->Text = PS(saved.empty() ? L"No v1.7 cycle has run yet." : saved);
        }
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

        auto update_progress_dashboard = [=]()
        {
            const std::wstring st = engine->stage;
            int rank = 0;
            if (st.find(L"VALIDATING") != std::wstring::npos) rank = 1;
            else if (st.find(L"PROJECT CONFIG") != std::wstring::npos) rank = 2;
            else if (st.find(L"ACCOUNT / SCHEDULER") != std::wstring::npos) rank = 3;
            else if (st.find(L"WORK ACQUISITION") != std::wstring::npos) rank = 4;
            else if (st.find(L"RESUMING") != std::wstring::npos) rank = 5;
            else if (st.find(L"DOWNLOADING") != std::wstring::npos) rank = 6;
            else if (st.find(L"SCIENCE COMPUTE") != std::wstring::npos) rank = 7;
            else if (st.find(L"UPLOADING") != std::wstring::npos) rank = 8;
            else if (st.find(L"REPORTING") != std::wstring::npos) rank = 9;
            else if (st.find(L"ACKNOWLEDGED") != std::wstring::npos ||
                     st.find(L"POST CYCLE") != std::wstring::npos ||
                     st.find(L"SERVER-AWARE WAIT") != std::wstring::npos ||
                     st.find(L"TEST TARGET COMPLETE") != std::wstring::npos) rank = 10;

            const auto clamp_pct = [](double v)
            {
                return (std::max)(0.0, (std::min)(100.0, v));
            };

            double scheduler_pct = 0.0;
            if (rank >= 4) scheduler_pct = 100.0;
            else if (rank == 3) scheduler_pct = 75.0;
            else if (rank == 2) scheduler_pct = 50.0;
            else if (rank == 1) scheduler_pct = 25.0;

            double download_pct = rank > 6 ? 100.0 : (rank == 6 ? 50.0 : 0.0);

            double science_pct = rank > 7 ? 100.0 : 0.0;
            bool science_estimated = false;
            double science_elapsed = 0.0;
            double science_eta = 0.0;
            if (rank == 7)
            {
                const double internal = periodsearch_progress() * 100.0;
                if (internal > 0.05 && internal <= 100.0)
                    science_pct = clamp_pct(internal);
                else
                {
                    const long long now = MonotonicMs10();
                    const long long started = engine->science_started_ms.load();
                    const double elapsed = started > 0 ? (std::max)(0LL, now - started) / 1000.0 : 0.0;
                    double expected = AvgV12(session->solve_times);
                    if (expected < 30.0) expected = 240.0;
                    science_pct = clamp_pct((std::min)(95.0, 100.0 * elapsed / expected));
                    science_estimated = true;
                }
            }

            if (rank == 7)
            {
                const long long eta_now = MonotonicMs10();
                const long long eta_started = engine->science_started_ms.load();
                science_elapsed = eta_started > 0 ? (std::max)(0LL, eta_now - eta_started) / 1000.0 : 0.0;
                if (science_elapsed > 0.0 && science_pct > 0.1 && science_pct < 99.9)
                    science_eta = science_elapsed * (100.0 - science_pct) / science_pct;
            }

            const double upload_pct = rank > 8 ? 100.0 : (rank == 8 ? 50.0 : 0.0);
            const double ack_pct = rank >= 10 ? 100.0 : (rank == 9 ? 50.0 : 0.0);

            const double wu_pct = clamp_pct(
                scheduler_pct * 0.15 +
                download_pct * 0.10 +
                science_pct * 0.65 +
                upload_pct * 0.05 +
                ack_pct * 0.05);

            double suite_pct = 0.0;
            if (engine->target_success > 0)
            {
                double fractional = static_cast<double>(engine->session_success);
                if (engine->running.load() && rank > 0 && rank < 10)
                    fractional += wu_pct / 100.0;
                suite_pct = clamp_pct(100.0 * fractional / static_cast<double>(engine->target_success));
            }

            suiteProgress->Value = suite_pct;
            wuProgress->Value = wu_pct;
            schedulerProgress->Value = scheduler_pct;
            downloadProgress->Value = download_pct;
            scienceProgress->Value = science_pct;
            uploadProgress->Value = upload_pct;
            ackProgress->Value = ack_pct;

            std::wstring suite_label;
            if (engine->target_success > 0)
            {
                suite_label = L"Suite overall: " + Fixed1V12(suite_pct) + L"%   completed=" +
                    std::to_wstring(engine->session_success) + L"/" + std::to_wstring(engine->target_success);

                if (engine->running.load() && !session->e2e_times.empty())
                {
                    const int remaining = (std::max)(0, engine->target_success - engine->session_success);
                    const double avg_cycle = AvgV12(session->e2e_times);
                    double suite_eta = static_cast<double>(remaining) * avg_cycle;
                    if (rank == 7 && science_eta > 0.0 && remaining > 0)
                        suite_eta = science_eta + static_cast<double>((std::max)(0, remaining - 1)) * avg_cycle;
                    if (suite_eta > 0.0) suite_label += L"   ETA~" + FormatDurationV17(suite_eta);
                }
            }
            else
                suite_label = L"Suite overall: continuous production   completed=" + std::to_wstring(engine->session_success);
            suiteProgressText->Text = PS(suite_label);

            wuProgressText->Text = PS(L"Current workunit: " + Fixed1V12(wu_pct) + L"%");
            schedulerProgressText->Text = PS(L"1. Project / scheduler / work acquisition: " + Fixed1V12(scheduler_pct) + L"%");
            downloadProgressText->Text = PS(L"2. Input download: " + Fixed1V12(download_pct) + L"%");
            scienceProgressText->Text = PS(L"3. PeriodSearch science: " + Fixed1V12(science_pct) +
                L"%" + (science_estimated ? std::wstring(L"  (estimated from elapsed/previous WUs)") : std::wstring()) +
                (science_eta > 0.0 ? std::wstring(L"   ETA~") + FormatDurationV17(science_eta) : std::wstring()));
            uploadProgressText->Text = PS(L"4. Result upload: " + Fixed1V12(upload_pct) + L"%");
            ackProgressText->Text = PS(L"5. Scheduler report / ACK: " + Fixed1V12(ack_pct) + L"%");

            std::wstring state = L"Stage: " + (st.empty() ? std::wstring(L"IDLE") : st);
            if (!engine->current_result.empty()) state += L"   Result: " + engine->current_result;
            if (rank == 7)
            {
                state += L"   elapsed=" + FormatDurationV17(science_elapsed);
                if (science_eta > 0.0) state += L"   ETA~" + FormatDurationV17(science_eta);
            }
            if (!engine->network_enabled.load()) state += L"   NETWORK PAUSED";
            if (engine->stop_requested.load()) state += L"   STOP REQUESTED";
            dashboardState->Text = PS(state);
        };

        auto update_all = [=]()
        {
            update_progress_dashboard();
            statsText->Text = PS(StatsText(*stats));
            if (session->started_ms > 0)
            {
                perfText->Text = PS(PerfTextV12(*session, *lifetime, *stats, *engine, mem_limit_mb));
                verificationText->Text = PS(VerificationTextV14(*session, *engine, !engine->running.load()));
            }
            recentText->Text = PS(RecentTextV12(*session));
        };

        auto finish_session = [=](const std::wstring& reason)
        {
            const long long now = MonotonicMs10();
            AccumulateStageV12(*session, now);
            session->finished_ms = now;
            session->tracked_stage.clear();
            session->stage_started_ms = 0;
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
            const std::wstring final_verification = VerificationTextV14(*session, *engine, true);
            verificationText->Text = PS(final_verification);
            SetSetting(L"V14LastVerification", final_verification);
            const std::wstring final_perf = PerfTextV12(*session, *lifetime, *stats, *engine, mem_limit_mb);
            perfText->Text = PS(final_perf);
            SetSetting(L"V14LastPerformance", final_perf);
            const std::wstring verdict = VerdictV14(*session, *engine, true);
            if (verdict == L"PASS") ++lifetime->v14_clean_sessions;
            SaveV12Lifetime(*lifetime);
            std::wstring final_report = report->Text ? report->Text->Data() : L"";
            final_report += L"\n\nFINAL SESSION VERIFICATION\n" + final_verification;
            report->Text = PS(final_report);
            SetSetting(L"V14LastReport", final_report);
            SetSetting(L"V14LastVerdict", verdict);

            start->IsEnabled = true;
            stop->IsEnabled = false;
            stop->Content = ref new String(L"Stop after current WU");
            reset->IsEnabled = true;
            runLab->IsEnabled = true;
            runLocal->IsEnabled = true;
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

        runLab->Click += ref new RoutedEventHandler([=](Object^, RoutedEventArgs^)
        {
            if (engine->running.load())
            {
                append(L"Optimization lab is disabled while a production workunit suite is running.");
                return;
            }

            runLab->IsEnabled = false;
            optimizationText->Text = ref new String(L"v1.7 optimization lab running: CPU/thread correctness and scaling...");
            append(L"v1.7 optimization lab started.");

            create_task([=]()
            {
                std::wstring r = L"v1.7 optimization laboratory\n";
                r += L"Build: " + BuildModeV15() + L"\n";
                r += L"CPU features: " + CpuFeatureSummary() + L"\n";
                r += ThreadCorrectnessV15(cpus) + L"\n";
                r += CpuScaleBenchmarkV15(cpus) + L"\n";
                r += MemoryBandwidthBenchmarkV15() + L"\n";
                r += BackendPolicyV15();
                return r;
            }).then([=](std::wstring cpu_report)
            {
                optimizationText->Text = PS(cpu_report + L"\nGPU compute microtest: running...");
                return GpuComputeBenchmarkV15().then([cpu_report](std::wstring gpu_report)
                {
                    return cpu_report + L"\n" + gpu_report;
                });
            }, ui).then([=](std::wstring full_report)
            {
                SetSetting(L"V15OptimizationLab", full_report);
                optimizationText->Text = PS(full_report);
                runLab->IsEnabled = true;
                append(L"v1.7 optimization lab finished and persisted.");
            }, ui).then([=](task<void> done)
            {
                try { done.get(); }
                catch (Exception^ ex)
                {
                    const std::wstring msg = ex->Message ? ex->Message->Data() : L"WinRT error";
                    optimizationText->Text = PS(L"v1.7 optimization lab FAIL: " + msg);
                    append(L"Optimization lab failed: " + msg);
                    runLab->IsEnabled = true;
                }
                catch (...)
                {
                    optimizationText->Text = ref new String(L"v1.7 optimization lab FAIL: unknown native error");
                    append(L"Optimization lab failed: unknown native error");
                    runLab->IsEnabled = true;
                }
            }, ui);
        });

        runLocal->Click += ref new RoutedEventHandler([=](Object^, RoutedEventArgs^)
        {
            if (engine->running.load())
            {
                append(L"Local-only tests are disabled while a production workunit is running.");
                return;
            }

            std::wstring project_url = NormalizeProjectUrlV16(url->Text ? url->Text->Data() : L"");
            if (project_url.empty()) project_url = L"https://asteroidsathome.net/boinc/";

            unsigned long long usage = 0;
            try { usage = MemoryManager::AppMemoryUsage; } catch (Exception^) {}
            const DiskInfo disk = QueryDiskInfo();

            const V14Preflight p = RunPreflightV14(
                cpus, mem_limit_mb, usage / 1048576ULL, disk, project_url, *stats);
            preflightText->Text = PS(p.text);
            SetSetting(L"V14LastPreflight", p.text);

            append(L"v1.7 local-only self-test finished: PASS=" + std::to_wstring(p.passed) +
                L", WARN=" + std::to_wstring(p.warnings) +
                L", FAIL=" + std::to_wstring(p.failures) + L".");
        });

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
            SetSetting(L"V14PreflightRuns", L"0");
            SetSetting(L"V14PreflightFailures", L"0");
            SetSetting(L"V14PacingViolations", L"0");
            SetSetting(L"V14CleanSessions", L"0");
            SetSetting(L"V14VerifiedWUs", L"0");
            SetSetting(L"V14LastPreflight", L"");
            SetSetting(L"V14LastVerification", L"");
            SetSetting(L"V14LastPerformance", L"");
            SetSetting(L"V14LastReport", L"");
            SetSetting(L"V14LastVerdict", L"");
            SetSetting(L"V15OptimizationLab", L"");
            *session = V12Session();
            report->Text = ref new String(L"No v1.7 cycle has run yet.");
            optimizationText->Text = ref new String(L"Not run yet. Run the v1.7 optimization lab before the production suite for the fullest test coverage.");
            preflightText->Text = ref new String(L"Preflight runs automatically when a v1.7 suite starts.");
            verificationText->Text = ref new String(L"No v1.7 validation session has completed yet.");
            perfText->Text = ref new String(L"Start v1.7 to collect the consolidated validation matrix and production telemetry.");
            update_all();
            append(L"Persistent production and v1.7 validation/scheduler statistics reset.");
        });

        saveDiag->Click += ref new RoutedEventHandler([=](Object^, RoutedEventArgs^)
        {
            std::wstring snapshot = L"BOINC Xbox v1.7 consolidated diagnostics snapshot\n\n";
            snapshot += L"OPTIMIZATION LAB\n" + std::wstring(optimizationText->Text ? optimizationText->Text->Data() : L"") + L"\n\n";
            snapshot += L"PREFLIGHT\n" + std::wstring(preflightText->Text ? preflightText->Text->Data() : L"") + L"\n\n";
            snapshot += L"VALIDATION MATRIX\n" + std::wstring(verificationText->Text ? verificationText->Text->Data() : L"") + L"\n\n";
            snapshot += L"PROGRESS DASHBOARD\n" +
                std::wstring(dashboardState->Text ? dashboardState->Text->Data() : L"") + L"\n" +
                std::wstring(suiteProgressText->Text ? suiteProgressText->Text->Data() : L"") + L"\n" +
                std::wstring(wuProgressText->Text ? wuProgressText->Text->Data() : L"") + L"\n" +
                std::wstring(schedulerProgressText->Text ? schedulerProgressText->Text->Data() : L"") + L"\n" +
                std::wstring(downloadProgressText->Text ? downloadProgressText->Text->Data() : L"") + L"\n" +
                std::wstring(scienceProgressText->Text ? scienceProgressText->Text->Data() : L"") + L"\n" +
                std::wstring(uploadProgressText->Text ? uploadProgressText->Text->Data() : L"") + L"\n" +
                std::wstring(ackProgressText->Text ? ackProgressText->Text->Data() : L"") + L"\n\n";
            snapshot += L"STATUS\n" + std::wstring(status->Text ? status->Text->Data() : L"") + L"\n\n";
            snapshot += L"PERFORMANCE\n" + std::wstring(perfText->Text ? perfText->Text->Data() : L"") + L"\n\n";
            snapshot += L"PRODUCTION STATS\n" + std::wstring(statsText->Text ? statsText->Text->Data() : L"") + L"\n\n";
            snapshot += L"LAST REPORT\n" + std::wstring(report->Text ? report->Text->Data() : L"") + L"\n\n";
            snapshot += L"RECENT WORKUNITS\n" + RecentTextV12(*session);
            SaveText(L"v16_diagnostics.txt", snapshot).then([=](task<void> t)
            {
                try
                {
                    t.get();
                    append(L"Diagnostics saved to LocalState\\v16_diagnostics.txt");
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
            const std::wstring raw_project_url = Trim(url->Text ? url->Text->Data() : L"");
            const std::wstring project_url = NormalizeProjectUrlV16(raw_project_url);
            if (project_url.empty())
            {
                append(L"ERROR: Project URL is not a valid absolute HTTPS URI. No network request was sent.");
                preflightText->Text = ref new String(L"FAIL  Absolute HTTPS project URL: invalid URI");
                return;
            }
            if (project_url != raw_project_url)
                append(L"Project URL normalized to: " + project_url);
            url->Text = PS(project_url);

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
            unsigned long long preflight_usage = 0;
            try { preflight_usage = MemoryManager::AppMemoryUsage; } catch (Exception^) {}
            const DiskInfo preflight_disk = QueryDiskInfo();
            const V14Preflight preflight = RunPreflightV14(
                cpus, mem_limit_mb, preflight_usage / 1048576ULL, preflight_disk, project_url, *stats);
            session->preflight_passes = preflight.passed;
            session->preflight_warnings = preflight.warnings;
            session->preflight_failures = preflight.failures;
            session->preflight_text = preflight.text;
            preflightText->Text = PS(preflight.text);
            SetSetting(L"V14LastPreflight", preflight.text);
            ++lifetime->v14_preflight_runs;
            if (preflight.failures > 0) ++lifetime->v14_preflight_failures;
            SaveV12Lifetime(*lifetime);
            if (preflight.failures > 0)
            {
                engine->running.store(false);
                engine->stage = L"PREFLIGHT FAILED";
                append(L"v1.7 preflight failed. Production work was not requested; review the validation box.");
                update_all();
                return;
            }

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
            runLab->IsEnabled = false;
            runLocal->IsEnabled = false;
            mode->IsEnabled = false;
            cadence->IsEnabled = false;
            log->Text = ref new String(L"");

            append(L"BOINC Xbox v1.7 graphical validation suite started.");
            append(L"Validated project URL: " + project_url);
            append(L"v1.7 bundle: 21-check preflight + local-only self-test + optimization lab + protocol-stage verification + scheduler wait validation + ETA telemetry + persistent final evidence + production soak testing.");
            append(L"Stable CPU science path remains single-instance for numerical safety; scheduler cadence is optimized independently.");
            append(L"Scheduler policy extra=" + std::to_wstring(session->success_delay_s) +
                L" s; initial server hint=" + std::to_wstring(session->server_delay_hint_s) +
                L" s; generic no-work uses exponential backoff.");
            append(L"CPU: " + CpuFeatureSummary());
            append(L"GPU probe: " + GpuProbeSummary());
            append(L"Build: " + BuildModeV15());
            append(L"Optimization lab: " + std::wstring(GetSetting(L"V15OptimizationLab").empty() ? L"not run in this install" : L"persisted result available"));

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

                const long long request_now = MonotonicMs10();
                if (session->wait_started_ms > 0 && session->planned_wait_s > 0)
                {
                    const double actual_wait = (std::max)(0LL, request_now - session->wait_started_ms) / 1000.0;
                    session->actual_wait_times.push_back(actual_wait);
                    if (actual_wait + 0.25 < static_cast<double>(session->planned_wait_s))
                    {
                        ++session->pacing_violations;
                        ++lifetime->v14_pacing_violations;
                        SaveV12Lifetime(*lifetime);
                        append(L"PACING TEST FAIL: next scheduler acquisition started before the planned wait expired.");
                    }
                    session->wait_started_ms = 0;
                    session->planned_wait_s = 0;
                }

                ++engine->session_attempts;
                session->cycle_started_ms = request_now;
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

                            const bool p_download = ProtocolPassV14(result.state->download);
                            const bool p_solve = ProtocolPassV14(result.state->solve);
                            const bool p_upload = ProtocolPassV14(result.state->upload);
                            const bool p_report = ProtocolPassV14(result.state->report);
                            const bool p_ack = ProtocolPassV14(result.state->ack);
                            if (p_download) ++session->download_passes;
                            if (p_solve) ++session->solve_passes;
                            if (p_upload) ++session->upload_passes;
                            if (p_report) ++session->report_passes;
                            if (p_ack) ++session->ack_passes;
                            if (p_download && p_solve && p_upload && p_report && p_ack)
                                ++lifetime->v14_verified_wus;

                            const std::wstring result_name = result.state->package.result_name;
                            if (!result_name.empty())
                            {
                                if (std::find(session->seen_results.begin(), session->seen_results.end(), result_name) != session->seen_results.end())
                                    ++session->duplicate_results;
                                else
                                    session->seen_results.push_back(result_name);
                            }

                            unsigned long long after_usage = 0;
                            try { after_usage = MemoryManager::AppMemoryUsage; } catch (Exception^) {}
                            session->memory_after_wu_mb.push_back(after_usage / 1048576ULL);
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
                    const std::wstring cycle_report = ReportV12(result, *stats, *engine, *session);
                    report->Text = PS(cycle_report);
                    SetSetting(L"V14LastReport", cycle_report);
                    const std::wstring live_verification = VerificationTextV14(*session, *engine, false);
                    verificationText->Text = PS(live_verification);
                    SetSetting(L"V14LastVerification", live_verification);
                    const std::wstring live_perf = PerfTextV12(*session, *lifetime, *stats, *engine, mem_limit_mb);
                    SetSetting(L"V14LastPerformance", live_perf);

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

                    session->planned_wait_s = delay_seconds;
                    session->wait_started_ms = MonotonicMs10();
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
        panel->Children->Append(dashboardTitle);
        panel->Children->Append(dashboardBox);
        panel->Children->Append(optimizationTitle);
        panel->Children->Append(optimizationBox);
        panel->Children->Append(preflightTitle);
        panel->Children->Append(preflightBox);
        panel->Children->Append(verifyTitle);
        panel->Children->Append(verifyBox);
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
