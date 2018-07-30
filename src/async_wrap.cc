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

#include "async_wrap-inl.h"
#include "env-inl.h"
#include "node_internals.h"
#include "util-inl.h"

#include "v8.h"
#include "v8-profiler.h"
#include "tracing/trace_event.h"

using v8::Context;
using v8::Function;
using v8::FunctionCallbackInfo;
using v8::FunctionTemplate;
using v8::HandleScope;
using v8::HeapProfiler;
using v8::Integer;
using v8::Isolate;
using v8::Local;
using v8::MaybeLocal;
using v8::Number;
using v8::Object;
using v8::ObjectTemplate;
using v8::Promise;
using v8::PromiseHookType;
using v8::PropertyCallbackInfo;
using v8::RetainedObjectInfo;
using v8::String;
using v8::Uint32;
using v8::Undefined;
using v8::Value;

using AsyncHooks = node::Environment::AsyncHooks;

namespace node {

static const char* const provider_names[] = {
#define V(PROVIDER)                                                           \
  #PROVIDER,
  NODE_ASYNC_PROVIDER_TYPES(V)
#undef V
};


// Report correct information in a heapdump.

class RetainedAsyncInfo: public RetainedObjectInfo {
 public:
  explicit RetainedAsyncInfo(uint16_t class_id, AsyncWrap* wrap);

  void Dispose() override;
  bool IsEquivalent(RetainedObjectInfo* other) override;
  intptr_t GetHash() override;
  const char* GetLabel() override;
  intptr_t GetSizeInBytes() override;

 private:
  const char* label_;
  const AsyncWrap* wrap_;
  const int length_;
};


RetainedAsyncInfo::RetainedAsyncInfo(uint16_t class_id, AsyncWrap* wrap)
    : label_(provider_names[class_id - NODE_ASYNC_ID_OFFSET]),
      wrap_(wrap),
      length_(wrap->self_size()) {
}


void RetainedAsyncInfo::Dispose() {
  delete this;
}


bool RetainedAsyncInfo::IsEquivalent(RetainedObjectInfo* other) {
  return label_ == other->GetLabel() &&
          wrap_ == static_cast<RetainedAsyncInfo*>(other)->wrap_;
}


intptr_t RetainedAsyncInfo::GetHash() {
  return reinterpret_cast<intptr_t>(wrap_);
}


const char* RetainedAsyncInfo::GetLabel() {
  return label_;
}


intptr_t RetainedAsyncInfo::GetSizeInBytes() {
  return length_;
}


RetainedObjectInfo* WrapperInfo(uint16_t class_id, Local<Value> wrapper) {
  // No class_id should be the provider type of NONE.
  CHECK_GT(class_id, NODE_ASYNC_ID_OFFSET);
  // And make sure the class_id doesn't extend past the last provider.
  CHECK_LE(class_id - NODE_ASYNC_ID_OFFSET, AsyncWrap::PROVIDERS_LENGTH);
  CHECK(wrapper->IsObject());
  CHECK(!wrapper.IsEmpty());

  Local<Object> object = wrapper.As<Object>();
  CHECK_GT(object->InternalFieldCount(), 0);

  AsyncWrap* wrap;
  ASSIGN_OR_RETURN_UNWRAP(&wrap, object, nullptr);

  return new RetainedAsyncInfo(class_id, wrap);
}


// end RetainedAsyncInfo


struct AsyncWrapObject : public AsyncWrap {
  static inline void New(const FunctionCallbackInfo<Value>& args) {
    Environment* env = Environment::GetCurrent(args);
    CHECK(args.IsConstructCall());
    CHECK(env->async_wrap_constructor_template()->HasInstance(args.This()));
    CHECK(args[0]->IsUint32());
    auto type = static_cast<ProviderType>(args[0].As<Uint32>()->Value());
    new AsyncWrapObject(env, args.This(), type);
  }

  inline AsyncWrapObject(Environment* env, Local<Object> object,
                         ProviderType type) : AsyncWrap(env, object, type) {}

  inline size_t self_size() const override { return sizeof(*this); }
};


static void DestroyAsyncIdsCallback(Environment* env, void* data) {
  Local<Function> fn = env->async_hooks_destroy_function();

  FatalTryCatch try_catch(env);

  do {
    std::vector<double> destroy_async_id_list;
    destroy_async_id_list.swap(*env->destroy_async_id_list());
    if (!env->can_call_into_js()) return;
    for (auto async_id : destroy_async_id_list) {
      // Want each callback to be cleaned up after itself, instead of cleaning
      // them all up after the while() loop completes.
      HandleScope scope(env->isolate());
      Local<Value> async_id_value = Number::New(env->isolate(), async_id);
      MaybeLocal<Value> ret = fn->Call(
          env->context(), Undefined(env->isolate()), 1, &async_id_value);

      if (ret.IsEmpty())
        return;
    }
  } while (!env->destroy_async_id_list()->empty());
}

static void DestroyAsyncIdsCallback(void* arg) {
  Environment* env = static_cast<Environment*>(arg);
  if (!env->destroy_async_id_list()->empty())
    DestroyAsyncIdsCallback(env, nullptr);
}


void Emit(Environment* env, double async_id, AsyncHooks::Fields type,
          Local<Function> fn) {
  AsyncHooks* async_hooks = env->async_hooks();

  if (async_hooks->fields()[type] == 0 || !env->can_call_into_js())
    return;

  v8::HandleScope handle_scope(env->isolate());
  Local<Value> async_id_value = Number::New(env->isolate(), async_id);
  FatalTryCatch try_catch(env);
  USE(fn->Call(env->context(), Undefined(env->isolate()), 1, &async_id_value));
}


void AsyncWrap::EmitPromiseResolve(Environment* env, double async_id) {
  Emit(env, async_id, AsyncHooks::kPromiseResolve,
       env->async_hooks_promise_resolve_function());
}


void AsyncWrap::EmitTraceEventBefore() {
  switch (provider_type()) {
#define V(PROVIDER)                                                           \
    case PROVIDER_ ## PROVIDER:                                               \
      TRACE_EVENT_NESTABLE_ASYNC_BEGIN0(                                      \
        TRACING_CATEGORY_NODE1(async_hooks),                                  \
        #PROVIDER "_CALLBACK", static_cast<int64_t>(get_async_id()));         \
      break;
    NODE_ASYNC_PROVIDER_TYPES(V)
#undef V
    default:
      UNREACHABLE();
  }
}


void AsyncWrap::EmitBefore(Environment* env, double async_id) {
  Emit(env, async_id, AsyncHooks::kBefore,
       env->async_hooks_before_function());
}


void AsyncWrap::EmitTraceEventAfter(ProviderType type, double async_id) {
  switch (type) {
#define V(PROVIDER)                                                           \
    case PROVIDER_ ## PROVIDER:                                               \
      TRACE_EVENT_NESTABLE_ASYNC_END0(                                        \
        TRACING_CATEGORY_NODE1(async_hooks),                                  \
        #PROVIDER "_CALLBACK", static_cast<int64_t>(async_id));               \
      break;
    NODE_ASYNC_PROVIDER_TYPES(V)
#undef V
    default:
      UNREACHABLE();
  }
}


void AsyncWrap::EmitAfter(Environment* env, double async_id) {
  // If the user's callback failed then the after() hooks will be called at the
  // end of _fatalException().
  Emit(env, async_id, AsyncHooks::kAfter,
       env->async_hooks_after_function());
}

class PromiseWrap : public AsyncWrap {
 public:
  PromiseWrap(Environment* env, Local<Object> object, bool silent)
      : AsyncWrap(env, object, PROVIDER_PROMISE, -1, silent) {
    MakeWeak();
  }
  size_t self_size() const override { return sizeof(*this); }

  static constexpr int kPromiseField = 1;
  static constexpr int kIsChainedPromiseField = 2;
  static constexpr int kInternalFieldCount = 3;

  static PromiseWrap* New(Environment* env,
                          Local<Promise> promise,
                          PromiseWrap* parent_wrap,
                          bool silent);
  static void GetPromise(Local<String> property,
                         const PropertyCallbackInfo<Value>& info);
  static void getIsChainedPromise(Local<String> property,
                                  const PropertyCallbackInfo<Value>& info);
};

PromiseWrap* PromiseWrap::New(Environment* env,
                              Local<Promise> promise,
                              PromiseWrap* parent_wrap,
                              bool silent) {
  Local<Object> object = env->promise_wrap_template()
                            ->NewInstance(env->context()).ToLocalChecked();
  object->SetInternalField(PromiseWrap::kPromiseField, promise);
  object->SetInternalField(PromiseWrap::kIsChainedPromiseField,
                           parent_wrap != nullptr ?
                              v8::True(env->isolate()) :
                              v8::False(env->isolate()));
  CHECK_EQ(promise->GetAlignedPointerFromInternalField(0), nullptr);
  promise->SetInternalField(0, object);
  return new PromiseWrap(env, object, silent);
}

void PromiseWrap::GetPromise(Local<String> property,
                             const PropertyCallbackInfo<Value>& info) {
  info.GetReturnValue().Set(info.Holder()->GetInternalField(kPromiseField));
}

void PromiseWrap::getIsChainedPromise(Local<String> property,
                                      const PropertyCallbackInfo<Value>& info) {
  info.GetReturnValue().Set(
    info.Holder()->GetInternalField(kIsChainedPromiseField));
}

static PromiseWrap* extractPromiseWrap(Local<Promise> promise) {
  Local<Value> resource_object_value = promise->GetInternalField(0);
  if (resource_object_value->IsObject()) {
    return Unwrap<PromiseWrap>(resource_object_value.As<Object>());
  }
  return nullptr;
}

static void PromiseHook(PromiseHookType type, Local<Promise> promise,
                        Local<Value> parent, void* arg) {
  Environment* env = static_cast<Environment*>(arg);
  PromiseWrap* wrap = extractPromiseWrap(promise);
  if (type == PromiseHookType::kInit || wrap == nullptr) {
    bool silent = type != PromiseHookType::kInit;

    // set parent promise's async Id as this promise's triggerAsyncId
    if (parent->IsPromise()) {
      // parent promise exists, current promise
      // is a chained promise, so we set parent promise's id as
      // current promise's triggerAsyncId
      Local<Promise> parent_promise = parent.As<Promise>();
      PromiseWrap* parent_wrap = extractPromiseWrap(parent_promise);
      if (parent_wrap == nullptr) {
        parent_wrap = PromiseWrap::New(env, parent_promise, nullptr, true);
      }

      AsyncHooks::DefaultTriggerAsyncIdScope trigger_scope(parent_wrap);
      wrap = PromiseWrap::New(env, promise, parent_wrap, silent);
    } else {
      wrap = PromiseWrap::New(env, promise, nullptr, silent);
    }
  }

  CHECK_NOT_NULL(wrap);
  if (type == PromiseHookType::kBefore) {
    env->async_hooks()->push_async_ids(
      wrap->get_async_id(), wrap->get_trigger_async_id());
    wrap->EmitTraceEventBefore();
    AsyncWrap::EmitBefore(wrap->env(), wrap->get_async_id());
  } else if (type == PromiseHookType::kAfter) {
    wrap->EmitTraceEventAfter(wrap->provider_type(), wrap->get_async_id());
    AsyncWrap::EmitAfter(wrap->env(), wrap->get_async_id());
    if (env->execution_async_id() == wrap->get_async_id()) {
      // This condition might not be true if async_hooks was enabled during
      // the promise callback execution.
      // Popping it off the stack can be skipped in that case, because it is
      // known that it would correspond to exactly one call with
      // PromiseHookType::kBefore that was not witnessed by the PromiseHook.
      env->async_hooks()->pop_async_id(wrap->get_async_id());
    }
  } else if (type == PromiseHookType::kResolve) {
    AsyncWrap::EmitPromiseResolve(wrap->env(), wrap->get_async_id());
  }
}


static void SetupHooks(const FunctionCallbackInfo<Value>& args) {
  Environment* env = Environment::GetCurrent(args);

  CHECK(args[0]->IsObject());

  // All of init, before, after, destroy are supplied by async_hooks
  // internally, so this should every only be called once. At which time all
  // the functions should be set. Detect this by checking if init !IsEmpty().
  CHECK(env->async_hooks_init_function().IsEmpty());

  Local<Object> fn_obj = args[0].As<Object>();

#define SET_HOOK_FN(name)                                                     \
  Local<Value> name##_v = fn_obj->Get(                                        \
      env->context(),                                                         \
      FIXED_ONE_BYTE_STRING(env->isolate(), #name)).ToLocalChecked();         \
  CHECK(name##_v->IsFunction());                                              \
  env->set_async_hooks_##name##_function(name##_v.As<Function>());

  SET_HOOK_FN(init);
  SET_HOOK_FN(before);
  SET_HOOK_FN(after);
  SET_HOOK_FN(destroy);
  SET_HOOK_FN(promise_resolve);
#undef SET_HOOK_FN

  {
    Local<FunctionTemplate> ctor =
        FunctionTemplate::New(env->isolate());
    ctor->SetClassName(FIXED_ONE_BYTE_STRING(env->isolate(), "PromiseWrap"));
    Local<ObjectTemplate> promise_wrap_template = ctor->InstanceTemplate();
    promise_wrap_template->SetInternalFieldCount(
        PromiseWrap::kInternalFieldCount);
    promise_wrap_template->SetAccessor(
        FIXED_ONE_BYTE_STRING(env->isolate(), "promise"),
        PromiseWrap::GetPromise);
    promise_wrap_template->SetAccessor(
        FIXED_ONE_BYTE_STRING(env->isolate(), "isChainedPromise"),
        PromiseWrap::getIsChainedPromise);
    env->set_promise_wrap_template(promise_wrap_template);
  }
}


static void EnablePromiseHook(const FunctionCallbackInfo<Value>& args) {
  Environment* env = Environment::GetCurrent(args);
  env->AddPromiseHook(PromiseHook, static_cast<void*>(env));
}


static void DisablePromiseHook(const FunctionCallbackInfo<Value>& args) {
  Environment* env = Environment::GetCurrent(args);

  // Delay the call to `RemovePromiseHook` because we might currently be
  // between the `before` and `after` calls of a Promise.
  env->isolate()->EnqueueMicrotask([](void* data) {
    Environment* env = static_cast<Environment*>(data);
    env->RemovePromiseHook(PromiseHook, data);
  }, static_cast<void*>(env));
}


class DestroyParam {
 public:
  double asyncId;
  Environment* env;
  Persistent<Object> target;
  Persistent<Object> propBag;
};


void AsyncWrap::WeakCallback(const v8::WeakCallbackInfo<DestroyParam>& info) {
  HandleScope scope(info.GetIsolate());

  std::unique_ptr<DestroyParam> p{info.GetParameter()};
  Local<Object> prop_bag = PersistentToLocal(info.GetIsolate(), p->propBag);

  Local<Value> val = prop_bag->Get(p->env->destroyed_string());
  if (val->IsFalse()) {
    AsyncWrap::EmitDestroy(p->env, p->asyncId);
  }
  // unique_ptr goes out of scope here and pointer is deleted.
}


static void RegisterDestroyHook(const FunctionCallbackInfo<Value>& args) {
  CHECK(args[0]->IsObject());
  CHECK(args[1]->IsNumber());
  CHECK(args[2]->IsObject());

  Isolate* isolate = args.GetIsolate();
  DestroyParam* p = new DestroyParam();
  p->asyncId = args[1].As<Number>()->Value();
  p->env = Environment::GetCurrent(args);
  p->target.Reset(isolate, args[0].As<Object>());
  p->propBag.Reset(isolate, args[2].As<Object>());
  p->target.SetWeak(
    p, AsyncWrap::WeakCallback, v8::WeakCallbackType::kParameter);
}


void AsyncWrap::GetAsyncId(const FunctionCallbackInfo<Value>& args) {
  AsyncWrap* wrap;
  args.GetReturnValue().Set(-1);
  ASSIGN_OR_RETURN_UNWRAP(&wrap, args.Holder());
  args.GetReturnValue().Set(wrap->get_async_id());
}


void AsyncWrap::PushAsyncIds(const FunctionCallbackInfo<Value>& args) {
  Environment* env = Environment::GetCurrent(args);
  // No need for CHECK(IsNumber()) on args because if FromJust() doesn't fail
  // then the checks in push_async_ids() and pop_async_id() will.
  double async_id = args[0]->NumberValue(env->context()).FromJust();
  double trigger_async_id = args[1]->NumberValue(env->context()).FromJust();
  env->async_hooks()->push_async_ids(async_id, trigger_async_id);
}


void AsyncWrap::PopAsyncIds(const FunctionCallbackInfo<Value>& args) {
  Environment* env = Environment::GetCurrent(args);
  double async_id = args[0]->NumberValue(env->context()).FromJust();
  args.GetReturnValue().Set(env->async_hooks()->pop_async_id(async_id));
}


void AsyncWrap::AsyncReset(const FunctionCallbackInfo<Value>& args) {
  AsyncWrap* wrap;
  ASSIGN_OR_RETURN_UNWRAP(&wrap, args.Holder());
  double execution_async_id = args[0]->IsNumber() ? args[0]->NumberValue() : -1;
  wrap->AsyncReset(execution_async_id);
}


void AsyncWrap::QueueDestroyAsyncId(const FunctionCallbackInfo<Value>& args) {
  CHECK(args[0]->IsNumber());
  AsyncWrap::EmitDestroy(
      Environment::GetCurrent(args), args[0]->NumberValue());
}

void AsyncWrap::AddWrapMethods(Environment* env,
                               Local<FunctionTemplate> constructor,
                               int flag) {
  env->SetProtoMethod(constructor, "getAsyncId", AsyncWrap::GetAsyncId);
  if (flag & kFlagHasReset)
    env->SetProtoMethod(constructor, "asyncReset", AsyncWrap::AsyncReset);
}

void AsyncWrap::Initialize(Local<Object> target,
                           Local<Value> unused,
                           Local<Context> context) {
  Environment* env = Environment::GetCurrent(context);
  Isolate* isolate = env->isolate();
  HandleScope scope(isolate);

  env->BeforeExit(DestroyAsyncIdsCallback, env);

  env->SetMethod(target, "setupHooks", SetupHooks);
  env->SetMethod(target, "pushAsyncIds", PushAsyncIds);
  env->SetMethod(target, "popAsyncIds", PopAsyncIds);
  env->SetMethod(target, "queueDestroyAsyncId", QueueDestroyAsyncId);
  env->SetMethod(target, "enablePromiseHook", EnablePromiseHook);
  env->SetMethod(target, "disablePromiseHook", DisablePromiseHook);
  env->SetMethod(target, "registerDestroyHook", RegisterDestroyHook);

  v8::PropertyAttribute ReadOnlyDontDelete =
      static_cast<v8::PropertyAttribute>(v8::ReadOnly | v8::DontDelete);

#define FORCE_SET_TARGET_FIELD(obj, str, field)                               \
  (obj)->DefineOwnProperty(context,                                           \
                           FIXED_ONE_BYTE_STRING(isolate, str),               \
                           field,                                             \
                           ReadOnlyDontDelete).FromJust()

  // Attach the uint32_t[] where each slot contains the count of the number of
  // callbacks waiting to be called on a particular event. It can then be
  // incremented/decremented from JS quickly to communicate to C++ if there are
  // any callbacks waiting to be called.
  FORCE_SET_TARGET_FIELD(target,
                         "async_hook_fields",
                         env->async_hooks()->fields().GetJSArray());

  // The following v8::Float64Array has 5 fields. These fields are shared in
  // this way to allow JS and C++ to read/write each value as quickly as
  // possible. The fields are represented as follows:
  //
  // kAsyncIdCounter: Maintains the state of the next unique id to be assigned.
  //
  // kDefaultTriggerAsyncId: Write the id of the resource responsible for a
  //   handle's creation just before calling the new handle's constructor.
  //   After the new handle is constructed kDefaultTriggerAsyncId is set back
  //   to -1.
  FORCE_SET_TARGET_FIELD(target,
                         "async_id_fields",
                         env->async_hooks()->async_id_fields().GetJSArray());

  target->Set(context,
              env->async_ids_stack_string(),
              env->async_hooks()->async_ids_stack().GetJSArray()).FromJust();

  Local<Object> constants = Object::New(isolate);
#define SET_HOOKS_CONSTANT(name)                                              \
  FORCE_SET_TARGET_FIELD(                                                     \
      constants, #name, Integer::New(isolate, AsyncHooks::name));

  SET_HOOKS_CONSTANT(kInit);
  SET_HOOKS_CONSTANT(kBefore);
  SET_HOOKS_CONSTANT(kAfter);
  SET_HOOKS_CONSTANT(kDestroy);
  SET_HOOKS_CONSTANT(kPromiseResolve);
  SET_HOOKS_CONSTANT(kTotals);
  SET_HOOKS_CONSTANT(kCheck);
  SET_HOOKS_CONSTANT(kExecutionAsyncId);
  SET_HOOKS_CONSTANT(kTriggerAsyncId);
  SET_HOOKS_CONSTANT(kAsyncIdCounter);
  SET_HOOKS_CONSTANT(kDefaultTriggerAsyncId);
  SET_HOOKS_CONSTANT(kStackLength);
#undef SET_HOOKS_CONSTANT
  FORCE_SET_TARGET_FIELD(target, "constants", constants);

  Local<Object> async_providers = Object::New(isolate);
#define V(p)                                                                  \
  FORCE_SET_TARGET_FIELD(                                                     \
      async_providers, #p, Integer::New(isolate, AsyncWrap::PROVIDER_ ## p));
  NODE_ASYNC_PROVIDER_TYPES(V)
#undef V
  FORCE_SET_TARGET_FIELD(target, "Providers", async_providers);

#undef FORCE_SET_TARGET_FIELD

  env->set_async_hooks_init_function(Local<Function>());
  env->set_async_hooks_before_function(Local<Function>());
  env->set_async_hooks_after_function(Local<Function>());
  env->set_async_hooks_destroy_function(Local<Function>());
  env->set_async_hooks_promise_resolve_function(Local<Function>());
  env->set_async_hooks_binding(target);

  {
    auto class_name = FIXED_ONE_BYTE_STRING(env->isolate(), "AsyncWrap");
    auto function_template = env->NewFunctionTemplate(AsyncWrapObject::New);
    function_template->SetClassName(class_name);
    AsyncWrap::AddWrapMethods(env, function_template);
    auto instance_template = function_template->InstanceTemplate();
    instance_template->SetInternalFieldCount(1);
    auto function =
        function_template->GetFunction(env->context()).ToLocalChecked();
    target->Set(env->context(), class_name, function).FromJust();
    env->set_async_wrap_constructor_template(function_template);
  }
}


void LoadAsyncWrapperInfo(Environment* env) {
  HeapProfiler* heap_profiler = env->isolate()->GetHeapProfiler();
#if 0
#define V(PROVIDER)                                                           \
  heap_profiler->SetWrapperClassInfoProvider(                                 \
      (NODE_ASYNC_ID_OFFSET + AsyncWrap::PROVIDER_ ## PROVIDER), WrapperInfo);
  NODE_ASYNC_PROVIDER_TYPES(V)
#undef V
#endif
}


AsyncWrap::AsyncWrap(Environment* env,
                     Local<Object> object,
                     ProviderType provider,
                     double execution_async_id)
    : AsyncWrap(env, object, provider, execution_async_id, false) {}

AsyncWrap::AsyncWrap(Environment* env,
                     Local<Object> object,
                     ProviderType provider,
                     double execution_async_id,
                     bool silent)
    : BaseObject(env, object),
      provider_type_(provider) {
  CHECK_NE(provider, PROVIDER_NONE);
  CHECK_GE(object->InternalFieldCount(), 1);

  // Shift provider value over to prevent id collision.
  persistent().SetWrapperClassId(NODE_ASYNC_ID_OFFSET + provider_type_);

  // Use AsyncReset() call to execute the init() callbacks.
  AsyncReset(execution_async_id, silent);
}


AsyncWrap::~AsyncWrap() {
  EmitTraceEventDestroy();
  EmitDestroy(env(), get_async_id());
}

void AsyncWrap::EmitTraceEventDestroy() {
  switch (provider_type()) {
  #define V(PROVIDER)                                                         \
    case PROVIDER_ ## PROVIDER:                                               \
      TRACE_EVENT_NESTABLE_ASYNC_END0(                                        \
        TRACING_CATEGORY_NODE1(async_hooks),                                  \
        #PROVIDER, static_cast<int64_t>(get_async_id()));                     \
      break;
    NODE_ASYNC_PROVIDER_TYPES(V)
  #undef V
    default:
      UNREACHABLE();
  }
}

void AsyncWrap::EmitDestroy(Environment* env, double async_id) {
  if (env->async_hooks()->fields()[AsyncHooks::kDestroy] == 0 ||
      !env->can_call_into_js()) {
    return;
  }

  if (env->destroy_async_id_list()->empty()) {
    env->SetUnrefImmediate(DestroyAsyncIdsCallback, nullptr);
  }

  env->destroy_async_id_list()->push_back(async_id);
}


// Generalized call for both the constructor and for handles that are pooled
// and reused over their lifetime. This way a new uid can be assigned when
// the resource is pulled out of the pool and put back into use.
void AsyncWrap::AsyncReset(double execution_async_id, bool silent) {
  async_id_ =
    execution_async_id == -1 ? env()->new_async_id() : execution_async_id;
  trigger_async_id_ = env()->get_default_trigger_async_id();

  switch (provider_type()) {
#define V(PROVIDER)                                                           \
    case PROVIDER_ ## PROVIDER:                                               \
      TRACE_EVENT_NESTABLE_ASYNC_BEGIN2(                                      \
        TRACING_CATEGORY_NODE1(async_hooks),                                  \
        #PROVIDER, static_cast<int64_t>(get_async_id()),                      \
        "executionAsyncId",                                                   \
        static_cast<int64_t>(env()->execution_async_id()),                    \
        "triggerAsyncId",                                                     \
        static_cast<int64_t>(get_trigger_async_id()));                        \
      break;
    NODE_ASYNC_PROVIDER_TYPES(V)
#undef V
    default:
      UNREACHABLE();
  }

  if (silent) return;

  EmitAsyncInit(env(), object(),
                env()->async_hooks()->provider_string(provider_type()),
                async_id_, trigger_async_id_);
}


void AsyncWrap::EmitAsyncInit(Environment* env,
                              Local<Object> object,
                              Local<String> type,
                              double async_id,
                              double trigger_async_id) {
  CHECK(!object.IsEmpty());
  CHECK(!type.IsEmpty());
  AsyncHooks* async_hooks = env->async_hooks();

  // Nothing to execute, so can continue normally.
  if (async_hooks->fields()[AsyncHooks::kInit] == 0) {
    return;
  }

  HandleScope scope(env->isolate());
  Local<Function> init_fn = env->async_hooks_init_function();

  Local<Value> argv[] = {
    Number::New(env->isolate(), async_id),
    type,
    Number::New(env->isolate(), trigger_async_id),
    object,
  };

  FatalTryCatch try_catch(env);
  USE(init_fn->Call(env->context(), object, arraysize(argv), argv));
}


MaybeLocal<Value> AsyncWrap::MakeCallback(const Local<Function> cb,
                                          int argc,
                                          Local<Value>* argv) {
  EmitTraceEventBefore();

  ProviderType provider = provider_type();
  async_context context { get_async_id(), get_trigger_async_id() };
  MaybeLocal<Value> ret = InternalMakeCallback(
      env(), object(), cb, argc, argv, context);

  // This is a static call with cached values because the `this` object may
  // no longer be alive at this point.
  EmitTraceEventAfter(provider, context.async_id);

  return ret;
}


/* Public C++ embedder API */


async_id AsyncHooksGetExecutionAsyncId(Isolate* isolate) {
  return Environment::GetCurrent(isolate)->execution_async_id();
}


async_id AsyncHooksGetTriggerAsyncId(Isolate* isolate) {
  return Environment::GetCurrent(isolate)->trigger_async_id();
}


async_context EmitAsyncInit(Isolate* isolate,
                            Local<Object> resource,
                            const char* name,
                            async_id trigger_async_id) {
  Local<String> type =
      String::NewFromUtf8(isolate, name, v8::NewStringType::kInternalized)
          .ToLocalChecked();
  return EmitAsyncInit(isolate, resource, type, trigger_async_id);
}

async_context EmitAsyncInit(Isolate* isolate,
                            Local<Object> resource,
                            v8::Local<v8::String> name,
                            async_id trigger_async_id) {
  Environment* env = Environment::GetCurrent(isolate);

  // Initialize async context struct
  if (trigger_async_id == -1)
    trigger_async_id = env->get_default_trigger_async_id();

  async_context context = {
    env->new_async_id(),  // async_id_
    trigger_async_id  // trigger_async_id_
  };

  // Run init hooks
  AsyncWrap::EmitAsyncInit(env, resource, name, context.async_id,
                           context.trigger_async_id);

  return context;
}

void EmitAsyncDestroy(Isolate* isolate, async_context asyncContext) {
  AsyncWrap::EmitDestroy(
      Environment::GetCurrent(isolate), asyncContext.async_id);
}

std::string AsyncWrap::diagnostic_name() const {
  return std::string(provider_names[provider_type()]) +
      " (" + std::to_string(env()->thread_id()) + ":" +
      std::to_string(static_cast<int64_t>(async_id_)) + ")";
}

}  // namespace node

NODE_BUILTIN_MODULE_CONTEXT_AWARE(async_wrap, node::AsyncWrap::Initialize)
