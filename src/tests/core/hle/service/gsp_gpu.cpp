// Copyright Denzen Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include <array>
#include <limits>

#include <catch2/catch_test_macros.hpp>

#include "core/core.h"
#include "core/hle/service/gsp/gsp_gpu.h"

namespace Service::GSP {

TEST_CASE("GSP framebuffer cache consumes shared dirty state once", "[core][gsp][service]") {
    FrameBufferStateCache cache;
    FrameBufferUpdate shared{};

    auto result = cache.Access(0, &shared);
    REQUIRE_FALSE(result.updated);
    REQUIRE(result.info == nullptr);
    REQUIRE_FALSE(cache.IsValid(0));

    shared.index.Assign(0);
    shared.is_dirty.Assign(true);
    shared.framebuffer_info[0].address_left = 0x11110000;
    shared.framebuffer_info[0].stride = 720;

    result = cache.Access(0, &shared);
    REQUIRE(result.updated);
    REQUIRE(result.info != nullptr);
    REQUIRE(result.info->address_left == 0x11110000);
    REQUIRE(result.info->stride == 720);
    REQUIRE_FALSE(shared.is_dirty.Value());
    REQUIRE(cache.IsValid(0));

    // Guest writes are invisible until it marks the shared entry dirty again.
    shared.framebuffer_info[0].address_left = 0x22220000;
    result = cache.Access(0, &shared);
    REQUIRE_FALSE(result.updated);
    REQUIRE(result.info->address_left == 0x11110000);

    shared.index.Assign(1);
    shared.is_dirty.Assign(true);
    shared.framebuffer_info[1].address_left = 0x33330000;
    result = cache.Access(0, &shared);
    REQUIRE(result.updated);
    REQUIRE(result.info->address_left == 0x33330000);
    REQUIRE_FALSE(shared.is_dirty.Value());
}

TEST_CASE("GSP framebuffer cache supports safe force refresh and invalid access",
          "[core][gsp][service]") {
    FrameBufferStateCache cache;
    FrameBufferUpdate shared{};

    shared.index.Assign(1);
    shared.framebuffer_info[1].address_right = 0x44440000;

    auto result = cache.Access(1, &shared, true);
    REQUIRE(result.updated);
    REQUIRE(result.info != nullptr);
    REQUIRE(result.info->address_right == 0x44440000);
    REQUIRE(cache.IsValid(1));

    result = cache.Access(2, &shared, true);
    REQUIRE_FALSE(result.updated);
    REQUIRE(result.info == nullptr);

    cache.Reset();
    REQUIRE_FALSE(cache.IsValid(0));
    REQUIRE_FALSE(cache.IsValid(1));
    REQUIRE(cache.Access(0, nullptr).info == nullptr);
}

TEST_CASE("GSP VRAM backup transition plan is deterministic", "[core][gsp][service]") {
    using Mask = VRAMBankBackupMask;
    using State = VramBackupHandler::State;

    auto transition = VramBackupHandler::PlanSave(State::STATE_0, State::STATE_0);
    REQUIRE(transition.applies);
    REQUIRE(transition.mask == Mask::MASK_1);
    REQUIRE(transition.next_state == State::STATE_1);

    transition = VramBackupHandler::PlanSave(State::STATE_1, State::STATE_1);
    REQUIRE(transition.applies);
    REQUIRE(transition.mask == Mask::MASK_2);
    REQUIRE(transition.next_state == State::STATE_2);

    REQUIRE_FALSE(VramBackupHandler::PlanSave(State::STATE_1, State::STATE_0).applies);
    REQUIRE_FALSE(VramBackupHandler::PlanSave(State::STATE_0, State::STATE_2).applies);

    transition = VramBackupHandler::PlanRestore(State::STATE_1, State::STATE_2);
    REQUIRE(transition.applies);
    REQUIRE(transition.mask == Mask::MASK_2);
    REQUIRE(transition.next_state == State::STATE_1);

    transition = VramBackupHandler::PlanRestore(State::STATE_0, State::STATE_1);
    REQUIRE(transition.applies);
    REQUIRE(transition.mask == Mask::MASK_1);
    REQUIRE(transition.next_state == State::STATE_0);

    REQUIRE_FALSE(VramBackupHandler::PlanRestore(State::STATE_0, State::STATE_2).applies);
    REQUIRE_FALSE(VramBackupHandler::PlanRestore(State::STATE_1, State::STATE_0).applies);
}

TEST_CASE("GSP VRAM backup ownership cannot cross sessions", "[core][gsp][service]") {
    VramBackupOwnership ownership;

    REQUIRE_FALSE(ownership.HasOwner());
    REQUIRE(ownership.Claim(1));
    REQUIRE(ownership.IsOwnedBy(1));
    REQUIRE(ownership.Claim(1));
    REQUIRE_FALSE(ownership.Claim(2));
    REQUIRE_FALSE(ownership.Release(2));
    REQUIRE(ownership.Release(1));
    REQUIRE_FALSE(ownership.HasOwner());
}

TEST_CASE("GSP serialized VRAM state requires every bank for a complete restore",
          "[core][gsp][service]") {
    using Handler = VramBackupHandler;
    using State = Handler::State;

    std::array<bool, Handler::BANK_COUNT> valid{};
    std::array<size_t, Handler::BANK_COUNT> sizes{};

    const auto mark_valid = [&](size_t bank) {
        valid[bank] = true;
        sizes[bank] = VRAM_BACKUP_BANK_SIZE;
    };
    for (const size_t bank : std::array<size_t, 8>{3, 4, 5, 7, 8, 9, 10, 11}) {
        mark_valid(bank);
    }

    REQUIRE(Handler::ValidateSerializedState(State::STATE_2, true, valid, sizes));

    valid[7] = false;
    sizes[7] = 0;
    REQUIRE_FALSE(Handler::ValidateSerializedState(State::STATE_2, true, valid, sizes));
    mark_valid(7);

    valid[3] = false;
    sizes[3] = 0;
    REQUIRE_FALSE(Handler::ValidateSerializedState(State::STATE_2, true, valid, sizes));

    mark_valid(3);
    REQUIRE_FALSE(Handler::ValidateSerializedState(State::STATE_2, false, valid, sizes));

    valid = {};
    sizes = {};
    REQUIRE(Handler::ValidateSerializedState(State::STATE_0, false, valid, sizes));

    sizes[0] = 1;
    REQUIRE_FALSE(Handler::ValidateSerializedState(State::STATE_0, false, valid, sizes));
    sizes[0] = 0;

    mark_valid(7);
    REQUIRE_FALSE(Handler::ValidateSerializedState(State::STATE_0, false, valid, sizes));
}

TEST_CASE("GSP VRAM backup storage is lazy", "[core][gsp][service]") {
    Core::System system;
    VramBackupHandler backup(system);

    REQUIRE(backup.GetCurrentState() == VramBackupHandler::State::STATE_0);
    REQUIRE(backup.GetAllocatedBankCount() == 0);
    REQUIRE_FALSE(backup.IsBankValid(0));
    REQUIRE_FALSE(backup.IsBankValid(std::numeric_limits<size_t>::max()));
}

} // namespace Service::GSP
