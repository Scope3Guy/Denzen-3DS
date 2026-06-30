// Copyright 2016 Citra Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include "common/archives.h"
#include "common/logging/log.h"
#include "core/core.h"
#include "core/hle/ipc_helpers.h"
#include "core/hle/kernel/shared_page.h"
#include "core/hle/service/nwm/nwm_ext.h"

SERIALIZE_EXPORT_IMPL(Service::NWM::NWM_EXT)
SERVICE_CONSTRUCT_IMPL(Service::NWM::NWM_EXT)

namespace Service::NWM {

NWM_EXT::NWM_EXT(Core::System& _system) : ServiceFramework("nwm::EXT"), system(_system) {
    static const FunctionInfo functions[] = {
        // clang-format off
        {0x0008, &NWM_EXT::ControlWirelessEnabled, "ControlWirelessEnabled"},
        // clang-format on
    };
    RegisterHandlers(functions);
}

void NWM_EXT::ControlWirelessEnabled(Kernel::HLERequestContext& ctx) {
    IPC::RequestParser rp(ctx);
    const bool enabled = rp.Pop<bool>();

    auto& shared_page = system.Kernel().GetSharedPageHandler();
    shared_page.SetWifiLinkLevel(enabled ? SharedPage::WifiLinkLevel::Best
                                         : SharedPage::WifiLinkLevel::Off);
    shared_page.SetWifiState(enabled ? SharedPage::WifiState::Internet
                                     : SharedPage::WifiState::Disabled);

    IPC::RequestBuilder rb = rp.MakeBuilder(1, 0);
    rb.Push(ResultSuccess);

    LOG_WARNING(Service_NWM, "(STUBBED) enabled={}", enabled);
}

} // namespace Service::NWM