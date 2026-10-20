/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include <atomic>
#include <string>

#include "xenia/base/logging.h"
#include "xenia/base/memory.h"
#include "xenia/cpu/processor.h"
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

dword_result_t KeCreateUserMode_entry(const ppc_context_t& ctx) {
  LogBackcompatCall("KeCreateUserMode", ctx);
  DumpGuest("r4", uint32_t(ctx->r[4]), 0x1000);
  DumpGuest("r6", uint32_t(ctx->r[6]), 0x800);
  DumpGuest("r7", uint32_t(ctx->r[7]), 0x800);
  return X_STATUS_SUCCESS;
}
DECLARE_XBOXKRNL_EXPORT1(KeCreateUserMode, kNone, kStub);

dword_result_t KeEnterUserMode_entry(const ppc_context_t& ctx) {
  // r3: the user-mode starting context (0xA40 bytes), r4: xefu's handler.
  LogBackcompatCall("KeEnterUserMode", ctx);
  uint32_t context = uint32_t(ctx->r[3]);
  DumpGuest("context", context, 0x600);
  auto heap = context ? kernel_memory()->LookupHeap(context) : nullptr;
  uint32_t protect = 0;
  if (heap && heap->QueryProtect(context, &protect) &&
      (protect & kMemoryProtectRead)) {
    auto p = kernel_memory()->TranslateVirtual<const uint8_t*>(context);
    // Whatever the layout, look at what the first pointer-like words hold.
    for (uint32_t off : {0x08u, 0x0Cu, 0x2Cu, 0x54u}) {
      uint32_t target = xe::load_and_swap<uint32_t>(p + off);
      DumpGuest(fmt::format("context+{:X}->", off).c_str(), target, 0x200);
    }
  }
  DumpGuest("handler", uint32_t(ctx->r[4]), 0x80);
  return X_STATUS_SUCCESS;
}
DECLARE_XBOXKRNL_EXPORT1(KeEnterUserMode, kNone, kStub);

dword_result_t KeLeaveUserMode_entry(const ppc_context_t& ctx) {
  LogBackcompatCall("KeLeaveUserMode", ctx);
  return X_STATUS_SUCCESS;
}
DECLARE_XBOXKRNL_EXPORT1(KeLeaveUserMode, kNone, kStub);

dword_result_t KeFlushUserModeCurrentTb_entry(const ppc_context_t& ctx) {
  LogBackcompatCall("KeFlushUserModeCurrentTb", ctx);
  return X_STATUS_SUCCESS;
}
DECLARE_XBOXKRNL_EXPORT1(KeFlushUserModeCurrentTb, kNone, kStub);

dword_result_t KeFlushCurrentEntireTb_entry(const ppc_context_t& ctx) {
  LogBackcompatCall("KeFlushCurrentEntireTb", ctx);
  return X_STATUS_SUCCESS;
}
DECLARE_XBOXKRNL_EXPORT1(KeFlushCurrentEntireTb, kNone, kStub);

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
