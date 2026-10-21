/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2015 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/kernel/util/shim_utils.h"
#include "xenia/kernel/xthread.h"
namespace xe {
namespace kernel {
namespace shim {

thread_local bool t_trace_exports = false;
std::atomic<cpu::Export*> g_traced_export{nullptr};
std::atomic<uint32_t> g_traced_export_returned{0};
ThreadExportTrace g_thread_export_traces[kThreadExportTraceSize];

thread_local StringBuffer string_buffer_;

StringBuffer* thread_local_string_buffer() { return &string_buffer_; }

XThread* ContextParam::CurrentXThread() const {
  return XThread::GetCurrentThread();
}

}  // namespace shim
}  // namespace kernel
}  // namespace xe
