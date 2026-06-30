// Copyright 2016 Citra Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include <algorithm>
#include <vector>
#include "common/archives.h"
#include "common/logging/log.h"
#include "core/hle/ipc_helpers.h"
#include "core/hle/service/cfg/cfg_nor.h"

SERIALIZE_EXPORT_IMPL(Service::CFG::CFG_NOR)

namespace Service::CFG {

CFG_NOR::CFG_NOR() : ServiceFramework("cfg:nor", 23) {
    static const FunctionInfo functions[] = {
        // clang-format off
        {0x0001, &CFG_NOR::Initialize, "Initialize"},
        {0x0002, &CFG_NOR::Shutdown, "Shutdown"},
        {0x0005, &CFG_NOR::ReadData, "ReadData"},
        {0x0006, &CFG_NOR::WriteData, "WriteData"},
        // clang-format on
    };
    RegisterHandlers(functions);
}

void CFG_NOR::Initialize(Kernel::HLERequestContext& ctx) {
    IPC::RequestParser rp(ctx);
    const u8 state = rp.Pop<u8>();

    LOG_WARNING(Service_CFG, "(STUBBED) called state={}", state);

    IPC::RequestBuilder rb = rp.MakeBuilder(1, 0);
    rb.Push(ResultSuccess);
}

void CFG_NOR::Shutdown(Kernel::HLERequestContext& ctx) {
    IPC::RequestParser rp(ctx);

    LOG_WARNING(Service_CFG, "(STUBBED) called");

    IPC::RequestBuilder rb = rp.MakeBuilder(1, 0);
    rb.Push(ResultSuccess);
}

void CFG_NOR::ReadData(Kernel::HLERequestContext& ctx) {
    IPC::RequestParser rp(ctx);
    const u32 offset = rp.Pop<u32>();
    const u32 size = rp.Pop<u32>();
    auto& buffer = rp.PopMappedBuffer();

    LOG_WARNING(Service_CFG, "(STUBBED) called offset=0x{:08X}, size=0x{:08X}", offset, size);

    std::vector<u8> data(std::min<std::size_t>(static_cast<std::size_t>(size), buffer.GetSize()));
    buffer.Write(data.data(), 0, data.size());

    IPC::RequestBuilder rb = rp.MakeBuilder(1, 2);
    rb.Push(ResultSuccess);
    rb.PushMappedBuffer(buffer);
}

void CFG_NOR::WriteData(Kernel::HLERequestContext& ctx) {
    IPC::RequestParser rp(ctx);
    const u32 offset = rp.Pop<u32>();
    const u32 size = rp.Pop<u32>();
    auto& buffer = rp.PopMappedBuffer();

    LOG_WARNING(Service_CFG, "(STUBBED) called offset=0x{:08X}, size=0x{:08X}", offset, size);

    IPC::RequestBuilder rb = rp.MakeBuilder(1, 2);
    rb.Push(ResultSuccess);
    rb.PushMappedBuffer(buffer);
}

} // namespace Service::CFG
