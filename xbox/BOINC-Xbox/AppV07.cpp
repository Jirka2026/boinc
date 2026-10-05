#include "pch.h"
#include "App.xaml.h"
#include "xbox_platform.h"
#include "PeriodSearchXbox.h"

#include <exception>
#include <iomanip>
#include <sstream>

using namespace BOINC_Xbox;
using namespace Platform;
using namespace concurrency;
using namespace Windows::ApplicationModel;
using namespace Windows::ApplicationModel::Activation;
using namespace Windows::Storage;
using namespace Windows::System;
using namespace Windows::UI;
using namespace Windows::UI::Xaml;
using namespace Windows::UI::Xaml::Controls;
using namespace Windows::UI::Xaml::Media;

namespace
{
    String^ PS(const std::wstring& value) { return ref new String(value.c_str()); }

    std::wstring MakeSmokeInput(const std::wstring& raw)
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
            std::wostringstream rebuilt;
            rebuilt << std::setprecision(12) << period_start << L" " << period_step << L" "
                    << period_start << L" " << fixed_or_free
                    << L" period_start period_step period_end fixed/free";
            lines[0] = rebuilt.str();
        }

        // Only the validation workload is reduced; the science solver remains unchanged.
        lines[10] = L"2                iteration stop condition";

        std::wostringstream output;
        for (size_t i = 0; i < lines.size(); ++i)
        {
            output << lines[i];
            if (i + 1 < lines.size()) output << L"\n";
        }
        return output.str();
    }

    std::wstring FirstNonEmptyLine(const std::wstring& text)
    {
        std::wistringstream input(text);
        std::wstring line;
        while (std::getline(input, line))
        {
            if (!line.empty() && line.back() == L'\r') line.pop_back();
            if (!line.empty()) return line;
        }
        return L"";
    }

    size_t CountNonEmptyLines(const std::wstring& text)
    {
        std::wistringstream input(text);
        std::wstring line;
        size_t count = 0;
        while (std::getline(input, line))
        {
            if (!line.empty() && line.back() == L'\r') line.pop_back();
            if (!line.empty()) ++count;
        }
        return count;
    }
}

App::App() { InitializeComponent(); }

void App::OnLaunched(LaunchActivatedEventArgs^)
{
    boinc_xbox_platform_init();
    unsigned int cpu_threads = std::thread::hardware_concurrency();
    if (!cpu_threads) cpu_threads = 1;

    unsigned long long memory_limit = 0;
    try { memory_limit = MemoryManager::AppMemoryUsageLimit; } catch (Exception^) {}

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
    auto subtitle = ref new TextBlock(); subtitle->Text = ref new String(L"v0.7 - Asteroids PeriodSearch science port"); subtitle->FontSize = 22; subtitle->HorizontalAlignment = HorizontalAlignment::Center;

    auto source = ref new TextBlock();
    source->Text = ref new String(
        L"Science engine: Asteroids@home PeriodSearch pure CPU\n"
        L"Upstream commit: ecd7f1dccb97acc3290a58f3920c9b1aa3a23656\n"
        L"Port mode: original full solver + Xbox/UWP BOINC compatibility layer");
    source->FontSize = 17; source->TextWrapping = TextWrapping::Wrap; source->Opacity = 0.86;

    auto runtime = ref new TextBlock();
    runtime->Text = PS(L"Xbox runtime: CPU threads=" + std::to_wstring(cpu_threads) + L", app memory limit=" + std::to_wstring(memory_limit / 1048576ULL) + L" MB");
    runtime->FontSize = 17;

    auto explanation = ref new TextBlock();
    explanation->Text = ref new String(
        L"This test executes the real upstream PeriodSearch inversion engine on the bundled Asteroids@home sample lightcurve. "
        L"For the first Xbox validation the input is reduced to one period and two optimizer iterations. "
        L"The numerical solver itself is the original PeriodSearch code, not a synthetic benchmark.");
    explanation->FontSize = 17; explanation->TextWrapping = TextWrapping::Wrap;

    auto run = ref new Button(); run->Content = ref new String(L"Run real PeriodSearch science test"); run->FontSize = 21; run->Padding = Thickness(26, 14, 26, 14);
    auto report_title = ref new TextBlock(); report_title->Text = ref new String(L"Science port report"); report_title->FontSize = 28; report_title->Margin = Thickness(0, 14, 0, 0);
    auto report = ref new TextBlock(); report->Text = ref new String(L"Not run yet."); report->FontSize = 18; report->TextWrapping = TextWrapping::Wrap;
    auto report_border = ref new Border(); report_border->Padding = Thickness(20); report_border->Background = ref new SolidColorBrush(ColorHelper::FromArgb(255, 32, 42, 51)); report_border->Child = report;
    auto log_title = ref new TextBlock(); log_title->Text = ref new String(L"Log"); log_title->FontSize = 24;
    auto log = ref new TextBlock(); log->Text = ref new String(L"Ready.\n"); log->FontSize = 16; log->TextWrapping = TextWrapping::Wrap;
    auto log_border = ref new Border(); log_border->Padding = Thickness(20); log_border->Background = ref new SolidColorBrush(ColorHelper::FromArgb(255, 24, 31, 38)); log_border->Child = log;

    auto append = [log](const std::wstring& value)
    {
        std::wstring current = log->Text ? log->Text->Data() : L"";
        current += value + L"\n";
        log->Text = PS(current);
    };

    run->Click += ref new RoutedEventHandler([=](Object^, RoutedEventArgs^)
    {
        run->IsEnabled = false;
        report->Text = ref new String(L"Running the real PeriodSearch solver...");
        log->Text = ref new String(L"");
        append(L"Loading official Asteroids@home PeriodSearch sample input...");
        const auto ui_context = task_continuation_context::use_current();

        create_task(Package::Current->InstalledLocation->GetFileAsync(ref new String(L"PeriodSearchSampleIn.txt")))
        .then([](StorageFile^ sample_file) { return create_task(FileIO::ReadTextAsync(sample_file)); })
        .then([=](String^ sample_text)
        {
            const std::wstring smoke_input = MakeSmokeInput(sample_text ? sample_text->Data() : L"");
            append(L"Sample prepared: one period, two optimizer iterations.");
            return create_task(ApplicationData::Current->LocalFolder->CreateFileAsync(ref new String(L"period_search_in"), CreationCollisionOption::ReplaceExisting))
                .then([smoke_input](StorageFile^ input_file) { return create_task(FileIO::WriteTextAsync(input_file, PS(smoke_input))); });
        }, ui_context)
        .then([=]()
        {
            append(L"Starting full native PeriodSearch code path on Xbox CPU...");
            const std::wstring work_directory = ApplicationData::Current->LocalFolder->Path->Data();
            return create_task([work_directory]() { return periodsearch_run(work_directory, true); });
        }, ui_context)
        .then([=](task<PeriodSearchRunResult> finished)
        {
            try
            {
                const PeriodSearchRunResult result = finished.get();
                const std::wstring first_line = FirstNonEmptyLine(result.output);
                const size_t output_lines = CountNonEmptyLines(result.output);
                const bool pass = result.exit_code == 0 && result.error.empty() && !first_line.empty();

                std::wostringstream text;
                text << L"v0.7 PeriodSearch science port\n\n";
                text << L"Upstream solver: " << (pass ? L"PASS" : L"FAIL") << L"\n";
                text << L"Source commit: ecd7f1dccb97acc3290a58f3920c9b1aa3a23656\n";
                text << L"Execution: original PeriodSearch pure CPU solver\n";
                text << L"Test workload: official sample, 1 period, 2 optimizer iterations\n";
                text << L"Exit code: " << result.exit_code << L"\n";
                text << std::fixed << std::setprecision(3) << L"Elapsed: " << result.elapsed_seconds << L" s\n";
                text << std::setprecision(6) << L"BOINC fraction_done: " << result.fraction_done << L"\n";
                text << L"Output lines: " << output_lines << L"\n";
                text << L"First result: " << (first_line.empty() ? L"(none)" : first_line) << L"\n";
                text << L"Output file: LocalState\\period_search_out\n";
                if (!result.error.empty()) text << L"Error: " << result.error << L"\n";
                report->Text = PS(text.str());
                append(pass ? L"PASS: real Asteroids PeriodSearch solver completed on Xbox." : L"FAIL: PeriodSearch did not produce a valid result.");
                append(L"Result is stored as LocalState\\period_search_out.");
            }
            catch (Exception^ ex)
            {
                report->Text = PS(L"FAIL: " + std::wstring(ex->Message->Data()));
                append(L"WinRT error: " + std::wstring(ex->Message->Data()));
            }
            catch (...)
            {
                report->Text = ref new String(L"FAIL: unknown exception");
                append(L"Unknown error.");
            }
            run->IsEnabled = true;
        }, ui_context);
    });

    panel->Children->Append(title); panel->Children->Append(subtitle); panel->Children->Append(source); panel->Children->Append(runtime); panel->Children->Append(explanation); panel->Children->Append(run); panel->Children->Append(report_title); panel->Children->Append(report_border); panel->Children->Append(log_title); panel->Children->Append(log_border);
    scroll->Content = panel; root->Children->Append(scroll); Window::Current->Content = root; Window::Current->Activate();
}
