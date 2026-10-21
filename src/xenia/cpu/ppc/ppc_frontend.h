/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2013 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_CPU_PPC_PPC_FRONTEND_H_
#define XENIA_CPU_PPC_PPC_FRONTEND_H_

#include <atomic>
#include <memory>

#include "xenia/base/type_pool.h"
#include "xenia/cpu/function.h"
#include "xenia/memory.h"

namespace xe {
namespace cpu {
class Processor;
}  // namespace cpu
}  // namespace xe

namespace xe {
namespace cpu {
namespace ppc {

class PPCTranslator;
typedef struct PPCContext_s PPCContext;

struct PPCBuiltins {
  int32_t global_lock_count;
  Function* check_global_lock;
  Function* enter_global_lock;
  Function* leave_global_lock;
  Function* syscall_handler;
};

// Called for every `sc` (LEV 0) before the default handling; returns true if
// it handled the call. Set by the kernel for emulated user mode.
using UserModeSyscallHook = bool (*)(PPCContext* ppc_context);
extern UserModeSyscallHook g_user_mode_syscall_hook;

// Research: the r2 values (xefu's x86 EIP) seen when user-mode code is
// resolved, as a ring (newest at index - 1); consecutive repeats skipped.
constexpr uint32_t kUserTraceSize = 256;
extern uint32_t g_user_trace[kUserTraceSize];
extern std::atomic<uint32_t> g_user_trace_index;

class PPCFrontend {
 public:
  explicit PPCFrontend(Processor* processor);
  ~PPCFrontend();

  bool Initialize();

  Processor* processor() const { return processor_; }
  Memory* memory() const;
  PPCBuiltins* builtins() { return &builtins_; }

  bool DeclareFunction(GuestFunction* function);
  bool DefineFunction(GuestFunction* function, uint32_t debug_info_flags);

 private:
  Processor* processor_;
  PPCBuiltins builtins_ = {0};
  TypePool<PPCTranslator, PPCFrontend*> translator_pool_;
};
// Checks the state of the global lock and sets scratch to the current MSR
// value.
void CheckGlobalLock(PPCContext* ppc_context, void* arg0, void* arg1);
}  // namespace ppc
}  // namespace cpu
}  // namespace xe

#endif  // XENIA_CPU_PPC_PPC_FRONTEND_H_
