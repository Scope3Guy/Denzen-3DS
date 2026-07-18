// Copyright Citra Emulator Project / Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <vector>
#include <boost/optional/optional.hpp>
#include <boost/serialization/export.hpp>
#include <boost/serialization/optional.hpp>
#include <boost/serialization/version.hpp>
#include "common/bit_field.h"
#include "common/common_types.h"
#include "core/hle/kernel/event.h"
#include "core/hle/kernel/hle_ipc.h"
#include "core/hle/service/gsp/gsp_command.h"
#include "core/hle/service/gsp/gsp_interrupt.h"
#include "core/hle/service/service.h"
#include "core/memory.h"

namespace Core {
class System;
}

namespace Kernel {
class HLERequestContext;
class Process;
class SharedMemory;
} // namespace Kernel

namespace Service::GSP {

struct FrameBufferInfo {
    static constexpr u32 PIXEL_FORMAT_MASK = 0x7;

    u32 active_fb{}; // 0 = first, 1 = second
    u32 address_left{};
    u32 address_right{};
    u32 stride{};   // maps to 0x1EF00X90 ?
    u32 format{};   // maps to 0x1EF00X70 ?
    u32 shown_fb{}; // maps to 0x1EF00X78 ?
    u32 unknown{};

    u32 GetPixelFormat() const {
        return format & PIXEL_FORMAT_MASK;
    }

    template <class Archive>
    void serialize(Archive& ar, const unsigned int) {
        ar & active_fb;
        ar & address_left;
        ar & address_right;
        ar & stride;
        ar & format;
        ar & shown_fb;
        ar & unknown;
    }
};
static_assert(sizeof(FrameBufferInfo) == 0x1c, "Struct has incorrect size");

struct FrameBufferUpdate {
    BitField<0, 1, u8> index;    // Index used for GSP::SetBufferSwap
    BitField<0, 1, u8> is_dirty; // true if GSP should update GPU framebuffer registers
    u16 pad1;

    FrameBufferInfo framebuffer_info[2];

    u32 pad2;
};
static_assert(sizeof(FrameBufferUpdate) == 0x40, "Struct has incorrect size");
static_assert(offsetof(FrameBufferUpdate, framebuffer_info[1]) == 0x20,
              "FrameBufferInfo element has incorrect alignment");

enum RelayEventQueueFlags : u32 {
    AllowSaveVramSysArea = (1 << 0),
};
DECLARE_ENUM_FLAG_OPERATORS(RelayEventQueueFlags)

// TODO(PabloMK7): Fill this in when the purpose of
// each bit is determined.
enum class VRAMBankBackupMask : u8 {
    MASK_0 = (1 << 0),
    MASK_1 = (1 << 1),
    MASK_2 = (1 << 2),
};
DECLARE_ENUM_FLAG_OPERATORS(VRAMBankBackupMask)

constexpr u32 FRAMEBUFFER_WIDTH = 240;
constexpr u32 FRAMEBUFFER_WIDTH_POW2 = 256;
constexpr u32 TOP_FRAMEBUFFER_HEIGHT = 400;
constexpr u32 BOTTOM_FRAMEBUFFER_HEIGHT = 320;
constexpr u32 FRAMEBUFFER_HEIGHT_POW2 = 512;

// These are the VRAM addresses that GSP copies framebuffers to in SaveVramSysArea.
constexpr VAddr FRAMEBUFFER_SAVE_AREA_TOP_LEFT = Memory::VRAM_VADDR + 0x273000;
constexpr VAddr FRAMEBUFFER_SAVE_AREA_TOP_RIGHT = Memory::VRAM_VADDR + 0x2B9800;
constexpr VAddr FRAMEBUFFER_SAVE_AREA_BOTTOM = Memory::VRAM_VADDR + 0x4C7800;

constexpr size_t VRAM_BACKUP_BANK_SIZE = 0x80000;

class GSP_GPU;

class FrameBufferStateCache {
public:
    struct UpdateResult {
        bool updated{};
        FrameBufferInfo* info{};
    };

    UpdateResult Access(u32 screen_index, FrameBufferUpdate* shared_update,
                        bool force_update = false) {
        if (screen_index >= framebuffer_infos.size()) {
            return {};
        }

        bool updated = false;
        if (shared_update && (shared_update->is_dirty.Value() || force_update)) {
            framebuffer_infos[screen_index] =
                shared_update->framebuffer_info[shared_update->index.Value()];
            shared_update->is_dirty.Assign(false);
            valid[screen_index] = true;
            updated = true;
        }

        return {updated,
                valid[screen_index] ? std::addressof(framebuffer_infos[screen_index]) : nullptr};
    }

    bool IsValid(u32 screen_index) const {
        return screen_index < valid.size() && valid[screen_index];
    }

    void Reset() {
        framebuffer_infos = {};
        valid = {};
    }

private:
    std::array<FrameBufferInfo, 2> framebuffer_infos{};
    std::array<bool, 2> valid{};

    template <class Archive>
    void serialize(Archive& ar, const unsigned int) {
        ar & framebuffer_infos;
        ar & valid;
    }
    friend class boost::serialization::access;
};

class SessionData : public Kernel::SessionRequestHandler::SessionDataBase {
public:
    SessionData() = default;
    SessionData(GSP_GPU* gsp);
    ~SessionData();

    GSP_GPU* gsp;

    /// Event triggered when GSP interrupt has been signalled
    std::shared_ptr<Kernel::Event> interrupt_event;
    /// Thread index into interrupt relay queue
    u32 thread_id;
    /// Whether RegisterInterruptRelayQueue was called for this session
    bool registered = false;
    /// Current relay queue flags
    RelayEventQueueFlags relay_queue_flags{};
    /// Event to be signaled on VBlank
    std::shared_ptr<Kernel::Event> on_vblank_event{};

private:
    template <class Archive>
    void serialize(Archive& ar, const unsigned int);
    friend class boost::serialization::access;
};

class VramBackupOwnership {
public:
    bool Claim(u32 thread_id) {
        if (owner_thread_id && *owner_thread_id != thread_id) {
            return false;
        }
        owner_thread_id = thread_id;
        return true;
    }

    bool IsOwnedBy(u32 thread_id) const {
        return owner_thread_id && *owner_thread_id == thread_id;
    }

    bool HasOwner() const {
        return owner_thread_id.has_value();
    }

    bool Release(u32 thread_id) {
        if (!IsOwnedBy(thread_id)) {
            return false;
        }
        owner_thread_id.reset();
        return true;
    }

    void Reset() {
        owner_thread_id.reset();
    }

private:
    boost::optional<u32> owner_thread_id;

    template <class Archive>
    void serialize(Archive& ar, const unsigned int) {
        ar & owner_thread_id;
    }
    friend class boost::serialization::access;
};

class VramBackupHandler {
public:
    VramBackupHandler() : system(Core::Global<Core::System>()) {}
    explicit VramBackupHandler(Core::System& system_) : system(system_) {}

    // These states and bank masks reflect the observed GSP state machine. Their semantic names
    // remain unknown, so callers use the numeric state names instead of inventing meaning.
    enum class State : u32 { STATE_0, STATE_1, STATE_2 };

    struct Transition {
        VRAMBankBackupMask mask{};
        State next_state{State::STATE_0};
        bool applies{};
    };

    static constexpr size_t BANK_COUNT = Memory::VRAM_SIZE / VRAM_BACKUP_BANK_SIZE;

    static Transition PlanSave(State requested_state, State current_state);
    static Transition PlanRestore(State requested_state, State current_state);
    static bool ValidateSerializedState(State state, bool has_owner,
                                        const std::array<bool, BANK_COUNT>& valid,
                                        const std::array<size_t, BANK_COUNT>& sizes);

    bool CanSaveCompleteCycle() const;
    bool CanRestoreCompleteCycle(u32 owner_thread_id) const;
    bool SaveVRAMWithState(State state, u32 owner_thread_id);
    bool RestoreVRAMWithState(State state, u32 owner_thread_id);

    void ResetState(bool release_storage = true) {
        curr_state = State::STATE_0;
        valid_banks = {};
        ownership.Reset();
        if (release_storage) {
            vram_backup = {};
        }
    }

    State GetCurrentState() const {
        return curr_state;
    }

    bool IsOwnedBy(u32 thread_id) const {
        return ownership.IsOwnedBy(thread_id);
    }

    bool IsBankValid(size_t bank_id) const {
        return bank_id < valid_banks.size() && valid_banks[bank_id];
    }

    size_t GetAllocatedBankCount() const {
        return std::count_if(vram_backup.begin(), vram_backup.end(),
                             [](const auto& bank) { return !bank.empty(); });
    }

private:
    Core::System& system;

    bool SaveVRAMBank(size_t bank_id);
    bool RestoreVRAMBank(size_t bank_id);
    bool AreBanksAccessible(VRAMBankBackupMask mask) const;
    bool AreBanksRestorable(VRAMBankBackupMask mask) const;

    // Vectors avoid large stack frames during serialization. Banks are allocated only when a
    // transition actually saves them.
    std::array<std::vector<u8>, BANK_COUNT> vram_backup{};
    std::array<bool, BANK_COUNT> valid_banks{};
    VramBackupOwnership ownership;

    // TODO: Confirm the exact bank roles on physical hardware. The mapping is retained from the
    // observed GSP implementation, while transitions and storage safety are tested independently.
    static constexpr std::array<VRAMBankBackupMask, BANK_COUNT> mask_per_bank = {
        VRAMBankBackupMask::MASK_0,                              // 0x1F000000 - 0x1F07FFFF
        VRAMBankBackupMask::MASK_0,                              // 0x1F080000 - 0x1F0FFFFF
        VRAMBankBackupMask::MASK_0,                              // 0x1F100000 - 0x1F17FFFF
        VRAMBankBackupMask::MASK_0 | VRAMBankBackupMask::MASK_1, // 0x1F180000 - 0x1F1FFFFF
        VRAMBankBackupMask::MASK_0 | VRAMBankBackupMask::MASK_1, // 0x1F200000 - 0x1F27FFFF
        VRAMBankBackupMask::MASK_0 | VRAMBankBackupMask::MASK_1, // 0x1F280000 - 0x1F2FFFFF
        VRAMBankBackupMask::MASK_0,                              // 0x1F300000 - 0x1F37FFFF
        VRAMBankBackupMask::MASK_0 | VRAMBankBackupMask::MASK_2, // 0x1F380000 - 0x1F3FFFFF
        VRAMBankBackupMask::MASK_0 | VRAMBankBackupMask::MASK_2, // 0x1F400000 - 0x1F47FFFF
        VRAMBankBackupMask::MASK_0 | VRAMBankBackupMask::MASK_1, // 0x1F480000 - 0x1F4FFFFF
        VRAMBankBackupMask::MASK_0 | VRAMBankBackupMask::MASK_1, // 0x1F500000 - 0x1F57FFFF
        VRAMBankBackupMask::MASK_0 | VRAMBankBackupMask::MASK_1, // 0x1F580000 - 0x1F5FFFFF
    };

    State curr_state = State::STATE_0;

    template <class Archive>
    void serialize(Archive& ar, const unsigned int file_version) {
        ar & vram_backup;
        ar & valid_banks;
        ar & curr_state;

        if (file_version >= 1) {
            ar & ownership;
        } else if (Archive::is_loading::value) {
            ownership.Reset();
        }

        if (Archive::is_loading::value) {
            std::array<size_t, BANK_COUNT> sizes{};
            std::transform(vram_backup.begin(), vram_backup.end(), sizes.begin(),
                           [](const auto& bank) { return bank.size(); });
            if (!ValidateSerializedState(curr_state, ownership.HasOwner(), valid_banks, sizes)) {
                ResetState();
            }
        }
    }
    friend class boost::serialization::access;
};

class GSP_GPU final : public ServiceFramework<GSP_GPU, SessionData> {
public:
    explicit GSP_GPU(Core::System& system);
    ~GSP_GPU() = default;

    void ClientDisconnected(std::shared_ptr<Kernel::ServerSession> server_session) override;

    /**
     * Signals that the specified interrupt type has occurred to userland code
     * @param interrupt_id ID of interrupt that is being signalled
     */
    void SignalInterrupt(InterruptId interrupt_id, u64 wait_delay_ns);

    /**
     * Retrieves the framebuffer info stored in the GSP shared memory for the
     * specified screen index and thread id.
     * @param thread_id GSP thread id of the process that accesses the structure that we are
     * requesting.
     * @param screen_index Index of the screen we are requesting (Top = 0, Bottom = 1).
     * @returns Cached framebuffer state and whether this access consumed a new shared update.
     */
    FrameBufferStateCache::UpdateResult GetFrameBufferInfo(u32 thread_id, u32 screen_index,
                                                           bool force_update = false);

    /// Gets a pointer to a thread command buffer in GSP shared memory
    CommandBuffer* GetCommandBuffer(u32 thread_id);

    /// Gets a pointer to the interrupt relay queue for a given thread index
    InterruptRelayQueue* GetInterruptRelayQueue(u32 thread_id);

    /**
     * Retreives the ID of the thread with GPU rights.
     */
    u32 GetActiveThreadId() {
        return thread_id_with_rights;
    }

    /**
     * Retreives the ID of the client thread with GPU rights.
     */
    u32 GetActiveClientThreadId() {
        return active_client_thread_id;
    }

    class ThreadCallback;

private:
    /**
     * Signals that the specified interrupt type has occurred to userland code for the specified GSP
     * thread id.
     * @param interrupt_id ID of interrupt that is being signalled.
     * @param thread_id GSP thread that will receive the interrupt.
     */
    void SignalInterruptForThread(InterruptId interrupt_id, u32 thread_id, u64 wait_delay_ns);

    void ProcessPendingInterrupt(size_t pending_interrupt_id);

    void ProcessPendingInterruptImpl(InterruptId interrupt_id, u32 thread_id);

    /**
     * GSP_GPU::WriteHWRegs service function
     *
     * Writes sequential GSP GPU hardware registers
     *
     *  Inputs:
     *      1 : address of first GPU register
     *      2 : number of registers to write sequentially
     *      4 : pointer to source data array
     */
    void WriteHWRegs(Kernel::HLERequestContext& ctx);

    /**
     * GSP_GPU::WriteHWRegsWithMask service function
     *
     * Updates sequential GSP GPU hardware registers using masks
     *
     *  Inputs:
     *      1 : address of first GPU register
     *      2 : number of registers to update sequentially
     *      4 : pointer to source data array
     *      6 : pointer to mask array
     */
    void WriteHWRegsWithMask(Kernel::HLERequestContext& ctx);

    /// Read a GSP GPU hardware register
    void ReadHWRegs(Kernel::HLERequestContext& ctx);

    /**
     * GSP_GPU::SetBufferSwap service function
     *
     * Updates GPU display framebuffer configuration using the specified parameters.
     *
     *  Inputs:
     *      1 : Screen ID (0 = top screen, 1 = bottom screen)
     *      2-7 : FrameBufferInfo structure
     *  Outputs:
     *      1: Result code
     */
    void SetBufferSwap(Kernel::HLERequestContext& ctx);

    /**
     * GSP_GPU::FlushDataCache service function
     *
     * This Function is a no-op, We aren't emulating the CPU cache any time soon.
     *
     *  Inputs:
     *      1 : Address
     *      2 : Size
     *      3 : Value 0, some descriptor for the KProcess Handle
     *      4 : KProcess handle
     *  Outputs:
     *      1 : Result of function, 0 on success, otherwise error code
     */
    void FlushDataCache(Kernel::HLERequestContext& ctx);

    /**
     * GSP_GPU::InvalidateDataCache service function
     *
     * This Function is a no-op, We aren't emulating the CPU cache any time soon.
     *
     *  Inputs:
     *      1 : Address
     *      2 : Size
     *      3 : Value 0, some descriptor for the KProcess Handle
     *      4 : KProcess handle
     *  Outputs:
     *      1 : Result of function, 0 on success, otherwise error code
     */
    void InvalidateDataCache(Kernel::HLERequestContext& ctx);

    /**
     * GSP_GPU::SetLcdForceBlack service function
     *
     * Enable or disable REG_LCDCOLORFILL with the color black.
     *
     *  Inputs:
     *      1: Black color fill flag (0 = don't fill, !0 = fill)
     *  Outputs:
     *      1: Result code
     */
    void SetLcdForceBlack(Kernel::HLERequestContext& ctx);

    /// This triggers handling of the GX command written to the command buffer in shared memory.
    void TriggerCmdReqQueue(Kernel::HLERequestContext& ctx);

    /**
     * GSP_GPU::SetAxiConfigQoSMode service function
     *  Inputs:
     *      1 : Mode, unused in emulator
     *  Outputs:
     *      1 : Result of function, 0 on success, otherwise error code
     */
    void SetAxiConfigQoSMode(Kernel::HLERequestContext& ctx);

    void SetPerfLogMode(Kernel::HLERequestContext& ctx);

    void GetPerfLog(Kernel::HLERequestContext& ctx);

    /**
     * GSP_GPU::RegisterInterruptRelayQueue service function
     *  Inputs:
     *      1 : "Flags" field, purpose is unknown
     *      3 : Handle to GSP synchronization event
     *  Outputs:
     *      1 : Result of function, 0x2A07 on success, otherwise error code
     *      2 : Thread index into GSP command buffer
     *      4 : Handle to GSP shared memory
     */
    void RegisterInterruptRelayQueue(Kernel::HLERequestContext& ctx);

    /**
     * GSP_GPU::UnregisterInterruptRelayQueue service function
     *  Outputs:
     *      1 : Result of function, 0 on success, otherwise error code
     */
    void UnregisterInterruptRelayQueue(Kernel::HLERequestContext& ctx);

    /**
     * GSP_GPU::TryAcquireRight service function
     *  Inputs:
     *      0 : Header code [0x00150002]
     *      1 : Handle translate header (0x0)
     *      2 : Process handle
     *  Outputs:
     *      1 : Result of function, 0 on success, otherwise error code
     */
    void TryAcquireRight(Kernel::HLERequestContext& ctx);

    /**
     * GSP_GPU::AcquireRight service function
     *  Inputs:
     *      0 : Header code [0x00160042]
     *      1 : Flags
     *      2 : Handle translate header (0x0)
     *      3 : Process handle
     *  Outputs:
     *      1 : Result of function, 0 on success, otherwise error code
     */
    void AcquireRight(Kernel::HLERequestContext& ctx);

    /**
     * GSP_GPU::ReleaseRight service function
     *  Outputs:
     *      1: Result code
     */
    void ReleaseRight(Kernel::HLERequestContext& ctx);

    /**
     * Releases rights to the GPU.
     * Will fail if the session_data doesn't have the GPU right
     */
    void ReleaseRight(SessionData* session_data);

    /**
     * GSP_GPU::ImportDisplayCaptureInfo service function
     *
     * Returns information about the current framebuffer state
     *
     *  Inputs:
     *      0: Header 0x00180000
     *  Outputs:
     *      0: Header Code[0x00180240]
     *      1: Result code
     *      2: Left framebuffer virtual address for the main screen
     *      3: Right framebuffer virtual address for the main screen
     *      4: Main screen framebuffer format
     *      5: Main screen framebuffer width
     *      6: Left framebuffer virtual address for the bottom screen
     *      7: Right framebuffer virtual address for the bottom screen
     *      8: Bottom screen framebuffer format
     *      9: Bottom screen framebuffer width
     */
    void ImportDisplayCaptureInfo(Kernel::HLERequestContext& ctx);

    /**
     * GSP_GPU::SaveVramSysArea service function
     *
     * Returns information about the current framebuffer state
     *
     *  Inputs:
     *      0: Header 0x00190000
     *  Outputs:
     *      0: Header Code[0x00190040]
     *      1: Result code
     */
    void SaveVramSysArea(Kernel::HLERequestContext& ctx);

    /**
     * GSP_GPU::RestoreVramSysArea service function
     *
     * Returns information about the current framebuffer state
     *
     *  Inputs:
     *      0: Header 0x001A0000
     *  Outputs:
     *      0: Header Code[0x001A0040]
     *      1: Result code
     */
    void RestoreVramSysArea(Kernel::HLERequestContext& ctx);

    /**
     * GSP_GPU::StoreDataCache service function
     *
     * This Function is a no-op, We aren't emulating the CPU cache any time soon.
     *
     *  Inputs:
     *      0 : Header code [0x001F0082]
     *      1 : Address
     *      2 : Size
     *      3 : Value 0, some descriptor for the KProcess Handle
     *      4 : KProcess handle
     *  Outputs:
     *      1 : Result of function, 0 on success, otherwise error code
     */
    void StoreDataCache(Kernel::HLERequestContext& ctx);

    /// Force the 3D LED State (0 = On, Non-Zero = Off)
    void SetLedForceOff(Kernel::HLERequestContext& ctx);

    /**
     * GSP_GPU::SetInternalPriorities service function
     *  Inputs:
     *      0 : Header code [0x001E0080]
     *      1 : Session thread priority
     *      2 : Session thread priority with rights
     *  Outputs:
     *      1 : Result of function, 0 on success, otherwise error code
     */
    void SetInternalPriorities(Kernel::HLERequestContext& ctx);

    /// Returns the session data for the specified registered thread id, or nullptr if not found.
    SessionData* FindRegisteredThreadData(u32 thread_id);

    u32 GetUnusedThreadId() const;

    std::unique_ptr<Kernel::SessionRequestHandler::SessionDataBase> MakeSessionData() override;

    Result AcquireGpuRight(const Kernel::HLERequestContext& ctx,
                           const std::shared_ptr<Kernel::Process>& process, u32 flag,
                           bool blocking);

    Core::System& system;

    /// GSP shared memory
    std::shared_ptr<Kernel::SharedMemory> shared_memory;

    /// Thread id that currently has GPU rights or std::numeric_limits<u32>::max() if none.
    u32 thread_id_with_rights = std::numeric_limits<u32>::max();

    /// Thread id of the client thread that has GPU rights
    u32 active_client_thread_id = std::numeric_limits<u32>::max();

    bool first_initialization = true;

    /// Full VRAM snapshot retained only to load save states created before Batch 3.
    boost::optional<std::vector<u8>> legacy_saved_vram;

    /// Maximum number of threads that can be registered at the same time in the GSP module.
    static constexpr u32 MaxGSPThreads = 4;

    /// Thread ids currently in use by the sessions connected to the GSPGPU service.
    std::array<bool, MaxGSPThreads> used_thread_ids{};

    /// The current thread needs a longer emulated texture copy completion
    bool delay_texture_copy_completion{};

    class PendingInterruptArray {
    public:
        PendingInterruptArray() {
            for (size_t i = 0; i < array_size; i++) {
                elements[i].first = InterruptId::COUNT;
            }
        }

        size_t Push(const std::pair<InterruptId, u32> elem) {
            if (elements[head].first != InterruptId::COUNT) {
                // If the head position is occupied, the queue is full
                return std::numeric_limits<size_t>::max();
            }

            elements[head] = elem;
            size_t index = head;
            head = (head + 1) % array_size;
            return index;
        }

        std::optional<std::pair<InterruptId, u32>> Pop(size_t at) {
            if (at >= array_size || elements[at].first == InterruptId::COUNT) {
                // Invalid index or already free
                return std::nullopt;
            }

            std::pair<InterruptId, u32> value = elements[at];
            elements[at].first = InterruptId::COUNT;

            return value;
        }

    private:
        static constexpr size_t array_size = 512;
        size_t head = 0;

        std::array<std::pair<InterruptId, u32>, array_size> elements;

        template <class Archive>
        void serialize(Archive& ar, const unsigned int) {
            ar & elements;
            ar & head;
        }
        friend class boost::serialization::access;
    };

    class PerformanceRecorder {
    public:
        struct PerformanceEntry {
            u32 delta_time{};
            u32 sum_time{};

            template <class Archive>
            void serialize(Archive& ar, const unsigned int) {
                ar & delta_time;
                ar & sum_time;
            }
            friend class boost::serialization::access;
        };

        using PerfArray = std::array<PerformanceEntry, static_cast<u8>(InterruptId::COUNT)>;

        PerformanceRecorder() = default;

        void Reset() {
            entries.fill({});
        }

        bool IsEnabled() {
            return enabled;
        }

        void SetEnabled(bool _enabled) {
            enabled = _enabled;
            if (enabled) {
                Reset();
            }
        }

        void UpdateTime(InterruptId id, u64 nanoseconds) {
            // These counters may overflow, which is normal.
            entries[static_cast<u8>(id)].delta_time = static_cast<u32>(nanoseconds);
            entries[static_cast<u8>(id)].sum_time += static_cast<u32>(nanoseconds);
        }

        const PerfArray& GetResults() {
            return entries;
        }

    private:
        PerfArray entries{};
        bool enabled{};

        template <class Archive>
        void serialize(Archive& ar, const unsigned int) {
            ar & entries;
            ar & enabled;
        }
        friend class boost::serialization::access;
    };

    // This array is only needed to keep track of delayed notifications and simulate the GPU
    // taking some time to finish the work, it doesn't exist on real hardware.
    PendingInterruptArray pending_interrupts;

    PerformanceRecorder perf_recorder;

    Core::TimingEventType* SignalInterruptEventType = nullptr;

    FrameBufferStateCache framebuffer_cache;
    std::array<bool, 2> force_buffer_swap{};

    VramBackupHandler vram_backup_handler;

    friend class SessionData;

    template <class Archive>
    void serialize(Archive& ar, const unsigned int);
    friend class boost::serialization::access;
};

} // namespace Service::GSP

BOOST_CLASS_EXPORT_KEY(Service::GSP::SessionData)
BOOST_CLASS_EXPORT_KEY(Service::GSP::GSP_GPU)
SERVICE_CONSTRUCT(Service::GSP::GSP_GPU)
BOOST_CLASS_VERSION(Service::GSP::VramBackupHandler, 1)
BOOST_CLASS_EXPORT_KEY(Service::GSP::GSP_GPU::ThreadCallback)
BOOST_CLASS_VERSION(Service::GSP::SessionData, 1)
BOOST_CLASS_VERSION(Service::GSP::GSP_GPU, 1)
