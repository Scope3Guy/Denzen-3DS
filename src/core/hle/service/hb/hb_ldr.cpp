// Copyright Citra Emulator Project / Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include "common/archives.h"
#include "core/core.h"
#include "core/hle/service/hb/hb_ldr.h"

SERIALIZE_EXPORT_IMPL(Service::HB::HB_LDR)

namespace Service::HB {

HB_LDR::HB_LDR() : ServiceFramework("hb:ldr", 1) {}

void InstallInterfaces(Core::System& system) {
    std::make_shared<HB_LDR>()->InstallAsNamedPort(system.Kernel());
}

} // namespace Service::HB