// Copyright Joyent, Inc. and other Node contributors.
//
// Permission is hereby granted, free of charge, to any person obtaining a
// copy of this software and associated documentation files (the
// "Software"), to deal in the Software without restriction, including
// without limitation the rights to use, copy, modify, merge, publish,
// distribute, sublicense, and/or sell copies of the Software, and to permit
// persons to whom the Software is furnished to do so, subject to the
// following conditions:
//
// The above copyright notice and this permission notice shall be included
// in all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
// OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
// MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN
// NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM,
// DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR
// OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE
// USE OR OTHER DEALINGS IN THE SOFTWARE.

#ifndef SRC_ENV_INL_H_
#define SRC_ENV_INL_H_

#if defined(NODE_WANT_INTERNALS) && NODE_WANT_INTERNALS

#include "aliased_buffer.h"
#include "env.h"
#include "node.h"
#include "util-inl.h"
#include "uv.h"
#include "v8.h"
#include "node_perf_common.h"
#include "node_context_data.h"
#include "node_worker.h"

#include <stddef.h>
#include <stdint.h>

namespace node {

inline v8::Isolate* IsolateData::isolate() const {
  return isolate_;
}

inline uv_loop_t* IsolateData::event_loop() const {
  return event_loop_;
}

inline bool IsolateData::uses_node_allocator() const {
  return uses_node_allocator_;
}

inline v8::ArrayBuffer::Allocator* IsolateData::allocator() const {
  return allocator_;
}

inline ArrayBufferAllocator* IsolateData::node_allocator() const {
  return node_allocator_;
}

inline MultiIsolatePlatform* IsolateData::platform() const {
  return platform_;
}

inline Environment::AsyncHooks::AsyncHooks()
    : async_ids_stack_(env()->isolate(), 16 * 2),
      fields_(env()->isolate(), kFieldsCount),
      async_id_fields_(env()->isolate(), kUidFieldsCount) {
  v8::HandleScope handle_scope(env()->isolate());

  // Always perform async_hooks checks, not just when async_hooks is enabled.
  // TODO(AndreasMadsen): Consider removing this for LTS releases.
  // See discussion in https://github.com/nodejs/node/pull/15454
  // When removing this, do it by reverting the commit. Otherwise the test
  // and flag changes won't be included.
  fields_[kCheck] = 1;

  // kDefaultTriggerAsyncId should be -1, this indicates that there is no
  // specified default value and it should fallback to the executionAsyncId.
  // 0 is not used as the magic value, because that indicates a missing context
  // which is different from a default context.
  async_id_fields_[AsyncHooks::kDefaultTriggerAsyncId] = -1;

  // kAsyncIdCounter should start at 1 because that'll be the id the execution
  // context during bootstrap (code that runs before entering uv_run()).
  async_id_fields_[AsyncHooks::kAsyncIdCounter] = 1;

  // Create all the provider strings that will be passed to JS. Place them in
  // an array so the array index matches the PROVIDER id offset. This way the
  // strings can be retrieved quickly.
#define V(Provider)                                                           \
  providers_[AsyncWrap::PROVIDER_ ## Provider].Set(                           \
      env()->isolate(),                                                       \
      v8::String::NewFromOneByte(                                             \
        env()->isolate(),                                                     \
        reinterpret_cast<const uint8_t*>(#Provider),                          \
        v8::NewStringType::kInternalized,                                     \
        sizeof(#Provider) - 1).ToLocalChecked());
  NODE_ASYNC_PROVIDER_TYPES(V)
#undef V
}

inline AliasedBuffer<uint32_t, v8::Uint32Array>&
Environment::AsyncHooks::fields() {
  return fields_;
}

inline AliasedBuffer<double, v8::Float64Array>&
Environment::AsyncHooks::async_id_fields() {
  return async_id_fields_;
}

inline AliasedBuffer<double, v8::Float64Array>&
Environment::AsyncHooks::async_ids_stack() {
  return async_ids_stack_;
}

inline v8::Local<v8::String> Environment::AsyncHooks::provider_string(int idx) {
  return providers_[idx].Get(env()->isolate());
}

inline void Environment::AsyncHooks::no_force_checks() {
  fields_[kCheck] -= 1;
}

inline Environment* Environment::AsyncHooks::env() {
  return Environment::ForAsyncHooks(this);
}

// Remember to keep this code aligned with pushAsyncIds() in JS.
inline void Environment::AsyncHooks::push_async_ids(double async_id,
                                                    double trigger_async_id) {
  // Since async_hooks is experimental, do only perform the check
  // when async_hooks is enabled.
  if (fields_[kCheck] > 0) {
    CHECK_GE(async_id, -1);
    CHECK_GE(trigger_async_id, -1);
  }

  uint32_t offset = fields_[kStackLength];
  if (offset * 2 >= async_ids_stack_.Length())
    grow_async_ids_stack();
  async_ids_stack_[2 * offset] = async_id_fields_[kExecutionAsyncId];
  async_ids_stack_[2 * offset + 1] = async_id_fields_[kTriggerAsyncId];
  fields_[kStackLength] += 1;
  async_id_fields_[kExecutionAsyncId] = async_id;
  async_id_fields_[kTriggerAsyncId] = trigger_async_id;
}

// Remember to keep this code aligned with popAsyncIds() in JS.
inline bool Environment::AsyncHooks::pop_async_id(double async_id) {
  // In case of an exception then this may have already been reset, if the
  // stack was multiple MakeCallback()'s deep.
  if (fields_[kStackLength] == 0) return false;

  // Ask for the async_id to be restored as a check that the stack
  // hasn't been corrupted.
  // Since async_hooks is experimental, do only perform the check
  // when async_hooks is enabled.
  if (fields_[kCheck] > 0 && async_id_fields_[kExecutionAsyncId] != async_id) {
    fprintf(stderr,
            "Error: async hook stack has become corrupted ("
            "actual: %.f, expected: %.f)\n",
            async_id_fields_.GetValue(kExecutionAsyncId),
            async_id);
    DumpBacktrace(stderr);
    fflush(stderr);
    if (!env()->abort_on_uncaught_exception())
      exit(1);
    fprintf(stderr, "\n");
    fflush(stderr);
    ABORT_NO_BACKTRACE();
  }

  uint32_t offset = fields_[kStackLength] - 1;
  async_id_fields_[kExecutionAsyncId] = async_ids_stack_[2 * offset];
  async_id_fields_[kTriggerAsyncId] = async_ids_stack_[2 * offset + 1];
  fields_[kStackLength] = offset;

  return fields_[kStackLength] > 0;
}

// Keep in sync with clearAsyncIdStack in lib/internal/async_hooks.js.
inline void Environment::AsyncHooks::clear_async_id_stack() {
  async_id_fields_[kExecutionAsyncId] = 0;
  async_id_fields_[kTriggerAsyncId] = 0;
  fields_[kStackLength] = 0;
}

// The DefaultTriggerAsyncIdScope(AsyncWrap*) constructor is defined in
// async_wrap-inl.h to avoid a circular dependency.

inline Environment::AsyncHooks::DefaultTriggerAsyncIdScope
  ::DefaultTriggerAsyncIdScope(Environment* env,
                               double default_trigger_async_id)
    : async_id_fields_ref_(env->async_hooks()->async_id_fields()) {
  if (env->async_hooks()->fields()[AsyncHooks::kCheck] > 0) {
    CHECK_GE(default_trigger_async_id, 0);
  }

  old_default_trigger_async_id_ =
    async_id_fields_ref_[AsyncHooks::kDefaultTriggerAsyncId];
  async_id_fields_ref_[AsyncHooks::kDefaultTriggerAsyncId] =
    default_trigger_async_id;
}

inline Environment::AsyncHooks::DefaultTriggerAsyncIdScope
  ::~DefaultTriggerAsyncIdScope() {
  async_id_fields_ref_[AsyncHooks::kDefaultTriggerAsyncId] =
    old_default_trigger_async_id_;
}


Environment* Environment::ForAsyncHooks(AsyncHooks* hooks) {
  return ContainerOf(&Environment::async_hooks_, hooks);
}


inline Environment::AsyncCallbackScope::AsyncCallbackScope(Environment* env)
    : env_(env) {
  env_->makecallback_cntr_++;
}

inline Environment::AsyncCallbackScope::~AsyncCallbackScope() {
  env_->makecallback_cntr_--;
}

inline size_t Environment::makecallback_depth() const {
  return makecallback_cntr_;
}

inline Environment::ImmediateInfo::ImmediateInfo(v8::Isolate* isolate)
    : fields_(isolate, kFieldsCount) {}

inline AliasedBuffer<uint32_t, v8::Uint32Array>&
    Environment::ImmediateInfo::fields() {
  return fields_;
}

inline uint32_t Environment::ImmediateInfo::count() const {
  return fields_[kCount];
}

inline uint32_t Environment::ImmediateInfo::ref_count() const {
  return fields_[kRefCount];
}

inline bool Environment::ImmediateInfo::has_outstanding() const {
  return fields_[kHasOutstanding] == 1;
}

inline void Environment::ImmediateInfo::count_inc(uint32_t increment) {
  fields_[kCount] += increment;
}

inline void Environment::ImmediateInfo::count_dec(uint32_t decrement) {
  fields_[kCount] -= decrement;
}

inline void Environment::ImmediateInfo::ref_count_inc(uint32_t increment) {
  fields_[kRefCount] += increment;
}

inline void Environment::ImmediateInfo::ref_count_dec(uint32_t decrement) {
  fields_[kRefCount] -= decrement;
}

inline Environment::TickInfo::TickInfo(v8::Isolate* isolate)
    : fields_(isolate, kFieldsCount) {}

inline AliasedBuffer<uint8_t, v8::Uint8Array>& Environment::TickInfo::fields() {
  return fields_;
}

inline bool Environment::TickInfo::has_scheduled() const {
  return fields_[kHasScheduled] == 1;
}

inline bool Environment::TickInfo::has_thrown() const {
  return fields_[kHasThrown] == 1;
}

inline bool Environment::TickInfo::has_promise_rejections() const {
  return fields_[kHasPromiseRejections] == 1;
}

inline void Environment::TickInfo::promise_rejections_toggle_on() {
  fields_[kHasPromiseRejections] = 1;
}

inline void Environment::TickInfo::set_has_thrown(bool state) {
  fields_[kHasThrown] = state ? 1 : 0;
}

inline void Environment::AssignToContext(v8::Local<v8::Context> context,
                                         const ContextInfo& info) {
  context->SetAlignedPointerInEmbedderData(
      ContextEmbedderIndex::kEnvironment, this);
  // Used by EnvPromiseHook to know that we are on a node context.
  context->SetAlignedPointerInEmbedderData(
    ContextEmbedderIndex::kContextTag, Environment::kNodeContextTagPtr);
#if HAVE_INSPECTOR
  inspector_agent()->ContextCreated(context, info);
#endif  // HAVE_INSPECTOR
}

inline Environment* Environment::GetCurrent(v8::Isolate* isolate) {
  return GetCurrent(isolate->GetCurrentContext());
}

inline Environment* Environment::GetCurrent(v8::Local<v8::Context> context) {
  if (UNLIKELY(context.IsEmpty() ||
      context->GetNumberOfEmbedderDataFields() <
          ContextEmbedderIndex::kContextTag ||
      context->GetAlignedPointerFromEmbedderData(
          ContextEmbedderIndex::kContextTag) !=
          Environment::kNodeContextTagPtr)) {
    return nullptr;
  }

  return static_cast<Environment*>(
      context->GetAlignedPointerFromEmbedderData(
          ContextEmbedderIndex::kEnvironment));
}

inline Environment* Environment::GetCurrent(
    const v8::FunctionCallbackInfo<v8::Value>& info) {
  CHECK(info.Data()->IsExternal());
  return static_cast<Environment*>(info.Data().As<v8::External>()->Value());
}

template <typename T>
inline Environment* Environment::GetCurrent(
    const v8::PropertyCallbackInfo<T>& info) {
  CHECK(info.Data()->IsExternal());
  return static_cast<Environment*>(
      info.Data().template As<v8::External>()->Value());
}

inline Environment* Environment::GetThreadLocalEnv() {
  return static_cast<Environment*>(uv_key_get(&thread_local_env));
}

inline bool Environment::profiler_idle_notifier_started() const {
  return profiler_idle_notifier_started_;
}

inline v8::Isolate* Environment::isolate() const {
  return isolate_;
}

inline tracing::AgentWriterHandle* Environment::tracing_agent_writer() const {
  return tracing_agent_writer_;
}

inline Environment* Environment::from_immediate_check_handle(
    uv_check_t* handle) {
  return ContainerOf(&Environment::immediate_check_handle_, handle);
}

inline uv_check_t* Environment::immediate_check_handle() {
  return &immediate_check_handle_;
}

inline uv_idle_t* Environment::immediate_idle_handle() {
  return &immediate_idle_handle_;
}

inline void Environment::RegisterHandleCleanup(uv_handle_t* handle,
                                               HandleCleanupCb cb,
                                               void* arg) {
  handle_cleanup_queue_.push_back(HandleCleanup{handle, cb, arg});
}

template <typename T, typename OnCloseCallback>
inline void Environment::CloseHandle(T* handle, OnCloseCallback callback) {
  handle_cleanup_waiting_++;
  static_assert(sizeof(T) >= sizeof(uv_handle_t), "T is a libuv handle");
  static_assert(offsetof(T, data) == offsetof(uv_handle_t, data),
                "T is a libuv handle");
  static_assert(offsetof(T, close_cb) == offsetof(uv_handle_t, close_cb),
                "T is a libuv handle");
  struct CloseData {
    Environment* env;
    OnCloseCallback callback;
    void* original_data;
  };
  handle->data = new CloseData { this, callback, handle->data };
  uv_close(reinterpret_cast<uv_handle_t*>(handle), [](uv_handle_t* handle) {
    std::unique_ptr<CloseData> data { static_cast<CloseData*>(handle->data) };
    data->env->handle_cleanup_waiting_--;
    handle->data = data->original_data;
    data->callback(reinterpret_cast<T*>(handle));
  });
}

void Environment::IncreaseWaitingRequestCounter() {
  request_waiting_++;
}

void Environment::DecreaseWaitingRequestCounter() {
  request_waiting_--;
  CHECK_GE(request_waiting_, 0);
}

inline uv_loop_t* Environment::event_loop() const {
  return isolate_data()->event_loop();
}

inline Environment::AsyncHooks* Environment::async_hooks() {
  return &async_hooks_;
}

inline Environment::ImmediateInfo* Environment::immediate_info() {
  return &immediate_info_;
}

inline Environment::TickInfo* Environment::tick_info() {
  return &tick_info_;
}

inline uint64_t Environment::timer_base() const {
  return timer_base_;
}

inline bool Environment::printed_error() const {
  return printed_error_;
}

inline void Environment::set_printed_error(bool value) {
  printed_error_ = value;
}

inline void Environment::set_trace_sync_io(bool value) {
  options_->trace_sync_io = value;
}

inline bool Environment::abort_on_uncaught_exception() const {
  return options_->abort_on_uncaught_exception;
}

inline void Environment::set_abort_on_uncaught_exception(bool value) {
  options_->abort_on_uncaught_exception = value;
}

inline AliasedBuffer<uint32_t, v8::Uint32Array>&
Environment::should_abort_on_uncaught_toggle() {
  return should_abort_on_uncaught_toggle_;
}

Environment::ShouldNotAbortOnUncaughtScope::ShouldNotAbortOnUncaughtScope(
    Environment* env)
    : env_(env) {
  env_->should_not_abort_scope_counter_++;
}

Environment::ShouldNotAbortOnUncaughtScope::~ShouldNotAbortOnUncaughtScope() {
  Close();
}

void Environment::ShouldNotAbortOnUncaughtScope::Close() {
  if (env_ != nullptr) {
    env_->should_not_abort_scope_counter_--;
    env_ = nullptr;
  }
}

bool Environment::inside_should_not_abort_on_uncaught_scope() const {
  return should_not_abort_scope_counter_ > 0;
}

inline std::vector<double>* Environment::destroy_async_id_list() {
  return &destroy_async_id_list_;
}

inline double Environment::new_async_id() {
  async_hooks()->async_id_fields()[AsyncHooks::kAsyncIdCounter] += 1;
  return async_hooks()->async_id_fields()[AsyncHooks::kAsyncIdCounter];
}

inline double Environment::execution_async_id() {
  return async_hooks()->async_id_fields()[AsyncHooks::kExecutionAsyncId];
}

inline double Environment::trigger_async_id() {
  return async_hooks()->async_id_fields()[AsyncHooks::kTriggerAsyncId];
}

inline double Environment::get_default_trigger_async_id() {
  double default_trigger_async_id =
    async_hooks()->async_id_fields()[AsyncHooks::kDefaultTriggerAsyncId];
  // If defaultTriggerAsyncId isn't set, use the executionAsyncId
  if (default_trigger_async_id < 0)
    default_trigger_async_id = execution_async_id();
  return default_trigger_async_id;
}

inline double* Environment::heap_statistics_buffer() const {
  CHECK_NOT_NULL(heap_statistics_buffer_);
  return heap_statistics_buffer_;
}

inline void Environment::set_heap_statistics_buffer(double* pointer) {
  CHECK_NULL(heap_statistics_buffer_);  // Should be set only once.
  heap_statistics_buffer_ = pointer;
}

inline double* Environment::heap_space_statistics_buffer() const {
  CHECK_NOT_NULL(heap_space_statistics_buffer_);
  return heap_space_statistics_buffer_;
}

inline void Environment::set_heap_space_statistics_buffer(double* pointer) {
  CHECK_NULL(heap_space_statistics_buffer_);  // Should be set only once.
  heap_space_statistics_buffer_ = pointer;
}

inline char* Environment::http_parser_buffer() const {
  return http_parser_buffer_;
}

inline void Environment::set_http_parser_buffer(char* buffer) {
  CHECK_NULL(http_parser_buffer_);  // Should be set only once.
  http_parser_buffer_ = buffer;
}

inline bool Environment::http_parser_buffer_in_use() const {
  return http_parser_buffer_in_use_;
}

inline void Environment::set_http_parser_buffer_in_use(bool in_use) {
  http_parser_buffer_in_use_ = in_use;
}

inline http2::Http2State* Environment::http2_state() const {
  return http2_state_.get();
}

inline void Environment::set_http2_state(
    std::unique_ptr<http2::Http2State> buffer) {
  CHECK(!http2_state_);  // Should be set only once.
  http2_state_ = std::move(buffer);
}

bool Environment::debug_enabled(DebugCategory category) const {
#ifdef DEBUG
  CHECK_GE(static_cast<int>(category), 0);
  CHECK_LT(static_cast<int>(category),
           static_cast<int>(DebugCategory::CATEGORY_COUNT));
#endif
  return debug_enabled_[static_cast<int>(category)];
}

void Environment::set_debug_enabled(DebugCategory category, bool enabled) {
#ifdef DEBUG
  CHECK_GE(static_cast<int>(category), 0);
  CHECK_LT(static_cast<int>(category),
           static_cast<int>(DebugCategory::CATEGORY_COUNT));
#endif
  debug_enabled_[static_cast<int>(category)] = enabled;
}

inline AliasedBuffer<double, v8::Float64Array>*
Environment::fs_stats_field_array() {
  return &fs_stats_field_array_;
}

inline AliasedBuffer<uint64_t, v8::BigUint64Array>*
Environment::fs_stats_field_bigint_array() {
  return &fs_stats_field_bigint_array_;
}

inline std::vector<std::unique_ptr<fs::FileHandleReadWrap>>&
Environment::file_handle_read_wrap_freelist() {
  return file_handle_read_wrap_freelist_;
}

inline std::shared_ptr<EnvironmentOptions> Environment::options() {
  return options_;
}

inline std::shared_ptr<PerIsolateOptions> IsolateData::options() {
  return options_;
}

void Environment::CreateImmediate(native_immediate_callback cb,
                                  void* data,
                                  v8::Local<v8::Object> obj,
                                  bool ref) {
  native_immediate_callbacks_.push_back({
    cb,
    data,
    std::unique_ptr<Persistent<v8::Object>>(obj.IsEmpty() ?
        nullptr : new Persistent<v8::Object>(isolate_, obj)),
    ref
  });
  immediate_info()->count_inc(1);
}

void Environment::SetImmediate(native_immediate_callback cb,
                               void* data,
                               v8::Local<v8::Object> obj) {
  CreateImmediate(cb, data, obj, true);

  if (immediate_info()->ref_count() == 0)
    ToggleImmediateRef(true);
  immediate_info()->ref_count_inc(1);
}

void Environment::SetUnrefImmediate(native_immediate_callback cb,
                                    void* data,
                                    v8::Local<v8::Object> obj) {
  CreateImmediate(cb, data, obj, false);
}

inline bool Environment::can_call_into_js() const {
  return can_call_into_js_ && (is_main_thread() || !is_stopping_worker());
}

inline void Environment::set_can_call_into_js(bool can_call_into_js) {
  can_call_into_js_ = can_call_into_js;
}

inline bool Environment::is_main_thread() const {
  return thread_id_ == 0;
}

inline uint64_t Environment::thread_id() const {
  return thread_id_;
}

inline void Environment::set_thread_id(uint64_t id) {
  thread_id_ = id;
}

inline worker::Worker* Environment::worker_context() const {
  return worker_context_;
}

inline void Environment::set_worker_context(worker::Worker* context) {
  CHECK_EQ(worker_context_, nullptr);  // Should be set only once.
  worker_context_ = context;
}

inline void Environment::add_sub_worker_context(worker::Worker* context) {
  sub_worker_contexts_.insert(context);
}

inline void Environment::remove_sub_worker_context(worker::Worker* context) {
  sub_worker_contexts_.erase(context);
}

inline bool Environment::is_stopping_worker() const {
  CHECK(!is_main_thread());
  return worker_context_->is_stopped();
}

inline performance::performance_state* Environment::performance_state() {
  return performance_state_.get();
}

inline std::unordered_map<std::string, uint64_t>*
    Environment::performance_marks() {
  return &performance_marks_;
}

inline IsolateData* Environment::isolate_data() const {
  return isolate_data_;
}

inline char* Environment::AllocateUnchecked(size_t size) {
  return static_cast<char*>(
      isolate_data()->allocator()->AllocateUninitialized(size));
}

inline char* Environment::Allocate(size_t size) {
  char* ret = AllocateUnchecked(size);
  CHECK_NE(ret, nullptr);
  return ret;
}

inline void Environment::Free(char* data, size_t size) {
  if (data != nullptr)
    isolate_data()->allocator()->Free(data, size);
}

inline AllocatedBuffer Environment::AllocateManaged(size_t size, bool checked) {
  char* data = checked ? Allocate(size) : AllocateUnchecked(size);
  if (data == nullptr) size = 0;
  return AllocatedBuffer(this, uv_buf_init(data, size));
}

inline AllocatedBuffer::AllocatedBuffer(Environment* env, uv_buf_t buf)
    : env_(env), buffer_(buf) {}

inline void AllocatedBuffer::Resize(size_t len) {
  char* new_data = env_->Reallocate(buffer_.base, buffer_.len, len);
  CHECK_IMPLIES(len > 0, new_data != nullptr);
  buffer_ = uv_buf_init(new_data, len);
}

inline uv_buf_t AllocatedBuffer::release() {
  uv_buf_t ret = buffer_;
  buffer_ = uv_buf_init(nullptr, 0);
  return ret;
}

inline char* AllocatedBuffer::data() {
  return buffer_.base;
}

inline const char* AllocatedBuffer::data() const {
  return buffer_.base;
}

inline size_t AllocatedBuffer::size() const {
  return buffer_.len;
}

inline AllocatedBuffer::AllocatedBuffer(Environment* env)
    : env_(env), buffer_(uv_buf_init(nullptr, 0)) {}

inline AllocatedBuffer::AllocatedBuffer(AllocatedBuffer&& other)
    : AllocatedBuffer() {
  *this = std::move(other);
}

inline AllocatedBuffer& AllocatedBuffer::operator=(AllocatedBuffer&& other) {
  clear();
  env_ = other.env_;
  buffer_ = other.release();
  return *this;
}

inline AllocatedBuffer::~AllocatedBuffer() {
  clear();
}

inline void AllocatedBuffer::clear() {
  uv_buf_t buf = release();
  env_->Free(buf.base, buf.len);
}

// It's a bit awkward to define this Buffer::New() overload here, but it
// avoids a circular dependency with node_internals.h.
namespace Buffer {
v8::MaybeLocal<v8::Object> New(Environment* env,
                               char* data,
                               size_t length,
                               bool uses_malloc);
}

inline v8::MaybeLocal<v8::Object> AllocatedBuffer::ToBuffer() {
  CHECK_NOT_NULL(env_);
  v8::MaybeLocal<v8::Object> obj = Buffer::New(env_, data(), size(), false);
  if (!obj.IsEmpty()) release();
  return obj;
}

inline v8::Local<v8::ArrayBuffer> AllocatedBuffer::ToArrayBuffer() {
  CHECK_NOT_NULL(env_);
  uv_buf_t buf = release();
  return v8::ArrayBuffer::New(env_->isolate(),
                              buf.base,
                              buf.len,
                              v8::ArrayBufferCreationMode::kInternalized);
}

inline void Environment::ThrowError(const char* errmsg) {
  ThrowError(v8::Exception::Error, errmsg);
}

inline void Environment::ThrowTypeError(const char* errmsg) {
  ThrowError(v8::Exception::TypeError, errmsg);
}

inline void Environment::ThrowRangeError(const char* errmsg) {
  ThrowError(v8::Exception::RangeError, errmsg);
}

inline void Environment::ThrowError(
    v8::Local<v8::Value> (*fun)(v8::Local<v8::String>),
    const char* errmsg) {
  v8::HandleScope handle_scope(isolate());
  isolate()->ThrowException(fun(OneByteString(isolate(), errmsg)));
}

inline void Environment::ThrowErrnoException(int errorno,
                                             const char* syscall,
                                             const char* message,
                                             const char* path) {
  isolate()->ThrowException(
      ErrnoException(isolate(), errorno, syscall, message, path));
}

inline void Environment::ThrowUVException(int errorno,
                                          const char* syscall,
                                          const char* message,
                                          const char* path,
                                          const char* dest) {
  isolate()->ThrowException(
      UVException(isolate(), errorno, syscall, message, path, dest));
}

inline v8::Local<v8::FunctionTemplate>
    Environment::NewFunctionTemplate(v8::FunctionCallback callback,
                                     v8::Local<v8::Signature> signature,
                                     v8::ConstructorBehavior behavior,
                                     v8::SideEffectType side_effect_type) {
  v8::Local<v8::External> external = as_external();
  return v8::FunctionTemplate::New(isolate(), callback, external,
                                   signature, 0, behavior, side_effect_type);
}

inline void Environment::SetMethod(v8::Local<v8::Object> that,
                                   const char* name,
                                   v8::FunctionCallback callback) {
  v8::Local<v8::Function> function =
      NewFunctionTemplate(callback,
                          v8::Local<v8::Signature>(),
                          // TODO(TimothyGu): Investigate if SetMethod is ever
                          // used for constructors.
                          v8::ConstructorBehavior::kAllow,
                          v8::SideEffectType::kHasSideEffect)->GetFunction();
  // kInternalized strings are created in the old space.
  const v8::NewStringType type = v8::NewStringType::kInternalized;
  v8::Local<v8::String> name_string =
      v8::String::NewFromUtf8(isolate(), name, type).ToLocalChecked();
  that->Set(name_string, function);
  function->SetName(name_string);  // NODE_SET_METHOD() compatibility.
}

inline void Environment::SetMethodNoSideEffect(v8::Local<v8::Object> that,
                                               const char* name,
                                               v8::FunctionCallback callback) {
  v8::Local<v8::Function> function =
      NewFunctionTemplate(callback,
                          v8::Local<v8::Signature>(),
                          // TODO(TimothyGu): Investigate if SetMethod is ever
                          // used for constructors.
                          v8::ConstructorBehavior::kAllow,
                          v8::SideEffectType::kHasNoSideEffect)->GetFunction();
  // kInternalized strings are created in the old space.
  const v8::NewStringType type = v8::NewStringType::kInternalized;
  v8::Local<v8::String> name_string =
      v8::String::NewFromUtf8(isolate(), name, type).ToLocalChecked();
  that->Set(name_string, function);
  function->SetName(name_string);  // NODE_SET_METHOD() compatibility.
}

inline void Environment::SetProtoMethod(v8::Local<v8::FunctionTemplate> that,
                                        const char* name,
                                        v8::FunctionCallback callback) {
  v8::Local<v8::Signature> signature = v8::Signature::New(isolate(), that);
  v8::Local<v8::FunctionTemplate> t =
      NewFunctionTemplate(callback, signature, v8::ConstructorBehavior::kThrow,
                          v8::SideEffectType::kHasSideEffect);
  // kInternalized strings are created in the old space.
  const v8::NewStringType type = v8::NewStringType::kInternalized;
  v8::Local<v8::String> name_string =
      v8::String::NewFromUtf8(isolate(), name, type).ToLocalChecked();
  that->PrototypeTemplate()->Set(name_string, t);
  t->SetClassName(name_string);  // NODE_SET_PROTOTYPE_METHOD() compatibility.
}

inline void Environment::SetProtoMethodNoSideEffect(
    v8::Local<v8::FunctionTemplate> that,
    const char* name,
    v8::FunctionCallback callback) {
  v8::Local<v8::Signature> signature = v8::Signature::New(isolate(), that);
  v8::Local<v8::FunctionTemplate> t =
      NewFunctionTemplate(callback, signature, v8::ConstructorBehavior::kThrow,
                          v8::SideEffectType::kHasNoSideEffect);
  // kInternalized strings are created in the old space.
  const v8::NewStringType type = v8::NewStringType::kInternalized;
  v8::Local<v8::String> name_string =
      v8::String::NewFromUtf8(isolate(), name, type).ToLocalChecked();
  that->PrototypeTemplate()->Set(name_string, t);
  t->SetClassName(name_string);  // NODE_SET_PROTOTYPE_METHOD() compatibility.
}

inline void Environment::SetTemplateMethod(v8::Local<v8::FunctionTemplate> that,
                                           const char* name,
                                           v8::FunctionCallback callback) {
  v8::Local<v8::FunctionTemplate> t =
      NewFunctionTemplate(callback, v8::Local<v8::Signature>(),
                          v8::ConstructorBehavior::kAllow,
                          v8::SideEffectType::kHasSideEffect);
  // kInternalized strings are created in the old space.
  const v8::NewStringType type = v8::NewStringType::kInternalized;
  v8::Local<v8::String> name_string =
      v8::String::NewFromUtf8(isolate(), name, type).ToLocalChecked();
  that->Set(name_string, t);
  t->SetClassName(name_string);  // NODE_SET_METHOD() compatibility.
}

inline void Environment::SetTemplateMethodNoSideEffect(
    v8::Local<v8::FunctionTemplate> that,
    const char* name,
    v8::FunctionCallback callback) {
  v8::Local<v8::FunctionTemplate> t =
      NewFunctionTemplate(callback, v8::Local<v8::Signature>(),
                          v8::ConstructorBehavior::kAllow,
                          v8::SideEffectType::kHasNoSideEffect);
  // kInternalized strings are created in the old space.
  const v8::NewStringType type = v8::NewStringType::kInternalized;
  v8::Local<v8::String> name_string =
      v8::String::NewFromUtf8(isolate(), name, type).ToLocalChecked();
  that->Set(name_string, t);
  t->SetClassName(name_string);  // NODE_SET_METHOD() compatibility.
}

void Environment::AddCleanupHook(void (*fn)(void*), void* arg) {
  auto insertion_info = cleanup_hooks_.emplace(CleanupHookCallback {
    fn, arg, cleanup_hook_counter_++
  });
  // Make sure there was no existing element with these values.
  CHECK_EQ(insertion_info.second, true);
}

void Environment::RemoveCleanupHook(void (*fn)(void*), void* arg) {
  CleanupHookCallback search { fn, arg, 0 };
  cleanup_hooks_.erase(search);
}

size_t Environment::CleanupHookCallback::Hash::operator()(
    const CleanupHookCallback& cb) const {
  return std::hash<void*>()(cb.arg_);
}

bool Environment::CleanupHookCallback::Equal::operator()(
    const CleanupHookCallback& a, const CleanupHookCallback& b) const {
  return a.fn_ == b.fn_ && a.arg_ == b.arg_;
}

BaseObject* Environment::CleanupHookCallback::GetBaseObject() const {
  if (fn_ == BaseObject::DeleteMe)
    return static_cast<BaseObject*>(arg_);
  else
    return nullptr;
}

template <typename T>
void Environment::ForEachBaseObject(T&& iterator) {
  for (const auto& hook : cleanup_hooks_) {
    BaseObject* obj = hook.GetBaseObject();
    if (obj != nullptr)
      iterator(obj);
  }
}

#define VP(PropertyName, StringValue) V(v8::Private, PropertyName)
#define VY(PropertyName, StringValue) V(v8::Symbol, PropertyName)
#define VS(PropertyName, StringValue) V(v8::String, PropertyName)
#define V(TypeName, PropertyName)                                             \
  inline                                                                      \
  v8::Local<TypeName> IsolateData::PropertyName(v8::Isolate* isolate) const { \
    /* Strings are immutable so casting away const-ness here is okay. */      \
    return const_cast<IsolateData*>(this)->PropertyName ## _.Get(isolate);    \
  }
  PER_ISOLATE_PRIVATE_SYMBOL_PROPERTIES(VP)
  PER_ISOLATE_SYMBOL_PROPERTIES(VY)
  PER_ISOLATE_STRING_PROPERTIES(VS)
#undef V
#undef VS
#undef VY
#undef VP

#define VP(PropertyName, StringValue) V(v8::Private, PropertyName)
#define VY(PropertyName, StringValue) V(v8::Symbol, PropertyName)
#define VS(PropertyName, StringValue) V(v8::String, PropertyName)
#define V(TypeName, PropertyName)                                             \
  inline v8::Local<TypeName> Environment::PropertyName() const {              \
    return isolate_data()->PropertyName(isolate());                           \
  }
  PER_ISOLATE_PRIVATE_SYMBOL_PROPERTIES(VP)
  PER_ISOLATE_SYMBOL_PROPERTIES(VY)
  PER_ISOLATE_STRING_PROPERTIES(VS)
#undef V
#undef VS
#undef VY
#undef VP

#define V(PropertyName, TypeName)                                             \
  inline v8::Local<TypeName> Environment::PropertyName() const {              \
    return StrongPersistentToLocal(PropertyName ## _);                        \
  }                                                                           \
  inline void Environment::set_ ## PropertyName(v8::Local<TypeName> value) {  \
    PropertyName ## _.Reset(isolate(), value);                                \
  }
  ENVIRONMENT_STRONG_PERSISTENT_PROPERTIES(V)
#undef V

}  // namespace node

#endif  // defined(NODE_WANT_INTERNALS) && NODE_WANT_INTERNALS

#endif  // SRC_ENV_INL_H_
