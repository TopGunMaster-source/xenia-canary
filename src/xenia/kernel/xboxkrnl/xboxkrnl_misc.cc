/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "xenia/base/exception_handler.h"
#include "xenia/base/filesystem.h"
#include "xenia/base/logging.h"
#include "xenia/base/memory.h"
#include "xenia/base/platform.h"
#include "xenia/cpu/backend/backend.h"
#include "xenia/cpu/backend/code_cache.h"
#include "xenia/cpu/function.h"
#include "xenia/cpu/module.h"
#include "xenia/cpu/ppc/ppc_context.h"
#include "xenia/cpu/ppc/ppc_frontend.h"
#include "xenia/cpu/processor.h"
#include "xenia/emulator.h"
#include "xenia/kernel/kernel_state.h"
#include "xenia/kernel/util/shim_utils.h"
#include "xenia/kernel/xboxkrnl/xboxkrnl_private.h"
#include "xenia/kernel/xthread.h"
#include "xenia/xbox.h"

// User-mode faults restart the thread from a captured host context.
#if XE_PLATFORM_WIN32 && XE_ARCH_AMD64
#define XE_USER_MODE_FAULTS 1
#include "xenia/base/platform_win.h"
#else
#define XE_USER_MODE_FAULTS 0
#endif

DEFINE_bool(allow_mic_initialization, false,
            "Enable Mic Initialization\n"
            " Only set true when testing mic related functions",
            "Kernel");

namespace xe {
namespace kernel {
namespace xboxkrnl {

// Whether KeInsertQueueDpc hands DPCs to the timer thread (see
// QueueBackcompatDpc).
static std::atomic<bool> g_dpcs_on_timer_thread{false};

enum class XMicRequestType : uint16_t {
  MicGetStatus = 0x0000,
  MicStart = 0x0001,
  MicSetGain = 0x0002,
  MicGetData = 0x0007,
  MicGetSampleRates = 0x0008,
  MicGetCapabilities = 0x0009,
  MicUnk = 0x000B,
};

enum class XMicState : uint32_t {
  MicNotConnected = 0x00000000,
  MicConnected = 0x00000001,
  MicConnected2 = 0x00000002,
  MicConnected3 = 0x00000003,
  MicRecording = 0x00000005,
};

struct X_MIC_INFO {
  xe::be<XMicRequestType> request_type;  // 0
  xe::be<uint16_t> user_index;           // 2
  xe::be<XMicState> state;               // 4

  // These fields are only for requests that use overlap. Basically anything
  // other than MicGetStatus. Seems like there is also an event object which
  // creation isn't reported via kernel call.
  xe::be<uint32_t> unk1;            // 8
  xe::be<uint32_t> event_ptr;       // 0xC
  xe::be<uint32_t> completion_ptr;  // 0x10
  xe::be<uint32_t> unk4;            // 0x14
};

struct X_MIC_CAPABILITIES {
  /* Hardcoded fields for 0x6000 type aka. MicDevice. 0x5000 type (Rmc) does not
    have any hardcoded fields. features - 0 frame_length - 0x3E8 mic_color -
    0xFF
  */
  xe::be<uint32_t> features;
  xe::be<uint16_t> format_tag;  // Always 1
  xe::be<uint16_t> channels;
  xe::be<uint32_t> sample_rates;
  xe::be<uint16_t> bits_per_sample;
  xe::be<uint16_t> frame_length;
  xe::be<uint8_t> mic_color;
  xe::be<uint16_t> vendor_id;
  xe::be<uint16_t> product_id;
  xe::be<uint16_t> revision;
  xe::be<uint32_t> device_id;
};
static_assert_size(X_MIC_CAPABILITIES, 0x1C);

struct X_MIC_DEVICE {
  X_MIC_INFO info;
  // There seems to be one huge union here that depends on the request type.
  X_MIC_CAPABILITIES capabilities;
};

void KeEnableFpuExceptions_entry(
    const ppc_context_t& ctx) {  // dword_t enabled) {
  // TODO(benvanik): can we do anything about exceptions?
  // theres a lot more thats supposed to happen here, the floating point state
  // has to be saved to kthread, the irql changes, the machine state register is
  // changed to enable exceptions

  X_KTHREAD* kthread = ctx->TranslateVirtual(
      ctx->TranslateVirtualGPR<X_KPCR*>(ctx->r[13])->prcb_data.current_thread);
  kthread->fpu_exceptions_on = static_cast<uint32_t>(ctx->r[3]) != 0;
}
DECLARE_XBOXKRNL_EXPORT1(KeEnableFpuExceptions, kNone, kStub);

void KeSaveFloatingPointState_entry(const ppc_context_t& ctx) {
  // Probably we should use: thread_fpu_related to store/restore state
  X_KTHREAD* kthread = ctx->TranslateVirtual(
      ctx->TranslateVirtualGPR<X_KPCR*>(ctx->r[13])->prcb_data.current_thread);

  for (size_t i = 0; i < xe::countof(ctx->f); ++i) {
    kthread->fpu_context[i] = ctx->f[i];
  }
}
DECLARE_XBOXKRNL_EXPORT1(KeSaveFloatingPointState, kNone, kSketchy);

void KeRestoreFloatingPointState_entry(const ppc_context_t& ctx) {
  const X_KTHREAD* kthread = ctx->TranslateVirtual(
      ctx->TranslateVirtualGPR<X_KPCR*>(ctx->r[13])->prcb_data.current_thread);

  for (size_t i = 0; i < xe::countof(ctx->f); ++i) {
    ctx->f[i] = kthread->fpu_context[i];
  }
}
DECLARE_XBOXKRNL_EXPORT1(KeRestoreFloatingPointState, kNone, kSketchy);

static qword_result_t KeQueryInterruptTime_entry(const ppc_context_t& ctx) {
  auto kstate = ctx->kernel_state;
  uint32_t ts_bundle = kstate->GetKeTimestampBundle();
  X_TIME_STAMP_BUNDLE* bundle =
      ctx->TranslateVirtual<X_TIME_STAMP_BUNDLE*>(ts_bundle);

  return xe::load_and_swap<uint64_t>(&bundle->interrupt_time);
}
DECLARE_XBOXKRNL_EXPORT1(KeQueryInterruptTime, kNone, kImplemented);

dword_result_t MicDeviceRequest_entry(pointer_t<X_MIC_DEVICE> device_ptr) {
  if (!device_ptr) {
    return X_STATUS_INVALID_PARAMETER;
  }
  XELOGE("MicDeviceRequest State: {:08X} Action: {:04X} USER: {:08X}",
         static_cast<uint32_t>(device_ptr->info.state.get()),
         static_cast<uint16_t>(device_ptr->info.request_type.get()),
         device_ptr->info.user_index.get());
  if (device_ptr->info.user_index > XUserMaxUserCount) {
    return X_STATUS_INVALID_PARAMETER;
  }

  if (device_ptr->info.request_type > XMicRequestType::MicUnk) {
    return X_STATUS_INVALID_DEVICE_REQUEST;
  }

  device_ptr->info.state = cvars::allow_mic_initialization
                               ? XMicState::MicConnected
                               : XMicState::MicNotConnected;

  switch (device_ptr->info.request_type) {
    case XMicRequestType::MicGetStatus:
      return X_ERROR_SUCCESS;
    case XMicRequestType::MicStart:
    case XMicRequestType::MicGetData:
    case XMicRequestType::MicGetSampleRates:
      break;
    case XMicRequestType::MicGetCapabilities:
      device_ptr->capabilities.features = 0x100;
      device_ptr->capabilities.format_tag = 1;
      device_ptr->capabilities.mic_color = 0;
      break;
    default:
      break;
  }

  // This should be handled by DPC.
  if (device_ptr->info.completion_ptr) {
    // Not sure what args should be put there, but seems like negative r4 causes
    // instant bailout. For now leave it hardcoded to failure.
    uint64_t args[] = {device_ptr.guest_address(),
                       static_cast<uint64_t>(X_STATUS_UNSUCCESSFUL)};

    kernel_state()->processor()->Execute(
        XThread::GetCurrentThread()->thread_state(),
        device_ptr->info.completion_ptr, args, xe::countof(args));
  }

  return X_STATUS_PENDING;
}
DECLARE_XBOXKRNL_EXPORT1(MicDeviceRequest, kNone, kStub);

// ---------------------------------------------------------------------------
// Backward-compatibility (xefu) support, research stage.
//
// The 360's original-Xbox emulator runs the Xbox game in a separate user-mode
// address space that it describes with hardware page table entries and
// switches into with KeEnterUserMode. Xenia does not model that yet; these
// stubs log what xefu passes so the layout can be learned from real calls.
// ---------------------------------------------------------------------------

static void LogBackcompatCall(const char* name, const ppc_context_t& ctx,
                              uint32_t dump_reg = 0, uint32_t dump_len = 0) {
  static std::atomic<uint32_t> calls{0};
  if (calls.fetch_add(1) > 400) {
    return;
  }
  XELOGW(
      "BACKCOMPAT {}(r3={:08X} r4={:08X} r5={:08X} r6={:08X} r7={:08X} "
      "r8={:08X}) lr={:08X}",
      name, uint32_t(ctx->r[3]), uint32_t(ctx->r[4]), uint32_t(ctx->r[5]),
      uint32_t(ctx->r[6]), uint32_t(ctx->r[7]), uint32_t(ctx->r[8]),
      uint32_t(ctx->lr));
  if (!dump_reg || !dump_len) {
    return;
  }
  uint32_t addr = uint32_t(ctx->r[dump_reg]) & ~3u;
  auto heap = addr ? kernel_memory()->LookupHeap(addr) : nullptr;
  uint32_t protect = 0;
  uint32_t last_protect = 0;
  if (!heap || !heap->QueryProtect(addr, &protect) ||
      !(protect & kMemoryProtectRead) ||
      !heap->QueryProtect(addr + dump_len - 1, &last_protect) ||
      !(last_protect & kMemoryProtectRead)) {
    return;
  }
  auto p = kernel_memory()->TranslateVirtual<const uint8_t*>(addr);
  for (uint32_t off = 0; off < dump_len; off += 32) {
    std::string line;
    for (uint32_t i = 0; i < 32; i += 4) {
      line += fmt::format(" {:08X}", xe::load_and_swap<uint32_t>(p + off + i));
    }
    XELOGW("BACKCOMPAT   r{}+{:04X}:{}", dump_reg, off, line);
  }
}

static void DumpGuest(const char* label, uint32_t addr, uint32_t len) {
  addr &= ~3u;
  auto heap = addr ? kernel_memory()->LookupHeap(addr) : nullptr;
  uint32_t protect = 0, last_protect = 0;
  if (!heap || !heap->QueryProtect(addr, &protect) ||
      !(protect & kMemoryProtectRead) ||
      !heap->QueryProtect(addr + len - 1, &last_protect) ||
      !(last_protect & kMemoryProtectRead)) {
    XELOGW("BACKCOMPAT   {} @{:08X}: not mapped", label, addr);
    return;
  }
  auto p = kernel_memory()->TranslateVirtual<const uint8_t*>(addr);
  for (uint32_t off = 0; off < len; off += 32) {
    std::string line;
    bool nonzero = false;
    for (uint32_t i = 0; i < 32; i += 4) {
      uint32_t v = xe::load_and_swap<uint32_t>(p + off + i);
      nonzero |= v != 0;
      line += fmt::format(" {:08X}", v);
    }
    if (nonzero) {
      XELOGW("BACKCOMPAT   {} @{:08X}:{}", label, addr + off, line);
    }
  }
}

// The page table xefu hands KeCreateUserMode (r4). Each 32-bit entry at
// index 0x100 + n maps user segment n (16 MB at n << 24) to the physical
// 16 MB frame in its top byte, with attribute bits in the low 12 bits.
static uint32_t g_user_page_table = 0;
static void LogUserPageTableChanges();

static uint32_t UserSegmentEntry(uint32_t segment) {
  if (!g_user_page_table) {
    return 0;
  }
  auto table =
      kernel_memory()->TranslateVirtual<const uint8_t*>(g_user_page_table);
  return xe::load_and_swap<uint32_t>(table + 4 * (0x100 + segment));
}

// Kernel-visible address of a user-mode address (through the 16 MB-page
// physical view at 0xC0000000), or 0 if its segment is unmapped.
static uint32_t UserToKernelAddress(uint32_t user_address) {
  uint32_t entry = UserSegmentEntry(user_address >> 24);
  if (!entry) {
    return 0;
  }
  return 0xC0000000u | (entry & 0x1F000000u) | (user_address & 0x00FFFFFFu);
}

dword_result_t KeCreateUserMode_entry(const ppc_context_t& ctx) {
  g_dpcs_on_timer_thread = true;
  g_user_page_table = uint32_t(ctx->r[4]);
  // xefu writes through a null pointer (its WriteFile wrapper stores the byte
  // count even when the caller passes no place for it), which the 360 allows,
  // so the zero page must be writable.
  if (auto heap = kernel_state()->memory()->LookupHeap(0)) {
    heap->Protect(0, 0x10000, kMemoryProtectRead | kMemoryProtectWrite);
  }
  LogBackcompatCall("KeCreateUserMode", ctx);
  DumpGuest("r4", uint32_t(ctx->r[4]), 0x1000);
  DumpGuest("r6", uint32_t(ctx->r[6]), 0x800);
  DumpGuest("r7", uint32_t(ctx->r[7]), 0x800);
  return X_STATUS_SUCCESS;
}
DECLARE_XBOXKRNL_EXPORT1(KeCreateUserMode, kNone, kStub);

// ---- Emulated user mode -----------------------------------------------
//
// xefu runs the original Xbox game's translated code in user mode, in an
// address space of its own described by the page table above, and is called
// back whenever that code makes a system call (or faults). Here user mode is a
// second view of guest memory (an alias space) used as the thread's membase
// while user code runs; system calls are delivered to xefu's handler as a
// trap frame in kernel memory.

// User code lives in these segments; it is only ever executed in user mode,
// so code there is read from the alias space when translated.
constexpr uint32_t kUserCodeLow = 0xB0000000;
constexpr uint32_t kUserCodeHigh = 0xC0000000;

// Trap frame layout xefu reads (offsets from the frame it is handed).
constexpr uint32_t kFrameGpr = 0x50;   // 32 x u64
constexpr uint32_t kFrameCtr = 0x1A0;  // u64
constexpr uint32_t kFrameLr = 0x1A8;
constexpr uint32_t kFrameIar = 0x1AC;
constexpr uint32_t kFrameCr = 0x1B0;
constexpr uint32_t kFrameXer = 0x1B8;
constexpr uint32_t kFrameSize = 0x200;

// CONTEXT (0xA40 bytes) fields for the starting state.
constexpr uint32_t kCtxIar = 0x08;
constexpr uint32_t kCtxLr = 0x0C;
constexpr uint32_t kCtxCtr = 0x10;
constexpr uint32_t kCtxGpr = 0x18;
constexpr uint32_t kCtxCr = 0x118;
constexpr uint32_t kCtxXer = 0x11C;
constexpr uint32_t kCtxFpr = 0x128;
constexpr uint32_t kCtxVr = 0x240;

class UserModeCodeModule : public cpu::Module {
 public:
  explicit UserModeCodeModule(cpu::Processor* processor)
      : cpu::Module(processor) {}
  const std::string& name() const override {
    static const std::string name = "xefu user mode";
    return name;
  }
  bool is_executable() const override { return false; }
  bool ContainsAddress(uint32_t address) override {
    return address >= kUserCodeLow && address < kUserCodeHigh;
  }

 protected:
  std::unique_ptr<cpu::Function> CreateFunction(uint32_t address) override {
    return std::unique_ptr<cpu::Function>(
        processor_->backend()->CreateGuestFunction(this, address));
  }
};

struct UserModeExit {
  uint32_t status;
};

// Exception record handed to xefu's handler (EXCEPTION_RECORD layout).
constexpr uint32_t kRecordCode = 0x00;
constexpr uint32_t kRecordAddress = 0x0C;
constexpr uint32_t kRecordParamCount = 0x10;
constexpr uint32_t kRecordParams = 0x14;
constexpr uint32_t kRecordSize = 0x50;

struct UserModeFault {
  bool pending = false;
  uint32_t iar = 0;      // faulting instruction
  uint32_t address = 0;  // data address
  bool is_write = false;
};

struct UserModeState {
  uint32_t handler = 0;
  uint32_t frame = 0;   // trap frame, kernel memory
  uint32_t record = 0;  // exception record, kernel memory
  uint32_t next_iar = 0;
  uint32_t guest_stack_depth = 0;
  UserModeFault fault;
  // The last fault answered by mapping the page and retrying (so a fault that
  // comes back at once is emulated instead).
  uint32_t remap_iar = 0;
  uint32_t remap_address = 0;
  cpu::ppc::PPCContext kernel_regs;
  cpu::ppc::PPCContext user_regs;
#if XE_USER_MODE_FAULTS
  // Where the thread restarts after a fault in user code: inside
  // KeEnterUserMode, with the guest code's host frames abandoned.
  CONTEXT restart;
#endif
};

static uint8_t* g_alias_base = nullptr;
static cpu::Module* g_user_module = nullptr;
static uint32_t g_alias_entries[0x100] = {};
static thread_local UserModeState* t_user_mode = nullptr;

// Where xefu's user-mode thread is, by source line, for diagnosing hangs
// (written to a file by the heartbeat, which doesn't need the log).
static std::atomic<uint32_t> g_phase{0};
#define XEFU_PHASE() g_phase.store(__LINE__, std::memory_order_relaxed)
static std::atomic<uint32_t> g_syscall_logs{0};

static void SetCrFromValue(cpu::ppc::PPCContext* pc, uint32_t cr) {
  auto fields = reinterpret_cast<uint8_t*>(&pc->cr0.value);
  for (int i = 0; i < 8; ++i) {
    uint32_t field = (cr >> (28 - 4 * i)) & 0xF;
    fields[i * 4 + 0] = (field >> 3) & 1;
    fields[i * 4 + 1] = (field >> 2) & 1;
    fields[i * 4 + 2] = (field >> 1) & 1;
    fields[i * 4 + 3] = field & 1;
  }
}
static uint32_t XerValue(const cpu::ppc::PPCContext* pc) {
  return (uint32_t(pc->xer_so & 1) << 31) | (uint32_t(pc->xer_ov & 1) << 30) |
         (uint32_t(pc->xer_ca & 1) << 29);
}
static void SetXer(cpu::ppc::PPCContext* pc, uint32_t xer) {
  pc->xer_so = (xer >> 31) & 1;
  pc->xer_ov = (xer >> 30) & 1;
  pc->xer_ca = (xer >> 29) & 1;
}

#if XE_USER_MODE_FAULTS
static bool UserModeFaultCallback(Exception* ex, void* data);
#endif

// xefu's page table structure (at KeCreateUserMode's r4), as observed:
// * +0x400 + 4 * n: 16 MB pages for user segment n (n << 24): physical frame
//   in the top byte, PowerPC page attributes in the low bits (PP in bits 0-1).
// * +0x840 + (address >> 28): page size of each 256 MB: bit 1 set for 64 KB
//   pages, else 4 KB pages.
// * 64 KB pages: +0x800 + 2 * (address >> 27), for each 128 MB, the physical
//   address >> 13 of a table of 2048 32-bit entries (physical page |
//   attributes); 0 if none. This is how the Xbox's RAM (user 0x80000000+) is
//   mapped.
// * 4 KB pages: +2 * (address >> 23), for each 8 MB, the same for a table of
//   2048 4 KB pages.
// PP 2 is read/write for user mode, 1 and 3 read only, 0 no access.
constexpr uint32_t kUserPageShift = 16;
constexpr uint32_t kUserPageCount = 1u << (32 - kUserPageShift);
// Per 64 KB page: mapped physical address | 1 (read only) or 2 (read/write),
// or 0.
static std::vector<uint32_t> g_alias_pages;

// With page-granular alias views (see Memory::alias_page_granular): per 4 KB
// page, the same, for 64 KB pages whose 4 KB pages are mapped one by one
// (g_alias_has_small).
static std::vector<uint32_t> g_alias_small;
static std::vector<uint8_t> g_alias_has_small;

// Unmaps the 4 KB views of a 64 KB page; returns how many there were.
static uint32_t UnmapSmallUserPages(uint32_t page) {
  uint32_t count = 0;
  for (uint32_t k = 0; k < 16; ++k) {
    uint32_t sub_page = (page << 4) | k;
    if (g_alias_small[sub_page]) {
      kernel_memory()->UnmapAliasRange(
          g_alias_base, (page << kUserPageShift) + (k << 12), 0x1000);
      g_alias_small[sub_page] = 0;
      ++count;
    }
  }
  g_alias_has_small[page] = 0;
  return count;
}

static uint32_t PageMappingFromEntry(uint32_t entry, uint32_t physical) {
  switch (entry & 3) {
    case 2:
      return physical | 2;
    case 1:
    case 3:
      return physical | 1;
    default:
      return 0;
  }
}

// Brings one 64 KB page of the alias space in line with xefu's page table.
static void SyncUserPage(uint32_t page, uint32_t* mapped_count,
                         uint32_t* unmapped_count) {
  auto memory = kernel_memory();
  auto table = memory->TranslateVirtual<const uint8_t*>(g_user_page_table);
  bool granular = memory->alias_page_granular();
  uint32_t& mapped = *mapped_count;
  uint32_t& unmapped = *unmapped_count;
  static std::atomic<uint32_t> page_logs{0};
  uint32_t address = page << kUserPageShift;
  if (g_alias_entries[address >> 24]) {
    return;  // covered by a 16 MB page
  }
  uint32_t want = 0;
  bool use_small = false;
  uint32_t want_small[16] = {};
  if (table[0x840 + (address >> 28)] & 2) {
    // 64 KB pages: a table per 128 MB.
    uint32_t table_page =
        xe::load_and_swap<uint16_t>(table + 0x800 + 2 * (address >> 27));
    if (table_page) {
      auto entries =
          memory->TranslatePhysical<const uint8_t*>(uint32_t(table_page) << 13);
      uint32_t entry =
          xe::load_and_swap<uint32_t>(entries + 4 * ((address >> 16) & 0x7FF));
      if (entry) {
        want = PageMappingFromEntry(entry, entry & 0x1FFF0000u);
      }
    }
  } else {
    // 4 KB pages: a table per 8 MB. Without page-granular views, the 16
    // pages here map as one 64 KB view if they're physically contiguous and
    // 64 KB aligned (unmapped ones then become accessible).
    uint32_t table_page =
        xe::load_and_swap<uint16_t>(table + 2 * (address >> 23));
    if (table_page) {
      auto entries =
          memory->TranslatePhysical<const uint8_t*>(uint32_t(table_page) << 13);
      uint32_t base = 0, access = 0, present = 0;
      bool consistent = true, same_access = true;
      for (uint32_t k = 0; k < 16; ++k) {
        uint32_t entry = xe::load_and_swap<uint32_t>(
            entries + 4 * (((address >> 12) & 0x7FF) + k));
        uint32_t mapping =
            entry ? PageMappingFromEntry(entry, entry & 0x1FFFF000u) : 0;
        want_small[k] = mapping;
        if (!mapping) {
          // Not mapped (a guard page below a stack, say): the others still
          // are, just not as one 64 KB view.
          continue;
        }
        ++present;
        uint32_t page_base = (entry & 0x1FFFF000u) - (k << 12);
        if (!access) {
          base = page_base;
        } else {
          if (page_base != base) {
            consistent = false;
          }
          if ((mapping & 3) != access) {
            same_access = false;
          }
        }
        access = std::max(access, mapping & 3);
      }
      bool whole = access && consistent && present == 16 && !(base & 0xFFFF);
      if (granular && access && !(whole && same_access)) {
        use_small = true;
      } else if (whole) {
        want = base | access;
      } else if (access && page_logs.fetch_add(1) < 20) {
        XELOGW(
            "BACKCOMPAT can't map user 4 KB pages at {:08X} as 64 KB "
            "(physical {:08X}{})",
            address, base, consistent ? ", unaligned" : ", scattered");
      }
    }
  }
  // 0xE0000000+ is mapped like the rest: user-mode code is translated
  // without Xenia's 4 KB host offset for those addresses.
  if (use_small) {
    if (g_alias_pages[page]) {
      memory->UnmapAliasRange(g_alias_base, address, 1u << kUserPageShift);
      g_alias_pages[page] = 0;
      ++unmapped;
    }
    g_alias_has_small[page] = 1;
    for (uint32_t k = 0; k < 16; ++k) {
      uint32_t sub_page = (page << 4) | k;
      uint32_t have_small = g_alias_small[sub_page];
      if (want_small[k] == have_small) {
        continue;
      }
      uint32_t small_address = address + (k << 12);
      if (have_small) {
        memory->UnmapAliasRange(g_alias_base, small_address, 0x1000);
        ++unmapped;
      }
      g_alias_small[sub_page] = 0;
      if (want_small[k] && memory->MapAliasRange(g_alias_base, small_address,
                                                 0x1000, want_small[k] & ~3u,
                                                 (want_small[k] & 3) == 2)) {
        g_alias_small[sub_page] = want_small[k];
        ++mapped;
      }
    }
    return;
  }
  if (g_alias_has_small[page]) {
    unmapped += UnmapSmallUserPages(page);
  }
  uint32_t have = g_alias_pages[page];
  if (want == have) {
    return;
  }
  if (have) {
    memory->UnmapAliasRange(g_alias_base, address, 1u << kUserPageShift);
    ++unmapped;
  }
  g_alias_pages[page] = 0;
  if (want && memory->MapAliasRange(g_alias_base, address, 1u << kUserPageShift,
                                    want & ~3u, (want & 3) == 2)) {
    g_alias_pages[page] = want;
    ++mapped;
  }
}

// Brings the alias space in line with xefu's page table.
static bool SyncUserSpace() {
  XEFU_PHASE();
  auto memory = kernel_memory();
  if (!g_alias_base) {
    g_alias_base = memory->CreateAliasSpace();
    if (!g_alias_base) {
      return false;
    }
    g_alias_pages.assign(kUserPageCount, 0);
    g_alias_has_small.assign(kUserPageCount, 0);
    g_alias_small.assign(kUserPageCount * 16, 0);
    memory->SetCodeAliasRange(g_alias_base, kUserCodeLow, kUserCodeHigh);
    auto processor = kernel_state()->processor();
    auto module = std::make_unique<UserModeCodeModule>(processor);
    g_user_module = module.get();
    processor->AddModule(std::move(module));
#if XE_USER_MODE_FAULTS
    // Ahead of the emulator's guest crash handler, which would otherwise
    // stop at the first fault in user code.
    ExceptionHandler::InstallFirst(UserModeFaultCallback, nullptr);
#endif
  }
  if (!g_user_page_table) {
    return true;
  }
  auto table = memory->TranslateVirtual<const uint8_t*>(g_user_page_table);

  // 16 MB pages: one view per segment.
  for (uint32_t seg = 0; seg < 0x100; ++seg) {
    uint32_t entry = UserSegmentEntry(seg);
    if (entry == g_alias_entries[seg]) {
      continue;
    }
    if (g_alias_entries[seg]) {
      memory->UnmapAliasRange(g_alias_base, seg << 24, 0x01000000);
    }
    g_alias_entries[seg] = 0;
    if (entry) {
      // Pages of this segment mapped individually before go first.
      for (uint32_t page = seg << 8; page < (seg + 1) << 8; ++page) {
        if (g_alias_pages[page]) {
          memory->UnmapAliasRange(g_alias_base, page << kUserPageShift,
                                  1u << kUserPageShift);
          g_alias_pages[page] = 0;
        }
        if (g_alias_has_small[page]) {
          UnmapSmallUserPages(page);
        }
      }
      if (memory->MapAliasRange(g_alias_base, seg << 24, 0x01000000,
                                entry & 0x1F000000u)) {
        g_alias_entries[seg] = entry;
        XELOGI("BACKCOMPAT user segment {:02X}000000 -> physical {:08X}", seg,
               entry & 0x1F000000u);
      }
    }
  }

  // 64 KB and 4 KB pages: one view per 64 KB, or, where 4 KB pages don't
  // line up and the host allows it, one per 4 KB page.
  static std::atomic<uint32_t> page_logs{0};
  uint32_t mapped = 0, unmapped = 0;
  for (uint32_t page = 0; page < kUserPageCount; ++page) {
    SyncUserPage(page, &mapped, &unmapped);
  }
  if ((mapped || unmapped) && page_logs.fetch_add(1) < 40) {
    XELOGI("BACKCOMPAT user 64 KB pages: {} mapped, {} unmapped", mapped,
           unmapped);
  }
  return true;
}

static void WriteFrame(UserModeState* st, uint32_t iar) {
  auto f = kernel_memory()->TranslateVirtual<uint8_t*>(st->frame);
  std::memset(f, 0, kFrameSize);
  const auto& u = st->user_regs;
  for (int n = 0; n < 32; ++n) {
    xe::store_and_swap<uint64_t>(f + kFrameGpr + 8 * n, u.r[n]);
  }
  xe::store_and_swap<uint64_t>(f + kFrameCtr, u.ctr);
  xe::store_and_swap<uint32_t>(f + kFrameLr, uint32_t(u.lr));
  xe::store_and_swap<uint32_t>(f + kFrameIar, iar);
  xe::store_and_swap<uint32_t>(f + kFrameCr, uint32_t(u.cr()));
  xe::store_and_swap<uint32_t>(f + kFrameXer, XerValue(&u));
}

static uint32_t ReadFrame(UserModeState* st, cpu::ppc::PPCContext* pc) {
  auto f = kernel_memory()->TranslateVirtual<const uint8_t*>(st->frame);
  for (int n = 0; n < 32; ++n) {
    pc->r[n] = xe::load_and_swap<uint64_t>(f + kFrameGpr + 8 * n);
  }
  pc->ctr = xe::load_and_swap<uint64_t>(f + kFrameCtr);
  pc->lr = xe::load_and_swap<uint32_t>(f + kFrameLr);
  SetCrFromValue(pc, xe::load_and_swap<uint32_t>(f + kFrameCr));
  SetXer(pc, xe::load_and_swap<uint32_t>(f + kFrameXer));
  return xe::load_and_swap<uint32_t>(f + kFrameIar);
}

// Runs xefu's handler in kernel mode for an event in user code at `iar`
// (record 0 for a system call), with the user registers currently in `pc`.
// Returns the address to resume user code at; `pc` holds the user registers
// as the handler left them.
// Interrupts for user mode. xefu (in a DPC or another thread) raises an
// interrupt for the emulated x86 by setting CR bit 4 (cr1.lt) in the user
// thread's trap frame, which it finds through the thread's KTHREAD (+0x128),
// as the 360 kernel keeps it while user mode is interrupted. xefu's
// dispatcher tests that bit on entry. In Xenia user code keeps running
// meanwhile, so the bit is carried over to the live registers: right after
// DPCs, and before every entry into xefu's handler.
constexpr uint32_t kCrInterruptBit = 0x08000000u;  // CR bit 4
static std::atomic<cpu::ppc::PPCContext*> g_user_context{nullptr};
static std::atomic<uint32_t> g_user_interrupt_frame{0};
static std::atomic<uint32_t> g_interrupt_flags{0};

// xefu's state block (at user 0xB7000000), or null if it isn't mapped yet.
// +0xA4: the x86 EFLAGS (IF is 0x200); +0x254: the interrupt-pending byte.
static bool TranslateUserAddress(uint32_t address, bool write,
                                 uint32_t* physical_out);
static volatile uint8_t* UserStateBlock() {
  static std::atomic<uint32_t> physical{0};
  uint32_t address = physical.load();
  if (!address) {
    if (!TranslateUserAddress(0xB7000000u, false, &address)) {
      return nullptr;
    }
    physical.store(address);
  }
  return kernel_memory()->TranslatePhysical<volatile uint8_t*>(address);
}
static bool UserInterruptDeliverable(volatile uint8_t* state) {
  uint32_t eflags =
      xe::load_and_swap<uint32_t>(const_cast<const uint8_t*>(state + 0xA4));
  return state[0x254] && (eflags & 0x200);
}

static void PropagateUserInterruptFlag() {
  uint32_t frame = g_user_interrupt_frame.load();
  auto pc = g_user_context.load();
  if (!frame || !pc) {
    return;
  }
  auto cr =
      kernel_memory()->TranslateVirtual<xe::be<uint32_t>*>(frame + kFrameCr);
  uint32_t value = *cr;
  if (value & kCrInterruptBit) {
    *cr = value & ~kCrInterruptBit;
    // The flag tells user code an interrupt is pending; code that sees it
    // stops what it's doing (a copy in xefu's runtime returns early, leaving
    // the rest for after the interrupt) and goes to xefu's dispatcher, which
    // carries on as if nothing happened if no interrupt is pending after all.
    // The 360 only sets it on the way back to user mode; set from here, it
    // could outlive the interrupt (taken meanwhile when user code enables
    // interrupts, or disabled since), and a copy would stop half done. So
    // only while one is pending and interrupts are enabled.
    volatile uint8_t* state = UserStateBlock();
    if (state && !UserInterruptDeliverable(state)) {
      return;
    }
    // cr1.lt: the byte for CR bit 4.
    auto flag = reinterpret_cast<volatile uint8_t*>(&pc->cr0) + 4;
    *flag = 1;
    if (state && !UserInterruptDeliverable(state)) {
      *flag = 0;
    }
    g_interrupt_flags.fetch_add(1);
  }
}

// A guest call run to completion leaves the host/guest stackpoints as it found
// them; one that doesn't (guest code that never returned to its caller inside
// it) would pile them up until they overflow. Restores the depth and says so.
static void CheckStackpointsAfterCall(cpu::ppc::PPCContext* context,
                                      uint32_t depth_before, const char* what) {
  auto backend = kernel_state()->processor()->backend();
  uint32_t depth_after = backend->GetGuestStackDepth(context);
  if (depth_after == depth_before) {
    return;
  }
  static std::atomic<uint32_t> logs{0};
  if (logs.fetch_add(1) < 20) {
    XELOGW("BACKCOMPAT {} left {} stackpoints (depth {} -> {}), lr={:08X}",
           what, int32_t(depth_after - depth_before), depth_before, depth_after,
           uint32_t(context->lr));
  }
  backend->SetGuestStackDepth(context, depth_before);
}

static uint32_t RunUserModeHandler(UserModeState* st, cpu::ppc::PPCContext* pc,
                                   uint32_t record, uint32_t iar) {
  XEFU_PHASE();
  PropagateUserInterruptFlag();
  std::memcpy(&st->user_regs, pc, sizeof(*pc));
  WriteFrame(st, iar);

  // Run the handler as kernel code: kernel registers and memory view.
  uint64_t kernel_r1 = st->kernel_regs.r[1];
  std::memcpy(pc, &st->kernel_regs, sizeof(*pc));
  pc->r[1] = kernel_r1 - 0x400;  // keep clear of the caller's frame
  uint64_t args[] = {record, st->frame};
  auto thread = XThread::GetCurrentThread();
  XEFU_PHASE();
  uint32_t stack_depth =
      kernel_state()->processor()->backend()->GetGuestStackDepth(pc);
  kernel_state()->processor()->Execute(thread->thread_state(), st->handler,
                                       args, xe::countof(args));
  CheckStackpointsAfterCall(pc, stack_depth, "xefu's user-mode handler");
  XEFU_PHASE();

  // Back to user mode with whatever the handler left in the frame.
  std::memcpy(pc, &st->user_regs, sizeof(*pc));
  uint32_t resume = ReadFrame(st, pc);
  pc->virtual_membase = g_alias_base;
  return resume;
}

// `sc` from user code: hand it to xefu as (record = null, trap frame).
static std::atomic<uint32_t> g_trap_count{0};

// The most recent system calls from user code, logged at a stall: ports and
// values (x86 port I/O goes through sc 0xA/0xB with the port in r12).
struct SyscallRecord {
  uint32_t number, eip, r3, r4, r11, r12, r3_after, r11_after;
};
constexpr uint32_t kSyscallRingSize = 16384;
static SyscallRecord g_syscall_ring[kSyscallRingSize];
static std::atomic<uint32_t> g_syscall_ring_index{0};

static bool TranslateUserAddress(uint32_t address, bool write,
                                 uint32_t* physical_out);

static bool UserModeSyscallHook(cpu::ppc::PPCContext* pc) {
  XEFU_PHASE();
  UserModeState* st = t_user_mode;
  if (!st || pc->virtual_membase != g_alias_base) {
    return false;
  }
  uint32_t sc_address = uint32_t(pc->sc_address);
  if (pc->sc_kind == 1) {
    // A trap instruction: a program interrupt, which xefu's handler gets as
    // an illegal instruction record at the trap's address (and its
    // classifier tells traps apart by the instruction word).
    if (g_trap_count.fetch_add(1) < 50) {
      XELOGI("BACKCOMPAT trap at {:08X}", sc_address);
    }
    auto r = kernel_memory()->TranslateVirtual<uint8_t*>(st->record);
    std::memset(r, 0, kRecordSize);
    xe::store_and_swap<uint32_t>(r + kRecordCode, 0xC000001D);
    xe::store_and_swap<uint32_t>(r + kRecordAddress, sc_address);
    uint32_t iar = RunUserModeHandler(st, pc, st->record, sc_address);
    pc->sc_resume = (iar == sc_address + 4) ? 0 : iar;
    return true;
  }
  if (g_syscall_logs.fetch_add(1) < 200) {
    XELOGI("BACKCOMPAT syscall r0={:X} at {:08X} (r3={:08X} r4={:08X})",
           uint32_t(pc->r[0]), sc_address, uint32_t(pc->r[3]),
           uint32_t(pc->r[4]));
  }
  // The emulated Xbox kernel giving up on a title: its reboot-to-dashboard
  // path (hypercall 0x0702 at 0x80024627, reached through sc 0x10). Log the
  // registers and the x86 stack once, which hold the failure status.
  if (uint32_t(pc->r[0]) == 0x10 && uint32_t(pc->r[2]) == 0x80024627) {
    static std::atomic<bool> logged{false};
    if (!logged.exchange(true)) {
      std::string regs;
      for (int n = 0; n < 32; ++n) {
        regs += fmt::format(" r{}={:X}", n, uint32_t(pc->r[n]));
      }
      XELOGW("BACKCOMPAT the Xbox kernel is returning to the dashboard:{}",
             regs);
      std::string stack;
      uint32_t esp = uint32_t(pc->r[7]);
      for (uint32_t n = 0; n < 64; ++n) {
        uint32_t value = 0;
        bool ok = true;
        for (uint32_t b = 0; b < 4 && ok; ++b) {
          uint32_t physical;
          ok = TranslateUserAddress(esp + 4 * n + b, false, &physical);
          if (ok) {
            value |=
                uint32_t(*kernel_memory()->TranslatePhysical<const uint8_t*>(
                    physical))
                << (8 * b);
          }
        }
        stack += ok ? fmt::format(" {:08X}", value) : std::string(" ????????");
      }
      XELOGW("BACKCOMPAT   x86 stack @{:08X}:{}", esp, stack);
    }
  }
  SyscallRecord& rec =
      g_syscall_ring[g_syscall_ring_index.fetch_add(1) % kSyscallRingSize];
  rec.number = uint32_t(pc->r[0]);
  rec.eip = uint32_t(pc->r[2]);
  rec.r3 = uint32_t(pc->r[3]);
  rec.r4 = uint32_t(pc->r[4]);
  rec.r11 = uint32_t(pc->r[11]);
  rec.r12 = uint32_t(pc->r[12]);
  // Like SRR0 for a system call, the frame's IAR is the instruction after
  // the sc; xefu leaves it there to return normally.
  uint32_t iar = RunUserModeHandler(st, pc, 0, sc_address + 4);
  XEFU_PHASE();
  rec.r3_after = uint32_t(pc->r[3]);
  rec.r11_after = uint32_t(pc->r[11]);
  pc->sc_resume = (iar == sc_address + 4) ? 0 : iar;
  return true;
}

// Translates a user address through xefu's page tables (16 MB, 64 KB and
// 4 KB pages), for an access Xenia's host views can't make itself (4 KB pages
// that don't line up with a 64 KB view). Returns false if xefu's tables don't
// allow the access.
static bool TranslateUserAddress(uint32_t address, bool write,
                                 uint32_t* physical_out) {
  if (!g_user_page_table) {
    return false;
  }
  auto memory = kernel_memory();
  auto table = memory->TranslateVirtual<const uint8_t*>(g_user_page_table);
  uint32_t entry = UserSegmentEntry(address >> 24);
  uint32_t physical = 0;
  if (entry) {
    physical = (entry & 0x1F000000u) | (address & 0x00FFFFFFu);
  } else {
    bool large = table[0x840 + (address >> 28)] & 2;
    uint32_t table_page = xe::load_and_swap<uint16_t>(
        table + (large ? 0x800 + 2 * (address >> 27) : 2 * (address >> 23)));
    if (!table_page) {
      return false;
    }
    auto entries =
        memory->TranslatePhysical<const uint8_t*>(uint32_t(table_page) << 13);
    entry = xe::load_and_swap<uint32_t>(
        entries +
        4 * (large ? (address >> 16) & 0x7FF : (address >> 12) & 0x7FF));
    uint32_t page_mask = large ? 0xFFFFu : 0xFFFu;
    physical = (entry & 0x1FFFFFFFu & ~page_mask) | (address & page_mask);
  }
  uint32_t pp = entry & 3;
  if (!entry || pp == 0 || (write && pp != 2)) {
    return false;
  }
  *physical_out = physical;
  return true;
}

// Copies `size` bytes between user memory (through xefu's page tables) and
// `bytes`. Fails without touching memory if any byte isn't accessible.
static bool AccessUserBytes(uint32_t address, uint32_t size, bool write,
                            uint8_t* bytes) {
  auto memory = kernel_memory();
  uint32_t physical[128];
  if (size > xe::countof(physical)) {
    return false;
  }
  for (uint32_t n = 0; n < size; ++n) {
    if (!TranslateUserAddress(address + n, write, &physical[n])) {
      return false;
    }
  }
  for (uint32_t n = 0; n < size; ++n) {
    auto host = memory->TranslatePhysical<uint8_t*>(physical[n]);
    if (write) {
      *host = bytes[n];
    } else {
      bytes[n] = *host;
    }
  }
  return true;
}

// The cache line, floating-point and vector forms of EmulateUserLoadStore
// (below): dcbz (128 bytes on the 360), lfs/lfd/stfs/stfd and their indexed
// and update forms, stfiwx, lvx/stvx and their VMX128 forms.
static bool EmulateUserCacheFloatVector(cpu::ppc::PPCContext* pc,
                                        uint32_t instr) {
  uint32_t op = instr >> 26;
  uint32_t rt = (instr >> 21) & 31;
  uint32_t ra = (instr >> 16) & 31;
  uint32_t rb = (instr >> 11) & 31;
  uint64_t base = ra ? pc->r[ra] : 0;
  uint32_t address = 0;
  enum Kind { kNone, kZero, kFloat, kDouble, kFloatWord, kVector } kind = kNone;
  bool is_load = true, update = false;
  uint32_t vr = rt;
  if (op >= 48 && op <= 55) {
    address = uint32_t(base + int16_t(instr & 0xFFFF));
    kind = (op & 2) ? kDouble : kFloat;
    is_load = op < 52;
    update = op & 1;
  } else if (op == 31) {
    address = uint32_t(base + pc->r[rb]);
    switch ((instr >> 1) & 0x3FF) {
      case 1014:  // dcbz
        kind = kZero;
        break;
      case 535:    // lfsx
      case 567:    // lfsux
      case 599:    // lfdx
      case 631:    // lfdux
      case 663:    // stfsx
      case 695:    // stfsux
      case 727:    // stfdx
      case 759: {  // stfdux
        uint32_t xo = (instr >> 1) & 0x3FF;
        kind = (xo & 64) ? kDouble : kFloat;
        is_load = xo < 663;
        update = xo & 32;
      } break;
      case 983:  // stfiwx
        kind = kFloatWord;
        is_load = false;
        break;
      case 103:  // lvx
      case 359:  // lvxl
        kind = kVector;
        break;
      case 231:  // stvx
      case 487:  // stvxl
        kind = kVector;
        is_load = false;
        break;
      default:
        return false;
    }
  } else if (op == 4) {
    // VMX128 indexed forms: the register is VD128l | VD128h << 5.
    uint32_t form = instr & 0x7F3;
    if (form != 0x0C3 && form != 0x1C3 && form != 0x2C3 && form != 0x3C3) {
      return false;
    }
    address = uint32_t(base + pc->r[rb]);
    kind = kVector;
    is_load = form == 0x0C3 || form == 0x2C3;
    vr = rt | (((instr >> 2) & 3) << 5);
  } else {
    return false;
  }
  if ((update && !ra) || kind == kNone) {
    return false;
  }
  uint8_t bytes[128] = {};
  switch (kind) {
    case kZero:
      if (!AccessUserBytes(address & ~127u, 128, true, bytes)) {
        return false;
      }
      break;
    case kFloat:
      if (is_load) {
        if (!AccessUserBytes(address, 4, false, bytes)) {
          return false;
        }
        uint32_t bits = xe::load_and_swap<uint32_t>(bytes);
        float value;
        std::memcpy(&value, &bits, 4);
        pc->f[rt] = value;
      } else {
        float value = float(pc->f[rt]);
        uint32_t bits;
        std::memcpy(&bits, &value, 4);
        xe::store_and_swap<uint32_t>(bytes, bits);
        if (!AccessUserBytes(address, 4, true, bytes)) {
          return false;
        }
      }
      break;
    case kDouble:
      if (is_load) {
        if (!AccessUserBytes(address, 8, false, bytes)) {
          return false;
        }
        uint64_t bits = xe::load_and_swap<uint64_t>(bytes);
        std::memcpy(&pc->f[rt], &bits, 8);
      } else {
        uint64_t bits;
        std::memcpy(&bits, &pc->f[rt], 8);
        xe::store_and_swap<uint64_t>(bytes, bits);
        if (!AccessUserBytes(address, 8, true, bytes)) {
          return false;
        }
      }
      break;
    case kFloatWord: {
      uint64_t bits;
      std::memcpy(&bits, &pc->f[rt], 8);
      xe::store_and_swap<uint32_t>(bytes, uint32_t(bits));
      if (!AccessUserBytes(address, 4, true, bytes)) {
        return false;
      }
    } break;
    case kVector:
      address &= ~15u;
      if (is_load) {
        if (!AccessUserBytes(address, 16, false, bytes)) {
          return false;
        }
        for (uint32_t n = 0; n < 4; ++n) {
          pc->v[vr].u32[n] = xe::load_and_swap<uint32_t>(bytes + 4 * n);
        }
      } else {
        for (uint32_t n = 0; n < 4; ++n) {
          xe::store_and_swap<uint32_t>(bytes + 4 * n, pc->v[vr].u32[n]);
        }
        if (!AccessUserBytes(address, 16, true, bytes)) {
          return false;
        }
      }
      break;
    default:
      return false;
  }
  if (update) {
    pc->r[ra] = address;
  }
  static std::atomic<uint32_t> logs{0};
  if (logs.fetch_add(1) < 20) {
    XELOGI("BACKCOMPAT emulated {:08X} at {:08X}", instr, address);
  }
  return true;
}

// Performs the integer load or store at `iar` (the faulting instruction) on
// guest physical memory through xefu's page tables, updating the guest
// registers in `pc`. For user pages Xenia can't map as host views. Returns
// false if the instruction isn't a supported load/store or xefu's tables
// don't allow the access (the fault then goes to xefu).
static bool EmulateUserLoadStore(cpu::ppc::PPCContext* pc, uint32_t iar) {
  auto memory = kernel_memory();
  uint32_t instr = xe::load_and_swap<uint32_t>(memory->TranslateCode(iar));
  if (EmulateUserCacheFloatVector(pc, instr)) {
    return true;
  }
  uint32_t op = instr >> 26;
  uint32_t rt = (instr >> 21) & 31;
  uint32_t ra = (instr >> 16) & 31;
  uint32_t rb = (instr >> 11) & 31;
  uint32_t size = 0;
  bool is_load = true, update = false, reversed = false, sign = false;
  uint64_t ea = ra ? pc->r[ra] : 0;
  int64_t d = int16_t(instr & 0xFFFF);
  switch (op) {
    case 32:
      size = 4;
      break;  // lwz
    case 33:
      size = 4;
      update = true;
      break;  // lwzu
    case 34:
      size = 1;
      break;  // lbz
    case 35:
      size = 1;
      update = true;
      break;  // lbzu
    case 36:
      size = 4;
      is_load = false;
      break;  // stw
    case 37:
      size = 4;
      is_load = false;
      update = true;
      break;
    case 38:
      size = 1;
      is_load = false;
      break;  // stb
    case 39:
      size = 1;
      is_load = false;
      update = true;
      break;
    case 40:
      size = 2;
      break;  // lhz
    case 41:
      size = 2;
      update = true;
      break;
    case 42:
      size = 2;
      sign = true;
      break;  // lha
    case 43:
      size = 2;
      sign = true;
      update = true;
      break;
    case 44:
      size = 2;
      is_load = false;
      break;  // sth
    case 45:
      size = 2;
      is_load = false;
      update = true;
      break;
    case 58:  // ld, ldu, lwa
      d = int16_t(instr & 0xFFFC);
      switch (instr & 3) {
        case 0:
          size = 8;
          break;
        case 1:
          size = 8;
          update = true;
          break;
        case 2:
          size = 4;
          sign = true;
          break;
        default:
          return false;
      }
      break;
    case 62:  // std, stdu
      d = int16_t(instr & 0xFFFC);
      is_load = false;
      switch (instr & 3) {
        case 0:
          size = 8;
          break;
        case 1:
          size = 8;
          update = true;
          break;
        default:
          return false;
      }
      break;
    case 31: {
      d = int64_t(pc->r[rb]);
      switch ((instr >> 1) & 0x3FF) {
        case 23:
          size = 4;
          break;  // lwzx
        case 55:
          size = 4;
          update = true;
          break;  // lwzux
        case 87:
          size = 1;
          break;  // lbzx
        case 119:
          size = 1;
          update = true;
          break;  // lbzux
        case 151:
          size = 4;
          is_load = false;
          break;  // stwx
        case 183:
          size = 4;
          is_load = false;
          update = true;
          break;
        case 215:
          size = 1;
          is_load = false;
          break;  // stbx
        case 247:
          size = 1;
          is_load = false;
          update = true;
          break;
        case 279:
          size = 2;
          break;  // lhzx
        case 311:
          size = 2;
          update = true;
          break;
        case 343:
          size = 2;
          sign = true;
          break;  // lhax
        case 407:
          size = 2;
          is_load = false;
          break;  // sthx
        case 439:
          size = 2;
          is_load = false;
          update = true;
          break;
        case 21:
          size = 8;
          break;  // ldx
        case 53:
          size = 8;
          update = true;
          break;  // ldux
        case 149:
          size = 8;
          is_load = false;
          break;  // stdx
        case 181:
          size = 8;
          is_load = false;
          update = true;
          break;
        case 341:
          size = 4;
          sign = true;
          break;  // lwax
        case 534:
          size = 4;
          reversed = true;
          break;  // lwbrx
        case 662:
          size = 4;
          is_load = false;
          reversed = true;
          break;
        case 790:
          size = 2;
          reversed = true;
          break;  // lhbrx
        case 918:
          size = 2;
          is_load = false;
          reversed = true;
          break;
        case 532:
          size = 8;
          reversed = true;
          break;  // ldbrx
        case 660:
          size = 8;
          is_load = false;
          reversed = true;
          break;
        default:
          return false;
      }
    } break;
    default:
      return false;
  }
  uint32_t address = uint32_t(ea + d);
  uint8_t bytes[8];
  uint32_t physical[8];
  for (uint32_t n = 0; n < size; ++n) {
    if (!TranslateUserAddress(address + n, !is_load, &physical[n])) {
      return false;
    }
  }
  if (is_load) {
    for (uint32_t n = 0; n < size; ++n) {
      bytes[n] = *memory->TranslatePhysical<const uint8_t*>(physical[n]);
    }
    uint64_t value = 0;
    for (uint32_t n = 0; n < size; ++n) {
      uint32_t byte = reversed ? size - 1 - n : n;
      value = (value << 8) | bytes[byte];
    }
    if (sign) {
      uint32_t shift = 64 - 8 * size;
      value = uint64_t(int64_t(value << shift) >> shift);
    }
    pc->r[rt] = value;
  } else {
    uint64_t value = pc->r[rt];
    for (uint32_t n = 0; n < size; ++n) {
      uint32_t byte = reversed ? n : size - 1 - n;
      bytes[byte] = uint8_t(value >> (8 * n));
    }
    for (uint32_t n = 0; n < size; ++n) {
      *memory->TranslatePhysical<uint8_t*>(physical[n]) = bytes[n];
    }
  }
  if (update && ra) {
    pc->r[ra] = address;
  }
  static std::atomic<uint32_t> logs{0};
  if (logs.fetch_add(1) < 20) {
    XELOGI("BACKCOMPAT emulated {} of {} bytes at {:08X} (physical {:08X})",
           is_load ? "load" : "store", size, address, physical[0]);
  }
  return true;
}

static void DescribeUserMapping(uint32_t address) {
  if (!g_user_page_table) {
    return;
  }
  auto memory = kernel_memory();
  auto table = memory->TranslateVirtual<const uint8_t*>(g_user_page_table);
  uint8_t size = table[0x840 + (address >> 28)];
  uint32_t segment = UserSegmentEntry(address >> 24);
  bool large = size & 2;
  uint32_t table_page = xe::load_and_swap<uint16_t>(
      table + (large ? 0x800 + 2 * (address >> 27) : 2 * (address >> 23)));
  uint32_t entry = 0;
  if (table_page) {
    auto entries =
        memory->TranslatePhysical<const uint8_t*>(uint32_t(table_page) << 13);
    entry = xe::load_and_swap<uint32_t>(
        entries +
        4 * (large ? (address >> 16) & 0x7FF : (address >> 12) & 0x7FF));
  }
  XELOGW(
      "BACKCOMPAT mapping of {:08X}: 16 MB entry {:08X}, page size {:02X}, "
      "table {:08X}, entry {:08X}, Xenia view {:08X}",
      address, segment, size, uint32_t(table_page) << 13, entry,
      g_alias_pages.empty() ? 0 : g_alias_pages[address >> kUserPageShift]);
}

static std::atomic<uint32_t> g_fault_logs{0};

// A fault in user code (taken in the host exception handler, below): hand
// xefu an access violation record and the trap frame. Returns where to
// resume.
[[maybe_unused]] static uint32_t DeliverUserModeFault(
    UserModeState* st, cpu::ppc::PPCContext* pc) {
  XEFU_PHASE();
  const UserModeFault& fault = st->fault;
  if (g_fault_logs.fetch_add(1) < 200) {
    XELOGI("BACKCOMPAT fault at {:08X}: {} {:08X} (x86 EIP {:08X})", fault.iar,
           fault.is_write ? "write" : "read", fault.address,
           uint32_t(pc->r[2]));
  }
  // A fault that keeps coming back: say how xefu's tables map the address.
  static uint32_t last_iar = 0, last_address = 0, repeats = 0;
  if (fault.iar == last_iar && fault.address == last_address) {
    if (++repeats == 3) {
      DescribeUserMapping(fault.address);
    }
  } else {
    last_iar = fault.iar;
    last_address = fault.address;
    repeats = 0;
  }
  auto r = kernel_memory()->TranslateVirtual<uint8_t*>(st->record);
  std::memset(r, 0, kRecordSize);
  xe::store_and_swap<uint32_t>(r + kRecordCode, 0xC0000005);
  xe::store_and_swap<uint32_t>(r + kRecordAddress, fault.iar);
  xe::store_and_swap<uint32_t>(r + kRecordParamCount, 2);
  xe::store_and_swap<uint32_t>(r + kRecordParams, fault.is_write ? 1 : 0);
  xe::store_and_swap<uint32_t>(r + kRecordParams + 4, fault.address);
  uint64_t before[32];
  std::memcpy(before, pc->r, sizeof(before));
  uint32_t resume = RunUserModeHandler(st, pc, st->record, fault.iar);
  // What xefu's handler changed, for the first faults it emulates.
  static std::atomic<uint32_t> diff_logs{0};
  if (diff_logs.fetch_add(1) < 40) {
    std::string changes;
    for (int i = 0; i < 32; ++i) {
      if (pc->r[i] != before[i]) {
        changes += fmt::format(" r{}:{:X}->{:X}", i, before[i], pc->r[i]);
      }
    }
    XELOGI("BACKCOMPAT   handler resumed at {:08X}; changed:{}", resume,
           changes.empty() ? std::string(" nothing") : changes);
  }
  return resume;
}

#if XE_USER_MODE_FAULTS
// Host access violations in the alias space come from user code. The guest
// context is exact at the faulting instruction (user code is translated
// without context promotion), so the thread is sent back to the restart
// point in KeEnterUserMode, which delivers the fault and runs on from where
// the handler says.
// Logs a host exception taken while a thread is in user mode that isn't
// delivered to xefu, so the log says where it happened.
static void LogUnhandledUserModeException(Exception* ex, const char* why) {
  static std::atomic<uint32_t> logs{0};
  if (logs.fetch_add(1) >= 20) {
    return;
  }
  bool av = ex->code() == Exception::Code::kAccessViolation;
  auto exe = reinterpret_cast<uint64_t>(GetModuleHandleW(nullptr));
  uint64_t pc = ex->pc();
  auto code_cache = kernel_state()->processor()->backend()->code_cache();
  auto function = code_cache->LookupFunction(pc);
  uint32_t guest = function ? function->MapMachineCodeToGuestAddress(pc) : 0;
  XELOGE(
      "BACKCOMPAT host exception in user mode ({}): {} pc={:X} (exe+{:X}) "
      "rsp={:X} fault={:X} op={} guest={:08X}",
      why, av ? "access violation" : "illegal instruction", pc, pc - exe,
      ex->thread_context()->int_registers[4], av ? ex->fault_address() : 0,
      av ? int(ex->access_violation_operation()) : -1, guest);
  FlushLog();
}

static bool UserModeFaultCallback(Exception* ex, void* data) {
  UserModeState* st = t_user_mode;
  if (!st || !g_alias_base) {
    return false;
  }
  XEFU_PHASE();
  if (ex->code() != Exception::Code::kAccessViolation) {
    LogUnhandledUserModeException(ex, "not an access violation");
    return false;
  }
  auto base = reinterpret_cast<uint64_t>(g_alias_base);
  uint64_t fault_address = ex->fault_address();
  if (fault_address < base || fault_address >= base + 0x100001000ull) {
    // Not user memory; for the other handlers (e.g. MMIO from the kernel
    // code xefu's handler runs).
    return false;
  }
  auto code_cache = kernel_state()->processor()->backend()->code_cache();
  auto function = code_cache->LookupFunction(ex->pc());
  if (!function) {
    LogUnhandledUserModeException(ex, "not in translated code");
    return false;
  }
  uint32_t iar = function->MapMachineCodeToGuestAddress(ex->pc());
  if (!kernel_memory()->IsCodeAliasAddress(iar)) {
    LogUnhandledUserModeException(ex, "not in user code");
    return false;
  }
  uint64_t offset = fault_address - base;
  st->fault.pending = true;
  st->fault.iar = iar;
  st->fault.address = uint32_t(offset);
  st->fault.is_write = ex->access_violation_operation() ==
                       Exception::AccessViolationOperation::kWrite;

  const CONTEXT& c = st->restart;
  ex->set_resume_pc(c.Rip);
  ex->thread_context()->eflags = c.EFlags;
  const DWORD64* gprs = &c.Rax;
  for (uint32_t n = 0; n < 16; ++n) {
    ex->ModifyIntRegister(n) = gprs[n];
  }
  const M128A* xmms = &c.Xmm0;
  for (uint32_t n = 0; n < 16; ++n) {
    std::memcpy(&ex->ModifyXmmRegister(n), &xmms[n], sizeof(vec128_t));
  }
  return true;
}
#endif  // XE_USER_MODE_FAULTS

// Research: a log line every few seconds while user mode runs, so a quiet
// stretch shows whether the game is progressing (the x86 EIP is in r2) or
// stuck.
static std::atomic<uint32_t> g_sweep_count{0};
static std::atomic<uint32_t> g_timer_dpc_count{0};
static std::atomic<cpu::ppc::PPCContext*> g_heartbeat_context{nullptr};

static std::string DescribeKernelTimers();

static void StartUserModeHeartbeat(cpu::ppc::PPCContext* pc) {
  static std::atomic<bool> started{false};
  g_heartbeat_context = pc;
  if (started.exchange(true)) {
    return;
  }
  auto status_path =
      kernel_state()->emulator()->storage_root() / "xefu_heartbeat.txt";
  std::thread([status_path] {
    for (uint32_t tick = 0; tick < 2000; ++tick) {
      std::this_thread::sleep_for(std::chrono::seconds(3));
      auto pc = g_heartbeat_context.load();
      // First, without the log (which a hang may be holding): where the
      // user-mode thread is, and the last kernel export it called.
      if (FILE* file = xe::filesystem::OpenFile(status_path, "a")) {
        cpu::Export* last_export = shim::g_traced_export.load();
        std::string line = fmt::format(
            "tick {} phase line {} last export {} ({}) EIP {:08X}, {} system "
            "calls, {} faults, {} timer DPCs, {} interrupts raised\n",
            tick, g_phase.load(), last_export ? last_export->name : "none",
            shim::g_traced_export_returned.load() ? "returned" : "in it",
            pc ? uint32_t(pc->r[2]) : 0, g_syscall_logs.load(),
            g_fault_logs.load(), g_timer_dpc_count.load(),
            g_interrupt_flags.load());
        fputs(line.c_str(), file);
        // xefu's state block (user 0xB7000000, physical 0x07000000): the
        // synchronous service request (+0x298 = -1 while waiting, +0x29C =
        // done) and the asynchronous queue (written at +0x2A0, read at
        // -0x3FBC).
        auto state_block =
            kernel_memory()->TranslatePhysical<const uint8_t*>(0x07000000u);
        auto state_word = [&](int32_t offset) {
          return xe::load_and_swap<uint32_t>(state_block + offset);
        };
        fputs(fmt::format("  service request {:08X} done {:08X}, queue write "
                          "{:08X} read {:08X}\n",
                          state_word(0x298), state_word(0x29C),
                          state_word(0x2A0), state_word(-0x3FBC))
                  .c_str(),
              file);
        fputs(DescribeKernelTimers().c_str(), file);
        for (const auto& trace : shim::g_thread_export_traces) {
          cpu::Export* thread_export = trace.last_export.load();
          if (thread_export) {
            fputs(fmt::format("  thread {:X}: {} ({})\n",
                              trace.thread_id.load(), thread_export->name,
                              trace.returned.load() ? "returned" : "in it")
                      .c_str(),
                  file);
          }
        }
        fclose(file);
      }
      if (!pc) {
        continue;
      }
      uint32_t eip = uint32_t(pc->r[2]);
      XELOGI(
          "BACKCOMPAT heartbeat: x86 EIP {:08X} ESP {:08X}, {} system calls, "
          "{} faults, {} code sweeps, {} timer DPCs, {} traps, {} interrupts "
          "raised",
          eip, uint32_t(pc->r[7]), g_syscall_logs.load(), g_fault_logs.load(),
          g_sweep_count.load(), g_timer_dpc_count.load(), g_trap_count.load(),
          g_interrupt_flags.load());
      // Where it seems to stay: the x86 code around it and the registers.
      static uint32_t last_eip = 0, dumps = 0;
      if (eip == last_eip && dumps < 3) {
        ++dumps;
        std::string regs;
        for (int n = 0; n < 32; ++n) {
          regs += fmt::format(" r{}={:X}", n, uint32_t(pc->r[n]));
        }
        XELOGI("BACKCOMPAT   registers:{}", regs);
        // The x86 stack (little-endian words), for the callers.
        auto read_x86_word = [](uint32_t address, uint32_t* value) {
          uint32_t v = 0;
          for (uint32_t n = 0; n < 4; ++n) {
            uint32_t physical;
            if (!TranslateUserAddress(address + n, false, &physical)) {
              return false;
            }
            v |= uint32_t(*kernel_memory()->TranslatePhysical<const uint8_t*>(
                     physical))
                 << (8 * n);
          }
          *value = v;
          return true;
        };
        std::string stack;
        uint32_t esp = uint32_t(pc->r[7]);
        for (uint32_t n = 0; n < 48; ++n) {
          uint32_t value;
          stack += read_x86_word(esp + 4 * n, &value)
                       ? fmt::format(" {:08X}", value)
                       : std::string(" ????????");
        }
        XELOGI("BACKCOMPAT   x86 stack @{:08X}:{}", esp, stack);
        std::string trace;
        uint32_t index = cpu::ppc::g_user_trace_index.load();
        for (uint32_t n = 64; n > 0; --n) {
          trace += fmt::format(
              " {:08X}",
              cpu::ppc::g_user_trace[(index - n) % cpu::ppc::kUserTraceSize]);
        }
        XELOGI("BACKCOMPAT   recent x86 EIPs (oldest first):{}", trace);
        if (dumps == 1) {
          uint32_t end = g_syscall_ring_index.load();
          uint32_t count = std::min(end, kSyscallRingSize);
          auto path =
              kernel_state()->emulator()->storage_root() / "xefu_syscalls.txt";
          if (FILE* file = xe::filesystem::OpenFile(path, "w")) {
            for (uint32_t n = count; n > 0; --n) {
              const SyscallRecord& r =
                  g_syscall_ring[(end - n) % kSyscallRingSize];
              std::string line = fmt::format(
                  "{} {:X} eip={:08X} r3={:08X} r4={:08X} r11={:08X} "
                  "r12={:08X} -> r3={:08X} r11={:08X}\n",
                  end - n, r.number, r.eip, r.r3, r.r4, r.r11, r.r12,
                  r.r3_after, r.r11_after);
              fputs(line.c_str(), file);
            }
            fclose(file);
            XELOGI("BACKCOMPAT   wrote {} system calls to {}", count,
                   xe::path_to_utf8(path));
          }
        }
        // Once: the emulated Xbox's 64 MB of RAM (physical 0x10000000 on the
        // 360), to study offline.
        if (dumps == 1) {
          auto path =
              kernel_state()->emulator()->storage_root() / "xefu_xbox_ram.bin";
          if (FILE* file = xe::filesystem::OpenFile(path, "wb")) {
            fwrite(
                kernel_memory()->TranslatePhysical<const uint8_t*>(0x10000000u),
                1, 0x04000000, file);
            fclose(file);
            XELOGI("BACKCOMPAT   wrote the Xbox's RAM to {}",
                   xe::path_to_utf8(path));
          }
          // And xefu's user-mode runtime and translated code (16 MB pages
          // B8 and B9: physical 0x08000000 and 0x09000000).
          auto code_path =
              kernel_state()->emulator()->storage_root() / "xefu_user_code.bin";
          if (FILE* file = xe::filesystem::OpenFile(code_path, "wb")) {
            fwrite(
                kernel_memory()->TranslatePhysical<const uint8_t*>(0x08000000u),
                1, 0x02000000, file);
            fclose(file);
          }
          // xefu's user-mode state (16 MB pages B6 and B7: physical
          // 0x06000000) and xefu's own image and data (0x82000000+).
          auto state_path = kernel_state()->emulator()->storage_root() /
                            "xefu_user_state.bin";
          if (FILE* file = xe::filesystem::OpenFile(state_path, "wb")) {
            fwrite(
                kernel_memory()->TranslatePhysical<const uint8_t*>(0x06000000u),
                1, 0x02000000, file);
            fclose(file);
          }
          auto image_path =
              kernel_state()->emulator()->storage_root() / "xefu_image.bin";
          if (FILE* file = xe::filesystem::OpenFile(image_path, "wb")) {
            fwrite(
                kernel_memory()->TranslateVirtual<const uint8_t*>(0x82000000u),
                1, 0x00400000, file);
            fclose(file);
          }
        }
        for (uint32_t line = 0; line < 8; ++line) {
          std::string bytes;
          uint32_t start = (eip & ~0xFu) - 0x40 + line * 16;
          for (uint32_t n = 0; n < 16; ++n) {
            uint32_t physical;
            if (TranslateUserAddress(start + n, false, &physical)) {
              bytes += fmt::format(
                  " {:02X}",
                  *kernel_memory()->TranslatePhysical<const uint8_t*>(
                      physical));
            } else {
              bytes += " ??";
            }
          }
          XELOGI("BACKCOMPAT   x86 @{:08X}:{}", start, bytes);
        }
      }
      last_eip = eip;
    }
  }).detach();
}

dword_result_t KeEnterUserMode_entry(const ppc_context_t& ctx) {
  // r3: CONTEXT with the starting state, r4: xefu's handler.
  LogBackcompatCall("KeEnterUserMode", ctx);
  uint32_t context = uint32_t(ctx->r[3]);
  if (!context || !SyncUserSpace()) {
    return X_STATUS_UNSUCCESSFUL;
  }
  cpu::ppc::g_user_mode_syscall_hook = UserModeSyscallHook;

  auto thread = XThread::GetCurrentThread();
  auto thread_state = thread->thread_state();
  auto pc = thread_state->context();

  auto st = std::make_unique<UserModeState>();
  st->handler = uint32_t(ctx->r[4]);
  st->frame = kernel_memory()->SystemHeapAlloc(kFrameSize);
  st->record = kernel_memory()->SystemHeapAlloc(kRecordSize);
  std::memcpy(&st->kernel_regs, pc, sizeof(*pc));

  // Load the user registers from the CONTEXT.
  auto c = kernel_memory()->TranslateVirtual<const uint8_t*>(context);
  uint32_t iar = xe::load_and_swap<uint32_t>(c + kCtxIar);
  pc->lr = xe::load_and_swap<uint32_t>(c + kCtxLr);
  pc->ctr = xe::load_and_swap<uint64_t>(c + kCtxCtr);
  for (int n = 0; n < 32; ++n) {
    pc->r[n] = xe::load_and_swap<uint64_t>(c + kCtxGpr + 8 * n);
  }
  SetCrFromValue(pc, xe::load_and_swap<uint32_t>(c + kCtxCr));
  SetXer(pc, xe::load_and_swap<uint32_t>(c + kCtxXer));
  for (int n = 0; n < 32; ++n) {
    uint64_t bits = xe::load_and_swap<uint64_t>(c + kCtxFpr + 8 * n);
    std::memcpy(&pc->f[n], &bits, 8);
  }
  for (int n = 0; n < 128; ++n) {
    for (int w = 0; w < 4; ++w) {
      pc->v[n].u32[w] =
          xe::load_and_swap<uint32_t>(c + kCtxVr + 16 * n + 4 * w);
    }
  }
  pc->sc_resume = 0;
  pc->virtual_membase = g_alias_base;
  LogUserPageTableChanges();  // baseline for the research diff
  XELOGI("BACKCOMPAT entering user mode at {:08X} (r1={:08X} r2={:08X})", iar,
         uint32_t(pc->r[1]), uint32_t(pc->r[2]));

  auto processor = kernel_state()->processor();
  auto backend = processor->backend();
  UserModeState* previous = t_user_mode;
  UserModeState* state = st.get();
  state->next_iar = iar;
  state->guest_stack_depth = backend->GetGuestStackDepth(pc);
  t_user_mode = state;
  shim::t_trace_exports = true;
  StartUserModeHeartbeat(pc);
  // The trap frame xefu raises interrupts in (see PropagateUserInterruptFlag).
  auto kthread = thread->guest_object<X_KTHREAD>();
  uint32_t previous_trap_frame = kthread->unk_128;
  kthread->unk_128 = state->frame;
  g_user_interrupt_frame = state->frame;
  g_user_context = pc;
  uint32_t status = X_STATUS_SUCCESS;
  try {
    // Only memory (state) is reliable after a restart: locals changed past
    // this point would come back with stale values.
#if XE_USER_MODE_FAULTS
    RtlCaptureContext(&state->restart);
#endif
    if (state->fault.pending) {
      state->fault.pending = false;
      backend->SetGuestStackDepth(pc, state->guest_stack_depth);
      // Pages xefu maps that Xenia couldn't (4 KB pages): do the access
      // here. Others are xefu's to handle, except the Xbox's GPU registers
      // (0xFD000000-0xFEFFFFFF), which xefu emulates on faults.
      uint32_t fault_address = state->fault.address;
      bool gpu = fault_address >= 0xFD000000u && fault_address < 0xFF000000u;
      // A page xefu mapped without a TB flush (a new mapping needs none):
      // map it and retry, rather than emulating every access to it.
      bool retry = false;
      uint32_t physical = 0;
      if (!gpu && kernel_memory()->alias_page_granular() &&
          !(state->fault.iar == state->remap_iar &&
            fault_address == state->remap_address) &&
          TranslateUserAddress(fault_address, state->fault.is_write,
                               &physical)) {
        uint32_t mapped = 0, unmapped = 0;
        SyncUserPage(fault_address >> kUserPageShift, &mapped, &unmapped);
        retry = mapped != 0;
      }
      if (retry) {
        state->remap_iar = state->fault.iar;
        state->remap_address = fault_address;
        state->next_iar = state->fault.iar;
      } else {
        state->remap_iar = 0;
        state->remap_address = 0;
        if (!gpu && EmulateUserLoadStore(pc, state->fault.iar)) {
          state->next_iar = state->fault.iar + 4;
        } else {
          state->next_iar = DeliverUserModeFault(state, pc);
        }
      }
    }
    XEFU_PHASE();
    processor->ExecuteRaw(thread_state, state->next_iar);
    XELOGW("BACKCOMPAT user mode code returned to the kernel");
  } catch (const UserModeExit& exit) {
    status = exit.status;
  }
  t_user_mode = previous;
  g_user_interrupt_frame = 0;
  g_user_context = nullptr;
  kthread->unk_128 = previous_trap_frame;
  std::memcpy(pc, &st->kernel_regs, sizeof(*pc));
  kernel_memory()->SystemHeapFree(st->record);
  kernel_memory()->SystemHeapFree(st->frame);
  XELOGI("BACKCOMPAT left user mode, status {:08X}", status);
  return status;
}
DECLARE_XBOXKRNL_EXPORT1(KeEnterUserMode, kNone, kSketchy);

dword_result_t KeLeaveUserMode_entry(const ppc_context_t& ctx) {
  LogBackcompatCall("KeLeaveUserMode", ctx);
  if (t_user_mode) {
    throw UserModeExit{uint32_t(ctx->r[3])};
  }
  return X_STATUS_SUCCESS;
}
DECLARE_XBOXKRNL_EXPORT1(KeLeaveUserMode, kNone, kSketchy);

// Research: what xefu changes in its page table structure (1 MB at the
// KeCreateUserMode r4) before flushing the TB, and the address list it
// leaves at +0x880 (count) / +0x884 (entries).
static void LogUserPageTableChanges() {
  static std::vector<uint32_t> previous;
  static uint32_t calls = 0;
  if (!g_user_page_table || calls >= 4) {
    return;
  }
  ++calls;
  constexpr uint32_t kWords = 0x100000 / 4;
  auto table =
      kernel_memory()->TranslateVirtual<const uint8_t*>(g_user_page_table);
  std::vector<uint32_t> current(kWords);
  for (uint32_t n = 0; n < kWords; ++n) {
    current[n] = xe::load_and_swap<uint32_t>(table + 4 * n);
  }
  uint32_t count = current[0x880 / 4];
  std::string list;
  for (uint32_t n = 0; n < std::min<uint32_t>(count, 16); ++n) {
    list += fmt::format(" {:08X}", current[0x884 / 4 + n]);
  }
  XELOGI("BACKCOMPAT TB flush list: count {}:{}", count, list);
  uint32_t logged = 0;
  for (uint32_t n = 0; n < kWords && logged < 96; ++n) {
    uint32_t before = previous.empty() ? 0 : previous[n];
    if (current[n] != before) {
      XELOGI("BACKCOMPAT   page table +{:05X}: {:08X} -> {:08X}", n * 4, before,
             current[n]);
      ++logged;
    }
  }
  previous = std::move(current);
}

dword_result_t KeFlushUserModeCurrentTb_entry(const ppc_context_t& ctx) {
  static std::atomic<uint32_t> flushes{0};
  if (flushes.fetch_add(1) < 20) {
    LogBackcompatCall("KeFlushUserModeCurrentTb", ctx);
  }
  LogUserPageTableChanges();
  SyncUserSpace();
  return X_STATUS_SUCCESS;
}
DECLARE_XBOXKRNL_EXPORT1(KeFlushUserModeCurrentTb, kNone, kSketchy);

dword_result_t KeFlushCurrentEntireTb_entry(const ppc_context_t& ctx) {
  LogBackcompatCall("KeFlushCurrentEntireTb", ctx);
  if (g_alias_base) {
    SyncUserSpace();
  }
  return X_STATUS_SUCCESS;
}
DECLARE_XBOXKRNL_EXPORT1(KeFlushCurrentEntireTb, kNone, kSketchy);

// Drops Xenia's translations of user code at the physical range
// [physical, physical + length), wherever user mode maps it.
static void InvalidateUserCode(uint32_t physical, uint32_t length) {
  if (!g_user_module || !length) {
    return;
  }
  auto processor = kernel_state()->processor();
  uint32_t physical_end = physical + length;
  uint32_t dropped = 0;
  // 16 MB pages.
  for (uint32_t seg = kUserCodeLow >> 24; seg < kUserCodeHigh >> 24; ++seg) {
    uint32_t entry = g_alias_entries[seg];
    if (!entry) {
      continue;
    }
    uint32_t frame = entry & 0x1F000000u;
    uint32_t low = std::max(physical, frame);
    uint32_t high = std::min(physical_end, frame + 0x01000000u);
    if (low < high) {
      uint32_t user = seg << 24;
      dropped += processor->InvalidateFunctions(
          g_user_module, user + (low - frame), user + (high - frame));
    }
  }
  // 64 KB pages.
  for (uint32_t page = kUserCodeLow >> kUserPageShift;
       page < kUserCodeHigh >> kUserPageShift; ++page) {
    if (!g_alias_has_small.empty() && g_alias_has_small[page]) {
      for (uint32_t k = 0; k < 16; ++k) {
        uint32_t sub_page = g_alias_small[(page << 4) | k];
        if (!sub_page) {
          continue;
        }
        uint32_t frame = sub_page & ~0xFFFu;
        uint32_t low = std::max(physical, frame);
        uint32_t high = std::min(physical_end, frame + 0x1000u);
        if (low < high) {
          uint32_t user = (page << kUserPageShift) + (k << 12);
          dropped += processor->InvalidateFunctions(
              g_user_module, user + (low - frame), user + (high - frame));
        }
      }
      continue;
    }
    uint32_t mapping = g_alias_pages.empty() ? 0 : g_alias_pages[page];
    if (!mapping) {
      continue;
    }
    uint32_t frame = mapping & ~0xFFFFu;
    uint32_t low = std::max(physical, frame);
    uint32_t high = std::min(physical_end, frame + (1u << kUserPageShift));
    if (low < high) {
      uint32_t user = page << kUserPageShift;
      dropped += processor->InvalidateFunctions(
          g_user_module, user + (low - frame), user + (high - frame));
    }
  }
  static std::atomic<uint32_t> logs{0};
  if (dropped && logs.fetch_add(1) < 50) {
    XELOGI("BACKCOMPAT dropped {} translations of rewritten code at {:08X}",
           dropped, physical);
  }
}

dword_result_t KeSweepIcacheRange_entry(const ppc_context_t& ctx) {
  // xefu calls this after writing newly translated code (through the kernel
  // view of physical memory): Xenia's translations of that code are stale.
  static std::atomic<uint32_t> sweeps{0};
  uint32_t n = sweeps.fetch_add(1);
  g_sweep_count = n + 1;
  if (n < 50) {
    LogBackcompatCall("KeSweepIcacheRange", ctx);
  }
  if (n < 12) {
    DumpGuest("swept", uint32_t(ctx->r[3]),
              std::min<uint32_t>(uint32_t(ctx->r[4]), 0x400));
  }
  uint32_t address = uint32_t(ctx->r[3]);
  uint32_t length = uint32_t(ctx->r[4]);
  if (address >= 0xC0000000u && address < 0xE0000000u) {
    InvalidateUserCode(address & 0x1FFFFFFFu, length);
  }
  return X_STATUS_SUCCESS;
}
DECLARE_XBOXKRNL_EXPORT1(KeSweepIcacheRange, kNone, kStub);

// MmLockAndMapSegmentArray(unknown, segments, length, unknown) maps the pages
// listed in a segment array (one page-aligned virtual address per entry) at
// one contiguous virtual address and returns that address.
// MmUnlockAndUnmapSegmentArray(address) undoes it. xefu uses the pair to copy
// file data into the emulated Xbox's RAM, whose pages are scattered. Xenia
// can't remap guest pages, so the mapping is a bounce buffer: it starts as a
// copy of the pages and is copied back into them when unmapped.
struct SegmentArrayMapping {
  uint32_t base;
  uint32_t length;
  std::vector<uint32_t> pages;
};
static std::mutex g_segment_mapping_mutex;
static std::vector<SegmentArrayMapping> g_segment_mappings;

dword_result_t MmLockAndMapSegmentArray_entry(const ppc_context_t& ctx) {
  static std::atomic<uint32_t> calls{0};
  if (calls.fetch_add(1) < 20) {
    LogBackcompatCall("MmLockAndMapSegmentArray", ctx, 3, 0x40);
  }
  uint32_t segments = uint32_t(ctx->r[4]);
  uint32_t length = uint32_t(ctx->r[5]);
  uint32_t page_count = (length + 0xFFF) >> 12;
  if (!segments || !page_count || page_count > 0x4000) {
    return 0;
  }
  auto memory = kernel_state()->memory();
  SegmentArrayMapping mapping;
  mapping.length = page_count << 12;
  mapping.base = memory->SystemHeapAlloc(mapping.length, 0x1000);
  if (!mapping.base) {
    XELOGE("BACKCOMPAT MmLockAndMapSegmentArray: out of memory for {} pages",
           page_count);
    return 0;
  }
  auto segment_list = memory->TranslateVirtual<xe::be<uint32_t>*>(segments);
  for (uint32_t i = 0; i < page_count; ++i) {
    uint32_t page = uint32_t(segment_list[i]) & ~0xFFFu;
    mapping.pages.push_back(page);
    std::memcpy(memory->TranslateVirtual(mapping.base + (i << 12)),
                memory->TranslateVirtual(page), 0x1000);
  }
  uint32_t base = mapping.base;
  std::lock_guard<std::mutex> lock(g_segment_mapping_mutex);
  g_segment_mappings.push_back(std::move(mapping));
  return base;
}
DECLARE_XBOXKRNL_EXPORT1(MmLockAndMapSegmentArray, kMemory, kImplemented);

dword_result_t MmUnlockAndUnmapSegmentArray_entry(const ppc_context_t& ctx) {
  static std::atomic<uint32_t> calls{0};
  if (calls.fetch_add(1) < 20) {
    LogBackcompatCall("MmUnlockAndUnmapSegmentArray", ctx);
  }
  uint32_t address = uint32_t(ctx->r[3]);
  SegmentArrayMapping mapping;
  {
    std::lock_guard<std::mutex> lock(g_segment_mapping_mutex);
    auto it =
        std::find_if(g_segment_mappings.begin(), g_segment_mappings.end(),
                     [address](const SegmentArrayMapping& m) {
                       return address >= m.base && address < m.base + m.length;
                     });
    if (it == g_segment_mappings.end()) {
      XELOGW("BACKCOMPAT MmUnlockAndUnmapSegmentArray: {:08X} isn't mapped",
             address);
      return X_STATUS_SUCCESS;
    }
    mapping = std::move(*it);
    g_segment_mappings.erase(it);
  }
  auto memory = kernel_state()->memory();
  for (size_t i = 0; i < mapping.pages.size(); ++i) {
    std::memcpy(memory->TranslateVirtual(mapping.pages[i]),
                memory->TranslateVirtual(mapping.base + uint32_t(i << 12)),
                0x1000);
  }
  memory->SystemHeapFree(mapping.base);
  return X_STATUS_SUCCESS;
}
DECLARE_XBOXKRNL_EXPORT1(MmUnlockAndUnmapSegmentArray, kMemory, kImplemented);

dword_result_t IoDismountVolume_entry(const ppc_context_t& ctx) {
  LogBackcompatCall("IoDismountVolume", ctx);
  return X_STATUS_SUCCESS;
}
DECLARE_XBOXKRNL_EXPORT1(IoDismountVolume, kFileSystem, kStub);

dword_result_t IoDismountVolumeByFileHandle_entry(const ppc_context_t& ctx) {
  LogBackcompatCall("IoDismountVolumeByFileHandle", ctx);
  return X_STATUS_SUCCESS;
}
DECLARE_XBOXKRNL_EXPORT1(IoDismountVolumeByFileHandle, kFileSystem, kStub);

// Kernel timers with DPCs (KeSetTimerEx / KeCancelTimer), which xefu uses to
// drive the emulated Xbox's timer interrupt. A host thread with a guest
// context runs each timer's DPC routine when it expires, and rearms periodic
// ones. The timer's signal state is set too, for anything waiting on it.
struct PendingKernelTimer {
  uint32_t timer;
  uint32_t dpc;
  std::chrono::steady_clock::time_point due;
  std::chrono::milliseconds period;
};
static std::mutex g_kernel_timer_mutex;
static std::condition_variable g_kernel_timer_cv;
static std::vector<PendingKernelTimer> g_kernel_timers;
static object_ref<XHostThread> g_kernel_timer_thread;
// DPCs queued with KeInsertQueueDpc while xefu runs, run one at a time on the
// timer thread (see QueueBackcompatDpc).
struct QueuedDpc {
  uint32_t dpc, arg1, arg2;
};
static std::vector<QueuedDpc> g_queued_dpcs;

// The pending kernel timers, for the heartbeat (which mustn't block on them).
static std::string DescribeKernelTimers() {
  std::unique_lock<std::mutex> lock(g_kernel_timer_mutex, std::try_to_lock);
  if (!lock.owns_lock()) {
    return "  kernel timers: (locked)\n";
  }
  auto now = std::chrono::steady_clock::now();
  std::string text =
      fmt::format("  kernel timers ({}):", g_kernel_timers.size());
  for (const auto& timer : g_kernel_timers) {
    text += fmt::format(
        " {:08X} dpc {:08X} in {} ms period {} ms;", timer.timer, timer.dpc,
        std::chrono::duration_cast<std::chrono::milliseconds>(timer.due - now)
            .count(),
        timer.period.count());
  }
  return text + "\n";
}

static bool RemoveKernelTimer(uint32_t timer) {
  for (auto it = g_kernel_timers.begin(); it != g_kernel_timers.end(); ++it) {
    if (it->timer == timer) {
      g_kernel_timers.erase(it);
      return true;
    }
  }
  return false;
}

static int KernelTimerThreadMain() {
  auto thread = XThread::GetCurrentThread();
  auto thread_state = thread->thread_state();
  auto memory = kernel_memory();
  while (true) {
    PendingKernelTimer fired;
    QueuedDpc queued = {};
    {
      std::unique_lock<std::mutex> lock(g_kernel_timer_mutex);
      while (true) {
        if (!g_queued_dpcs.empty()) {
          queued = g_queued_dpcs.front();
          g_queued_dpcs.erase(g_queued_dpcs.begin());
          break;
        }
        auto now = std::chrono::steady_clock::now();
        auto next = g_kernel_timers.end();
        for (auto it = g_kernel_timers.begin(); it != g_kernel_timers.end();
             ++it) {
          if (next == g_kernel_timers.end() || it->due < next->due) {
            next = it;
          }
        }
        if (next == g_kernel_timers.end()) {
          g_kernel_timer_cv.wait(lock);
          continue;
        }
        if (next->due > now) {
          g_kernel_timer_cv.wait_until(lock, next->due);
          continue;
        }
        fired = *next;
        if (next->period.count()) {
          next->due += next->period;
          if (next->due < now) {
            next->due = now + next->period;  // don't try to catch up
          }
        } else {
          g_kernel_timers.erase(next);
        }
        break;
      }
    }
    uint32_t dpc_address;
    uint32_t dpc_arg1 = 0, dpc_arg2 = 0;
    if (queued.dpc) {
      dpc_address = queued.dpc;
      dpc_arg1 = queued.arg1;
      dpc_arg2 = queued.arg2;
    } else {
      auto timer = memory->TranslateVirtual<X_KTIMER*>(fired.timer);
      timer->header.signal_state = 1;
      if (!fired.dpc) {
        continue;
      }
      dpc_address = fired.dpc;
    }
    auto dpc = memory->TranslateVirtual<XDPC*>(dpc_address);
    uint32_t routine = dpc->routine;
    if (!routine) {
      continue;
    }
    if (g_timer_dpc_count.fetch_add(1) < 5) {
      XELOGI("BACKCOMPAT timer {:08X} fired: DPC {:08X} routine {:08X}",
             fired.timer, fired.dpc, routine);
    }
    auto ppc_context = thread_state->context();
    DPCImpersonationScope dpc_scope{};
    kernel_state()->BeginDPCImpersonation(ppc_context, dpc_scope);
    uint64_t args[] = {dpc_address, uint32_t(dpc->context), dpc_arg1, dpc_arg2};
    uint32_t stack_depth =
        kernel_state()->processor()->backend()->GetGuestStackDepth(ppc_context);
    kernel_state()->processor()->Execute(thread_state, routine, args,
                                         xe::countof(args));
    CheckStackpointsAfterCall(ppc_context, stack_depth, "a timer DPC");
    kernel_state()->EndDPCImpersonation(ppc_context, dpc_scope);
    PropagateUserInterruptFlag();
  }
  return 0;
}

// With g_kernel_timer_mutex held.
static void EnsureKernelTimerThread() {
  if (!g_kernel_timer_thread) {
    g_kernel_timer_thread = object_ref<XHostThread>(
        new XHostThread(kernel_state(), 128 * 1024, 0, KernelTimerThreadMain,
                        kernel_state()->GetSystemProcess()));
    g_kernel_timer_thread->set_name("Kernel Timer DPCs");
    g_kernel_timer_thread->Create();
  }
}

// While xefu runs, KeInsertQueueDpc queues DPCs for the timer thread, which
// runs them, and the timers' DPCs, one at a time, as one processor would.
// Run inline on the queuing thread, xefu's DPCs ran concurrently with each
// other (two threads in its interrupt controller emulation deadlocked) and
// while the queuing thread held its spinlocks. Returns -1 when not handled
// here, else KeInsertQueueDpc's result.
int QueueBackcompatDpc(uint32_t dpc, uint32_t arg1, uint32_t arg2) {
  if (!g_dpcs_on_timer_thread) {
    return -1;
  }
  {
    std::lock_guard<std::mutex> lock(g_kernel_timer_mutex);
    for (const QueuedDpc& queued : g_queued_dpcs) {
      if (queued.dpc == dpc) {
        return 0;  // already queued
      }
    }
    g_queued_dpcs.push_back({dpc, arg1, arg2});
    EnsureKernelTimerThread();
  }
  g_kernel_timer_cv.notify_all();
  return 1;
}

// KeRemoveQueueDpc for those: -1 when not handled here.
int RemoveBackcompatDpc(uint32_t dpc) {
  if (!g_dpcs_on_timer_thread) {
    return -1;
  }
  std::lock_guard<std::mutex> lock(g_kernel_timer_mutex);
  for (auto it = g_queued_dpcs.begin(); it != g_queued_dpcs.end(); ++it) {
    if (it->dpc == dpc) {
      g_queued_dpcs.erase(it);
      return 1;
    }
  }
  return 0;
}

dword_result_t KeSetTimerEx_entry(const ppc_context_t& ctx) {
  // KeSetTimerEx(timer, due_time, period in ms, dpc). Due time: negative is
  // relative, in 100 ns units; positive is absolute system time.
  static std::atomic<uint32_t> logs{0};
  if (logs.fetch_add(1) < 10) {
    LogBackcompatCall("KeSetTimerEx", ctx);
  }
  uint32_t timer = uint32_t(ctx->r[3]);
  int64_t due_time = int64_t(ctx->r[4]);
  int32_t period = int32_t(ctx->r[5]);
  uint32_t dpc = uint32_t(ctx->r[6]);
  auto now = std::chrono::steady_clock::now();
  std::chrono::steady_clock::time_point due = now;
  if (due_time < 0) {
    due = now + std::chrono::microseconds(-due_time / 10);
  } else {
    int64_t delta = due_time - int64_t(Clock::QueryGuestSystemTime());
    if (delta > 0) {
      due = now + std::chrono::microseconds(delta / 10);
    }
  }
  // Timers expire on the kernel's clock interrupt, every millisecond: a due
  // time that has passed (xefu asks for 100 ns, to get the next tick) is the
  // next tick. xefu's 1 ms Xbox clock depends on this.
  {
    using namespace std::chrono;
    static const steady_clock::time_point epoch = steady_clock::now();
    auto ticks = duration_cast<milliseconds>(due - epoch).count() + 1;
    auto now_ticks = duration_cast<milliseconds>(now - epoch).count() + 1;
    due = epoch + milliseconds(std::max(ticks, now_ticks));
  }
  {
    static std::atomic<uint32_t> far_logs{0};
    if (due - now > std::chrono::seconds(1) && far_logs.fetch_add(1) < 20) {
      XELOGW(
          "BACKCOMPAT KeSetTimerEx({:08X}) due in {} ms (due time {:X}), "
          "lr={:08X}",
          timer,
          std::chrono::duration_cast<std::chrono::milliseconds>(due - now)
              .count(),
          uint64_t(due_time), uint32_t(ctx->lr));
    }
  }
  auto header = &kernel_memory()->TranslateVirtual<X_KTIMER*>(timer)->header;
  header->signal_state = 0;
  bool was_set;
  {
    std::lock_guard<std::mutex> lock(g_kernel_timer_mutex);
    was_set = RemoveKernelTimer(timer);
    g_kernel_timers.push_back(
        {timer, dpc, due, std::chrono::milliseconds(std::max(period, 0))});
    EnsureKernelTimerThread();
  }
  g_kernel_timer_cv.notify_all();
  return was_set ? 1 : 0;
}
DECLARE_XBOXKRNL_EXPORT1(KeSetTimerEx, kThreading, kSketchy);

dword_result_t KeCancelTimer_entry(const ppc_context_t& ctx) {
  static std::atomic<uint32_t> logs{0};
  if (logs.fetch_add(1) < 20) {
    LogBackcompatCall("KeCancelTimer", ctx);
  }
  bool was_set;
  {
    std::lock_guard<std::mutex> lock(g_kernel_timer_mutex);
    was_set = RemoveKernelTimer(uint32_t(ctx->r[3]));
  }
  g_kernel_timer_cv.notify_all();
  return was_set ? 1 : 0;
}
DECLARE_XBOXKRNL_EXPORT1(KeCancelTimer, kThreading, kSketchy);

}  // namespace xboxkrnl
}  // namespace kernel
}  // namespace xe

DECLARE_XBOXKRNL_EMPTY_REGISTER_EXPORTS(Misc);
