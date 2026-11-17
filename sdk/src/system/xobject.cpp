/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2022 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#include <vector>
#include <string>
#include <chrono>
#include <atomic>
#include <mutex>

#include <rex/chrono/clock.h>
#include <rex/cvar.h>
#include <rex/system/wait_diag.h>  // tw2de (2026-10-25)
#include <rex/stream.h>
#include <rex/system/kernel_state.h>
#include <rex/system/util/string_utils.h>  // For TranslateAnsiStringAddress
#include <rex/system/xobject.h>
// #include <rex/kernel/xboxkrnl/private.h>  // TODO: JIT only
#include <rex/system/xenumerator.h>
#include <rex/system/xevent.h>
#include <rex/system/xfile.h>
#include <rex/system/xmodule.h>
#include <rex/system/xmutant.h>
#include <rex/system/xnotifylistener.h>
#include <rex/system/xsemaphore.h>
#include <rex/system/xsymboliclink.h>
#include <rex/system/xthread.h>
#include <rex/system/thread_state.h>
#include <rex/memory/utils.h>
#include <rex/logging.h>
#include <fmt/format.h>
#include <rex/sys_counters.h>

#include "global_lock_spin.h"

// tw2de (2026-10-24): the periodic reports and stall diagnostics of the SDK and the title, off by default (they wrote
// a few dozen lines every 10 s on every run, some from the game thread, and the stall diagnostics pause every thread).
REXCVAR_DEFINE_BOOL(log_diagnostics, false, "Logging",
                    "Diagnostic reports in the log: the 10 s reports (renderer, audio, I/O, guest waits, memory, fence "
                    "waits, memexport) and the slow-load diagnostics ([slots], [vins], [resmgr] and their stack dumps "
                    "into rex_stall.log)");
// TW2 (2026-11-17): the saves the main menu offers (Load / Continue), with each step that leads to them.
REXCVAR_DEFINE_BOOL(tw2de_diag_saves, false, "Game/Diagnostics",
                    "Log (\"[saves]\" lines) the save list and what leads to it: the content enumerators "
                    "and their reads, sign-in and XUID answers, the storage device queries, the notifications "
                    "taken, the save containers opened and closed, and file opens and directory reads "
                    "outside game:. For a run where the main menu shows no saves.");

namespace rex::system {

XObject::XObject(Type type) : kernel_state_(nullptr), pointer_ref_count_(1), type_(type) {
  handles_.reserve(10);
}

XObject::XObject(KernelState* kernel_state, Type type)
    : kernel_state_(kernel_state),
      pointer_ref_count_(1),
      type_(type),
      guest_object_ptr_(0),
      allocated_guest_object_(false) {
  handles_.reserve(10);

  // TODO: Assert kernel_state != nullptr in this constructor.
  if (kernel_state) {
    kernel_state->object_table()->AddHandle(this, nullptr);
  }
}

XObject::~XObject() {
  // TODO: these are being asserted true... find out why
  // assert_true(handles_.empty());
  // assert_zero(pointer_ref_count_);

  if (allocated_guest_object_) {
    uint32_t ptr = guest_object_ptr_ - sizeof(X_OBJECT_HEADER);
    auto header = memory()->TranslateVirtual<X_OBJECT_HEADER*>(ptr);

    // Free the object creation info
    if (header->object_type_ptr) {
      memory()->SystemHeapFree(header->object_type_ptr);
    }

    memory()->SystemHeapFree(ptr);
  }
}

rex::Runtime* XObject::emulator() const {
  return kernel_state_->emulator_;
}
KernelState* XObject::kernel_state() const {
  return kernel_state_;
}
rex::memory::Memory* XObject::memory() const {
  return kernel_state_->memory();
}

XObject::Type XObject::type() const {
  return type_;
}

void XObject::RetainHandle() {
  kernel_state_->object_table()->RetainHandle(handles_[0]);
}

bool XObject::ReleaseHandle() {
  // FIXME: Return true when handle is actually released.
  return kernel_state_->object_table()->ReleaseHandle(handles_[0]) == X_STATUS_SUCCESS;
}

X_STATUS XObject::Delete() {
  if (kernel_state_ == nullptr) {
    // Fake return value for api-scanner
    return X_STATUS_SUCCESS;
  } else {
    if (!name_.empty()) {
      kernel_state_->object_table()->RemoveNameMapping(name_);
    }
    return kernel_state_->object_table()->RemoveHandle(handles_[0]);
  }
}

bool XObject::SaveObject(stream::ByteStream* stream) {
  stream->Write<uint32_t>(allocated_guest_object_);
  stream->Write<uint32_t>(guest_object_ptr_);

  stream->Write(uint32_t(handles_.size()));
  stream->Write(&handles_[0], handles_.size() * sizeof(X_HANDLE));

  return true;
}

bool XObject::RestoreObject(stream::ByteStream* stream) {
  allocated_guest_object_ = stream->Read<uint32_t>() > 0;
  guest_object_ptr_ = stream->Read<uint32_t>();

  handles_.resize(stream->Read<uint32_t>());
  stream->Read(&handles_[0], handles_.size() * sizeof(X_HANDLE));

  // Restore our pointer to our handles in the object table.
  for (size_t i = 0; i < handles_.size(); i++) {
    kernel_state_->object_table()->RestoreHandle(handles_[i], this);
  }

  return true;
}

object_ref<XObject> XObject::Restore(KernelState* kernel_state, Type type,
                                     stream::ByteStream* stream) {
  switch (type) {
    case Type::Enumerator:
      break;
    case Type::Event:
      return XEvent::Restore(kernel_state, stream);
    case Type::File:
      return XFile::Restore(kernel_state, stream);
    case Type::IOCompletion:
      break;
    case Type::Module:
      return XModule::Restore(kernel_state, stream);
    case Type::Mutant:
      return XMutant::Restore(kernel_state, stream);
    case Type::NotifyListener:
      return XNotifyListener::Restore(kernel_state, stream);
    case Type::Semaphore:
      return XSemaphore::Restore(kernel_state, stream);
    case Type::Session:
      break;
    case Type::Socket:
      break;
    case Type::SymbolicLink:
      return XSymbolicLink::Restore(kernel_state, stream);
    case Type::Thread:
      return XThread::Restore(kernel_state, stream);
    case Type::Timer:
      break;
    case Type::Undefined:
      break;
  }

  assert_always("No restore handler exists for this object!");
  return nullptr;
}

void XObject::SetAttributes(uint32_t obj_attributes_ptr) {
  if (!obj_attributes_ptr) {
    return;
  }

  auto name = util::TranslateAnsiStringAddress(
      memory(),
      memory::load_and_swap<uint32_t>(memory()->TranslateVirtual(obj_attributes_ptr + 4)));
  if (!name.empty()) {
    name_ = std::string(name);
    kernel_state_->object_table()->AddNameMapping(name_, handles_[0]);
  }
}

uint32_t XObject::TimeoutTicksToMs(int64_t timeout_ticks) {
  if (timeout_ticks > 0) {
    // Absolute time, based on January 1, 1601 (guest system time).
    const int64_t now = static_cast<int64_t>(rex::chrono::Clock::QueryGuestSystemTime());
    if (timeout_ticks <= now) return 0;
    const uint64_t ms = static_cast<uint64_t>(timeout_ticks - now) / 10000;
    return ms >= 0xFFFFFFFEull ? 0xFFFFFFFEu : static_cast<uint32_t>(ms);
  } else if (timeout_ticks < 0) {
    // Relative time.
    return (uint32_t)(-timeout_ticks / 10000);  // Ticks -> MS
  } else {
    return 0;
  }
}

// tw2de (2026-10-22): which guest thread waits on what, for how long, from where in the game (its LR: the
// return address of the wait's caller, a guest address). ReportLongGuestWaits logs every guest thread blocked for 3
// s or more: the menu music never starts on the Switch, and the thread that would start it waits on something.
namespace {
struct GuestWaitInfo {
  std::atomic<int64_t> since_ms{0};  // 0: not waiting
  std::atomic<uint32_t> lr{0};
  std::atomic<uint32_t> type{0};
  std::atomic<uint32_t> count{0};
  std::atomic<uint32_t> handle{0};
  std::atomic<int32_t> timeout_ms{-1};
  std::atomic<uint32_t> callers[4] = {};  // the guest return addresses above the wait's caller, from its back chain
  uint32_t thread_id = 0;
  std::string name;
};
std::mutex g_guest_waits_mutex;
std::vector<GuestWaitInfo*> g_guest_waits;
thread_local GuestWaitInfo* t_guest_wait = nullptr;

int64_t WaitNowMs() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

struct GuestWaitScope {
  GuestWaitInfo* info = nullptr;
  GuestWaitScope(XObject* first, uint32_t count, uint64_t* opt_timeout) {
    // tw2de (2026-10-25): only for ReportLongGuestWaits, which reports with log_diagnostics alone: without it the
    // stack walk and the stores were ~0.5 us of every guest wait (1,450 mutant waits a frame in the 2026-10-25 run).
    static const bool diagnostics = REXCVAR_GET(log_diagnostics);
    if (!diagnostics)
      return;
    auto* thread = XThread::GetCurrentThread();
    if (!thread)
      return;
    if (!t_guest_wait) {
      t_guest_wait = new GuestWaitInfo();
      t_guest_wait->thread_id = thread->thread_id();
      std::lock_guard<std::mutex> lock(g_guest_waits_mutex);
      g_guest_waits.push_back(t_guest_wait);
    }
    info = t_guest_wait;
    auto* ts = rex::runtime::ThreadState::Get();
    info->lr.store(ts ? uint32_t(ts->context()->lr) : 0, std::memory_order_relaxed);
    // The guest stack's back chain: each frame's word 0 is the caller's r1, and a function that calls saves its LR
    // at -8 from that (stw r12,-8(r1) before stwu). The wait's caller is often a wrapper (WaitForSingleObject):
    // the callers above it say who waits.
    // Only inside the thread's own stack (KTHREAD stack_limit..stack_base): the outermost frame's back chain points
    // at the stack's top, past which nothing is mapped (v61 read guest 0x70300000 there and faulted).
    uint32_t frame = ts ? uint32_t(ts->context()->r1.u32) : 0;
    uint32_t stack_low = 0, stack_high = 0;
    if (thread->is_guest_thread()) {
      auto* kt = thread->guest_object<X_KTHREAD>();
      stack_high = kt->stack_base;
      stack_low = kt->stack_limit;
    }
    auto* mem = kernel_state()->memory();
    auto in_stack = [&](uint32_t a) { return stack_low && a >= stack_low && a + 4 <= stack_high; };
    for (int i = 0; i < 4; ++i) {
      uint32_t ra = 0;
      if (in_stack(frame)) {
        const uint32_t prev = memory::load_and_swap<uint32_t>(mem->TranslateVirtual(frame));
        if (prev > frame && in_stack(prev - 8)) {
          ra = memory::load_and_swap<uint32_t>(mem->TranslateVirtual(prev - 8));
          frame = prev;
        } else {
          frame = 0;
        }
      }
      info->callers[i].store(ra, std::memory_order_relaxed);
    }
    // tw2de (2026-10-25): no copy of the object's name here any more (the report looks it up by handle): every guest
    // wait took g_guest_waits_mutex, one mutex for all threads, and built a string. 536 waits a frame on the main
    // thread alone in the 2026-10-25 [syncwait] run, ~4 us each uncontended.
    info->type.store(first ? uint32_t(first->type()) : 0, std::memory_order_relaxed);
    info->handle.store(first ? first->handle() : 0, std::memory_order_relaxed);
    info->count.store(count, std::memory_order_relaxed);
    // relative timeouts are negative 100 ns ticks; an absolute one is shown as 0
    const int64_t ticks = opt_timeout ? int64_t(*opt_timeout) : 0;
    info->timeout_ms.store(opt_timeout ? int32_t(std::min<int64_t>(ticks < 0 ? -ticks / 10000 : 0, 0x7FFFFFFF)) : -1,
                           std::memory_order_relaxed);
    info->since_ms.store(WaitNowMs(), std::memory_order_release);
  }
  ~GuestWaitScope() {
    if (info)
      info->since_ms.store(0, std::memory_order_release);
  }
};
}  // namespace

void ReportLongGuestWaits() {
  if (!REXCVAR_GET(log_diagnostics))
    return;
  const int64_t now = WaitNowMs();
  std::vector<GuestWaitInfo*> list;
  {
    std::lock_guard<std::mutex> lock(g_guest_waits_mutex);
    list = g_guest_waits;
  }
  static const char* const kTypes[] = {"undefined", "enumerator", "event", "file", "io completion", "module",
                                       "mutant", "notify listener", "semaphore", "session", "socket",
                                       "symbolic link", "thread", "timer", "device"};
  std::string out;
  size_t n = 0;
  for (GuestWaitInfo* w : list) {
    const int64_t since = w->since_ms.load(std::memory_order_acquire);
    if (!since || now - since < 3000)
      continue;
    const uint32_t type = w->type.load(std::memory_order_relaxed);
    const int32_t timeout = w->timeout_ms.load(std::memory_order_relaxed);
    std::string name;
    if (auto obj = kernel_state()->object_table()->LookupObject<XObject>(w->handle.load(std::memory_order_relaxed)))
      name = obj->name();
    std::string callers;
    for (int i = 0; i < 4; ++i) {
      const uint32_t ra = w->callers[i].load(std::memory_order_relaxed);
      if (!ra)
        break;
      callers += fmt::format(" <- {:08X}", ra);
    }
    // tw2de (2026-10-22, the slow loads): a semaphore says when it was last released, and by whom. Released
    // after the wait began with the thread still blocked = a lost wake-up here; not released = the game's own wait.
    std::string sem;
    if (type == 8) {
      if (auto obj = kernel_state()->object_table()->LookupObject<XSemaphore>(w->handle.load(std::memory_order_relaxed))) {
        const int64_t last = obj->diag_last_release_ms.load(std::memory_order_relaxed);
        sem = fmt::format(" [sem max {}: {} releases, {} failed; last {}{}]", obj->maximum_count(),
                          obj->diag_releases.load(std::memory_order_relaxed),
                          obj->diag_failed.load(std::memory_order_relaxed),
                          last ? fmt::format("{:.1f} s ago by thread {:X} (lr {:08X})", double(now - last) / 1000.0,
                                             obj->diag_last_releaser.load(std::memory_order_relaxed),
                                             obj->diag_last_releaser_lr.load(std::memory_order_relaxed))
                               : std::string("never"),
                          last && last > since ? ", AFTER the wait began" : "");
      }
    }
    callers += sem;
    out += fmt::format("\n    thread {:X}: {:.1f} s on {} {} handle {:08X}{}{}, timeout {}, called from guest {:08X}{}",
                       w->thread_id, double(now - since) / 1000.0, w->count.load(std::memory_order_relaxed),
                       type < std::size(kTypes) ? kTypes[type] : "?", w->handle.load(std::memory_order_relaxed),
                       name.empty() ? "" : " '" + name + "'",
                       w->count.load(std::memory_order_relaxed) > 1 ? " (the first)" : "",
                       timeout < 0 ? std::string("none") : fmt::format("{} ms", timeout),
                       w->lr.load(std::memory_order_relaxed), callers);
    ++n;
  }
  REXLOG_INFO("[waits] {} guest threads blocked 3 s or more{}", n, out);
}

X_STATUS XObject::Wait(uint32_t wait_reason, uint32_t processor_mode, uint32_t alertable,
                       uint64_t* opt_timeout) {
  REX_SYS_CENSUS(rex::syscount::kCensusWait, 1);
  auto wait_handle = GetWaitHandle();
  if (!wait_handle) {
    // Object doesn't support waiting.
    return X_STATUS_SUCCESS;
  }

  auto timeout_ms = opt_timeout ? std::chrono::milliseconds(chrono::Clock::ScaleGuestDurationMillis(
                                      TimeoutTicksToMs(*opt_timeout)))
                                : std::chrono::milliseconds::max();

  XThread::CheckTitleTermination();
  GuestWaitScope wait_scope(this, 1, opt_timeout);
  // tw2de (2026-10-25, tw2de_diag_sync_waits): the marks NtWaitForSingleObjectEx_entry reads.
  const bool diag = rex::system::wait_diag::on.load(std::memory_order_relaxed);
  if (diag)
    rex::system::wait_diag::t_before_wait = rex::system::wait_diag::Now();
  auto result = rex::thread::Wait(wait_handle, alertable ? true : false, timeout_ms);
  if (diag)
    rex::system::wait_diag::t_after_wait = rex::system::wait_diag::Now();
  XThread::CheckTitleTermination();
  switch (result) {
    case rex::thread::WaitResult::kSuccess:
      WaitCallback();
      return X_STATUS_SUCCESS;
    case rex::thread::WaitResult::kUserCallback:
      // Or X_STATUS_ALERTED?
      return X_STATUS_USER_APC;
    case rex::thread::WaitResult::kTimeout:
      rex::thread::MaybeYield();
      return X_STATUS_TIMEOUT;
    default:
    case rex::thread::WaitResult::kAbandoned:
    case rex::thread::WaitResult::kFailed:
      return X_STATUS_ABANDONED_WAIT_0;
  }
}

X_STATUS XObject::SignalAndWait(XObject* signal_object, XObject* wait_object, uint32_t wait_reason,
                                uint32_t processor_mode, uint32_t alertable,
                                uint64_t* opt_timeout) {
  auto timeout_ms = opt_timeout ? std::chrono::milliseconds(chrono::Clock::ScaleGuestDurationMillis(
                                      TimeoutTicksToMs(*opt_timeout)))
                                : std::chrono::milliseconds::max();

  GuestWaitScope wait_scope(wait_object, 1, opt_timeout);
  auto result =
      rex::thread::SignalAndWait(signal_object->GetWaitHandle(), wait_object->GetWaitHandle(),
                                 alertable ? true : false, timeout_ms);
  switch (result) {
    case rex::thread::WaitResult::kSuccess:
      wait_object->WaitCallback();
      return X_STATUS_SUCCESS;
    case rex::thread::WaitResult::kUserCallback:
      // Or X_STATUS_ALERTED?
      return X_STATUS_USER_APC;
    case rex::thread::WaitResult::kTimeout:
      rex::thread::MaybeYield();
      return X_STATUS_TIMEOUT;
    default:
    case rex::thread::WaitResult::kAbandoned:
    case rex::thread::WaitResult::kFailed:
      return X_STATUS_ABANDONED_WAIT_0;
  }
}

X_STATUS XObject::WaitMultiple(uint32_t count, XObject** objects, uint32_t wait_type,
                               uint32_t wait_reason, uint32_t processor_mode, uint32_t alertable,
                               uint64_t* opt_timeout) {
  std::vector<rex::thread::WaitHandle*> wait_handles(count);
  for (size_t i = 0; i < count; ++i) {
    wait_handles[i] = objects[i]->GetWaitHandle();
    assert_not_null(wait_handles[i]);
  }

  auto timeout_ms = opt_timeout ? std::chrono::milliseconds(chrono::Clock::ScaleGuestDurationMillis(
                                      TimeoutTicksToMs(*opt_timeout)))
                                : std::chrono::milliseconds::max();

  XThread::CheckTitleTermination();
  GuestWaitScope wait_scope(count ? objects[0] : nullptr, count, opt_timeout);
  if (wait_type) {
    auto result =
        rex::thread::WaitAny(std::move(wait_handles), alertable ? true : false, timeout_ms);
    XThread::CheckTitleTermination();
    switch (result.first) {
      case rex::thread::WaitResult::kSuccess:
        objects[result.second]->WaitCallback();

        return X_STATUS(result.second);
      case rex::thread::WaitResult::kUserCallback:
        // Or X_STATUS_ALERTED?
        return X_STATUS_USER_APC;
      case rex::thread::WaitResult::kTimeout:
        rex::thread::MaybeYield();
        return X_STATUS_TIMEOUT;
      default:
      case rex::thread::WaitResult::kAbandoned:
        return X_STATUS(X_STATUS_ABANDONED_WAIT_0 + result.second);
      case rex::thread::WaitResult::kFailed:
        return X_STATUS_UNSUCCESSFUL;
    }
  } else {
    auto result =
        rex::thread::WaitAll(std::move(wait_handles), alertable ? true : false, timeout_ms);
    XThread::CheckTitleTermination();
    switch (result) {
      case rex::thread::WaitResult::kSuccess:
        for (uint32_t i = 0; i < count; i++) {
          objects[i]->WaitCallback();
        }

        return X_STATUS_SUCCESS;
      case rex::thread::WaitResult::kUserCallback:
        // Or X_STATUS_ALERTED?
        return X_STATUS_USER_APC;
      case rex::thread::WaitResult::kTimeout:
        rex::thread::MaybeYield();
        return X_STATUS_TIMEOUT;
      default:
      case rex::thread::WaitResult::kAbandoned:
      case rex::thread::WaitResult::kFailed:
        return X_STATUS_ABANDONED_WAIT_0;
    }
  }
}

uint8_t* XObject::CreateNative(uint32_t size) {
  auto global_lock = rex::thread::global_critical_region::AcquireDirect();

  uint32_t total_size = size + sizeof(X_OBJECT_HEADER);

  auto mem = memory()->SystemHeapAlloc(total_size);
  if (!mem) {
    // Out of memory!
    return nullptr;
  }

  allocated_guest_object_ = true;
  memory()->Zero(mem, total_size);
  SetNativePointer(mem + sizeof(X_OBJECT_HEADER), true);

  auto header = memory()->TranslateVirtual<X_OBJECT_HEADER*>(mem);

  auto object_type = memory()->SystemHeapAlloc(sizeof(X_OBJECT_TYPE));
  if (object_type) {
    // Set it up in the header.
    // Some kernel method is accessing this struct and dereferencing a member
    // @ offset 0x14
    header->object_type_ptr = object_type;
  }

  return memory()->TranslateVirtual(guest_object_ptr_);
}

void XObject::SetNativePointer(uint32_t native_ptr, bool uninitialized) {
  auto global_lock = rex::thread::global_critical_region::AcquireDirect();

  // If hit: We've already setup the native ptr with CreateNative!
  assert_zero(guest_object_ptr_);

  auto header = kernel_state_->memory()->TranslateVirtual<X_DISPATCH_HEADER*>(native_ptr);

  // Memory uninitialized, so don't bother with the check.
  if (!uninitialized) {
    assert_true(!(header->wait_list_blink & 0x1));
  }

  // Stash pointer in struct.
  // FIXME: This assumes the object has a dispatch header (some don't!)
  StashHandle(header, handle());

  guest_object_ptr_ = native_ptr;
}

object_ref<XObject> XObject::GetNativeObject(KernelState* kernel_state, void* native_ptr,
                                             int32_t as_type) {
  assert_not_null(native_ptr);
  REX_SYS_CENSUS(rex::syscount::kCensusNativeObject, 1);

  // Unfortunately the XDK seems to inline some KeInitialize calls, meaning
  // we never see it and just randomly start getting passed events/timers/etc.
  // Luckily it seems like all other calls (Set/Reset/Wait/etc) are used and
  // we don't have to worry about PPC code poking the struct. Because of that,
  // we init on first use, store our handle in the struct, and dereference it
  // each time.
  // We identify this by setting wait_list_flink to a magic value. When set,
  // wait_list_blink will hold a handle to our object.

  auto global_lock = AcquireGlobalSpin(rex::thread::global_critical_region::mutex());

  auto header = reinterpret_cast<X_DISPATCH_HEADER*>(native_ptr);

  if (as_type == -1) {
    as_type = header->type;
  }

  if (header->wait_list_flink == kXObjSignature) {
    // Already initialized.
    // TODO: assert if the type of the object != as_type
    uint32_t handle = header->wait_list_blink;
    auto object = kernel_state->object_table()->LookupObject<XObject>(handle);

    // TODO(benvanik): assert nothing has been changed in the struct.
    return object;
  } else {
    // First use, create new.
    // https://www.nirsoft.net/kernel_struct/vista/KOBJECTS.html
    XObject* object = nullptr;
    switch (as_type) {
      case 0:  // EventNotificationObject
      case 1:  // EventSynchronizationObject
      {
        auto ev = new XEvent(kernel_state);
        ev->InitializeNative(native_ptr, header);
        object = ev;
      } break;
      case 2:  // MutantObject
      {
        auto mutant = new XMutant(kernel_state);
        mutant->InitializeNative(native_ptr, header);
        object = mutant;
      } break;
      case 5:  // SemaphoreObject
      {
        auto sem = new XSemaphore(kernel_state);
        auto success = sem->InitializeNative(native_ptr, header);
        // Can't report failure to the guest at late initialization:
        assert_true(success);
        object = sem;
      } break;
      case 3:   // ProcessObject
      case 4:   // QueueObject
      case 6:   // ThreadObject
      case 7:   // GateObject
      case 8:   // TimerNotificationObject
      case 9:   // TimerSynchronizationObject
      case 18:  // ApcObject
      case 19:  // DpcObject
      case 20:  // DeviceQueueObject
      case 21:  // EventPairObject
      case 22:  // InterruptObject
      case 23:  // ProfileObject
      case 24:  // ThreadedDpcObject
      default:
        assert_always();
        return NULL;
    }

    // Stash pointer in struct.
    // FIXME: This assumes the object contains a dispatch header (some don't!)
    StashHandle(header, object->handle());

    return object_ref<XObject>(object);
  }
}

}  // namespace rex::system
