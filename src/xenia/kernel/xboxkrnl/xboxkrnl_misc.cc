/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include <atomic>
#include <cstring>
#include <memory>
#include <string>

#include "xenia/base/logging.h"
#include "xenia/base/memory.h"
#include "xenia/cpu/backend/backend.h"
#include "xenia/cpu/module.h"
#include "xenia/cpu/ppc/ppc_context.h"
#include "xenia/cpu/ppc/ppc_frontend.h"
#include "xenia/cpu/processor.h"
#include "xenia/kernel/kernel_state.h"
#include "xenia/kernel/util/shim_utils.h"
#include "xenia/kernel/xboxkrnl/xboxkrnl_private.h"
#include "xenia/kernel/xthread.h"
#include "xenia/xbox.h"

DEFINE_bool(allow_mic_initialization, false,
            "Enable Mic Initialization\n"
            " Only set true when testing mic related functions",
            "Kernel");

namespace xe {
namespace kernel {
namespace xboxkrnl {

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
  g_user_page_table = uint32_t(ctx->r[4]);
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

struct UserModeState {
  uint32_t handler = 0;
  uint32_t frame = 0;  // trap frame, kernel memory
  cpu::ppc::PPCContext kernel_regs;
  cpu::ppc::PPCContext user_regs;
};

static uint8_t* g_alias_base = nullptr;
static uint32_t g_alias_entries[0x100] = {};
static thread_local UserModeState* t_user_mode = nullptr;
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

// Brings the alias space in line with xefu's page table.
static bool SyncUserSpace() {
  auto memory = kernel_memory();
  if (!g_alias_base) {
    g_alias_base = memory->CreateAliasSpace();
    if (!g_alias_base) {
      return false;
    }
    memory->SetCodeAliasRange(g_alias_base, kUserCodeLow, kUserCodeHigh);
    auto processor = kernel_state()->processor();
    processor->AddModule(std::make_unique<UserModeCodeModule>(processor));
  }
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
      if (memory->MapAliasRange(g_alias_base, seg << 24, 0x01000000,
                                entry & 0x1F000000u)) {
        g_alias_entries[seg] = entry;
        XELOGI("BACKCOMPAT user segment {:02X}000000 -> physical {:08X}", seg,
               entry & 0x1F000000u);
      }
    }
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

// `sc` from user code: hand it to xefu as (record = null, trap frame).
static bool UserModeSyscallHook(cpu::ppc::PPCContext* pc) {
  UserModeState* st = t_user_mode;
  if (!st || pc->virtual_membase != g_alias_base) {
    return false;
  }
  uint32_t sc_address = uint32_t(pc->sc_address);
  if (g_syscall_logs.fetch_add(1) < 200) {
    XELOGI("BACKCOMPAT syscall r0={:X} at {:08X} (r3={:08X} r4={:08X})",
           uint32_t(pc->r[0]), sc_address, uint32_t(pc->r[3]),
           uint32_t(pc->r[4]));
  }
  std::memcpy(&st->user_regs, pc, sizeof(*pc));
  WriteFrame(st, sc_address);

  // Run the handler as kernel code: kernel registers and memory view.
  uint64_t kernel_r1 = st->kernel_regs.r[1];
  std::memcpy(pc, &st->kernel_regs, sizeof(*pc));
  pc->r[1] = kernel_r1 - 0x400;  // keep clear of the caller's frame
  uint64_t args[] = {0, st->frame};
  auto thread = XThread::GetCurrentThread();
  kernel_state()->processor()->Execute(thread->thread_state(), st->handler,
                                       args, xe::countof(args));

  // Back to user mode with whatever the handler left in the frame.
  std::memcpy(pc, &st->user_regs, sizeof(*pc));
  uint32_t iar = ReadFrame(st, pc);
  pc->virtual_membase = g_alias_base;
  pc->sc_resume = (iar == sc_address + 4) ? 0 : iar;
  return true;
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
  XELOGI("BACKCOMPAT entering user mode at {:08X} (r1={:08X} r2={:08X})", iar,
         uint32_t(pc->r[1]), uint32_t(pc->r[2]));

  UserModeState* previous = t_user_mode;
  t_user_mode = st.get();
  uint32_t status = X_STATUS_SUCCESS;
  try {
    kernel_state()->processor()->ExecuteRaw(thread_state, iar);
    XELOGW("BACKCOMPAT user mode code returned to the kernel");
  } catch (const UserModeExit& exit) {
    status = exit.status;
  }
  t_user_mode = previous;
  std::memcpy(pc, &st->kernel_regs, sizeof(*pc));
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

dword_result_t KeFlushUserModeCurrentTb_entry(const ppc_context_t& ctx) {
  LogBackcompatCall("KeFlushUserModeCurrentTb", ctx);
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

dword_result_t KeSweepIcacheRange_entry(const ppc_context_t& ctx) {
  // xefu calls this after writing newly translated code. Xenia will need to
  // drop its own translations of that range here; logged for now.
  LogBackcompatCall("KeSweepIcacheRange", ctx);
  return X_STATUS_SUCCESS;
}
DECLARE_XBOXKRNL_EXPORT1(KeSweepIcacheRange, kNone, kStub);

dword_result_t MmLockAndMapSegmentArray_entry(const ppc_context_t& ctx) {
  LogBackcompatCall("MmLockAndMapSegmentArray", ctx, 3, 0x40);
  return X_STATUS_SUCCESS;
}
DECLARE_XBOXKRNL_EXPORT1(MmLockAndMapSegmentArray, kNone, kStub);

dword_result_t MmUnlockAndUnmapSegmentArray_entry(const ppc_context_t& ctx) {
  LogBackcompatCall("MmUnlockAndUnmapSegmentArray", ctx);
  return X_STATUS_SUCCESS;
}
DECLARE_XBOXKRNL_EXPORT1(MmUnlockAndUnmapSegmentArray, kNone, kStub);

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

dword_result_t KeSetTimerEx_entry(const ppc_context_t& ctx) {
  // KeSetTimerEx(timer, due_time, period, dpc): not yet backed by a real
  // timer; returns "timer was not already set".
  LogBackcompatCall("KeSetTimerEx", ctx);
  return 0;
}
DECLARE_XBOXKRNL_EXPORT1(KeSetTimerEx, kThreading, kStub);

dword_result_t KeCancelTimer_entry(const ppc_context_t& ctx) {
  LogBackcompatCall("KeCancelTimer", ctx);
  return 0;
}
DECLARE_XBOXKRNL_EXPORT1(KeCancelTimer, kThreading, kStub);

}  // namespace xboxkrnl
}  // namespace kernel
}  // namespace xe

DECLARE_XBOXKRNL_EMPTY_REGISTER_EXPORTS(Misc);
