#pragma once

#include "App.g.h"

namespace BOINC_Xbox
{
    ref class App sealed
    {
    public:
        App();

    protected:
        virtual void OnLaunched(Windows::ApplicationModel::Activation::LaunchActivatedEventArgs^ e) override;
    };
}
