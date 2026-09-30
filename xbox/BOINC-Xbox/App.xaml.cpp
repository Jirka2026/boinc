#include "pch.h"
#include "App.xaml.h"
#include "MainPage.xaml.h"

using namespace BOINC_Xbox;
using namespace Platform;
using namespace Windows::ApplicationModel::Activation;
using namespace Windows::UI::Xaml;
using namespace Windows::UI::Xaml::Controls;
using namespace Windows::UI::Xaml::Interop;

App::App()
{
    InitializeComponent();
}

void App::OnLaunched(LaunchActivatedEventArgs^ e)
{
    auto rootFrame = dynamic_cast<Frame^>(Window::Current->Content);

    if (rootFrame == nullptr)
    {
        rootFrame = ref new Frame();
        Window::Current->Content = rootFrame;
    }

    if (rootFrame->Content == nullptr)
    {
        rootFrame->Navigate(TypeName(MainPage::typeid), e->Arguments);
    }

    Window::Current->Activate();
}
