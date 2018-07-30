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

#ifndef SRC_NODE_INTERNALS_H_
#define SRC_NODE_INTERNALS_H_

#if defined(NODE_WANT_INTERNALS) && NODE_WANT_INTERNALS

#include "node.h"
#include "node_mutex.h"
#include "node_persistent.h"
#include "util-inl.h"
#include "env-inl.h"
#include "uv.h"
#include "v8.h"
#include "node_perf_common.h"
#include "node_debug_options.h"
#include "node_api.h"

#include <stdint.h>
#include <stdlib.h>

#include <string>
#include <vector>

// Custom constants used by both node_constants.cc and node_zlib.cc
#define Z_MIN_WINDOWBITS 8
#define Z_MAX_WINDOWBITS 15
#define Z_DEFAULT_WINDOWBITS 15
// Fewer than 64 bytes per chunk is not recommended.
// Technically it could work with as few as 8, but even 64 bytes
// is low.  Usually a MB or more is best.
#define Z_MIN_CHUNK 64
#define Z_MAX_CHUNK std::numeric_limits<double>::infinity()
#define Z_DEFAULT_CHUNK (16 * 1024)
#define Z_MIN_MEMLEVEL 1
#define Z_MAX_MEMLEVEL 9
#define Z_DEFAULT_MEMLEVEL 8
#define Z_MIN_LEVEL -1
#define Z_MAX_LEVEL 9
#define Z_DEFAULT_LEVEL Z_DEFAULT_COMPRESSION

enum {
  NM_F_BUILTIN  = 1 << 0,
  NM_F_LINKED   = 1 << 1,
  NM_F_INTERNAL = 1 << 2,
};

struct sockaddr;

// Variation on NODE_DEFINE_CONSTANT that sets a String value.
#define NODE_DEFINE_STRING_CONSTANT(target, name, constant)                   \
  do {                                                                        \
    v8::Isolate* isolate = target->GetIsolate();                              \
    v8::Local<v8::String> constant_name =                                     \
        v8::String::NewFromUtf8(isolate, name);                               \
    v8::Local<v8::String> constant_value =                                    \
        v8::String::NewFromUtf8(isolate, constant);                           \
    v8::PropertyAttribute constant_attributes =                               \
        static_cast<v8::PropertyAttribute>(v8::ReadOnly | v8::DontDelete);    \
    target->DefineOwnProperty(isolate->GetCurrentContext(),                   \
                              constant_name,                                  \
                              constant_value,                                 \
                              constant_attributes).FromJust();                \
  } while (0)


#if HAVE_OPENSSL
#define NODE_BUILTIN_OPENSSL_MODULES(V) V(crypto) V(tls_wrap)
#else
#define NODE_BUILTIN_OPENSSL_MODULES(V)
#endif

#if NODE_HAVE_I18N_SUPPORT
#define NODE_BUILTIN_ICU_MODULES(V) V(icu)
#else
#define NODE_BUILTIN_ICU_MODULES(V)
#endif

// A list of built-in modules. In order to do module registration
// in node::Init(), need to add built-in modules in the following list.
// Then in node::RegisterBuiltinModules(), it calls modules' registration
// function. This helps the built-in modules are loaded properly when
// node is built as static library. No need to depends on the
// __attribute__((constructor)) like mechanism in GCC.
#define NODE_BUILTIN_STANDARD_MODULES(V)                                      \
    V(async_wrap)                                                             \
    V(buffer)                                                                 \
    V(cares_wrap)                                                             \
    V(config)                                                                 \
    V(contextify)                                                             \
    V(domain)                                                                 \
    V(fs)                                                                     \
    V(fs_event_wrap)                                                          \
    V(http2)                                                                  \
    V(http_parser)                                                            \
    V(inspector)                                                              \
    V(js_stream)                                                              \
    V(messaging)                                                              \
    V(module_wrap)                                                            \
    V(os)                                                                     \
    V(performance)                                                            \
    V(pipe_wrap)                                                              \
    V(process_wrap)                                                           \
    V(serdes)                                                                 \
    V(signal_wrap)                                                            \
    V(spawn_sync)                                                             \
    V(stream_pipe)                                                            \
    V(stream_wrap)                                                            \
    V(string_decoder)                                                         \
    V(symbols)                                                                \
    V(tcp_wrap)                                                               \
    V(timer_wrap)                                                             \
    V(trace_events)                                                           \
    V(tty_wrap)                                                               \
    V(types)                                                                  \
    V(udp_wrap)                                                               \
    V(url)                                                                    \
    V(util)                                                                   \
    V(uv)                                                                     \
    V(v8)                                                                     \
    V(worker)                                                                 \
    V(zlib)

#define NODE_BUILTIN_MODULES(V)                                               \
  NODE_BUILTIN_STANDARD_MODULES(V)                                            \
  NODE_BUILTIN_OPENSSL_MODULES(V)                                             \
  NODE_BUILTIN_ICU_MODULES(V)

#define NODE_MODULE_CONTEXT_AWARE_CPP(modname, regfunc, priv, flags)          \
  static node::node_module _module = {                                        \
    NODE_MODULE_VERSION,                                                      \
    flags,                                                                    \
    nullptr,                                                                  \
    __FILE__,                                                                 \
    nullptr,                                                                  \
    (node::addon_context_register_func) (regfunc),                            \
    NODE_STRINGIFY(modname),                                                  \
    priv,                                                                     \
    nullptr                                                                   \
  };                                                                          \
  void _register_ ## modname() {                                              \
    node_module_register(&_module);                                           \
  }


#define NODE_BUILTIN_MODULE_CONTEXT_AWARE(modname, regfunc)                   \
  NODE_MODULE_CONTEXT_AWARE_CPP(modname, regfunc, nullptr, NM_F_BUILTIN)

namespace node {

// Set in node.cc by ParseArgs with the value of --openssl-config.
// Used in node_crypto.cc when initializing OpenSSL.
extern std::string openssl_config;

// Set in node.cc by ParseArgs when --preserve-symlinks is used.
// Used in node_config.cc to set a constant on process.binding('config')
// that is used by lib/module.js
extern bool config_preserve_symlinks;

// Set in node.cc by ParseArgs when --preserve-symlinks-main is used.
// Used in node_config.cc to set a constant on process.binding('config')
// that is used by lib/module.js
extern bool config_preserve_symlinks_main;

// Set in node.cc by ParseArgs when --experimental-modules is used.
// Used in node_config.cc to set a constant on process.binding('config')
// that is used by lib/module.js
extern bool config_experimental_modules;

// Set in node.cc by ParseArgs when --experimental-vm-modules is used.
// Used in node_config.cc to set a constant on process.binding('config')
// that is used by lib/vm.js
extern bool config_experimental_vm_modules;

// Set in node.cc by ParseArgs when --experimental-vm-modules is used.
// Used in node_config.cc to set a constant on process.binding('config')
// that is used by lib/vm.js
extern bool config_experimental_worker;

// Set in node.cc by ParseArgs when --experimental-repl-await is used.
// Used in node_config.cc to set a constant on process.binding('config')
// that is used by lib/repl.js.
extern bool config_experimental_repl_await;

// Set in node.cc by ParseArgs when --loader is used.
// Used in node_config.cc to set a constant on process.binding('config')
// that is used by lib/internal/bootstrap/node.js
extern std::string config_userland_loader;

// Set in node.cc by ParseArgs when --expose-internals or --expose_internals is
// used.
// Used in node_config.cc to set a constant on process.binding('config')
// that is used by lib/internal/bootstrap/node.js
extern bool config_expose_internals;

// Set in node.cc by ParseArgs when --redirect-warnings= is used.
// Used to redirect warning output to a file rather than sending
// it to stderr.
extern std::string config_warning_file;  // NOLINT(runtime/string)

// Set in node.cc by ParseArgs when --pending-deprecation or
// NODE_PENDING_DEPRECATION is used
extern bool config_pending_deprecation;

// Tells whether it is safe to call v8::Isolate::GetCurrent().
extern bool v8_initialized;

class DebugOptions;

// Contains initial debug options.
// Set in node.cc.
// Used in node_config.cc.
extern DebugOptions debug_options;

// Forward declaration
class Environment;

// If persistent.IsWeak() == false, then do not call persistent.Reset()
// while the returned Local<T> is still in scope, it will destroy the
// reference to the object.
template <class TypeName>
inline v8::Local<TypeName> PersistentToLocal(
    v8::Isolate* isolate,
    const Persistent<TypeName>& persistent);

// Convert a struct sockaddr to a { address: '1.2.3.4', port: 1234 } JS object.
// Sets address and port properties on the info object and returns it.
// If |info| is omitted, a new object is returned.
v8::Local<v8::Object> AddressToJS(
    Environment* env,
    const sockaddr* addr,
    v8::Local<v8::Object> info = v8::Local<v8::Object>());

template <typename T, int (*F)(const typename T::HandleType*, sockaddr*, int*)>
void GetSockOrPeerName(const v8::FunctionCallbackInfo<v8::Value>& args) {
  T* wrap;
  ASSIGN_OR_RETURN_UNWRAP(&wrap,
                          args.Holder(),
                          args.GetReturnValue().Set(UV_EBADF));
  CHECK(args[0]->IsObject());
  sockaddr_storage storage;
  int addrlen = sizeof(storage);
  sockaddr* const addr = reinterpret_cast<sockaddr*>(&storage);
  const int err = F(&wrap->handle_, addr, &addrlen);
  if (err == 0)
    AddressToJS(wrap->env(), addr, args[0].As<v8::Object>());
  args.GetReturnValue().Set(err);
}

void FatalException(v8::Isolate* isolate,
                    v8::Local<v8::Value> error,
                    v8::Local<v8::Message> message);


void SignalExit(int signo);
#ifdef __POSIX__
void RegisterSignalHandler(int signal,
                           void (*handler)(int signal),
                           bool reset_handler = false);
#endif

bool SafeGetenv(const char* key, std::string* text);

std::string GetHumanReadableProcessName();
void GetHumanReadableProcessName(char (*name)[1024]);

template <typename T, size_t N>
constexpr size_t arraysize(const T(&)[N]) { return N; }

#ifndef ROUND_UP
# define ROUND_UP(a, b) ((a) % (b) ? ((a) + (b)) - ((a) % (b)) : (a))
#endif

#ifdef __GNUC__
# define MUST_USE_RESULT __attribute__((warn_unused_result))
#else
# define MUST_USE_RESULT
#endif

bool IsExceptionDecorated(Environment* env, v8::Local<v8::Value> er);

enum ErrorHandlingMode { CONTEXTIFY_ERROR, FATAL_ERROR, MODULE_ERROR };
void AppendExceptionLine(Environment* env,
                         v8::Local<v8::Value> er,
                         v8::Local<v8::Message> message,
                         enum ErrorHandlingMode mode);

NO_RETURN void FatalError(const char* location, const char* message);

// Like a `TryCatch` but exits the process if an exception was caught.
class FatalTryCatch : public v8::TryCatch {
 public:
  explicit FatalTryCatch(Environment* env)
      : TryCatch(env->isolate()), env_(env) {}
  ~FatalTryCatch();

 private:
  Environment* env_;
};

class SlicedArguments {
 public:
  inline explicit SlicedArguments(
      const v8::FunctionCallbackInfo<v8::Value>& args,
      size_t start = 0);
  inline size_t size() const { return size_; }
  inline v8::Local<v8::Value>* data() { return data_; }

 private:
  size_t size_;
  v8::Local<v8::Value>* data_;
  v8::Local<v8::Value> fixed_[64];
  std::vector<v8::Local<v8::Value>> dynamic_;
};

SlicedArguments::SlicedArguments(
    const v8::FunctionCallbackInfo<v8::Value>& args,
    size_t start) : size_(0), data_(fixed_) {
  const size_t length = static_cast<size_t>(args.Length());
  if (start >= length) return;
  const size_t size = length - start;

  if (size > arraysize(fixed_)) {
    dynamic_.resize(size);
    data_ = dynamic_.data();
  }

  for (size_t i = 0; i < size; ++i)
    data_[i] = args[i + start];

  size_ = size;
}

void ReportException(Environment* env,
                     v8::Local<v8::Value> er,
                     v8::Local<v8::Message> message);

v8::Maybe<bool> ProcessEmitWarning(Environment* env, const char* fmt, ...);
v8::Maybe<bool> ProcessEmitDeprecationWarning(Environment* env,
                                              const char* warning,
                                              const char* deprecation_code);

template <typename NativeT, typename V8T>
v8::Local<v8::Value> FillStatsArray(AliasedBuffer<NativeT, V8T>* fields_ptr,
                    const uv_stat_t* s, int offset = 0) {
  AliasedBuffer<NativeT, V8T>& fields = *fields_ptr;
  fields[offset + 0] = s->st_dev;
  fields[offset + 1] = s->st_mode;
  fields[offset + 2] = s->st_nlink;
  fields[offset + 3] = s->st_uid;
  fields[offset + 4] = s->st_gid;
  fields[offset + 5] = s->st_rdev;
#if defined(__POSIX__)
  fields[offset + 6] = s->st_blksize;
#else
  fields[offset + 6] = 0;
#endif
  fields[offset + 7] = s->st_ino;
  fields[offset + 8] = s->st_size;
#if defined(__POSIX__)
  fields[offset + 9] = s->st_blocks;
#else
  fields[offset + 9] = 0;
#endif
// Dates.
// NO-LINT because the fields are 'long' and we just want to cast to `unsigned`
#define X(idx, name)                                                    \
  /* NOLINTNEXTLINE(runtime/int) */                                     \
  fields[offset + idx] = ((unsigned long)(s->st_##name.tv_sec) * 1e3) + \
  /* NOLINTNEXTLINE(runtime/int) */                                     \
                ((unsigned long)(s->st_##name.tv_nsec) / 1e6);          \

  X(10, atim)
  X(11, mtim)
  X(12, ctim)
  X(13, birthtim)
#undef X

  return fields_ptr->GetJSArray();
}

inline v8::Local<v8::Value> FillGlobalStatsArray(Environment* env,
                                                 const uv_stat_t* s,
                                                 bool use_bigint = false,
                                                 int offset = 0) {
  if (use_bigint) {
    return node::FillStatsArray(
        env->fs_stats_field_bigint_array(), s, offset);
  } else {
    return node::FillStatsArray(env->fs_stats_field_array(), s, offset);
  }
}

void SetupBootstrapObject(Environment* env,
                          v8::Local<v8::Object> bootstrapper);
void SetupProcessObject(Environment* env,
                        int argc,
                        const char* const* argv,
                        int exec_argc,
                        const char* const* exec_argv);

// Call _register<module_name> functions for all of
// the built-in modules. Because built-in modules don't
// use the __attribute__((constructor)). Need to
// explicitly call the _register* functions.
void RegisterBuiltinModules();

enum Endianness {
  kLittleEndian,  // _Not_ LITTLE_ENDIAN, clashes with endian.h.
  kBigEndian
};

inline enum Endianness GetEndianness() {
  // Constant-folded by the compiler.
  const union {
    uint8_t u8[2];
    uint16_t u16;
  } u = {
    { 1, 0 }
  };
  return u.u16 == 1 ? kLittleEndian : kBigEndian;
}

inline bool IsLittleEndian() {
  return GetEndianness() == kLittleEndian;
}

inline bool IsBigEndian() {
  return GetEndianness() == kBigEndian;
}

class ArrayBufferAllocator : public v8::ArrayBuffer::Allocator {
 public:
  inline uint32_t* zero_fill_field() { return &zero_fill_field_; }

  virtual void* Allocate(size_t size);  // Defined in src/node.cc
  virtual void* AllocateUninitialized(size_t size)
    { return node::UncheckedMalloc(size); }
  virtual void Free(void* data, size_t) { free(data); }

 private:
  uint32_t zero_fill_field_ = 1;  // Boolean but exposed as uint32 to JS land.
};

namespace Buffer {
v8::MaybeLocal<v8::Object> Copy(Environment* env, const char* data, size_t len);
v8::MaybeLocal<v8::Object> New(Environment* env, size_t size);
// Takes ownership of |data|.
v8::MaybeLocal<v8::Object> New(Environment* env,
                               char* data,
                               size_t length,
                               void (*callback)(char* data, void* hint),
                               void* hint);
// Takes ownership of |data|.  Must allocate |data| with malloc() or realloc()
// because ArrayBufferAllocator::Free() deallocates it again with free().
// Mixing operator new and free() is undefined behavior so don't do that.
v8::MaybeLocal<v8::Object> New(Environment* env, char* data, size_t length);

inline
v8::MaybeLocal<v8::Uint8Array> New(Environment* env,
                                   v8::Local<v8::ArrayBuffer> ab,
                                   size_t byte_offset,
                                   size_t length) {
  v8::Local<v8::Uint8Array> ui = v8::Uint8Array::New(ab, byte_offset, length);
  v8::Maybe<bool> mb =
      ui->SetPrototype(env->context(), env->buffer_prototype_object());
  if (mb.IsNothing())
    return v8::MaybeLocal<v8::Uint8Array>();
  return ui;
}

// Construct a Buffer from a MaybeStackBuffer (and also its subclasses like
// Utf8Value and TwoByteValue).
// If |buf| is invalidated, an empty MaybeLocal is returned, and nothing is
// changed.
// If |buf| contains actual data, this method takes ownership of |buf|'s
// underlying buffer. However, |buf| itself can be reused even after this call,
// but its capacity, if increased through AllocateSufficientStorage, is not
// guaranteed to stay the same.
template <typename T>
static v8::MaybeLocal<v8::Object> New(Environment* env,
                                      MaybeStackBuffer<T>* buf) {
  v8::MaybeLocal<v8::Object> ret;
  char* src = reinterpret_cast<char*>(buf->out());
  const size_t len_in_bytes = buf->length() * sizeof(buf->out()[0]);

  if (buf->IsAllocated())
    ret = New(env, src, len_in_bytes);
  else if (!buf->IsInvalidated())
    ret = Copy(env, src, len_in_bytes);

  if (ret.IsEmpty())
    return ret;

  if (buf->IsAllocated())
    buf->Release();

  return ret;
}
}  // namespace Buffer

v8::MaybeLocal<v8::Value> InternalMakeCallback(
    Environment* env,
    v8::Local<v8::Object> recv,
    const v8::Local<v8::Function> callback,
    int argc,
    v8::Local<v8::Value> argv[],
    async_context asyncContext);

class NODE_EXTERN InternalCallbackScope {
 public:
  // Tell the constructor whether its `object` parameter may be empty or not.
  enum ResourceExpectation { kRequireResource, kAllowEmptyResource };
  InternalCallbackScope(Environment* env,
                        v8::Local<v8::Object> object,
                        const async_context& asyncContext,
                        ResourceExpectation expect = kRequireResource);
  // Utility that can be used by AsyncWrap classes.
  explicit InternalCallbackScope(AsyncWrap* async_wrap);
  ~InternalCallbackScope();
  void Close();

  inline bool Failed() const { return failed_; }
  inline void MarkAsFailed() { failed_ = true; }
  inline bool IsInnerMakeCallback() const {
    return callback_scope_.in_makecallback();
  }

 private:
  Environment* env_;
  async_context async_context_;
  v8::Local<v8::Object> object_;
  Environment::AsyncCallbackScope callback_scope_;
  bool failed_ = false;
  bool pushed_ids_ = false;
  bool closed_ = false;
};

class ThreadPoolWork {
 public:
  explicit inline ThreadPoolWork(Environment* env) : env_(env) {}
  inline virtual ~ThreadPoolWork() = default;

  inline void ScheduleWork();
  inline int CancelWork();

  virtual void DoThreadPoolWork() = 0;
  virtual void AfterThreadPoolWork(int status) = 0;

 private:
  Environment* env_;
  uv_work_t work_req_;
};

void ThreadPoolWork::ScheduleWork() {
  env_->IncreaseWaitingRequestCounter();
  int status = uv_queue_work(
      env_->event_loop(),
      &work_req_,
      [](uv_work_t* req) {
        ThreadPoolWork* self = ContainerOf(&ThreadPoolWork::work_req_, req);
        self->DoThreadPoolWork();
      },
      [](uv_work_t* req, int status) {
        ThreadPoolWork* self = ContainerOf(&ThreadPoolWork::work_req_, req);
        self->env_->DecreaseWaitingRequestCounter();
        self->AfterThreadPoolWork(status);
      });
  CHECK_EQ(status, 0);
}

int ThreadPoolWork::CancelWork() {
  return uv_cancel(reinterpret_cast<uv_req_t*>(&work_req_));
}

static inline const char* errno_string(int errorno) {
#define ERRNO_CASE(e)  case e: return #e;
  switch (errorno) {
#ifdef EACCES
  ERRNO_CASE(EACCES);
#endif

#ifdef EADDRINUSE
  ERRNO_CASE(EADDRINUSE);
#endif

#ifdef EADDRNOTAVAIL
  ERRNO_CASE(EADDRNOTAVAIL);
#endif

#ifdef EAFNOSUPPORT
  ERRNO_CASE(EAFNOSUPPORT);
#endif

#ifdef EAGAIN
  ERRNO_CASE(EAGAIN);
#endif

#ifdef EWOULDBLOCK
# if EAGAIN != EWOULDBLOCK
  ERRNO_CASE(EWOULDBLOCK);
# endif
#endif

#ifdef EALREADY
  ERRNO_CASE(EALREADY);
#endif

#ifdef EBADF
  ERRNO_CASE(EBADF);
#endif

#ifdef EBADMSG
  ERRNO_CASE(EBADMSG);
#endif

#ifdef EBUSY
  ERRNO_CASE(EBUSY);
#endif

#ifdef ECANCELED
  ERRNO_CASE(ECANCELED);
#endif

#ifdef ECHILD
  ERRNO_CASE(ECHILD);
#endif

#ifdef ECONNABORTED
  ERRNO_CASE(ECONNABORTED);
#endif

#ifdef ECONNREFUSED
  ERRNO_CASE(ECONNREFUSED);
#endif

#ifdef ECONNRESET
  ERRNO_CASE(ECONNRESET);
#endif

#ifdef EDEADLK
  ERRNO_CASE(EDEADLK);
#endif

#ifdef EDESTADDRREQ
  ERRNO_CASE(EDESTADDRREQ);
#endif

#ifdef EDOM
  ERRNO_CASE(EDOM);
#endif

#ifdef EDQUOT
  ERRNO_CASE(EDQUOT);
#endif

#ifdef EEXIST
  ERRNO_CASE(EEXIST);
#endif

#ifdef EFAULT
  ERRNO_CASE(EFAULT);
#endif

#ifdef EFBIG
  ERRNO_CASE(EFBIG);
#endif

#ifdef EHOSTUNREACH
  ERRNO_CASE(EHOSTUNREACH);
#endif

#ifdef EIDRM
  ERRNO_CASE(EIDRM);
#endif

#ifdef EILSEQ
  ERRNO_CASE(EILSEQ);
#endif

#ifdef EINPROGRESS
  ERRNO_CASE(EINPROGRESS);
#endif

#ifdef EINTR
  ERRNO_CASE(EINTR);
#endif

#ifdef EINVAL
  ERRNO_CASE(EINVAL);
#endif

#ifdef EIO
  ERRNO_CASE(EIO);
#endif

#ifdef EISCONN
  ERRNO_CASE(EISCONN);
#endif

#ifdef EISDIR
  ERRNO_CASE(EISDIR);
#endif

#ifdef ELOOP
  ERRNO_CASE(ELOOP);
#endif

#ifdef EMFILE
  ERRNO_CASE(EMFILE);
#endif

#ifdef EMLINK
  ERRNO_CASE(EMLINK);
#endif

#ifdef EMSGSIZE
  ERRNO_CASE(EMSGSIZE);
#endif

#ifdef EMULTIHOP
  ERRNO_CASE(EMULTIHOP);
#endif

#ifdef ENAMETOOLONG
  ERRNO_CASE(ENAMETOOLONG);
#endif

#ifdef ENETDOWN
  ERRNO_CASE(ENETDOWN);
#endif

#ifdef ENETRESET
  ERRNO_CASE(ENETRESET);
#endif

#ifdef ENETUNREACH
  ERRNO_CASE(ENETUNREACH);
#endif

#ifdef ENFILE
  ERRNO_CASE(ENFILE);
#endif

#ifdef ENOBUFS
  ERRNO_CASE(ENOBUFS);
#endif

#ifdef ENODATA
  ERRNO_CASE(ENODATA);
#endif

#ifdef ENODEV
  ERRNO_CASE(ENODEV);
#endif

#ifdef ENOENT
  ERRNO_CASE(ENOENT);
#endif

#ifdef ENOEXEC
  ERRNO_CASE(ENOEXEC);
#endif

#ifdef ENOLINK
  ERRNO_CASE(ENOLINK);
#endif

#ifdef ENOLCK
# if ENOLINK != ENOLCK
  ERRNO_CASE(ENOLCK);
# endif
#endif

#ifdef ENOMEM
  ERRNO_CASE(ENOMEM);
#endif

#ifdef ENOMSG
  ERRNO_CASE(ENOMSG);
#endif

#ifdef ENOPROTOOPT
  ERRNO_CASE(ENOPROTOOPT);
#endif

#ifdef ENOSPC
  ERRNO_CASE(ENOSPC);
#endif

#ifdef ENOSR
  ERRNO_CASE(ENOSR);
#endif

#ifdef ENOSTR
  ERRNO_CASE(ENOSTR);
#endif

#ifdef ENOSYS
  ERRNO_CASE(ENOSYS);
#endif

#ifdef ENOTCONN
  ERRNO_CASE(ENOTCONN);
#endif

#ifdef ENOTDIR
  ERRNO_CASE(ENOTDIR);
#endif

#ifdef ENOTEMPTY
# if ENOTEMPTY != EEXIST
  ERRNO_CASE(ENOTEMPTY);
# endif
#endif

#ifdef ENOTSOCK
  ERRNO_CASE(ENOTSOCK);
#endif

#ifdef ENOTSUP
  ERRNO_CASE(ENOTSUP);
#else
# ifdef EOPNOTSUPP
  ERRNO_CASE(EOPNOTSUPP);
# endif
#endif

#ifdef ENOTTY
  ERRNO_CASE(ENOTTY);
#endif

#ifdef ENXIO
  ERRNO_CASE(ENXIO);
#endif


#ifdef EOVERFLOW
  ERRNO_CASE(EOVERFLOW);
#endif

#ifdef EPERM
  ERRNO_CASE(EPERM);
#endif

#ifdef EPIPE
  ERRNO_CASE(EPIPE);
#endif

#ifdef EPROTO
  ERRNO_CASE(EPROTO);
#endif

#ifdef EPROTONOSUPPORT
  ERRNO_CASE(EPROTONOSUPPORT);
#endif

#ifdef EPROTOTYPE
  ERRNO_CASE(EPROTOTYPE);
#endif

#ifdef ERANGE
  ERRNO_CASE(ERANGE);
#endif

#ifdef EROFS
  ERRNO_CASE(EROFS);
#endif

#ifdef ESPIPE
  ERRNO_CASE(ESPIPE);
#endif

#ifdef ESRCH
  ERRNO_CASE(ESRCH);
#endif

#ifdef ESTALE
  ERRNO_CASE(ESTALE);
#endif

#ifdef ETIME
  ERRNO_CASE(ETIME);
#endif

#ifdef ETIMEDOUT
  ERRNO_CASE(ETIMEDOUT);
#endif

#ifdef ETXTBSY
  ERRNO_CASE(ETXTBSY);
#endif

#ifdef EXDEV
  ERRNO_CASE(EXDEV);
#endif

  default: return "";
  }
}

#define NODE_MODULE_CONTEXT_AWARE_INTERNAL(modname, regfunc)                  \
  NODE_MODULE_CONTEXT_AWARE_CPP(modname, regfunc, nullptr, NM_F_INTERNAL)

#define TRACING_CATEGORY_NODE "node"
#define TRACING_CATEGORY_NODE1(one)                                           \
    TRACING_CATEGORY_NODE ","                                                 \
    TRACING_CATEGORY_NODE "." #one
#define TRACING_CATEGORY_NODE2(one, two)                                      \
    TRACING_CATEGORY_NODE ","                                                 \
    TRACING_CATEGORY_NODE "." #one ","                                        \
    TRACING_CATEGORY_NODE "." #one "." #two

// Functions defined in node.cc that are exposed via the bootstrapper object

void Chdir(const v8::FunctionCallbackInfo<v8::Value>& args);
void CPUUsage(const v8::FunctionCallbackInfo<v8::Value>& args);
void Hrtime(const v8::FunctionCallbackInfo<v8::Value>& args);
void MemoryUsage(const v8::FunctionCallbackInfo<v8::Value>& args);
void RawDebug(const v8::FunctionCallbackInfo<v8::Value>& args);
void Umask(const v8::FunctionCallbackInfo<v8::Value>& args);

#if defined(__POSIX__) && !defined(__ANDROID__) && !defined(__CloudABI__)
void SetGid(const v8::FunctionCallbackInfo<v8::Value>& args);
void SetEGid(const v8::FunctionCallbackInfo<v8::Value>& args);
void SetUid(const v8::FunctionCallbackInfo<v8::Value>& args);
void SetEUid(const v8::FunctionCallbackInfo<v8::Value>& args);
void SetGroups(const v8::FunctionCallbackInfo<v8::Value>& args);
void InitGroups(const v8::FunctionCallbackInfo<v8::Value>& args);
#endif  // __POSIX__ && !defined(__ANDROID__) && !defined(__CloudABI__)

}  // namespace node

void napi_module_register_by_symbol(v8::Local<v8::Object> exports,
                                    v8::Local<v8::Value> module,
                                    v8::Local<v8::Context> context,
                                    napi_addon_register_func init);

#endif  // defined(NODE_WANT_INTERNALS) && NODE_WANT_INTERNALS

#endif  // SRC_NODE_INTERNALS_H_
