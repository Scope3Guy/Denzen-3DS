// Copyright Citra Emulator Project / Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#pragma once

#include "core/hle/service/service.h"

namespace Core {
class System;
}

namespace Service::HB {

class HB_LDR final : public ServiceFramework<HB_LDR> {
public:
    HB_LDR();

private:
    SERVICE_SERIALIZATION_SIMPLE
};

void InstallInterfaces(Core::System& system);

} // namespace Service::HB

BOOST_CLASS_EXPORT_KEY(Service::HB::HB_LDR)