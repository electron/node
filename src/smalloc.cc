#include "smalloc.h"

#include "env.h"
#include "env-inl.h"
#include "node.h"
#include "node_internals.h"
#include "v8-profiler.h"
#include "v8.h"

#include <string.h>

#define ALLOC_ID (0xA10C)
#define EXTERNAL_ARRAY_TYPES(V)                                               \
  V(Int8, kExternalInt8Array)                                                 \
  V(Uint8, kExternalUint8Array)                                               \
  V(Int16, kExternalInt16Array)                                               \
  V(Uint16, kExternalUint16Array)                                             \
  V(Int32, kExternalInt32Array)                                               \
  V(Uint32, kExternalUint32Array)                                             \
  V(Float, kExternalFloat32Array)                                             \
  V(Double, kExternalFloat64Array)                                            \
  V(Uint8Clamped, kExternalUint8ClampedArray)

#define MIN(a, b) ((a) < (b) ? (a) : (b))
#define MAX(a, b) ((a) > (b) ? (a) : (b))


namespace node {
namespace smalloc {

using v8::Context;
using v8::External;
using v8::ExternalArrayType;
using v8::FunctionCallbackInfo;
using v8::Handle;
using v8::HandleScope;
using v8::HeapProfiler;
using v8::Isolate;
using v8::Local;
using v8::Object;
using v8::Persistent;
using v8::RetainedObjectInfo;
using v8::Uint32;
using v8::Value;
using v8::WeakCallbackData;
using v8::kExternalUint8Array;


class CallbackInfo {
 public:
  enum Ownership {
    kInternal,
    kExternal
  };

  static inline void Free(char* data, void* hint);
  static inline CallbackInfo* New(Isolate* isolate,
                                  Ownership ownership,
                                  Handle<Object> object,
                                  FreeCallback callback,
                                  void* hint = 0);
  inline void Dispose(Isolate* isolate);
  inline Persistent<Object>* persistent();
 private:
  static void WeakCallback(const WeakCallbackData<Object, CallbackInfo>&);
  inline void WeakCallback(Isolate* isolate, Local<Object> object);
  inline CallbackInfo(Isolate* isolate,
                      Ownership ownership,
                      Handle<Object> object,
                      FreeCallback callback,
                      void* hint);
  ~CallbackInfo();
  const Ownership ownership_;
  Persistent<Object> persistent_;
  FreeCallback const callback_;
  void* const hint_;
  DISALLOW_COPY_AND_ASSIGN(CallbackInfo);
};


void CallbackInfo::Free(char* data, void*) {
  ::free(data);
}


CallbackInfo* CallbackInfo::New(Isolate* isolate,
                                CallbackInfo::Ownership ownership,
                                Handle<Object> object,
                                FreeCallback callback,
                                void* hint) {
  return new CallbackInfo(isolate, ownership, object, callback, hint);
}


void CallbackInfo::Dispose(Isolate* isolate) {
  WeakCallback(isolate, PersistentToLocal(isolate, persistent_));
}


Persistent<Object>* CallbackInfo::persistent() {
  return &persistent_;
}


CallbackInfo::CallbackInfo(Isolate* isolate,
                           CallbackInfo::Ownership ownership,
                           Handle<Object> object,
                           FreeCallback callback,
                           void* hint)
    : ownership_(ownership),
      persistent_(isolate, object),
      callback_(callback),
      hint_(hint) {
  persistent_.SetWeak(this, WeakCallback);
  persistent_.SetWrapperClassId(ALLOC_ID);
  persistent_.MarkIndependent();
  isolate->AdjustAmountOfExternalAllocatedMemory(sizeof(*this));
}


CallbackInfo::~CallbackInfo() {
  persistent_.Reset();
}


void CallbackInfo::WeakCallback(
    const WeakCallbackData<Object, CallbackInfo>& data) {
  data.GetParameter()->WeakCallback(data.GetIsolate(), data.GetValue());
}


inline size_t InternalExternalArraySize(enum ExternalArrayType type) {
  switch (type) {
    case v8::kExternalUint8Array:
      return sizeof(uint8_t);
    case v8::kExternalInt8Array:
      return sizeof(int8_t);
    case v8::kExternalInt16Array:
      return sizeof(int16_t);
    case v8::kExternalUint16Array:
      return sizeof(uint16_t);
    case v8::kExternalInt32Array:
      return sizeof(int32_t);
    case v8::kExternalUint32Array:
      return sizeof(uint32_t);
    case v8::kExternalFloat32Array:
      return sizeof(float);   // NOLINT(runtime/sizeof)
    case v8::kExternalFloat64Array:
      return sizeof(double);  // NOLINT(runtime/sizeof)
    case v8::kExternalUint8ClampedArray:
      return sizeof(uint8_t);
  }
  return 0;
}


void CallbackInfo::WeakCallback(Isolate* isolate, Local<Object> object) {
  void* array_data = object->GetIndexedPropertiesExternalArrayData();
  size_t array_length = object->GetIndexedPropertiesExternalArrayDataLength();
  enum ExternalArrayType array_type =
      object->GetIndexedPropertiesExternalArrayDataType();
  size_t array_size = InternalExternalArraySize(array_type);
  CHECK_GT(array_size, 0);
  if (array_size > 1 && array_data != NULL) {
    CHECK_GT(array_length * array_size, array_length);  // Overflow check.
    array_length *= array_size;
  }
  object->SetIndexedPropertiesToExternalArrayData(nullptr, array_type, 0);
  callback_(static_cast<char*>(array_data), hint_);
  int64_t change_in_bytes = -static_cast<int64_t>(sizeof(*this));
  if (ownership_ == kInternal)
    change_in_bytes -= static_cast<int64_t>(array_length);
  isolate->AdjustAmountOfExternalAllocatedMemory(change_in_bytes);
  delete this;
}


// return size of external array type, or 0 if unrecognized
size_t ExternalArraySize(enum ExternalArrayType type) {
  return InternalExternalArraySize(type);
}


// copyOnto(source, source_start, dest, dest_start, copy_length)
void CopyOnto(const FunctionCallbackInfo<Value>& args) {
  Environment* env = Environment::GetCurrent(args);

  ASSERT(args[0]->IsObject());
  ASSERT(args[2]->IsObject());

  Local<Object> source = args[0].As<Object>();
  Local<Object> dest = args[2].As<Object>();

  ASSERT(source->HasIndexedPropertiesInExternalArrayData());
  ASSERT(dest->HasIndexedPropertiesInExternalArrayData());

  size_t source_start = args[1]->Uint32Value();
  size_t dest_start = args[3]->Uint32Value();
  size_t copy_length = args[4]->Uint32Value();
  char* source_data = static_cast<char*>(
      source->GetIndexedPropertiesExternalArrayData());
  char* dest_data = static_cast<char*>(
      dest->GetIndexedPropertiesExternalArrayData());

  size_t source_length = source->GetIndexedPropertiesExternalArrayDataLength();
  enum ExternalArrayType source_type =
    source->GetIndexedPropertiesExternalArrayDataType();
  size_t source_size = InternalExternalArraySize(source_type);

  size_t dest_length = dest->GetIndexedPropertiesExternalArrayDataLength();
  enum ExternalArrayType dest_type =
    dest->GetIndexedPropertiesExternalArrayDataType();
  size_t dest_size = InternalExternalArraySize(dest_type);

  // optimization for Uint8 arrays (i.e. Buffers)
  if (source_size != 1 || dest_size != 1) {
    if (source_size == 0)
      return env->ThrowTypeError("unknown source external array type");
    if (dest_size == 0)
      return env->ThrowTypeError("unknown dest external array type");

    if (source_length * source_size < source_length)
      return env->ThrowRangeError("source_length * source_size overflow");
    if (copy_length * source_size < copy_length)
      return env->ThrowRangeError("copy_length * source_size overflow");
    if (dest_length * dest_size < dest_length)
      return env->ThrowRangeError("dest_length * dest_size overflow");

    source_length *= source_size;
    copy_length *= source_size;
    dest_length *= dest_size;
  }

  // necessary to check in case (source|dest)_start _and_ copy_length overflow
  if (copy_length > source_length)
    return env->ThrowRangeError("copy_length > source_length");
  if (copy_length > dest_length)
    return env->ThrowRangeError("copy_length > dest_length");
  if (source_start > source_length)
    return env->ThrowRangeError("source_start > source_length");
  if (dest_start > dest_length)
    return env->ThrowRangeError("dest_start > dest_length");

  // now we can guarantee these will catch oob access and *_start overflow
  if (source_start + copy_length > source_length)
    return env->ThrowRangeError("source_start + copy_length > source_length");
  if (dest_start + copy_length > dest_length)
    return env->ThrowRangeError("dest_start + copy_length > dest_length");

  memmove(dest_data + dest_start, source_data + source_start, copy_length);
}


// dest will always be same type as source
// for internal use:
//    dest._data = sliceOnto(source, dest, start, end);
void SliceOnto(const FunctionCallbackInfo<Value>& args) {
  Local<Object> source = args[0].As<Object>();
  Local<Object> dest = args[1].As<Object>();

  CHECK(source->HasIndexedPropertiesInExternalArrayData());
  CHECK_EQ(false, dest->HasIndexedPropertiesInExternalArrayData());

  char* source_data = static_cast<char*>(
      source->GetIndexedPropertiesExternalArrayData());
  size_t source_len = source->GetIndexedPropertiesExternalArrayDataLength();
  enum ExternalArrayType source_type =
    source->GetIndexedPropertiesExternalArrayDataType();
  size_t source_size = InternalExternalArraySize(source_type);

  CHECK_NE(source_size, 0);

  size_t start = args[2]->Uint32Value();
  size_t end = args[3]->Uint32Value();
  size_t length = end - start;

  if (source_size > 1) {
    CHECK_GE(length * source_size, length);
    length *= source_size;
  }

  CHECK(source_data != nullptr || length == 0);
  CHECK_LE(end, source_len);
  CHECK_LE(start, end);

  dest->SetIndexedPropertiesToExternalArrayData(source_data + start,
                                                source_type,
                                                length);
  args.GetReturnValue().Set(source);
}


// for internal use:
//    alloc(obj, n[, type]);
void Alloc(const FunctionCallbackInfo<Value>& args) {
  Environment* env = Environment::GetCurrent(args);

  ASSERT(args[0]->IsObject());

  Local<Object> obj = args[0].As<Object>();

  ASSERT(!obj->HasIndexedPropertiesInExternalArrayData());

  size_t length = args[1]->Uint32Value();
  enum ExternalArrayType array_type;

  // it's faster to not pass the default argument then use Uint32Value
  if (args[2]->IsUndefined()) {
    array_type = kExternalUint8Array;
  } else {
    array_type = static_cast<ExternalArrayType>(args[2]->Uint32Value());
    size_t type_length = InternalExternalArraySize(array_type);
    CHECK_GE(type_length * length, length);
    length *= type_length;
  }

  Alloc(env, obj, length, array_type);
  args.GetReturnValue().Set(obj);
}


void Alloc(Environment* env,
           Handle<Object> obj,
           size_t length,
           enum ExternalArrayType type) {
  size_t type_size = InternalExternalArraySize(type);

  CHECK_LE(length, kMaxLength);
  CHECK_GT(type_size, 0);

  if (length == 0)
    return Alloc(env, obj, nullptr, length, type);

  char* data = static_cast<char*>(malloc(length));
  if (data == nullptr) {
    FatalError("node::smalloc::Alloc(node::Environment*, "
               " v8::Handle<v8::Object>, size_t, v8::ExternalArrayType)",
               "Out Of Memory");
    UNREACHABLE();
  }

  Alloc(env, obj, data, length, type);
}


void Alloc(Environment* env,
           Handle<Object> obj,
           char* data,
           size_t length,
           enum ExternalArrayType type) {
  CHECK_EQ(false, obj->HasIndexedPropertiesInExternalArrayData());
  env->isolate()->AdjustAmountOfExternalAllocatedMemory(length);
  size_t size = length / InternalExternalArraySize(type);
  obj->SetIndexedPropertiesToExternalArrayData(data, type, size);
  CallbackInfo::New(env->isolate(),
                    CallbackInfo::kInternal,
                    obj,
                    CallbackInfo::Free);
}


// for internal use: dispose(obj);
void AllocDispose(const FunctionCallbackInfo<Value>& args) {
  Environment* env = Environment::GetCurrent(args);
  AllocDispose(env, args[0].As<Object>());
}


void AllocDispose(Environment* env, Handle<Object> obj) {
  HandleScope handle_scope(env->isolate());

  if (env->using_smalloc_alloc_cb()) {
    Local<Value> ext_v = obj->GetHiddenValue(env->smalloc_p_string());
    if (ext_v->IsExternal()) {
      Local<External> ext = ext_v.As<External>();
      CallbackInfo* info = static_cast<CallbackInfo*>(ext->Value());
      info->Dispose(env->isolate());
      return;
    }
  }

  char* data = static_cast<char*>(obj->GetIndexedPropertiesExternalArrayData());
  size_t length = obj->GetIndexedPropertiesExternalArrayDataLength();
  enum ExternalArrayType array_type =
    obj->GetIndexedPropertiesExternalArrayDataType();
  size_t array_size = InternalExternalArraySize(array_type);

  CHECK_GT(array_size, 0);
  CHECK_GE(length * array_size, length);

  length *= array_size;

  if (data != nullptr) {
    obj->SetIndexedPropertiesToExternalArrayData(nullptr,
                                                 kExternalUint8Array,
                                                 0);
    free(data);
  }
  if (length != 0) {
    int64_t change_in_bytes = -static_cast<int64_t>(length);
    env->isolate()->AdjustAmountOfExternalAllocatedMemory(change_in_bytes);
  }
}


static void Alloc(Environment* env,
                  CallbackInfo::Ownership ownership,
                  Handle<Object> obj,
                  char* data,
                  size_t length,
                  FreeCallback fn,
                  void* hint,
                  enum ExternalArrayType type) {
  CHECK_EQ(false, obj->HasIndexedPropertiesInExternalArrayData());
  Isolate* isolate = env->isolate();
  HandleScope handle_scope(isolate);
  env->set_using_smalloc_alloc_cb(true);
  CallbackInfo* info = CallbackInfo::New(isolate, ownership, obj, fn, hint);
  obj->SetHiddenValue(env->smalloc_p_string(), External::New(isolate, info));
  size_t size = length / InternalExternalArraySize(type);
  obj->SetIndexedPropertiesToExternalArrayData(data, type, size);
}


void Alloc(Environment* env,
           Handle<Object> obj,
           size_t length,
           FreeCallback fn,
           void* hint,
           enum ExternalArrayType type) {
  CHECK_LE(length, kMaxLength);

  size_t type_size = InternalExternalArraySize(type);

  CHECK_GT(type_size, 0);
  CHECK_GE(length * type_size, length);

  length *= type_size;

  char* data = static_cast<char*>(malloc(length));
  if (data == nullptr) {
    FatalError("node::smalloc::Alloc(node::Environment*, "
               " v8::Handle<v8::Object>, size_t, node::FreeCallback,"
               " void*, v8::ExternalArrayType)", "Out Of Memory");
    UNREACHABLE();
  }

  env->isolate()->AdjustAmountOfExternalAllocatedMemory(length);
  Alloc(env, CallbackInfo::kInternal, obj, data, length, fn, hint, type);
}


void Alloc(Environment* env,
           Handle<Object> obj,
           char* data,
           size_t length,
           FreeCallback fn,
           void* hint,
           enum ExternalArrayType type) {
  Alloc(env, CallbackInfo::kExternal, obj, data, length, fn, hint, type);
}


void HasExternalData(const FunctionCallbackInfo<Value>& args) {
  Environment* env = Environment::GetCurrent(args);
  ASSERT(args[0]->IsObject());
  args.GetReturnValue().Set(HasExternalData(env, args[0].As<Object>()));
}


bool HasExternalData(Environment* env, Local<Object> obj) {
  return obj->HasIndexedPropertiesInExternalArrayData();
}

void IsTypedArray(const FunctionCallbackInfo<Value>& args) {
  args.GetReturnValue().Set(args[0]->IsTypedArray());
}

void AllocTruncate(const FunctionCallbackInfo<Value>& args) {
  Environment* env = Environment::GetCurrent(args);

  Local<Object> obj = args[0].As<Object>();

  // can't perform this check in JS
  if (!obj->HasIndexedPropertiesInExternalArrayData())
    return env->ThrowTypeError("object has no external array data");

  char* data = static_cast<char*>(obj->GetIndexedPropertiesExternalArrayData());
  enum ExternalArrayType array_type =
      obj->GetIndexedPropertiesExternalArrayDataType();
  int length = obj->GetIndexedPropertiesExternalArrayDataLength();

  unsigned int new_len = args[1]->Uint32Value();
  if (new_len > kMaxLength)
    return env->ThrowRangeError("truncate length is bigger than kMaxLength");

  if (static_cast<int>(new_len) > length)
    return env->ThrowRangeError("truncate length is bigger than current one");

  obj->SetIndexedPropertiesToExternalArrayData(data,
                                               array_type,
                                               static_cast<int>(new_len));
}



class RetainedAllocInfo: public RetainedObjectInfo {
 public:
  explicit RetainedAllocInfo(Handle<Value> wrapper);

  virtual void Dispose() override;
  virtual bool IsEquivalent(RetainedObjectInfo* other) override;
  virtual intptr_t GetHash() override;
  virtual const char* GetLabel() override;
  virtual intptr_t GetSizeInBytes() override;

 private:
  static const char label_[];
  char* data_;
  int length_;
};


const char RetainedAllocInfo::label_[] = "smalloc";


RetainedAllocInfo::RetainedAllocInfo(Handle<Value> wrapper) {
  Local<Object> obj = wrapper.As<Object>();
  length_ = obj->GetIndexedPropertiesExternalArrayDataLength();
  data_ = static_cast<char*>(obj->GetIndexedPropertiesExternalArrayData());
}


void RetainedAllocInfo::Dispose() {
  delete this;
}


bool RetainedAllocInfo::IsEquivalent(RetainedObjectInfo* other) {
  return label_ == other->GetLabel() &&
         data_ == static_cast<RetainedAllocInfo*>(other)->data_;
}


intptr_t RetainedAllocInfo::GetHash() {
  return reinterpret_cast<intptr_t>(data_);
}


const char* RetainedAllocInfo::GetLabel() {
  return label_;
}


intptr_t RetainedAllocInfo::GetSizeInBytes() {
  return length_;
}


RetainedObjectInfo* WrapperInfo(uint16_t class_id, Handle<Value> wrapper) {
  return new RetainedAllocInfo(wrapper);
}


// User facing API.

void Alloc(Isolate* isolate,
           Handle<Object> obj,
           size_t length,
           enum ExternalArrayType type) {
  Alloc(Environment::GetCurrent(isolate), obj, length, type);
}


void Alloc(Isolate* isolate,
           Handle<Object> obj,
           char* data,
           size_t length,
           enum ExternalArrayType type) {
  Alloc(Environment::GetCurrent(isolate), obj, data, length, type);
}


void Alloc(Isolate* isolate,
           Handle<Object> obj,
           size_t length,
           FreeCallback fn,
           void* hint,
           enum ExternalArrayType type) {
  Alloc(Environment::GetCurrent(isolate), obj, length, fn, hint, type);
}


void Alloc(Isolate* isolate,
           Handle<Object> obj,
           char* data,
           size_t length,
           FreeCallback fn,
           void* hint,
           enum ExternalArrayType type) {
  Alloc(Environment::GetCurrent(isolate), obj, data, length, fn, hint, type);
}


void AllocDispose(Isolate* isolate, Handle<Object> obj) {
  AllocDispose(Environment::GetCurrent(isolate), obj);
}


bool HasExternalData(Isolate* isolate, Local<Object> obj) {
  return HasExternalData(Environment::GetCurrent(isolate), obj);
}


void Initialize(Handle<Object> exports,
                Handle<Value> unused,
                Handle<Context> context) {
  Environment* env = Environment::GetCurrent(context);
  Isolate* isolate = env->isolate();

  env->SetMethod(exports, "copyOnto", CopyOnto);
  env->SetMethod(exports, "sliceOnto", SliceOnto);

  env->SetMethod(exports, "alloc", Alloc);
  env->SetMethod(exports, "dispose", AllocDispose);
  env->SetMethod(exports, "truncate", AllocTruncate);

  env->SetMethod(exports, "hasExternalData", HasExternalData);
  env->SetMethod(exports, "isTypedArray", IsTypedArray);

  exports->Set(FIXED_ONE_BYTE_STRING(env->isolate(), "kMaxLength"),
               Uint32::NewFromUnsigned(env->isolate(), kMaxLength));

  Local<Object> types = Object::New(isolate);

  uint32_t kMinType = ~0;
  uint32_t kMaxType = 0;
#define V(name, value)                                                        \
  types->Set(FIXED_ONE_BYTE_STRING(env->isolate(), #name),                    \
             Uint32::NewFromUnsigned(env->isolate(), v8::value));             \
  kMinType = MIN(kMinType, static_cast<uint32_t>(v8::value));                 \
  kMaxType = MAX(kMinType, static_cast<uint32_t>(v8::value));
  EXTERNAL_ARRAY_TYPES(V)
#undef V

  exports->Set(FIXED_ONE_BYTE_STRING(env->isolate(), "types"), types);
  exports->Set(FIXED_ONE_BYTE_STRING(env->isolate(), "kMinType"),
               Uint32::NewFromUnsigned(env->isolate(), kMinType));
  exports->Set(FIXED_ONE_BYTE_STRING(env->isolate(), "kMaxType"),
               Uint32::NewFromUnsigned(env->isolate(), kMaxType));

  HeapProfiler* heap_profiler = env->isolate()->GetHeapProfiler();
#if 0
  heap_profiler->SetWrapperClassInfoProvider(ALLOC_ID, WrapperInfo);
#endif
}


}  // namespace smalloc
}  // namespace node

NODE_MODULE_CONTEXT_AWARE_BUILTIN(smalloc, node::smalloc::Initialize)
