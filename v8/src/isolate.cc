// Copyright 2012 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <stdlib.h>

#include "v8.h"

#include "ast.h"
#include "bootstrapper.h"
#include "codegen.h"
#include "compilation-cache.h"
#include "cpu-profiler.h"
#include "debug.h"
#include "deoptimizer.h"
#include "heap-profiler.h"
#include "hydrogen.h"
#include "isolate-inl.h"
#include "lithium-allocator.h"
#include "log.h"
#include "messages.h"
#include "platform.h"
#include "regexp-stack.h"
#include "runtime-profiler.h"
#include "sampler.h"
#include "scopeinfo.h"
#include "serialize.h"
#include "simulator.h"
#include "spaces.h"
#include "stub-cache.h"
#include "sweeper-thread.h"
#include "utils/random-number-generator.h"
#include "version.h"
#include "vm-state-inl.h"


namespace v8 {
namespace internal {

Atomic32 ThreadId::highest_thread_id_ = 0;

int ThreadId::AllocateThreadId() {
  int new_id = NoBarrier_AtomicIncrement(&highest_thread_id_, 1);
  return new_id;
}


int ThreadId::GetCurrentThreadId() {
  int thread_id = Thread::GetThreadLocalInt(Isolate::thread_id_key_);
  if (thread_id == 0) {
    thread_id = AllocateThreadId();
    Thread::SetThreadLocalInt(Isolate::thread_id_key_, thread_id);
  }
  return thread_id;
}


ThreadLocalTop::ThreadLocalTop() {
  InitializeInternal();
}


void ThreadLocalTop::InitializeInternal() {
  c_entry_fp_ = 0;
  handler_ = 0;
#ifdef USE_SIMULATOR
  simulator_ = NULL;
#endif
  js_entry_sp_ = NULL;
  external_callback_scope_ = NULL;
  current_vm_state_ = EXTERNAL;
  try_catch_handler_address_ = NULL;
  context_ = NULL;
  thread_id_ = ThreadId::Invalid();
  external_caught_exception_ = false;
  failed_access_check_callback_ = NULL;
  save_context_ = NULL;
  catcher_ = NULL;
  top_lookup_result_ = NULL;

  // These members are re-initialized later after deserialization
  // is complete.
  pending_exception_ = NULL;
  has_pending_message_ = false;
  rethrowing_message_ = false;
  pending_message_obj_ = NULL;
  pending_message_script_ = NULL;
  scheduled_exception_ = NULL;
}


void ThreadLocalTop::Initialize() {
  InitializeInternal();
#ifdef USE_SIMULATOR
  simulator_ = Simulator::current(isolate_);
#endif
  thread_id_ = ThreadId::Current();
}


v8::TryCatch* ThreadLocalTop::TryCatchHandler() {
  return TRY_CATCH_FROM_ADDRESS(try_catch_handler_address());
}


Isolate* Isolate::default_isolate_ = NULL;
Thread::LocalStorageKey Isolate::isolate_key_;
Thread::LocalStorageKey Isolate::thread_id_key_;
Thread::LocalStorageKey Isolate::per_isolate_thread_data_key_;
#ifdef DEBUG
Thread::LocalStorageKey PerThreadAssertScopeBase::thread_local_key;
#endif  // DEBUG
Mutex Isolate::process_wide_mutex_;
// TODO(dcarney): Remove with default isolate.
enum DefaultIsolateStatus {
  kDefaultIsolateUninitialized,
  kDefaultIsolateInitialized,
  kDefaultIsolateCrashIfInitialized
};
static DefaultIsolateStatus default_isolate_status_
    = kDefaultIsolateUninitialized;
Isolate::ThreadDataTable* Isolate::thread_data_table_ = NULL;
Atomic32 Isolate::isolate_counter_ = 0;

Isolate::PerIsolateThreadData*
    Isolate::FindOrAllocatePerThreadDataForThisThread() {
  ThreadId thread_id = ThreadId::Current();
  PerIsolateThreadData* per_thread = NULL;
  {
    LockGuard<Mutex> lock_guard(&process_wide_mutex_);
    per_thread = thread_data_table_->Lookup(this, thread_id);
    if (per_thread == NULL) {
      per_thread = new PerIsolateThreadData(this, thread_id);
      thread_data_table_->Insert(per_thread);
    }
  }
  ASSERT(thread_data_table_->Lookup(this, thread_id) == per_thread);
  return per_thread;
}


Isolate::PerIsolateThreadData* Isolate::FindPerThreadDataForThisThread() {
  ThreadId thread_id = ThreadId::Current();
  return FindPerThreadDataForThread(thread_id);
}


Isolate::PerIsolateThreadData* Isolate::FindPerThreadDataForThread(
    ThreadId thread_id) {
  PerIsolateThreadData* per_thread = NULL;
  {
    LockGuard<Mutex> lock_guard(&process_wide_mutex_);
    per_thread = thread_data_table_->Lookup(this, thread_id);
  }
  return per_thread;
}


void Isolate::SetCrashIfDefaultIsolateInitialized() {
  LockGuard<Mutex> lock_guard(&process_wide_mutex_);
  CHECK(default_isolate_status_ != kDefaultIsolateInitialized);
  default_isolate_status_ = kDefaultIsolateCrashIfInitialized;
}


void Isolate::EnsureDefaultIsolate() {
  LockGuard<Mutex> lock_guard(&process_wide_mutex_);
  CHECK(default_isolate_status_ != kDefaultIsolateCrashIfInitialized);
  if (default_isolate_ == NULL) {
    isolate_key_ = Thread::CreateThreadLocalKey();
    thread_id_key_ = Thread::CreateThreadLocalKey();
    per_isolate_thread_data_key_ = Thread::CreateThreadLocalKey();
#ifdef DEBUG
    PerThreadAssertScopeBase::thread_local_key = Thread::CreateThreadLocalKey();
#endif  // DEBUG
    thread_data_table_ = new Isolate::ThreadDataTable();
    default_isolate_ = new Isolate();
  }
  // Can't use SetIsolateThreadLocals(default_isolate_, NULL) here
  // because a non-null thread data may be already set.
  if (Thread::GetThreadLocal(isolate_key_) == NULL) {
    Thread::SetThreadLocal(isolate_key_, default_isolate_);
  }
}

struct StaticInitializer {
  StaticInitializer() {
    Isolate::EnsureDefaultIsolate();
  }
} static_initializer;


Address Isolate::get_address_from_id(Isolate::AddressId id) {
  return isolate_addresses_[id];
}


char* Isolate::Iterate(ObjectVisitor* v, char* thread_storage) {
  ThreadLocalTop* thread = reinterpret_cast<ThreadLocalTop*>(thread_storage);
  Iterate(v, thread);
  return thread_storage + sizeof(ThreadLocalTop);
}


void Isolate::IterateThread(ThreadVisitor* v, char* t) {
  ThreadLocalTop* thread = reinterpret_cast<ThreadLocalTop*>(t);
  v->VisitThread(this, thread);
}


void Isolate::Iterate(ObjectVisitor* v, ThreadLocalTop* thread) {
  // Visit the roots from the top for a given thread.
  v->VisitPointer(&thread->pending_exception_);
  v->VisitPointer(&(thread->pending_message_obj_));
  v->VisitPointer(BitCast<Object**>(&(thread->pending_message_script_)));
  v->VisitPointer(BitCast<Object**>(&(thread->context_)));
  v->VisitPointer(&thread->scheduled_exception_);

  for (v8::TryCatch* block = thread->TryCatchHandler();
       block != NULL;
       block = TRY_CATCH_FROM_ADDRESS(block->next_)) {
    v->VisitPointer(BitCast<Object**>(&(block->exception_)));
    v->VisitPointer(BitCast<Object**>(&(block->message_obj_)));
    v->VisitPointer(BitCast<Object**>(&(block->message_script_)));
  }

  // Iterate over pointers on native execution stack.
  for (StackFrameIterator it(this, thread); !it.done(); it.Advance()) {
    it.frame()->Iterate(v);
  }

  // Iterate pointers in live lookup results.
  thread->top_lookup_result_->Iterate(v);
}


void Isolate::Iterate(ObjectVisitor* v) {
  ThreadLocalTop* current_t = thread_local_top();
  Iterate(v, current_t);
}


void Isolate::IterateDeferredHandles(ObjectVisitor* visitor) {
  for (DeferredHandles* deferred = deferred_handles_head_;
       deferred != NULL;
       deferred = deferred->next_) {
    deferred->Iterate(visitor);
  }
}


#ifdef DEBUG
bool Isolate::IsDeferredHandle(Object** handle) {
  // Each DeferredHandles instance keeps the handles to one job in the
  // concurrent recompilation queue, containing a list of blocks.  Each block
  // contains kHandleBlockSize handles except for the first block, which may
  // not be fully filled.
  // We iterate through all the blocks to see whether the argument handle
  // belongs to one of the blocks.  If so, it is deferred.
  for (DeferredHandles* deferred = deferred_handles_head_;
       deferred != NULL;
       deferred = deferred->next_) {
    List<Object**>* blocks = &deferred->blocks_;
    for (int i = 0; i < blocks->length(); i++) {
      Object** block_limit = (i == 0) ? deferred->first_block_limit_
                                      : blocks->at(i) + kHandleBlockSize;
      if (blocks->at(i) <= handle && handle < block_limit) return true;
    }
  }
  return false;
}
#endif  // DEBUG


void Isolate::RegisterTryCatchHandler(v8::TryCatch* that) {
  // The ARM simulator has a separate JS stack.  We therefore register
  // the C++ try catch handler with the simulator and get back an
  // address that can be used for comparisons with addresses into the
  // JS stack.  When running without the simulator, the address
  // returned will be the address of the C++ try catch handler itself.
  Address address = reinterpret_cast<Address>(
      SimulatorStack::RegisterCTryCatch(reinterpret_cast<uintptr_t>(that)));
  thread_local_top()->set_try_catch_handler_address(address);
}


void Isolate::UnregisterTryCatchHandler(v8::TryCatch* that) {
  ASSERT(thread_local_top()->TryCatchHandler() == that);
  thread_local_top()->set_try_catch_handler_address(
      reinterpret_cast<Address>(that->next_));
  thread_local_top()->catcher_ = NULL;
  SimulatorStack::UnregisterCTryCatch();
}


Handle<String> Isolate::StackTraceString() {
  if (stack_trace_nesting_level_ == 0) {
    stack_trace_nesting_level_++;
    HeapStringAllocator allocator;
    StringStream::ClearMentionedObjectCache(this);
    StringStream accumulator(&allocator);
    incomplete_message_ = &accumulator;
    PrintStack(&accumulator);
    Handle<String> stack_trace = accumulator.ToString(this);
    incomplete_message_ = NULL;
    stack_trace_nesting_level_ = 0;
    return stack_trace;
  } else if (stack_trace_nesting_level_ == 1) {
    stack_trace_nesting_level_++;
    OS::PrintError(
      "\n\nAttempt to print stack while printing stack (double fault)\n");
    OS::PrintError(
      "If you are lucky you may find a partial stack dump on stdout.\n\n");
    incomplete_message_->OutputToStdOut();
    return factory()->empty_string();
  } else {
    OS::Abort();
    // Unreachable
    return factory()->empty_string();
  }
}


void Isolate::PushStackTraceAndDie(unsigned int magic,
                                   Object* object,
                                   Map* map,
                                   unsigned int magic2) {
  const int kMaxStackTraceSize = 8192;
  Handle<String> trace = StackTraceString();
  uint8_t buffer[kMaxStackTraceSize];
  int length = Min(kMaxStackTraceSize - 1, trace->length());
  String::WriteToFlat(*trace, buffer, 0, length);
  buffer[length] = '\0';
  // TODO(dcarney): convert buffer to utf8?
  OS::PrintError("Stacktrace (%x-%x) %p %p: %s\n",
                 magic, magic2,
                 static_cast<void*>(object), static_cast<void*>(map),
                 reinterpret_cast<char*>(buffer));
  OS::Abort();
}


// Determines whether the given stack frame should be displayed in
// a stack trace.  The caller is the error constructor that asked
// for the stack trace to be collected.  The first time a construct
// call to this function is encountered it is skipped.  The seen_caller
// in/out parameter is used to remember if the caller has been seen
// yet.
static bool IsVisibleInStackTrace(StackFrame* raw_frame,
                                  Object* caller,
                                  bool* seen_caller) {
  // Only display JS frames.
  if (!raw_frame->is_java_script()) return false;
  JavaScriptFrame* frame = JavaScriptFrame::cast(raw_frame);
  JSFunction* fun = frame->function();
  if ((fun == caller) && !(*seen_caller)) {
    *seen_caller = true;
    return false;
  }
  // Skip all frames until we've seen the caller.
  if (!(*seen_caller)) return false;
  // Also, skip non-visible built-in functions and any call with the builtins
  // object as receiver, so as to not reveal either the builtins object or
  // an internal function.
  // The --builtins-in-stack-traces command line flag allows including
  // internal call sites in the stack trace for debugging purposes.
  if (!FLAG_builtins_in_stack_traces) {
    if (frame->receiver()->IsJSBuiltinsObject() ||
        (fun->IsBuiltin() && !fun->shared()->native())) {
      return false;
    }
  }
  return true;
}


Handle<JSArray> Isolate::CaptureSimpleStackTrace(Handle<JSObject> error_object,
                                                 Handle<Object> caller,
                                                 int limit) {
  limit = Max(limit, 0);  // Ensure that limit is not negative.
  int initial_size = Min(limit, 10);
  Handle<FixedArray> elements =
      factory()->NewFixedArrayWithHoles(initial_size * 4 + 1);

  // If the caller parameter is a function we skip frames until we're
  // under it before starting to collect.
  bool seen_caller = !caller->IsJSFunction();
  // First element is reserved to store the number of sloppy frames.
  int cursor = 1;
  int frames_seen = 0;
  int sloppy_frames = 0;
  bool encountered_strict_function = false;
  for (StackFrameIterator iter(this);
       !iter.done() && frames_seen < limit;
       iter.Advance()) {
    StackFrame* raw_frame = iter.frame();
    if (IsVisibleInStackTrace(raw_frame, *caller, &seen_caller)) {
      frames_seen++;
      JavaScriptFrame* frame = JavaScriptFrame::cast(raw_frame);
      // Set initial size to the maximum inlining level + 1 for the outermost
      // function.
      List<FrameSummary> frames(FLAG_max_inlining_levels + 1);
      frame->Summarize(&frames);
      for (int i = frames.length() - 1; i >= 0; i--) {
        if (cursor + 4 > elements->length()) {
          int new_capacity = JSObject::NewElementsCapacity(elements->length());
          Handle<FixedArray> new_elements =
              factory()->NewFixedArrayWithHoles(new_capacity);
          for (int i = 0; i < cursor; i++) {
            new_elements->set(i, elements->get(i));
          }
          elements = new_elements;
        }
        ASSERT(cursor + 4 <= elements->length());

        Handle<Object> recv = frames[i].receiver();
        Handle<JSFunction> fun = frames[i].function();
        Handle<Code> code = frames[i].code();
        Handle<Smi> offset(Smi::FromInt(frames[i].offset()), this);
        // The stack trace API should not expose receivers and function
        // objects on frames deeper than the top-most one with a strict
        // mode function.  The number of sloppy frames is stored as
        // first element in the result array.
        if (!encountered_strict_function) {
          if (fun->shared()->strict_mode() == STRICT) {
            encountered_strict_function = true;
          } else {
            sloppy_frames++;
          }
        }
        elements->set(cursor++, *recv);
        elements->set(cursor++, *fun);
        elements->set(cursor++, *code);
        elements->set(cursor++, *offset);
      }
    }
  }
  elements->set(0, Smi::FromInt(sloppy_frames));
  Handle<JSArray> result = factory()->NewJSArrayWithElements(elements);
  result->set_length(Smi::FromInt(cursor));
  return result;
}


void Isolate::CaptureAndSetDetailedStackTrace(Handle<JSObject> error_object) {
  if (capture_stack_trace_for_uncaught_exceptions_) {
    // Capture stack trace for a detailed exception message.
    Handle<String> key = factory()->hidden_stack_trace_string();
    Handle<JSArray> stack_trace = CaptureCurrentStackTrace(
        stack_trace_for_uncaught_exceptions_frame_limit_,
        stack_trace_for_uncaught_exceptions_options_);
    JSObject::SetHiddenProperty(error_object, key, stack_trace);
  }
}


Handle<JSArray> Isolate::CaptureCurrentStackTrace(
    int frame_limit, StackTrace::StackTraceOptions options) {
  // Ensure no negative values.
  int limit = Max(frame_limit, 0);
  Handle<JSArray> stack_trace = factory()->NewJSArray(frame_limit);

  Handle<String> column_key =
      factory()->InternalizeOneByteString(STATIC_ASCII_VECTOR("column"));
  Handle<String> line_key =
      factory()->InternalizeOneByteString(STATIC_ASCII_VECTOR("lineNumber"));
  Handle<String> script_id_key =
      factory()->InternalizeOneByteString(STATIC_ASCII_VECTOR("scriptId"));
  Handle<String> script_name_key =
      factory()->InternalizeOneByteString(STATIC_ASCII_VECTOR("scriptName"));
  Handle<String> script_name_or_source_url_key =
      factory()->InternalizeOneByteString(
          STATIC_ASCII_VECTOR("scriptNameOrSourceURL"));
  Handle<String> function_key =
      factory()->InternalizeOneByteString(STATIC_ASCII_VECTOR("functionName"));
  Handle<String> eval_key =
      factory()->InternalizeOneByteString(STATIC_ASCII_VECTOR("isEval"));
  Handle<String> constructor_key =
      factory()->InternalizeOneByteString(STATIC_ASCII_VECTOR("isConstructor"));

  StackTraceFrameIterator it(this);
  int frames_seen = 0;
  while (!it.done() && (frames_seen < limit)) {
    JavaScriptFrame* frame = it.frame();
    // Set initial size to the maximum inlining level + 1 for the outermost
    // function.
    List<FrameSummary> frames(FLAG_max_inlining_levels + 1);
    frame->Summarize(&frames);
    for (int i = frames.length() - 1; i >= 0 && frames_seen < limit; i--) {
      // Create a JSObject to hold the information for the StackFrame.
      Handle<JSObject> stack_frame = factory()->NewJSObject(object_function());

      Handle<JSFunction> fun = frames[i].function();
      Handle<Script> script(Script::cast(fun->shared()->script()));

      if (options & StackTrace::kLineNumber) {
        int script_line_offset = script->line_offset()->value();
        int position = frames[i].code()->SourcePosition(frames[i].pc());
        int line_number = Script::GetLineNumber(script, position);
        // line_number is already shifted by the script_line_offset.
        int relative_line_number = line_number - script_line_offset;
        if (options & StackTrace::kColumnOffset && relative_line_number >= 0) {
          Handle<FixedArray> line_ends(FixedArray::cast(script->line_ends()));
          int start = (relative_line_number == 0) ? 0 :
              Smi::cast(line_ends->get(relative_line_number - 1))->value() + 1;
          int column_offset = position - start;
          if (relative_line_number == 0) {
            // For the case where the code is on the same line as the script
            // tag.
            column_offset += script->column_offset()->value();
          }
          JSObject::SetLocalPropertyIgnoreAttributes(
              stack_frame, column_key,
              Handle<Smi>(Smi::FromInt(column_offset + 1), this), NONE).Check();
        }
       JSObject::SetLocalPropertyIgnoreAttributes(
            stack_frame, line_key,
            Handle<Smi>(Smi::FromInt(line_number + 1), this), NONE).Check();
      }

      if (options & StackTrace::kScriptId) {
        Handle<Smi> script_id(script->id(), this);
        JSObject::SetLocalPropertyIgnoreAttributes(
            stack_frame, script_id_key, script_id, NONE).Check();
      }

      if (options & StackTrace::kScriptName) {
        Handle<Object> script_name(script->name(), this);
        JSObject::SetLocalPropertyIgnoreAttributes(
            stack_frame, script_name_key, script_name, NONE).Check();
      }

      if (options & StackTrace::kScriptNameOrSourceURL) {
        Handle<Object> result = Script::GetNameOrSourceURL(script);
        JSObject::SetLocalPropertyIgnoreAttributes(
            stack_frame, script_name_or_source_url_key, result, NONE).Check();
      }

      if (options & StackTrace::kFunctionName) {
        Handle<Object> fun_name(fun->shared()->name(), this);
        if (!fun_name->BooleanValue()) {
          fun_name = Handle<Object>(fun->shared()->inferred_name(), this);
        }
        JSObject::SetLocalPropertyIgnoreAttributes(
            stack_frame, function_key, fun_name, NONE).Check();
      }

      if (options & StackTrace::kIsEval) {
        Handle<Object> is_eval =
            script->compilation_type() == Script::COMPILATION_TYPE_EVAL ?
                factory()->true_value() : factory()->false_value();
        JSObject::SetLocalPropertyIgnoreAttributes(
            stack_frame, eval_key, is_eval, NONE).Check();
      }

      if (options & StackTrace::kIsConstructor) {
        Handle<Object> is_constructor = (frames[i].is_constructor()) ?
            factory()->true_value() : factory()->false_value();
        JSObject::SetLocalPropertyIgnoreAttributes(
            stack_frame, constructor_key, is_constructor, NONE).Check();
      }

      FixedArray::cast(stack_trace->elements())->set(frames_seen, *stack_frame);
      frames_seen++;
    }
    it.Advance();
  }

  stack_trace->set_length(Smi::FromInt(frames_seen));
  return stack_trace;
}


void Isolate::PrintStack(FILE* out) {
  if (stack_trace_nesting_level_ == 0) {
    stack_trace_nesting_level_++;
    StringStream::ClearMentionedObjectCache(this);
    HeapStringAllocator allocator;
    StringStream accumulator(&allocator);
    incomplete_message_ = &accumulator;
    PrintStack(&accumulator);
    accumulator.OutputToFile(out);
    InitializeLoggingAndCounters();
    accumulator.Log(this);
    incomplete_message_ = NULL;
    stack_trace_nesting_level_ = 0;
  } else if (stack_trace_nesting_level_ == 1) {
    stack_trace_nesting_level_++;
    OS::PrintError(
      "\n\nAttempt to print stack while printing stack (double fault)\n");
    OS::PrintError(
      "If you are lucky you may find a partial stack dump on stdout.\n\n");
    incomplete_message_->OutputToFile(out);
  }
}


static void PrintFrames(Isolate* isolate,
                        StringStream* accumulator,
                        StackFrame::PrintMode mode) {
  StackFrameIterator it(isolate);
  for (int i = 0; !it.done(); it.Advance()) {
    it.frame()->Print(accumulator, mode, i++);
  }
}


void Isolate::PrintStack(StringStream* accumulator) {
  if (!IsInitialized()) {
    accumulator->Add(
        "\n==== JS stack trace is not available =======================\n\n");
    accumulator->Add(
        "\n==== Isolate for the thread is not initialized =============\n\n");
    return;
  }
  // The MentionedObjectCache is not GC-proof at the moment.
  DisallowHeapAllocation no_gc;
  ASSERT(StringStream::IsMentionedObjectCacheClear(this));

  // Avoid printing anything if there are no frames.
  if (c_entry_fp(thread_local_top()) == 0) return;

  accumulator->Add(
      "\n==== JS stack trace =========================================\n\n");
  PrintFrames(this, accumulator, StackFrame::OVERVIEW);

  accumulator->Add(
      "\n==== Details ================================================\n\n");
  PrintFrames(this, accumulator, StackFrame::DETAILS);

  accumulator->PrintMentionedObjectCache(this);
  accumulator->Add("=====================\n\n");
}


void Isolate::SetFailedAccessCheckCallback(
    v8::FailedAccessCheckCallback callback) {
  thread_local_top()->failed_access_check_callback_ = callback;
}


static inline AccessCheckInfo* GetAccessCheckInfo(Isolate* isolate,
                                                  Handle<JSObject> receiver) {
  JSFunction* constructor = JSFunction::cast(receiver->map()->constructor());
  if (!constructor->shared()->IsApiFunction()) return NULL;

  Object* data_obj =
     constructor->shared()->get_api_func_data()->access_check_info();
  if (data_obj == isolate->heap()->undefined_value()) return NULL;

  return AccessCheckInfo::cast(data_obj);
}


void Isolate::ReportFailedAccessCheck(Handle<JSObject> receiver,
                                      v8::AccessType type) {
  if (!thread_local_top()->failed_access_check_callback_) return;

  ASSERT(receiver->IsAccessCheckNeeded());
  ASSERT(context());

  // Get the data object from access check info.
  HandleScope scope(this);
  Handle<Object> data;
  { DisallowHeapAllocation no_gc;
    AccessCheckInfo* access_check_info = GetAccessCheckInfo(this, receiver);
    if (!access_check_info) return;
    data = handle(access_check_info->data(), this);
  }

  // Leaving JavaScript.
  VMState<EXTERNAL> state(this);
  thread_local_top()->failed_access_check_callback_(
      v8::Utils::ToLocal(receiver),
      type,
      v8::Utils::ToLocal(data));
}


enum MayAccessDecision {
  YES, NO, UNKNOWN
};


static MayAccessDecision MayAccessPreCheck(Isolate* isolate,
                                           Handle<JSObject> receiver,
                                           v8::AccessType type) {
  DisallowHeapAllocation no_gc;
  // During bootstrapping, callback functions are not enabled yet.
  if (isolate->bootstrapper()->IsActive()) return YES;

  if (receiver->IsJSGlobalProxy()) {
    Object* receiver_context = JSGlobalProxy::cast(*receiver)->native_context();
    if (!receiver_context->IsContext()) return NO;

    // Get the native context of current top context.
    // avoid using Isolate::native_context() because it uses Handle.
    Context* native_context =
        isolate->context()->global_object()->native_context();
    if (receiver_context == native_context) return YES;

    if (Context::cast(receiver_context)->security_token() ==
        native_context->security_token())
      return YES;
  }

  return UNKNOWN;
}


bool Isolate::MayNamedAccess(Handle<JSObject> receiver,
                             Handle<Object> key,
                             v8::AccessType type) {
  ASSERT(receiver->IsJSGlobalProxy() || receiver->IsAccessCheckNeeded());

  // Skip checks for hidden properties access.  Note, we do not
  // require existence of a context in this case.
  if (key.is_identical_to(factory()->hidden_string())) return true;

  // Check for compatibility between the security tokens in the
  // current lexical context and the accessed object.
  ASSERT(context());

  MayAccessDecision decision = MayAccessPreCheck(this, receiver, type);
  if (decision != UNKNOWN) return decision == YES;

  HandleScope scope(this);
  Handle<Object> data;
  v8::NamedSecurityCallback callback;
  { DisallowHeapAllocation no_gc;
    AccessCheckInfo* access_check_info = GetAccessCheckInfo(this, receiver);
    if (!access_check_info) return false;
    Object* fun_obj = access_check_info->named_callback();
    callback = v8::ToCData<v8::NamedSecurityCallback>(fun_obj);
    if (!callback) return false;
    data = handle(access_check_info->data(), this);
  }

  LOG(this, ApiNamedSecurityCheck(*key));

  // Leaving JavaScript.
  VMState<EXTERNAL> state(this);
  return callback(v8::Utils::ToLocal(receiver),
                  v8::Utils::ToLocal(key),
                  type,
                  v8::Utils::ToLocal(data));
}


bool Isolate::MayIndexedAccess(Handle<JSObject> receiver,
                               uint32_t index,
                               v8::AccessType type) {
  ASSERT(receiver->IsJSGlobalProxy() || receiver->IsAccessCheckNeeded());
  // Check for compatibility between the security tokens in the
  // current lexical context and the accessed object.
  ASSERT(context());

  MayAccessDecision decision = MayAccessPreCheck(this, receiver, type);
  if (decision != UNKNOWN) return decision == YES;

  HandleScope scope(this);
  Handle<Object> data;
  v8::IndexedSecurityCallback callback;
  { DisallowHeapAllocation no_gc;
    // Get named access check callback
    AccessCheckInfo* access_check_info = GetAccessCheckInfo(this, receiver);
    if (!access_check_info) return false;
    Object* fun_obj = access_check_info->indexed_callback();
    callback = v8::ToCData<v8::IndexedSecurityCallback>(fun_obj);
    if (!callback) return false;
    data = handle(access_check_info->data(), this);
  }

  LOG(this, ApiIndexedSecurityCheck(index));

  // Leaving JavaScript.
  VMState<EXTERNAL> state(this);
  return callback(
      v8::Utils::ToLocal(receiver), index, type, v8::Utils::ToLocal(data));
}


const char* const Isolate::kStackOverflowMessage =
  "Uncaught RangeError: Maximum call stack size exceeded";


Object* Isolate::StackOverflow() {
  HandleScope scope(this);
  // At this point we cannot create an Error object using its javascript
  // constructor.  Instead, we copy the pre-constructed boilerplate and
  // attach the stack trace as a hidden property.
  Handle<String> key = factory()->stack_overflow_string();
  Handle<JSObject> boilerplate = Handle<JSObject>::cast(
      Object::GetProperty(js_builtins_object(), key).ToHandleChecked());
  Handle<JSObject> exception = factory()->CopyJSObject(boilerplate);
  DoThrow(*exception, NULL);

  // Get stack trace limit.
  Handle<Object> error = Object::GetProperty(
      this, js_builtins_object(), "$Error").ToHandleChecked();
  if (!error->IsJSObject()) return heap()->exception();

  Handle<String> stackTraceLimit =
      factory()->InternalizeUtf8String("stackTraceLimit");
  ASSERT(!stackTraceLimit.is_null());
  Handle<Object> stack_trace_limit =
      JSObject::GetDataProperty(Handle<JSObject>::cast(error),
                                stackTraceLimit);
  if (!stack_trace_limit->IsNumber()) return heap()->exception();
  double dlimit = stack_trace_limit->Number();
  int limit = std::isnan(dlimit) ? 0 : static_cast<int>(dlimit);

  Handle<JSArray> stack_trace = CaptureSimpleStackTrace(
      exception, factory()->undefined_value(), limit);
  JSObject::SetHiddenProperty(exception,
                              factory()->hidden_stack_trace_string(),
                              stack_trace);
  return heap()->exception();
}


Object* Isolate::TerminateExecution() {
  DoThrow(heap_.termination_exception(), NULL);
  return heap()->exception();
}


void Isolate::CancelTerminateExecution() {
  if (try_catch_handler()) {
    try_catch_handler()->has_terminated_ = false;
  }
  if (has_pending_exception() &&
      pending_exception() == heap_.termination_exception()) {
    thread_local_top()->external_caught_exception_ = false;
    clear_pending_exception();
  }
  if (has_scheduled_exception() &&
      scheduled_exception() == heap_.termination_exception()) {
    thread_local_top()->external_caught_exception_ = false;
    clear_scheduled_exception();
  }
}


Object* Isolate::Throw(Object* exception, MessageLocation* location) {
  DoThrow(exception, location);
  return heap()->exception();
}


Object* Isolate::ReThrow(Object* exception) {
  bool can_be_caught_externally = false;
  bool catchable_by_javascript = is_catchable_by_javascript(exception);
  ShouldReportException(&can_be_caught_externally, catchable_by_javascript);

  thread_local_top()->catcher_ = can_be_caught_externally ?
      try_catch_handler() : NULL;

  // Set the exception being re-thrown.
  set_pending_exception(exception);
  return heap()->exception();
}


Object* Isolate::ThrowIllegalOperation() {
  if (FLAG_stack_trace_on_illegal) PrintStack(stdout);
  return Throw(heap_.illegal_access_string());
}


Object* Isolate::ThrowInvalidStringLength() {
  return Throw(*factory()->NewRangeError(
      "invalid_string_length", HandleVector<Object>(NULL, 0)));
}


void Isolate::ScheduleThrow(Object* exception) {
  // When scheduling a throw we first throw the exception to get the
  // error reporting if it is uncaught before rescheduling it.
  Throw(exception);
  PropagatePendingExceptionToExternalTryCatch();
  if (has_pending_exception()) {
    thread_local_top()->scheduled_exception_ = pending_exception();
    thread_local_top()->external_caught_exception_ = false;
    clear_pending_exception();
  }
}


void Isolate::RestorePendingMessageFromTryCatch(v8::TryCatch* handler) {
  ASSERT(handler == try_catch_handler());
  ASSERT(handler->HasCaught());
  ASSERT(handler->rethrow_);
  ASSERT(handler->capture_message_);
  Object* message = reinterpret_cast<Object*>(handler->message_obj_);
  Object* script = reinterpret_cast<Object*>(handler->message_script_);
  ASSERT(message->IsJSMessageObject() || message->IsTheHole());
  ASSERT(script->IsScript() || script->IsTheHole());
  thread_local_top()->pending_message_obj_ = message;
  thread_local_top()->pending_message_script_ = script;
  thread_local_top()->pending_message_start_pos_ = handler->message_start_pos_;
  thread_local_top()->pending_message_end_pos_ = handler->message_end_pos_;
}


Object* Isolate::PromoteScheduledException() {
  Object* thrown = scheduled_exception();
  clear_scheduled_exception();
  // Re-throw the exception to avoid getting repeated error reporting.
  return ReThrow(thrown);
}


void Isolate::PrintCurrentStackTrace(FILE* out) {
  StackTraceFrameIterator it(this);
  while (!it.done()) {
    HandleScope scope(this);
    // Find code position if recorded in relocation info.
    JavaScriptFrame* frame = it.frame();
    int pos = frame->LookupCode()->SourcePosition(frame->pc());
    Handle<Object> pos_obj(Smi::FromInt(pos), this);
    // Fetch function and receiver.
    Handle<JSFunction> fun(frame->function());
    Handle<Object> recv(frame->receiver(), this);
    // Advance to the next JavaScript frame and determine if the
    // current frame is the top-level frame.
    it.Advance();
    Handle<Object> is_top_level = it.done()
        ? factory()->true_value()
        : factory()->false_value();
    // Generate and print stack trace line.
    Handle<String> line =
        Execution::GetStackTraceLine(recv, fun, pos_obj, is_top_level);
    if (line->length() > 0) {
      line->PrintOn(out);
      PrintF(out, "\n");
    }
  }
}


void Isolate::ComputeLocation(MessageLocation* target) {
  *target = MessageLocation(Handle<Script>(heap_.empty_script()), -1, -1);
  StackTraceFrameIterator it(this);
  if (!it.done()) {
    JavaScriptFrame* frame = it.frame();
    JSFunction* fun = frame->function();
    Object* script = fun->shared()->script();
    if (script->IsScript() &&
        !(Script::cast(script)->source()->IsUndefined())) {
      int pos = frame->LookupCode()->SourcePosition(frame->pc());
      // Compute the location from the function and the reloc info.
      Handle<Script> casted_script(Script::cast(script));
      *target = MessageLocation(casted_script, pos, pos + 1);
    }
  }
}


bool Isolate::ShouldReportException(bool* can_be_caught_externally,
                                    bool catchable_by_javascript) {
  // Find the top-most try-catch handler.
  StackHandler* handler =
      StackHandler::FromAddress(Isolate::handler(thread_local_top()));
  while (handler != NULL && !handler->is_catch()) {
    handler = handler->next();
  }

  // Get the address of the external handler so we can compare the address to
  // determine which one is closer to the top of the stack.
  Address external_handler_address =
      thread_local_top()->try_catch_handler_address();

  // The exception has been externally caught if and only if there is
  // an external handler which is on top of the top-most try-catch
  // handler.
  *can_be_caught_externally = external_handler_address != NULL &&
      (handler == NULL || handler->address() > external_handler_address ||
       !catchable_by_javascript);

  if (*can_be_caught_externally) {
    // Only report the exception if the external handler is verbose.
    return try_catch_handler()->is_verbose_;
  } else {
    // Report the exception if it isn't caught by JavaScript code.
    return handler == NULL;
  }
}


bool Isolate::IsErrorObject(Handle<Object> obj) {
  if (!obj->IsJSObject()) return false;

  Handle<String> error_key =
      factory()->InternalizeOneByteString(STATIC_ASCII_VECTOR("$Error"));
  Handle<Object> error_constructor = Object::GetProperty(
      js_builtins_object(), error_key).ToHandleChecked();

  DisallowHeapAllocation no_gc;
  for (Object* prototype = *obj; !prototype->IsNull();
       prototype = prototype->GetPrototype(this)) {
    if (!prototype->IsJSObject()) return false;
    if (JSObject::cast(prototype)->map()->constructor() ==
        *error_constructor) {
      return true;
    }
  }
  return false;
}

static int fatal_exception_depth = 0;

void Isolate::DoThrow(Object* exception, MessageLocation* location) {
  ASSERT(!has_pending_exception());

  HandleScope scope(this);
  Handle<Object> exception_handle(exception, this);

  // Determine reporting and whether the exception is caught externally.
  bool catchable_by_javascript = is_catchable_by_javascript(exception);
  bool can_be_caught_externally = false;
  bool should_report_exception =
      ShouldReportException(&can_be_caught_externally, catchable_by_javascript);
  bool report_exception = catchable_by_javascript && should_report_exception;
  bool try_catch_needs_message =
      can_be_caught_externally && try_catch_handler()->capture_message_ &&
      !thread_local_top()->rethrowing_message_;
  bool bootstrapping = bootstrapper()->IsActive();

  thread_local_top()->rethrowing_message_ = false;

  // Notify debugger of exception.
  if (catchable_by_javascript) {
    debugger_->OnException(exception_handle, report_exception);
  }

  // Generate the message if required.
  if (report_exception || try_catch_needs_message) {
    MessageLocation potential_computed_location;
    if (location == NULL) {
      // If no location was specified we use a computed one instead.
      ComputeLocation(&potential_computed_location);
      location = &potential_computed_location;
    }
    // It's not safe to try to make message objects or collect stack traces
    // while the bootstrapper is active since the infrastructure may not have
    // been properly initialized.
    if (!bootstrapping) {
      Handle<JSArray> stack_trace_object;
      if (capture_stack_trace_for_uncaught_exceptions_) {
        if (IsErrorObject(exception_handle)) {
          // We fetch the stack trace that corresponds to this error object.
          Handle<String> key = factory()->hidden_stack_trace_string();
          Object* stack_property =
              JSObject::cast(*exception_handle)->GetHiddenProperty(key);
          // Property lookup may have failed.  In this case it's probably not
          // a valid Error object.
          if (stack_property->IsJSArray()) {
            stack_trace_object = Handle<JSArray>(JSArray::cast(stack_property));
          }
        }
        if (stack_trace_object.is_null()) {
          // Not an error object, we capture at throw site.
          stack_trace_object = CaptureCurrentStackTrace(
              stack_trace_for_uncaught_exceptions_frame_limit_,
              stack_trace_for_uncaught_exceptions_options_);
        }
      }

      Handle<Object> exception_arg = exception_handle;
      // If the exception argument is a custom object, turn it into a string
      // before throwing as uncaught exception.  Note that the pending
      // exception object to be set later must not be turned into a string.
      if (exception_arg->IsJSObject() && !IsErrorObject(exception_arg)) {
        MaybeHandle<Object> maybe_exception =
            Execution::ToDetailString(this, exception_arg);
        if (!maybe_exception.ToHandle(&exception_arg)) {
          exception_arg = factory()->InternalizeOneByteString(
              STATIC_ASCII_VECTOR("exception"));
        }
      }
      Handle<Object> message_obj = MessageHandler::MakeMessageObject(
          this,
          "uncaught_exception",
          location,
          HandleVector<Object>(&exception_arg, 1),
          stack_trace_object);
      thread_local_top()->pending_message_obj_ = *message_obj;
      if (location != NULL) {
        thread_local_top()->pending_message_script_ = *location->script();
        thread_local_top()->pending_message_start_pos_ = location->start_pos();
        thread_local_top()->pending_message_end_pos_ = location->end_pos();
      }

      // If the abort-on-uncaught-exception flag is specified, abort on any
      // exception not caught by JavaScript, even when an external handler is
      // present.  This flag is intended for use by JavaScript developers, so
      // print a user-friendly stack trace (not an internal one).
      if (fatal_exception_depth == 0 &&
          FLAG_abort_on_uncaught_exception &&
          (report_exception || can_be_caught_externally)) {
        fatal_exception_depth++;
        PrintF(stderr,
               "%s\n\nFROM\n",
               MessageHandler::GetLocalizedMessage(this, message_obj).get());
        PrintCurrentStackTrace(stderr);
        OS::Abort();
      }
    } else if (location != NULL && !location->script().is_null()) {
      // We are bootstrapping and caught an error where the location is set
      // and we have a script for the location.
      // In this case we could have an extension (or an internal error
      // somewhere) and we print out the line number at which the error occured
      // to the console for easier debugging.
      int line_number =
          location->script()->GetLineNumber(location->start_pos()) + 1;
      if (exception->IsString() && location->script()->name()->IsString()) {
        OS::PrintError(
            "Extension or internal compilation error: %s in %s at line %d.\n",
            String::cast(exception)->ToCString().get(),
            String::cast(location->script()->name())->ToCString().get(),
            line_number);
      } else if (location->script()->name()->IsString()) {
        OS::PrintError(
            "Extension or internal compilation error in %s at line %d.\n",
            String::cast(location->script()->name())->ToCString().get(),
            line_number);
      } else {
        OS::PrintError("Extension or internal compilation error.\n");
      }
    }
  }

  // Save the message for reporting if the the exception remains uncaught.
  thread_local_top()->has_pending_message_ = report_exception;

  // Do not forget to clean catcher_ if currently thrown exception cannot
  // be caught.  If necessary, ReThrow will update the catcher.
  thread_local_top()->catcher_ = can_be_caught_externally ?
      try_catch_handler() : NULL;

  set_pending_exception(*exception_handle);
}


bool Isolate::IsExternallyCaught() {
  ASSERT(has_pending_exception());

  if ((thread_local_top()->catcher_ == NULL) ||
      (try_catch_handler() != thread_local_top()->catcher_)) {
    // When throwing the exception, we found no v8::TryCatch
    // which should care about this exception.
    return false;
  }

  if (!is_catchable_by_javascript(pending_exception())) {
    return true;
  }

  // Get the address of the external handler so we can compare the address to
  // determine which one is closer to the top of the stack.
  Address external_handler_address =
      thread_local_top()->try_catch_handler_address();
  ASSERT(external_handler_address != NULL);

  // The exception has been externally caught if and only if there is
  // an external handler which is on top of the top-most try-finally
  // handler.
  // There should be no try-catch blocks as they would prohibit us from
  // finding external catcher in the first place (see catcher_ check above).
  //
  // Note, that finally clause would rethrow an exception unless it's
  // aborted by jumps in control flow like return, break, etc. and we'll
  // have another chances to set proper v8::TryCatch.
  StackHandler* handler =
      StackHandler::FromAddress(Isolate::handler(thread_local_top()));
  while (handler != NULL && handler->address() < external_handler_address) {
    ASSERT(!handler->is_catch());
    if (handler->is_finally()) return false;

    handler = handler->next();
  }

  return true;
}


void Isolate::ReportPendingMessages() {
  ASSERT(has_pending_exception());
  PropagatePendingExceptionToExternalTryCatch();

  HandleScope scope(this);
  if (thread_local_top_.pending_exception_ ==
          heap()->termination_exception()) {
    // Do nothing: if needed, the exception has been already propagated to
    // v8::TryCatch.
  } else {
    if (thread_local_top_.has_pending_message_) {
      thread_local_top_.has_pending_message_ = false;
      if (!thread_local_top_.pending_message_obj_->IsTheHole()) {
        HandleScope scope(this);
        Handle<Object> message_obj(thread_local_top_.pending_message_obj_,
                                   this);
        if (!thread_local_top_.pending_message_script_->IsTheHole()) {
          Handle<Script> script(
              Script::cast(thread_local_top_.pending_message_script_));
          int start_pos = thread_local_top_.pending_message_start_pos_;
          int end_pos = thread_local_top_.pending_message_end_pos_;
          MessageLocation location(script, start_pos, end_pos);
          MessageHandler::ReportMessage(this, &location, message_obj);
        } else {
          MessageHandler::ReportMessage(this, NULL, message_obj);
        }
      }
    }
  }
  clear_pending_message();
}


MessageLocation Isolate::GetMessageLocation() {
  ASSERT(has_pending_exception());

  if (thread_local_top_.pending_exception_ != heap()->termination_exception() &&
      thread_local_top_.has_pending_message_ &&
      !thread_local_top_.pending_message_obj_->IsTheHole() &&
      !thread_local_top_.pending_message_obj_->IsTheHole()) {
    Handle<Script> script(
        Script::cast(thread_local_top_.pending_message_script_));
    int start_pos = thread_local_top_.pending_message_start_pos_;
    int end_pos = thread_local_top_.pending_message_end_pos_;
    return MessageLocation(script, start_pos, end_pos);
  }

  return MessageLocation();
}


bool Isolate::OptionalRescheduleException(bool is_bottom_call) {
  ASSERT(has_pending_exception());
  PropagatePendingExceptionToExternalTryCatch();

  bool is_termination_exception =
      pending_exception() == heap_.termination_exception();

  // Do not reschedule the exception if this is the bottom call.
  bool clear_exception = is_bottom_call;

  if (is_termination_exception) {
    if (is_bottom_call) {
      thread_local_top()->external_caught_exception_ = false;
      clear_pending_exception();
      return false;
    }
  } else if (thread_local_top()->external_caught_exception_) {
    // If the exception is externally caught, clear it if there are no
    // JavaScript frames on the way to the C++ frame that has the
    // external handler.
    ASSERT(thread_local_top()->try_catch_handler_address() != NULL);
    Address external_handler_address =
        thread_local_top()->try_catch_handler_address();
    JavaScriptFrameIterator it(this);
    if (it.done() || (it.frame()->sp() > external_handler_address)) {
      clear_exception = true;
    }
  }

  // Clear the exception if needed.
  if (clear_exception) {
    thread_local_top()->external_caught_exception_ = false;
    clear_pending_exception();
    return false;
  }

  // Reschedule the exception.
  thread_local_top()->scheduled_exception_ = pending_exception();
  clear_pending_exception();
  return true;
}


void Isolate::SetCaptureStackTraceForUncaughtExceptions(
      bool capture,
      int frame_limit,
      StackTrace::StackTraceOptions options) {
  capture_stack_trace_for_uncaught_exceptions_ = capture;
  stack_trace_for_uncaught_exceptions_frame_limit_ = frame_limit;
  stack_trace_for_uncaught_exceptions_options_ = options;
}


Handle<Context> Isolate::native_context() {
  return Handle<Context>(context()->global_object()->native_context());
}


Handle<Context> Isolate::global_context() {
  return Handle<Context>(context()->global_object()->global_context());
}


Handle<Context> Isolate::GetCallingNativeContext() {
  JavaScriptFrameIterator it(this);
  if (debug_->InDebugger()) {
    while (!it.done()) {
      JavaScriptFrame* frame = it.frame();
      Context* context = Context::cast(frame->context());
      if (context->native_context() == *debug_->debug_context()) {
        it.Advance();
      } else {
        break;
      }
    }
  }
  if (it.done()) return Handle<Context>::null();
  JavaScriptFrame* frame = it.frame();
  Context* context = Context::cast(frame->context());
  return Handle<Context>(context->native_context());
}


char* Isolate::ArchiveThread(char* to) {
  OS::MemCopy(to, reinterpret_cast<char*>(thread_local_top()),
              sizeof(ThreadLocalTop));
  InitializeThreadLocal();
  clear_pending_exception();
  clear_pending_message();
  clear_scheduled_exception();
  return to + sizeof(ThreadLocalTop);
}


char* Isolate::RestoreThread(char* from) {
  OS::MemCopy(reinterpret_cast<char*>(thread_local_top()), from,
              sizeof(ThreadLocalTop));
  // This might be just paranoia, but it seems to be needed in case a
  // thread_local_top_ is restored on a separate OS thread.
#ifdef USE_SIMULATOR
  thread_local_top()->simulator_ = Simulator::current(this);
#endif
  ASSERT(context() == NULL || context()->IsContext());
  return from + sizeof(ThreadLocalTop);
}


Isolate::ThreadDataTable::ThreadDataTable()
    : list_(NULL) {
}


Isolate::ThreadDataTable::~ThreadDataTable() {
  // TODO(svenpanne) The assertion below would fire if an embedder does not
  // cleanly dispose all Isolates before disposing v8, so we are conservative
  // and leave it out for now.
  // ASSERT_EQ(NULL, list_);
}


Isolate::PerIsolateThreadData::~PerIsolateThreadData() {
#if defined(USE_SIMULATOR)
  delete simulator_;
#endif
}


Isolate::PerIsolateThreadData*
    Isolate::ThreadDataTable::Lookup(Isolate* isolate,
                                     ThreadId thread_id) {
  for (PerIsolateThreadData* data = list_; data != NULL; data = data->next_) {
    if (data->Matches(isolate, thread_id)) return data;
  }
  return NULL;
}


void Isolate::ThreadDataTable::Insert(Isolate::PerIsolateThreadData* data) {
  if (list_ != NULL) list_->prev_ = data;
  data->next_ = list_;
  list_ = data;
}


void Isolate::ThreadDataTable::Remove(PerIsolateThreadData* data) {
  if (list_ == data) list_ = data->next_;
  if (data->next_ != NULL) data->next_->prev_ = data->prev_;
  if (data->prev_ != NULL) data->prev_->next_ = data->next_;
  delete data;
}


void Isolate::ThreadDataTable::RemoveAllThreads(Isolate* isolate) {
  PerIsolateThreadData* data = list_;
  while (data != NULL) {
    PerIsolateThreadData* next = data->next_;
    if (data->isolate() == isolate) Remove(data);
    data = next;
  }
}


#ifdef DEBUG
#define TRACE_ISOLATE(tag)                                              \
  do {                                                                  \
    if (FLAG_trace_isolates) {                                          \
      PrintF("Isolate %p (id %d)" #tag "\n",                            \
             reinterpret_cast<void*>(this), id());                      \
    }                                                                   \
  } while (false)
#else
#define TRACE_ISOLATE(tag)
#endif


Isolate::Isolate()
    : embedder_data_(),
      state_(UNINITIALIZED),
      entry_stack_(NULL),
      stack_trace_nesting_level_(0),
      incomplete_message_(NULL),
      bootstrapper_(NULL),
      runtime_profiler_(NULL),
      compilation_cache_(NULL),
      counters_(NULL),
      code_range_(NULL),
      logger_(NULL),
      stats_table_(NULL),
      stub_cache_(NULL),
      deoptimizer_data_(NULL),
      materialized_object_store_(NULL),
      capture_stack_trace_for_uncaught_exceptions_(false),
      stack_trace_for_uncaught_exceptions_frame_limit_(0),
      stack_trace_for_uncaught_exceptions_options_(StackTrace::kOverview),
      memory_allocator_(NULL),
      keyed_lookup_cache_(NULL),
      context_slot_cache_(NULL),
      descriptor_lookup_cache_(NULL),
      handle_scope_implementer_(NULL),
      unicode_cache_(NULL),
      runtime_zone_(this),
      inner_pointer_to_code_cache_(NULL),
      write_iterator_(NULL),
      global_handles_(NULL),
      eternal_handles_(NULL),
      thread_manager_(NULL),
      has_installed_extensions_(false),
      string_tracker_(NULL),
      regexp_stack_(NULL),
      date_cache_(NULL),
      code_stub_interface_descriptors_(NULL),
      call_descriptors_(NULL),
      // TODO(bmeurer) Initialized lazily because it depends on flags; can
      // be fixed once the default isolate cleanup is done.
      random_number_generator_(NULL),
      has_fatal_error_(false),
      use_crankshaft_(true),
      initialized_from_snapshot_(false),
      cpu_profiler_(NULL),
      heap_profiler_(NULL),
      function_entry_hook_(NULL),
      deferred_handles_head_(NULL),
      optimizing_compiler_thread_(NULL),
      sweeper_thread_(NULL),
      num_sweeper_threads_(0),
      stress_deopt_count_(0),
      next_optimization_id_(0) {
  id_ = NoBarrier_AtomicIncrement(&isolate_counter_, 1);
  TRACE_ISOLATE(constructor);

  memset(isolate_addresses_, 0,
      sizeof(isolate_addresses_[0]) * (kIsolateAddressCount + 1));

  heap_.isolate_ = this;
  stack_guard_.isolate_ = this;

  // ThreadManager is initialized early to support locking an isolate
  // before it is entered.
  thread_manager_ = new ThreadManager();
  thread_manager_->isolate_ = this;

#ifdef DEBUG
  // heap_histograms_ initializes itself.
  memset(&js_spill_information_, 0, sizeof(js_spill_information_));
#endif

  handle_scope_data_.Initialize();

#define ISOLATE_INIT_EXECUTE(type, name, initial_value)                        \
  name##_ = (initial_value);
  ISOLATE_INIT_LIST(ISOLATE_INIT_EXECUTE)
#undef ISOLATE_INIT_EXECUTE

#define ISOLATE_INIT_ARRAY_EXECUTE(type, name, length)                         \
  memset(name##_, 0, sizeof(type) * length);
  ISOLATE_INIT_ARRAY_LIST(ISOLATE_INIT_ARRAY_EXECUTE)
#undef ISOLATE_INIT_ARRAY_EXECUTE

  InitializeLoggingAndCounters();
  debug_ = new Debug(this);
  debugger_ = new Debugger(this);
}


void Isolate::TearDown() {
  TRACE_ISOLATE(tear_down);

  // Temporarily set this isolate as current so that various parts of
  // the isolate can access it in their destructors without having a
  // direct pointer. We don't use Enter/Exit here to avoid
  // initializing the thread data.
  PerIsolateThreadData* saved_data = CurrentPerIsolateThreadData();
  Isolate* saved_isolate = UncheckedCurrent();
  SetIsolateThreadLocals(this, NULL);

  Deinit();

  { LockGuard<Mutex> lock_guard(&process_wide_mutex_);
    thread_data_table_->RemoveAllThreads(this);
  }

  if (serialize_partial_snapshot_cache_ != NULL) {
    delete[] serialize_partial_snapshot_cache_;
    serialize_partial_snapshot_cache_ = NULL;
  }

  if (!IsDefaultIsolate()) {
    delete this;
  }

  // Restore the previous current isolate.
  SetIsolateThreadLocals(saved_isolate, saved_data);
}


void Isolate::GlobalTearDown() {
  delete thread_data_table_;
}


void Isolate::Deinit() {
  if (state_ == INITIALIZED) {
    TRACE_ISOLATE(deinit);

    debugger()->UnloadDebugger();

    if (concurrent_recompilation_enabled()) {
      optimizing_compiler_thread_->Stop();
      delete optimizing_compiler_thread_;
      optimizing_compiler_thread_ = NULL;
    }

    for (int i = 0; i < num_sweeper_threads_; i++) {
      sweeper_thread_[i]->Stop();
      delete sweeper_thread_[i];
      sweeper_thread_[i] = NULL;
    }
    delete[] sweeper_thread_;
    sweeper_thread_ = NULL;

    if (FLAG_job_based_sweeping &&
        heap_.mark_compact_collector()->IsConcurrentSweepingInProgress()) {
      heap_.mark_compact_collector()->WaitUntilSweepingCompleted();
    }

    if (FLAG_hydrogen_stats) GetHStatistics()->Print();

    if (FLAG_print_deopt_stress) {
      PrintF(stdout, "=== Stress deopt counter: %u\n", stress_deopt_count_);
    }

    // We must stop the logger before we tear down other components.
    Sampler* sampler = logger_->sampler();
    if (sampler && sampler->IsActive()) sampler->Stop();

    delete deoptimizer_data_;
    deoptimizer_data_ = NULL;
    builtins_.TearDown();
    bootstrapper_->TearDown();

    if (runtime_profiler_ != NULL) {
      delete runtime_profiler_;
      runtime_profiler_ = NULL;
    }
    heap_.TearDown();
    logger_->TearDown();

    delete heap_profiler_;
    heap_profiler_ = NULL;
    delete cpu_profiler_;
    cpu_profiler_ = NULL;

    // The default isolate is re-initializable due to legacy API.
    state_ = UNINITIALIZED;
  }
}


void Isolate::PushToPartialSnapshotCache(Object* obj) {
  int length = serialize_partial_snapshot_cache_length();
  int capacity = serialize_partial_snapshot_cache_capacity();

  if (length >= capacity) {
    int new_capacity = static_cast<int>((capacity + 10) * 1.2);
    Object** new_array = new Object*[new_capacity];
    for (int i = 0; i < length; i++) {
      new_array[i] = serialize_partial_snapshot_cache()[i];
    }
    if (capacity != 0) delete[] serialize_partial_snapshot_cache();
    set_serialize_partial_snapshot_cache(new_array);
    set_serialize_partial_snapshot_cache_capacity(new_capacity);
  }

  serialize_partial_snapshot_cache()[length] = obj;
  set_serialize_partial_snapshot_cache_length(length + 1);
}


void Isolate::SetIsolateThreadLocals(Isolate* isolate,
                                     PerIsolateThreadData* data) {
  Thread::SetThreadLocal(isolate_key_, isolate);
  Thread::SetThreadLocal(per_isolate_thread_data_key_, data);
}


Isolate::~Isolate() {
  TRACE_ISOLATE(destructor);

  // Has to be called while counters_ are still alive
  runtime_zone_.DeleteKeptSegment();

  // The entry stack must be empty when we get here.
  ASSERT(entry_stack_ == NULL || entry_stack_->previous_item == NULL);

  delete entry_stack_;
  entry_stack_ = NULL;

  delete[] assembler_spare_buffer_;
  assembler_spare_buffer_ = NULL;

  delete unicode_cache_;
  unicode_cache_ = NULL;

  delete date_cache_;
  date_cache_ = NULL;

  delete[] code_stub_interface_descriptors_;
  code_stub_interface_descriptors_ = NULL;

  delete[] call_descriptors_;
  call_descriptors_ = NULL;

  delete regexp_stack_;
  regexp_stack_ = NULL;

  delete descriptor_lookup_cache_;
  descriptor_lookup_cache_ = NULL;
  delete context_slot_cache_;
  context_slot_cache_ = NULL;
  delete keyed_lookup_cache_;
  keyed_lookup_cache_ = NULL;

  delete stub_cache_;
  stub_cache_ = NULL;
  delete stats_table_;
  stats_table_ = NULL;

  delete materialized_object_store_;
  materialized_object_store_ = NULL;

  delete logger_;
  logger_ = NULL;

  delete counters_;
  counters_ = NULL;

  delete handle_scope_implementer_;
  handle_scope_implementer_ = NULL;

  delete compilation_cache_;
  compilation_cache_ = NULL;
  delete bootstrapper_;
  bootstrapper_ = NULL;
  delete inner_pointer_to_code_cache_;
  inner_pointer_to_code_cache_ = NULL;
  delete write_iterator_;
  write_iterator_ = NULL;

  delete thread_manager_;
  thread_manager_ = NULL;

  delete string_tracker_;
  string_tracker_ = NULL;

  delete memory_allocator_;
  memory_allocator_ = NULL;
  delete code_range_;
  code_range_ = NULL;
  delete global_handles_;
  global_handles_ = NULL;
  delete eternal_handles_;
  eternal_handles_ = NULL;

  delete string_stream_debug_object_cache_;
  string_stream_debug_object_cache_ = NULL;

  delete external_reference_table_;
  external_reference_table_ = NULL;

  delete random_number_generator_;
  random_number_generator_ = NULL;

  delete debugger_;
  debugger_ = NULL;
  delete debug_;
  debug_ = NULL;
}


void Isolate::InitializeThreadLocal() {
  thread_local_top_.isolate_ = this;
  thread_local_top_.Initialize();
}


void Isolate::PropagatePendingExceptionToExternalTryCatch() {
  ASSERT(has_pending_exception());

  bool external_caught = IsExternallyCaught();
  thread_local_top_.external_caught_exception_ = external_caught;

  if (!external_caught) return;

  if (thread_local_top_.pending_exception_ ==
             heap()->termination_exception()) {
    try_catch_handler()->can_continue_ = false;
    try_catch_handler()->has_terminated_ = true;
    try_catch_handler()->exception_ = heap()->null_value();
  } else {
    v8::TryCatch* handler = try_catch_handler();
    ASSERT(thread_local_top_.pending_message_obj_->IsJSMessageObject() ||
           thread_local_top_.pending_message_obj_->IsTheHole());
    ASSERT(thread_local_top_.pending_message_script_->IsScript() ||
           thread_local_top_.pending_message_script_->IsTheHole());
    handler->can_continue_ = true;
    handler->has_terminated_ = false;
    handler->exception_ = pending_exception();
    // Propagate to the external try-catch only if we got an actual message.
    if (thread_local_top_.pending_message_obj_->IsTheHole()) return;

    handler->message_obj_ = thread_local_top_.pending_message_obj_;
    handler->message_script_ = thread_local_top_.pending_message_script_;
    handler->message_start_pos_ = thread_local_top_.pending_message_start_pos_;
    handler->message_end_pos_ = thread_local_top_.pending_message_end_pos_;
  }
}


void Isolate::InitializeLoggingAndCounters() {
  if (logger_ == NULL) {
    logger_ = new Logger(this);
  }
  if (counters_ == NULL) {
    counters_ = new Counters(this);
  }
}


bool Isolate::Init(Deserializer* des) {
  ASSERT(state_ != INITIALIZED);
  TRACE_ISOLATE(init);

  stress_deopt_count_ = FLAG_deopt_every_n_times;

  has_fatal_error_ = false;

  use_crankshaft_ = FLAG_crankshaft
      && !Serializer::enabled(this)
      && CpuFeatures::SupportsCrankshaft();

  if (function_entry_hook() != NULL) {
    // When function entry hooking is in effect, we have to create the code
    // stubs from scratch to get entry hooks, rather than loading the previously
    // generated stubs from disk.
    // If this assert fires, the initialization path has regressed.
    ASSERT(des == NULL);
  }

  // The initialization process does not handle memory exhaustion.
  DisallowAllocationFailure disallow_allocation_failure(this);

  memory_allocator_ = new MemoryAllocator(this);
  code_range_ = new CodeRange(this);

  // Safe after setting Heap::isolate_, and initializing StackGuard
  heap_.SetStackLimits();

#define ASSIGN_ELEMENT(CamelName, hacker_name)                  \
  isolate_addresses_[Isolate::k##CamelName##Address] =          \
      reinterpret_cast<Address>(hacker_name##_address());
  FOR_EACH_ISOLATE_ADDRESS_NAME(ASSIGN_ELEMENT)
#undef ASSIGN_ELEMENT

  string_tracker_ = new StringTracker();
  string_tracker_->isolate_ = this;
  compilation_cache_ = new CompilationCache(this);
  keyed_lookup_cache_ = new KeyedLookupCache();
  context_slot_cache_ = new ContextSlotCache();
  descriptor_lookup_cache_ = new DescriptorLookupCache();
  unicode_cache_ = new UnicodeCache();
  inner_pointer_to_code_cache_ = new InnerPointerToCodeCache(this);
  write_iterator_ = new ConsStringIteratorOp();
  global_handles_ = new GlobalHandles(this);
  eternal_handles_ = new EternalHandles();
  bootstrapper_ = new Bootstrapper(this);
  handle_scope_implementer_ = new HandleScopeImplementer(this);
  stub_cache_ = new StubCache(this);
  materialized_object_store_ = new MaterializedObjectStore(this);
  regexp_stack_ = new RegExpStack();
  regexp_stack_->isolate_ = this;
  date_cache_ = new DateCache();
  code_stub_interface_descriptors_ =
      new CodeStubInterfaceDescriptor[CodeStub::NUMBER_OF_IDS];
  call_descriptors_ =
      new CallInterfaceDescriptor[NUMBER_OF_CALL_DESCRIPTORS];
  cpu_profiler_ = new CpuProfiler(this);
  heap_profiler_ = new HeapProfiler(heap());

  // Enable logging before setting up the heap
  logger_->SetUp(this);

  // Initialize other runtime facilities
#if defined(USE_SIMULATOR)
#if V8_TARGET_ARCH_ARM || V8_TARGET_ARCH_ARM64 || V8_TARGET_ARCH_MIPS
  Simulator::Initialize(this);
#endif
#endif

  { // NOLINT
    // Ensure that the thread has a valid stack guard.  The v8::Locker object
    // will ensure this too, but we don't have to use lockers if we are only
    // using one thread.
    ExecutionAccess lock(this);
    stack_guard_.InitThread(lock);
  }

  // SetUp the object heap.
  ASSERT(!heap_.HasBeenSetUp());
  if (!heap_.SetUp()) {
    V8::FatalProcessOutOfMemory("heap setup");
    return false;
  }

  deoptimizer_data_ = new DeoptimizerData(memory_allocator_);

  const bool create_heap_objects = (des == NULL);
  if (create_heap_objects && !heap_.CreateHeapObjects()) {
    V8::FatalProcessOutOfMemory("heap object creation");
    return false;
  }

  if (create_heap_objects) {
    // Terminate the cache array with the sentinel so we can iterate.
    PushToPartialSnapshotCache(heap_.undefined_value());
  }

  InitializeThreadLocal();

  bootstrapper_->Initialize(create_heap_objects);
  builtins_.SetUp(this, create_heap_objects);

  if (FLAG_log_internal_timer_events) {
    set_event_logger(Logger::LogInternalEvents);
  } else {
    set_event_logger(Logger::EmptyLogInternalEvents);
  }

  // Set default value if not yet set.
  // TODO(yangguo): move this to ResourceConstraints::ConfigureDefaults
  // once ResourceConstraints becomes an argument to the Isolate constructor.
  if (max_available_threads_ < 1) {
    // Choose the default between 1 and 4.
    max_available_threads_ = Max(Min(CPU::NumberOfProcessorsOnline(), 4), 1);
  }

  if (!FLAG_job_based_sweeping) {
    num_sweeper_threads_ =
        SweeperThread::NumberOfThreads(max_available_threads_);
  }

  if (FLAG_trace_hydrogen || FLAG_trace_hydrogen_stubs) {
    PrintF("Concurrent recompilation has been disabled for tracing.\n");
  } else if (OptimizingCompilerThread::Enabled(max_available_threads_)) {
    optimizing_compiler_thread_ = new OptimizingCompilerThread(this);
    optimizing_compiler_thread_->Start();
  }

  if (num_sweeper_threads_ > 0) {
    sweeper_thread_ = new SweeperThread*[num_sweeper_threads_];
    for (int i = 0; i < num_sweeper_threads_; i++) {
      sweeper_thread_[i] = new SweeperThread(this);
      sweeper_thread_[i]->Start();
    }
  }

  // If we are deserializing, read the state into the now-empty heap.
  if (!create_heap_objects) {
    des->Deserialize(this);
  }
  stub_cache_->Initialize();

  // Finish initialization of ThreadLocal after deserialization is done.
  clear_pending_exception();
  clear_pending_message();
  clear_scheduled_exception();

  // Deserializing may put strange things in the root array's copy of the
  // stack guard.
  heap_.SetStackLimits();

  // Quiet the heap NaN if needed on target platform.
  if (!create_heap_objects) Assembler::QuietNaN(heap_.nan_value());

  runtime_profiler_ = new RuntimeProfiler(this);

  // If we are deserializing, log non-function code objects and compiled
  // functions found in the snapshot.
  if (!create_heap_objects &&
      (FLAG_log_code ||
       FLAG_ll_prof ||
       FLAG_perf_jit_prof ||
       FLAG_perf_basic_prof ||
       logger_->is_logging_code_events())) {
    HandleScope scope(this);
    LOG(this, LogCodeObjects());
    LOG(this, LogCompiledFunctions());
  }

  // If we are profiling with the Linux perf tool, we need to disable
  // code relocation.
  if (FLAG_perf_jit_prof || FLAG_perf_basic_prof) {
    FLAG_compact_code_space = false;
  }

  CHECK_EQ(static_cast<int>(OFFSET_OF(Isolate, embedder_data_)),
           Internals::kIsolateEmbedderDataOffset);
  CHECK_EQ(static_cast<int>(OFFSET_OF(Isolate, heap_.roots_)),
           Internals::kIsolateRootsOffset);

  state_ = INITIALIZED;
  time_millis_at_init_ = OS::TimeCurrentMillis();

  if (!create_heap_objects) {
    // Now that the heap is consistent, it's OK to generate the code for the
    // deopt entry table that might have been referred to by optimized code in
    // the snapshot.
    HandleScope scope(this);
    Deoptimizer::EnsureCodeForDeoptimizationEntry(
        this,
        Deoptimizer::LAZY,
        kDeoptTableSerializeEntryCount - 1);
  }

  if (!Serializer::enabled(this)) {
    // Ensure that all stubs which need to be generated ahead of time, but
    // cannot be serialized into the snapshot have been generated.
    HandleScope scope(this);
    CodeStub::GenerateFPStubs(this);
    StoreBufferOverflowStub::GenerateFixedRegStubsAheadOfTime(this);
    StubFailureTrampolineStub::GenerateAheadOfTime(this);
    // Ensure interface descriptors are initialized even when stubs have been
    // deserialized out of the snapshot without using the graph builder.
    FastCloneShallowArrayStub::InstallDescriptors(this);
    BinaryOpICStub::InstallDescriptors(this);
    BinaryOpWithAllocationSiteStub::InstallDescriptors(this);
    CompareNilICStub::InstallDescriptors(this);
    ToBooleanStub::InstallDescriptors(this);
    ToNumberStub::InstallDescriptors(this);
    ArrayConstructorStubBase::InstallDescriptors(this);
    InternalArrayConstructorStubBase::InstallDescriptors(this);
    FastNewClosureStub::InstallDescriptors(this);
    FastNewContextStub::InstallDescriptors(this);
    NumberToStringStub::InstallDescriptors(this);
    StringAddStub::InstallDescriptors(this);
    RegExpConstructResultStub::InstallDescriptors(this);
  }

  CallDescriptors::InitializeForIsolate(this);

  initialized_from_snapshot_ = (des != NULL);

  return true;
}


// Initialized lazily to allow early
// v8::V8::SetAddHistogramSampleFunction calls.
StatsTable* Isolate::stats_table() {
  if (stats_table_ == NULL) {
    stats_table_ = new StatsTable;
  }
  return stats_table_;
}


void Isolate::Enter() {
  Isolate* current_isolate = NULL;
  PerIsolateThreadData* current_data = CurrentPerIsolateThreadData();
  if (current_data != NULL) {
    current_isolate = current_data->isolate_;
    ASSERT(current_isolate != NULL);
    if (current_isolate == this) {
      ASSERT(Current() == this);
      ASSERT(entry_stack_ != NULL);
      ASSERT(entry_stack_->previous_thread_data == NULL ||
             entry_stack_->previous_thread_data->thread_id().Equals(
                 ThreadId::Current()));
      // Same thread re-enters the isolate, no need to re-init anything.
      entry_stack_->entry_count++;
      return;
    }
  }

  // Threads can have default isolate set into TLS as Current but not yet have
  // PerIsolateThreadData for it, as it requires more advanced phase of the
  // initialization. For example, a thread might be the one that system used for
  // static initializers - in this case the default isolate is set in TLS but
  // the thread did not yet Enter the isolate. If PerisolateThreadData is not
  // there, use the isolate set in TLS.
  if (current_isolate == NULL) {
    current_isolate = Isolate::UncheckedCurrent();
  }

  PerIsolateThreadData* data = FindOrAllocatePerThreadDataForThisThread();
  ASSERT(data != NULL);
  ASSERT(data->isolate_ == this);

  EntryStackItem* item = new EntryStackItem(current_data,
                                            current_isolate,
                                            entry_stack_);
  entry_stack_ = item;

  SetIsolateThreadLocals(this, data);

  // In case it's the first time some thread enters the isolate.
  set_thread_id(data->thread_id());
}


void Isolate::Exit() {
  ASSERT(entry_stack_ != NULL);
  ASSERT(entry_stack_->previous_thread_data == NULL ||
         entry_stack_->previous_thread_data->thread_id().Equals(
             ThreadId::Current()));

  if (--entry_stack_->entry_count > 0) return;

  ASSERT(CurrentPerIsolateThreadData() != NULL);
  ASSERT(CurrentPerIsolateThreadData()->isolate_ == this);

  // Pop the stack.
  EntryStackItem* item = entry_stack_;
  entry_stack_ = item->previous_item;

  PerIsolateThreadData* previous_thread_data = item->previous_thread_data;
  Isolate* previous_isolate = item->previous_isolate;

  delete item;

  // Reinit the current thread for the isolate it was running before this one.
  SetIsolateThreadLocals(previous_isolate, previous_thread_data);
}


void Isolate::LinkDeferredHandles(DeferredHandles* deferred) {
  deferred->next_ = deferred_handles_head_;
  if (deferred_handles_head_ != NULL) {
    deferred_handles_head_->previous_ = deferred;
  }
  deferred_handles_head_ = deferred;
}


void Isolate::UnlinkDeferredHandles(DeferredHandles* deferred) {
#ifdef DEBUG
  // In debug mode assert that the linked list is well-formed.
  DeferredHandles* deferred_iterator = deferred;
  while (deferred_iterator->previous_ != NULL) {
    deferred_iterator = deferred_iterator->previous_;
  }
  ASSERT(deferred_handles_head_ == deferred_iterator);
#endif
  if (deferred_handles_head_ == deferred) {
    deferred_handles_head_ = deferred_handles_head_->next_;
  }
  if (deferred->next_ != NULL) {
    deferred->next_->previous_ = deferred->previous_;
  }
  if (deferred->previous_ != NULL) {
    deferred->previous_->next_ = deferred->next_;
  }
}


HStatistics* Isolate::GetHStatistics() {
  if (hstatistics() == NULL) set_hstatistics(new HStatistics());
  return hstatistics();
}


HTracer* Isolate::GetHTracer() {
  if (htracer() == NULL) set_htracer(new HTracer(id()));
  return htracer();
}


CodeTracer* Isolate::GetCodeTracer() {
  if (code_tracer() == NULL) set_code_tracer(new CodeTracer(id()));
  return code_tracer();
}


Map* Isolate::get_initial_js_array_map(ElementsKind kind) {
  Context* native_context = context()->native_context();
  Object* maybe_map_array = native_context->js_array_maps();
  if (!maybe_map_array->IsUndefined()) {
    Object* maybe_transitioned_map =
        FixedArray::cast(maybe_map_array)->get(kind);
    if (!maybe_transitioned_map->IsUndefined()) {
      return Map::cast(maybe_transitioned_map);
    }
  }
  return NULL;
}


bool Isolate::IsFastArrayConstructorPrototypeChainIntact() {
  Map* root_array_map =
      get_initial_js_array_map(GetInitialFastElementsKind());
  ASSERT(root_array_map != NULL);
  JSObject* initial_array_proto = JSObject::cast(*initial_array_prototype());

  // Check that the array prototype hasn't been altered WRT empty elements.
  if (root_array_map->prototype() != initial_array_proto) return false;
  if (initial_array_proto->elements() != heap()->empty_fixed_array()) {
    return false;
  }

  // Check that the object prototype hasn't been altered WRT empty elements.
  JSObject* initial_object_proto = JSObject::cast(*initial_object_prototype());
  Object* root_array_map_proto = initial_array_proto->GetPrototype();
  if (root_array_map_proto != initial_object_proto) return false;
  if (initial_object_proto->elements() != heap()->empty_fixed_array()) {
    return false;
  }

  return initial_object_proto->GetPrototype()->IsNull();
}


CodeStubInterfaceDescriptor*
    Isolate::code_stub_interface_descriptor(int index) {
  return code_stub_interface_descriptors_ + index;
}


CallInterfaceDescriptor*
    Isolate::call_descriptor(CallDescriptorKey index) {
  ASSERT(0 <= index && index < NUMBER_OF_CALL_DESCRIPTORS);
  return &call_descriptors_[index];
}


Object* Isolate::FindCodeObject(Address a) {
  return inner_pointer_to_code_cache()->GcSafeFindCodeForInnerPointer(a);
}


#ifdef DEBUG
#define ISOLATE_FIELD_OFFSET(type, name, ignored)                       \
const intptr_t Isolate::name##_debug_offset_ = OFFSET_OF(Isolate, name##_);
ISOLATE_INIT_LIST(ISOLATE_FIELD_OFFSET)
ISOLATE_INIT_ARRAY_LIST(ISOLATE_FIELD_OFFSET)
#undef ISOLATE_FIELD_OFFSET
#endif


Handle<JSObject> Isolate::GetSymbolRegistry() {
  if (heap()->symbol_registry()->IsUndefined()) {
    Handle<Map> map = factory()->NewMap(JS_OBJECT_TYPE, JSObject::kHeaderSize);
    Handle<JSObject> registry = factory()->NewJSObjectFromMap(map);
    heap()->set_symbol_registry(*registry);

    static const char* nested[] = {
      "for", "for_api", "for_intern", "keyFor", "private_api", "private_intern"
    };
    for (unsigned i = 0; i < ARRAY_SIZE(nested); ++i) {
      Handle<String> name = factory()->InternalizeUtf8String(nested[i]);
      Handle<JSObject> obj = factory()->NewJSObjectFromMap(map);
      JSObject::NormalizeProperties(obj, KEEP_INOBJECT_PROPERTIES, 8);
      JSObject::SetProperty(registry, name, obj, NONE, STRICT).Assert();
    }
  }
  return Handle<JSObject>::cast(factory()->symbol_registry());
}


void Isolate::AddCallCompletedCallback(CallCompletedCallback callback) {
  for (int i = 0; i < call_completed_callbacks_.length(); i++) {
    if (callback == call_completed_callbacks_.at(i)) return;
  }
  call_completed_callbacks_.Add(callback);
}


void Isolate::RemoveCallCompletedCallback(CallCompletedCallback callback) {
  for (int i = 0; i < call_completed_callbacks_.length(); i++) {
    if (callback == call_completed_callbacks_.at(i)) {
      call_completed_callbacks_.Remove(i);
    }
  }
}


void Isolate::FireCallCompletedCallback() {
  bool has_call_completed_callbacks = !call_completed_callbacks_.is_empty();
  bool run_microtasks = autorun_microtasks() && microtask_pending();
  if (!has_call_completed_callbacks && !run_microtasks) return;

  if (!handle_scope_implementer()->CallDepthIsZero()) return;
  // Fire callbacks.  Increase call depth to prevent recursive callbacks.
  handle_scope_implementer()->IncrementCallDepth();
  if (run_microtasks) Execution::RunMicrotasks(this);
  for (int i = 0; i < call_completed_callbacks_.length(); i++) {
    call_completed_callbacks_.at(i)();
  }
  handle_scope_implementer()->DecrementCallDepth();
}


void Isolate::RunMicrotasks() {
  if (!microtask_pending())
    return;

  ASSERT(handle_scope_implementer()->CallDepthIsZero());

  // Increase call depth to prevent recursive callbacks.
  handle_scope_implementer()->IncrementCallDepth();
  Execution::RunMicrotasks(this);
  handle_scope_implementer()->DecrementCallDepth();
}


} }  // namespace v8::internal
