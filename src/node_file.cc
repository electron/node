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

#include "aliased_buffer.h"
#include "node_buffer.h"
#include "node_internals.h"
#include "node_stat_watcher.h"
#include "node_file.h"
#include "tracing/trace_event.h"

#include "req_wrap-inl.h"
#include "stream_base-inl.h"
#include "string_bytes.h"
#include "string_search.h"

#include <fcntl.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <string.h>
#include <errno.h>
#include <limits.h>

#if defined(__MINGW32__) || defined(_MSC_VER)
# include <io.h>
#endif

#include <memory>

namespace node {

namespace fs {

using v8::Array;
using v8::Context;
using v8::EscapableHandleScope;
using v8::Float64Array;
using v8::Function;
using v8::FunctionCallbackInfo;
using v8::FunctionTemplate;
using v8::HandleScope;
using v8::Int32;
using v8::Integer;
using v8::Isolate;
using v8::Local;
using v8::MaybeLocal;
using v8::Number;
using v8::Object;
using v8::ObjectTemplate;
using v8::Promise;
using v8::String;
using v8::Symbol;
using v8::Uint32;
using v8::Undefined;
using v8::Value;

#ifndef MIN
# define MIN(a, b) ((a) < (b) ? (a) : (b))
#endif

#define GET_OFFSET(a) ((a)->IsNumber() ? (a).As<Integer>()->Value() : -1)
#define TRACE_NAME(name) "fs.sync." #name
#define GET_TRACE_ENABLED                                                  \
  (*TRACE_EVENT_API_GET_CATEGORY_GROUP_ENABLED                             \
  (TRACING_CATEGORY_NODE2(fs, sync)) != 0)
#define FS_SYNC_TRACE_BEGIN(syscall, ...)                                  \
  if (GET_TRACE_ENABLED)                                                   \
  TRACE_EVENT_BEGIN(TRACING_CATEGORY_NODE2(fs, sync), TRACE_NAME(syscall), \
  ##__VA_ARGS__);
#define FS_SYNC_TRACE_END(syscall, ...)                                    \
  if (GET_TRACE_ENABLED)                                                   \
  TRACE_EVENT_END(TRACING_CATEGORY_NODE2(fs, sync), TRACE_NAME(syscall),   \
  ##__VA_ARGS__);

// We sometimes need to convert a C++ lambda function to a raw C-style function.
// This is helpful, because ReqWrap::Dispatch() does not recognize lambda
// functions, and thus does not wrap them properly.
typedef void(*uv_fs_callback_t)(uv_fs_t*);

// The FileHandle object wraps a file descriptor and will close it on garbage
// collection if necessary. If that happens, a process warning will be
// emitted (or a fatal exception will occur if the fd cannot be closed.)
FileHandle::FileHandle(Environment* env, int fd, Local<Object> obj)
    : AsyncWrap(env,
                obj.IsEmpty() ? env->fd_constructor_template()
                    ->NewInstance(env->context()).ToLocalChecked() : obj,
                AsyncWrap::PROVIDER_FILEHANDLE),
      StreamBase(env),
      fd_(fd) {
  MakeWeak();
  v8::PropertyAttribute attr =
      static_cast<v8::PropertyAttribute>(v8::ReadOnly | v8::DontDelete);
  object()->DefineOwnProperty(env->context(),
                              FIXED_ONE_BYTE_STRING(env->isolate(), "fd"),
                              Integer::New(env->isolate(), fd),
                              attr).FromJust();
}

void FileHandle::New(const FunctionCallbackInfo<Value>& args) {
  Environment* env = Environment::GetCurrent(args);
  CHECK(args.IsConstructCall());
  CHECK(args[0]->IsInt32());

  FileHandle* handle =
      new FileHandle(env, args[0].As<Int32>()->Value(), args.This());
  if (args[1]->IsNumber())
    handle->read_offset_ = args[1]->IntegerValue(env->context()).FromJust();
  if (args[2]->IsNumber())
    handle->read_length_ = args[2]->IntegerValue(env->context()).FromJust();
}

FileHandle::~FileHandle() {
  CHECK(!closing_);  // We should not be deleting while explicitly closing!
  Close();           // Close synchronously and emit warning
  CHECK(closed_);    // We have to be closed at the point
}


// Close the file descriptor if it hasn't already been closed. A process
// warning will be emitted using a SetImmediate to avoid calling back to
// JS during GC. If closing the fd fails at this point, a fatal exception
// will crash the process immediately.
inline void FileHandle::Close() {
  if (closed_) return;
  uv_fs_t req;
  int ret = uv_fs_close(env()->event_loop(), &req, fd_, nullptr);
  uv_fs_req_cleanup(&req);
  AfterClose();

  struct err_detail { int ret; int fd; };

  err_detail* detail = new err_detail { ret, fd_ };

  if (ret < 0) {
    // Do not unref this
    env()->SetImmediate([](Environment* env, void* data) {
      char msg[70];
      err_detail* detail = static_cast<err_detail*>(data);
      snprintf(msg, arraysize(msg),
              "Closing file descriptor %d on garbage collection failed",
              detail->fd);
      // This exception will end up being fatal for the process because
      // it is being thrown from within the SetImmediate handler and
      // there is no JS stack to bubble it to. In other words, tearing
      // down the process is the only reasonable thing we can do here.
      HandleScope handle_scope(env->isolate());
      env->ThrowUVException(detail->ret, "close", msg);
      delete detail;
    }, detail);
    return;
  }

  // If the close was successful, we still want to emit a process warning
  // to notify that the file descriptor was gc'd. We want to be noisy about
  // this because not explicitly closing the FileHandle is a bug.
  env()->SetUnrefImmediate([](Environment* env, void* data) {
    char msg[70];
    err_detail* detail = static_cast<err_detail*>(data);
    snprintf(msg, arraysize(msg),
            "Closing file descriptor %d on garbage collection",
            detail->fd);
    delete detail;
    ProcessEmitWarning(env, msg);
  }, detail);
}

void FileHandle::CloseReq::Resolve() {
  HandleScope scope(env()->isolate());
  InternalCallbackScope callback_scope(this);
  Local<Promise> promise = promise_.Get(env()->isolate());
  Local<Promise::Resolver> resolver = promise.As<Promise::Resolver>();
  resolver->Resolve(env()->context(), Undefined(env()->isolate())).FromJust();
}

void FileHandle::CloseReq::Reject(Local<Value> reason) {
  HandleScope scope(env()->isolate());
  InternalCallbackScope callback_scope(this);
  Local<Promise> promise = promise_.Get(env()->isolate());
  Local<Promise::Resolver> resolver = promise.As<Promise::Resolver>();
  resolver->Reject(env()->context(), reason).FromJust();
}

FileHandle* FileHandle::CloseReq::file_handle() {
  HandleScope scope(env()->isolate());
  Local<Value> val = ref_.Get(env()->isolate());
  Local<Object> obj = val.As<Object>();
  return Unwrap<FileHandle>(obj);
}

// Closes this FileHandle asynchronously and returns a Promise that will be
// resolved when the callback is invoked, or rejects with a UVException if
// there was a problem closing the fd. This is the preferred mechanism for
// closing the FD object even tho the object will attempt to close
// automatically on gc.
inline MaybeLocal<Promise> FileHandle::ClosePromise() {
  Isolate* isolate = env()->isolate();
  EscapableHandleScope scope(isolate);
  Local<Context> context = env()->context();
  auto maybe_resolver = Promise::Resolver::New(context);
  CHECK(!maybe_resolver.IsEmpty());
  Local<Promise::Resolver> resolver = maybe_resolver.ToLocalChecked();
  Local<Promise> promise = resolver.As<Promise>();
  CHECK(!reading_);
  if (!closed_ && !closing_) {
    closing_ = true;
    CloseReq* req = new CloseReq(env(), promise, object());
    auto AfterClose = uv_fs_callback_t{[](uv_fs_t* req) {
      CloseReq* close = static_cast<CloseReq*>(req->data);
      CHECK_NE(close, nullptr);
      close->file_handle()->AfterClose();
      Isolate* isolate = close->env()->isolate();
      if (req->result < 0) {
        close->Reject(UVException(isolate, req->result, "close"));
      } else {
        close->Resolve();
      }
      delete close;
    }};
    int ret = req->Dispatch(uv_fs_close, fd_, AfterClose);
    if (ret < 0) {
      req->Reject(UVException(isolate, ret, "close"));
      delete req;
    }
  } else {
    // Already closed. Just reject the promise immediately
    resolver->Reject(context, UVException(isolate, UV_EBADF, "close"))
        .FromJust();
  }
  return scope.Escape(promise);
}

void FileHandle::Close(const FunctionCallbackInfo<Value>& args) {
  FileHandle* fd;
  ASSIGN_OR_RETURN_UNWRAP(&fd, args.Holder());
  args.GetReturnValue().Set(fd->ClosePromise().ToLocalChecked());
}


void FileHandle::ReleaseFD(const FunctionCallbackInfo<Value>& args) {
  FileHandle* fd;
  ASSIGN_OR_RETURN_UNWRAP(&fd, args.Holder());
  // Just act as if this FileHandle has been closed.
  fd->AfterClose();
}


void FileHandle::AfterClose() {
  closing_ = false;
  closed_ = true;
  if (reading_ && !persistent().IsEmpty())
    EmitRead(UV_EOF);
}


FileHandleReadWrap::FileHandleReadWrap(FileHandle* handle, Local<Object> obj)
  : ReqWrap(handle->env(), obj, AsyncWrap::PROVIDER_FSREQWRAP),
    file_handle_(handle) {}

int FileHandle::ReadStart() {
  if (!IsAlive() || IsClosing())
    return UV_EOF;

  reading_ = true;

  if (current_read_)
    return 0;

  std::unique_ptr<FileHandleReadWrap> read_wrap;

  if (read_length_ == 0) {
    EmitRead(UV_EOF);
    return 0;
  }

  {
    // Create a new FileHandleReadWrap or re-use one.
    // Either way, we need these two scopes for AsyncReset() or otherwise
    // for creating the new instance.
    HandleScope handle_scope(env()->isolate());
    AsyncHooks::DefaultTriggerAsyncIdScope trigger_scope(this);

    auto& freelist = env()->file_handle_read_wrap_freelist();
    if (freelist.size() > 0) {
      read_wrap = std::move(freelist.back());
      freelist.pop_back();
      read_wrap->AsyncReset();
      read_wrap->file_handle_ = this;
    } else {
      Local<Object> wrap_obj = env()->filehandlereadwrap_template()
          ->NewInstance(env()->context()).ToLocalChecked();
      read_wrap.reset(new FileHandleReadWrap(this, wrap_obj));
    }
  }
  int64_t recommended_read = 65536;
  if (read_length_ >= 0 && read_length_ <= recommended_read)
    recommended_read = read_length_;

  read_wrap->buffer_ = EmitAlloc(recommended_read);

  current_read_ = std::move(read_wrap);

  current_read_->Dispatch(uv_fs_read,
                          fd_,
                          &current_read_->buffer_,
                          1,
                          read_offset_,
                          uv_fs_callback_t{[](uv_fs_t* req) {
    FileHandle* handle;
    {
      FileHandleReadWrap* req_wrap = FileHandleReadWrap::from_req(req);
      handle = req_wrap->file_handle_;
      CHECK_EQ(handle->current_read_.get(), req_wrap);
    }

    // ReadStart() checks whether current_read_ is set to determine whether
    // a read is in progress. Moving it into a local variable makes sure that
    // the ReadStart() call below doesn't think we're still actively reading.
    std::unique_ptr<FileHandleReadWrap> read_wrap =
        std::move(handle->current_read_);

    int result = req->result;
    uv_buf_t buffer = read_wrap->buffer_;

    uv_fs_req_cleanup(req);

    // Push the read wrap back to the freelist, or let it be destroyed
    // once we’re exiting the current scope.
    constexpr size_t wanted_freelist_fill = 100;
    auto& freelist = handle->env()->file_handle_read_wrap_freelist();
    if (freelist.size() < wanted_freelist_fill) {
      read_wrap->Reset();
      freelist.emplace_back(std::move(read_wrap));
    }

    if (result >= 0) {
      // Read at most as many bytes as we originally planned to.
      if (handle->read_length_ >= 0 && handle->read_length_ < result)
        result = handle->read_length_;

      // If we read data and we have an expected length, decrease it by
      // how much we have read.
      if (handle->read_length_ >= 0)
        handle->read_length_ -= result;

      // If we have an offset, increase it by how much we have read.
      if (handle->read_offset_ >= 0)
        handle->read_offset_ += result;
    }

    // Reading 0 bytes from a file always means EOF, or that we reached
    // the end of the requested range.
    if (result == 0)
      result = UV_EOF;

    handle->EmitRead(result, buffer);

    // Start over, if EmitRead() didn’t tell us to stop.
    if (handle->reading_)
      handle->ReadStart();
  }});

  return 0;
}

int FileHandle::ReadStop() {
  reading_ = false;
  return 0;
}

typedef SimpleShutdownWrap<ReqWrap<uv_fs_t>> FileHandleCloseWrap;

ShutdownWrap* FileHandle::CreateShutdownWrap(Local<Object> object) {
  return new FileHandleCloseWrap(this, object);
}

int FileHandle::DoShutdown(ShutdownWrap* req_wrap) {
  FileHandleCloseWrap* wrap = static_cast<FileHandleCloseWrap*>(req_wrap);
  closing_ = true;
  wrap->Dispatch(uv_fs_close, fd_, uv_fs_callback_t{[](uv_fs_t* req) {
    FileHandleCloseWrap* wrap = static_cast<FileHandleCloseWrap*>(
        FileHandleCloseWrap::from_req(req));
    FileHandle* handle = static_cast<FileHandle*>(wrap->stream());
    handle->AfterClose();

    int result = req->result;
    uv_fs_req_cleanup(req);
    wrap->Done(result);
  }});

  return 0;
}


void FSReqWrap::Reject(Local<Value> reject) {
  MakeCallback(env()->oncomplete_string(), 1, &reject);
}

void FSReqWrap::ResolveStat(const uv_stat_t* stat) {
  Resolve(node::FillGlobalStatsArray(env(), stat));
}

void FSReqWrap::Resolve(Local<Value> value) {
  Local<Value> argv[2] {
    Null(env()->isolate()),
    value
  };
  MakeCallback(env()->oncomplete_string(),
               value->IsUndefined() ? 1 : arraysize(argv),
               argv);
}

void FSReqWrap::SetReturnValue(const FunctionCallbackInfo<Value>& args) {
  args.GetReturnValue().SetUndefined();
}

void NewFSReqWrap(const FunctionCallbackInfo<Value>& args) {
  CHECK(args.IsConstructCall());
  Environment* env = Environment::GetCurrent(args.GetIsolate());
  new FSReqWrap(env, args.This());
}

FSReqAfterScope::FSReqAfterScope(FSReqBase* wrap, uv_fs_t* req)
    : wrap_(wrap),
      req_(req),
      handle_scope_(wrap->env()->isolate()),
      context_scope_(wrap->env()->context()) {
  CHECK_EQ(wrap_->req(), req);
}

FSReqAfterScope::~FSReqAfterScope() {
  uv_fs_req_cleanup(wrap_->req());
  delete wrap_;
}

// TODO(joyeecheung): create a normal context object, and
// construct the actual errors in the JS land using the context.
// The context should include fds for some fs APIs, currently they are
// missing in the error messages. The path, dest, syscall, fd, .etc
// can be put into the context before the binding is even invoked,
// the only information that has to come from the C++ layer is the
// error number (and possibly the syscall for abstraction),
// which is also why the errors should have been constructed
// in JS for more flexibility.
void FSReqAfterScope::Reject(uv_fs_t* req) {
  wrap_->Reject(UVException(wrap_->env()->isolate(),
                            req->result,
                            wrap_->syscall(),
                            nullptr,
                            req->path,
                            wrap_->data()));
}

bool FSReqAfterScope::Proceed() {
  if (req_->result < 0) {
    Reject(req_);
    return false;
  }
  return true;
}

void AfterNoArgs(uv_fs_t* req) {
  FSReqBase* req_wrap = static_cast<FSReqBase*>(req->data);
  FSReqAfterScope after(req_wrap, req);

  if (after.Proceed())
    req_wrap->Resolve(Undefined(req_wrap->env()->isolate()));
}

void AfterStat(uv_fs_t* req) {
  FSReqBase* req_wrap = static_cast<FSReqBase*>(req->data);
  FSReqAfterScope after(req_wrap, req);

  if (after.Proceed()) {
    req_wrap->ResolveStat(&req->statbuf);
  }
}

void AfterInteger(uv_fs_t* req) {
  FSReqBase* req_wrap = static_cast<FSReqBase*>(req->data);
  FSReqAfterScope after(req_wrap, req);

  if (after.Proceed())
    req_wrap->Resolve(Integer::New(req_wrap->env()->isolate(), req->result));
}

void AfterOpenFileHandle(uv_fs_t* req) {
  FSReqWrap* req_wrap = static_cast<FSReqWrap*>(req->data);
  FSReqAfterScope after(req_wrap, req);

  if (after.Proceed()) {
    FileHandle* fd = new FileHandle(req_wrap->env(), req->result);
    req_wrap->Resolve(fd->object());
  }
}

void AfterStringPath(uv_fs_t* req) {
  FSReqBase* req_wrap = static_cast<FSReqBase*>(req->data);
  FSReqAfterScope after(req_wrap, req);

  MaybeLocal<Value> link;
  Local<Value> error;

  if (after.Proceed()) {
    link = StringBytes::Encode(req_wrap->env()->isolate(),
                               static_cast<const char*>(req->path),
                               req_wrap->encoding(),
                               &error);
    if (link.IsEmpty())
      req_wrap->Reject(error);
    else
      req_wrap->Resolve(link.ToLocalChecked());
  }
}

void AfterStringPtr(uv_fs_t* req) {
  FSReqBase* req_wrap = static_cast<FSReqBase*>(req->data);
  FSReqAfterScope after(req_wrap, req);

  MaybeLocal<Value> link;
  Local<Value> error;

  if (after.Proceed()) {
    link = StringBytes::Encode(req_wrap->env()->isolate(),
                               static_cast<const char*>(req->ptr),
                               req_wrap->encoding(),
                               &error);
    if (link.IsEmpty())
      req_wrap->Reject(error);
    else
      req_wrap->Resolve(link.ToLocalChecked());
  }
}

void AfterScanDir(uv_fs_t* req) {
  FSReqBase* req_wrap = static_cast<FSReqBase*>(req->data);
  FSReqAfterScope after(req_wrap, req);

  if (after.Proceed()) {
    Environment* env = req_wrap->env();
    Local<Value> error;
    int r;
    Local<Array> names = Array::New(env->isolate(), 0);
    Local<Function> fn = env->push_values_to_array_function();
    Local<Value> name_argv[NODE_PUSH_VAL_TO_ARRAY_MAX];
    size_t name_idx = 0;

    for (int i = 0; ; i++) {
      uv_dirent_t ent;

      r = uv_fs_scandir_next(req, &ent);
      if (r == UV_EOF)
        break;
      if (r != 0) {
        return req_wrap->Reject(
            UVException(r, nullptr, req_wrap->syscall(),
                        static_cast<const char*>(req->path)));
      }

      MaybeLocal<Value> filename =
          StringBytes::Encode(env->isolate(),
                              ent.name,
                              req_wrap->encoding(),
                              &error);
      if (filename.IsEmpty())
        return req_wrap->Reject(error);

      name_argv[name_idx++] = filename.ToLocalChecked();

      if (name_idx >= arraysize(name_argv)) {
        fn->Call(env->context(), names, name_idx, name_argv)
            .ToLocalChecked();
        name_idx = 0;
      }
    }

    if (name_idx > 0) {
      fn->Call(env->context(), names, name_idx, name_argv)
          .ToLocalChecked();
    }

    req_wrap->Resolve(names);
  }
}


// This class is only used on sync fs calls.
// For async calls FSReqWrap is used.
class FSReqWrapSync {
 public:
  FSReqWrapSync() {}
  ~FSReqWrapSync() { uv_fs_req_cleanup(&req); }
  uv_fs_t req;

 private:
  DISALLOW_COPY_AND_ASSIGN(FSReqWrapSync);
};

// Returns nullptr if the operation fails from the start.
template <typename Func, typename... Args>
inline FSReqBase* AsyncDestCall(Environment* env,
    FSReqBase* req_wrap,
    const FunctionCallbackInfo<Value>& args,
    const char* syscall, const char* dest, size_t len,
    enum encoding enc, uv_fs_cb after, Func fn, Args... fn_args) {
  CHECK_NE(req_wrap, nullptr);
  req_wrap->Init(syscall, dest, len, enc);
  int err = req_wrap->Dispatch(fn, fn_args..., after);
  if (err < 0) {
    uv_fs_t* uv_req = req_wrap->req();
    uv_req->result = err;
    uv_req->path = nullptr;
    after(uv_req);  // after may delete req_wrap if there is an error
    req_wrap = nullptr;
  } else {
    req_wrap->SetReturnValue(args);
  }

  return req_wrap;
}

// Returns nullptr if the operation fails from the start.
template <typename Func, typename... Args>
inline FSReqBase* AsyncCall(Environment* env,
    FSReqBase* req_wrap,
    const FunctionCallbackInfo<Value>& args,
    const char* syscall, enum encoding enc,
    uv_fs_cb after, Func fn, Args... fn_args) {
  return AsyncDestCall(env, req_wrap, args,
                       syscall, nullptr, 0, enc,
                       after, fn, fn_args...);
}

// Template counterpart of SYNC_CALL, except that it only puts
// the error number and the syscall in the context instead of
// creating an error in the C++ land.
// ctx must be checked using value->IsObject() before being passed.
template <typename Func, typename... Args>
inline int SyncCall(Environment* env, Local<Value> ctx, FSReqWrapSync* req_wrap,
    const char* syscall, Func fn, Args... args) {
  env->PrintSyncTrace();
  int err = fn(env->event_loop(), &(req_wrap->req), args..., nullptr);
  if (err < 0) {
    Local<Context> context = env->context();
    Local<Object> ctx_obj = ctx.As<Object>();
    Isolate *isolate = env->isolate();
    ctx_obj->Set(context,
             env->errno_string(),
             Integer::New(isolate, err)).FromJust();
    ctx_obj->Set(context,
             env->syscall_string(),
             OneByteString(isolate, syscall)).FromJust();
  }
  return err;
}

inline FSReqBase* GetReqWrap(Environment* env, Local<Value> value) {
  if (value->IsObject()) {
    return Unwrap<FSReqBase>(value.As<Object>());
  } else if (value->StrictEquals(env->fs_use_promises_symbol())) {
    return new FSReqPromise<double, Float64Array>(env);
  }
  return nullptr;
}

void Access(const FunctionCallbackInfo<Value>& args) {
  Environment* env = Environment::GetCurrent(args.GetIsolate());
  HandleScope scope(env->isolate());

  const int argc = args.Length();
  CHECK_GE(argc, 2);

  CHECK(args[1]->IsInt32());
  int mode = args[1].As<Int32>()->Value();

  BufferValue path(env->isolate(), args[0]);
  CHECK_NE(*path, nullptr);

  FSReqBase* req_wrap_async = GetReqWrap(env, args[2]);
  if (req_wrap_async != nullptr) {  // access(path, mode, req)
    AsyncCall(env, req_wrap_async, args, "access", UTF8, AfterNoArgs,
              uv_fs_access, *path, mode);
  } else {  // access(path, mode, undefined, ctx)
    CHECK_EQ(argc, 4);
    FSReqWrapSync req_wrap_sync;
    FS_SYNC_TRACE_BEGIN(access);
    SyncCall(env, args[3], &req_wrap_sync, "access", uv_fs_access, *path, mode);
    FS_SYNC_TRACE_END(access);
  }
}


void Close(const FunctionCallbackInfo<Value>& args) {
  Environment* env = Environment::GetCurrent(args);

  const int argc = args.Length();
  CHECK_GE(argc, 2);

  CHECK(args[0]->IsInt32());
  int fd = args[0].As<Int32>()->Value();

  FSReqBase* req_wrap_async = GetReqWrap(env, args[1]);
  if (req_wrap_async != nullptr) {  // close(fd, req)
    AsyncCall(env, req_wrap_async, args, "close", UTF8, AfterNoArgs,
              uv_fs_close, fd);
  } else {  // close(fd, undefined, ctx)
    CHECK_EQ(argc, 3);
    FSReqWrapSync req_wrap_sync;
    FS_SYNC_TRACE_BEGIN(close);
    SyncCall(env, args[2], &req_wrap_sync, "close", uv_fs_close, fd);
    FS_SYNC_TRACE_END(close);
  }
}


// Used to speed up module loading.  Returns the contents of the file as
// a string or undefined when the file cannot be opened or "main" is not found
// in the file.
static void InternalModuleReadJSON(const FunctionCallbackInfo<Value>& args) {
  Environment* env = Environment::GetCurrent(args);
  uv_loop_t* loop = env->event_loop();

  CHECK(args[0]->IsString());
  node::Utf8Value path(env->isolate(), args[0]);

  if (strlen(*path) != path.length())
    return;  // Contains a nul byte.

  uv_fs_t open_req;
  const int fd = uv_fs_open(loop, &open_req, *path, O_RDONLY, 0, nullptr);
  uv_fs_req_cleanup(&open_req);

  if (fd < 0) {
    return;
  }

  std::shared_ptr<void> defer_close(nullptr, [fd, loop] (...) {
    uv_fs_t close_req;
    CHECK_EQ(0, uv_fs_close(loop, &close_req, fd, nullptr));
    uv_fs_req_cleanup(&close_req);
  });

  const size_t kBlockSize = 32 << 10;
  std::vector<char> chars;
  int64_t offset = 0;
  ssize_t numchars;
  do {
    const size_t start = chars.size();
    chars.resize(start + kBlockSize);

    uv_buf_t buf;
    buf.base = &chars[start];
    buf.len = kBlockSize;

    uv_fs_t read_req;
    numchars = uv_fs_read(loop, &read_req, fd, &buf, 1, offset, nullptr);
    uv_fs_req_cleanup(&read_req);

    if (numchars < 0)
      return;

    offset += numchars;
  } while (static_cast<size_t>(numchars) == kBlockSize);

  size_t start = 0;
  if (offset >= 3 && 0 == memcmp(&chars[0], "\xEF\xBB\xBF", 3)) {
    start = 3;  // Skip UTF-8 BOM.
  }

  const size_t size = offset - start;
  if (size == 0 || size == SearchString(&chars[start], size, "\"main\"")) {
    return;
  } else {
    Local<String> chars_string =
        String::NewFromUtf8(env->isolate(),
                            &chars[start],
                            String::kNormalString,
                            size);
    args.GetReturnValue().Set(chars_string);
  }
}

// Used to speed up module loading.  Returns 0 if the path refers to
// a file, 1 when it's a directory or < 0 on error (usually -ENOENT.)
// The speedup comes from not creating thousands of Stat and Error objects.
static void InternalModuleStat(const FunctionCallbackInfo<Value>& args) {
  Environment* env = Environment::GetCurrent(args);

  CHECK(args[0]->IsString());
  node::Utf8Value path(env->isolate(), args[0]);

  uv_fs_t req;
  int rc = uv_fs_stat(env->event_loop(), &req, *path, nullptr);
  if (rc == 0) {
    const uv_stat_t* const s = static_cast<const uv_stat_t*>(req.ptr);
    rc = !!(s->st_mode & S_IFDIR);
  }
  uv_fs_req_cleanup(&req);

  args.GetReturnValue().Set(rc);
}

static void Stat(const FunctionCallbackInfo<Value>& args) {
  Environment* env = Environment::GetCurrent(args);

  const int argc = args.Length();
  CHECK_GE(argc, 2);

  BufferValue path(env->isolate(), args[0]);
  CHECK_NE(*path, nullptr);

  FSReqBase* req_wrap_async = GetReqWrap(env, args[1]);
  if (req_wrap_async != nullptr) {  // stat(path, req)
    AsyncCall(env, req_wrap_async, args, "stat", UTF8, AfterStat,
              uv_fs_stat, *path);
  } else {  // stat(path, undefined, ctx)
    CHECK_EQ(argc, 3);
    FSReqWrapSync req_wrap_sync;
    FS_SYNC_TRACE_BEGIN(stat);
    int err = SyncCall(env, args[2], &req_wrap_sync, "stat", uv_fs_stat, *path);
    FS_SYNC_TRACE_END(stat);
    if (err != 0) {
      return;  // error info is in ctx
    }

    Local<Value> arr = node::FillGlobalStatsArray(env,
        static_cast<const uv_stat_t*>(req_wrap_sync.req.ptr));
    args.GetReturnValue().Set(arr);
  }
}

static void StatNoException(const FunctionCallbackInfo<Value>& args) {
  Environment* env = Environment::GetCurrent(args.GetIsolate());
  HandleScope scope(env->isolate());

  if (args.Length() < 1 || !args[0]->IsString()) {
    args.GetReturnValue().Set(v8::Boolean::New(env->isolate(), false));
    return;
  }

  String::Utf8Value path(args[0]);

  FSReqWrapSync req_wrap;
  int result = uv_fs_stat(uv_default_loop(), &req_wrap.req, *path, NULL);
  if (result < 0) {
    args.GetReturnValue().Set(v8::Boolean::New(env->isolate(), false));
  } else {
    Local<Value> arr = node::FillGlobalStatsArray(env,
        static_cast<const uv_stat_t*>(req_wrap.req.ptr));
    args.GetReturnValue().Set(arr);
  }
}

static void LStat(const FunctionCallbackInfo<Value>& args) {
  Environment* env = Environment::GetCurrent(args);

  const int argc = args.Length();
  CHECK_GE(argc, 2);

  BufferValue path(env->isolate(), args[0]);
  CHECK_NE(*path, nullptr);

  FSReqBase* req_wrap_async = GetReqWrap(env, args[1]);
  if (req_wrap_async != nullptr) {  // lstat(path, req)
    AsyncCall(env, req_wrap_async, args, "lstat", UTF8, AfterStat,
              uv_fs_lstat, *path);
  } else {  // lstat(path, undefined, ctx)
    CHECK_EQ(argc, 3);
    FSReqWrapSync req_wrap_sync;
    FS_SYNC_TRACE_BEGIN(lstat);
    int err = SyncCall(env, args[2], &req_wrap_sync, "lstat", uv_fs_lstat,
                       *path);
    FS_SYNC_TRACE_END(lstat);
    if (err != 0) {
      return;  // error info is in ctx
    }

    Local<Value> arr = node::FillGlobalStatsArray(env,
        static_cast<const uv_stat_t*>(req_wrap_sync.req.ptr));
    args.GetReturnValue().Set(arr);
  }
}

static void LStatNoException(const FunctionCallbackInfo<Value>& args) {
  Environment* env = Environment::GetCurrent(args.GetIsolate());
  HandleScope scope(env->isolate());

  String::Utf8Value path(args[0]);

  if (args.Length() < 1 || !args[0]->IsString()) {
    args.GetReturnValue().Set(v8::Boolean::New(env->isolate(), false));
    return;
  }

  FSReqWrapSync req_wrap;
  int result = uv_fs_lstat(uv_default_loop(), &req_wrap.req, *path, NULL);
  if (result < 0) {
    args.GetReturnValue().Set(v8::Boolean::New(env->isolate(), false));
  } else {
    Local<Value> arr = node::FillGlobalStatsArray(env,
        static_cast<const uv_stat_t*>(req_wrap.req.ptr));
    args.GetReturnValue().Set(arr);
  }
}

static void FStat(const FunctionCallbackInfo<Value>& args) {
  Environment* env = Environment::GetCurrent(args);

  const int argc = args.Length();
  CHECK_GE(argc, 2);

  CHECK(args[0]->IsInt32());
  int fd = args[0].As<Int32>()->Value();

  FSReqBase* req_wrap_async = GetReqWrap(env, args[1]);
  if (req_wrap_async != nullptr) {  // fstat(fd, req)
    AsyncCall(env, req_wrap_async, args, "fstat", UTF8, AfterStat,
              uv_fs_fstat, fd);
  } else {  // fstat(fd, undefined, ctx)
    CHECK_EQ(argc, 3);
    FSReqWrapSync req_wrap_sync;
    FS_SYNC_TRACE_BEGIN(fstat);
    int err = SyncCall(env, args[2], &req_wrap_sync, "fstat", uv_fs_fstat, fd);
    FS_SYNC_TRACE_END(fstat);
    if (err != 0) {
      return;  // error info is in ctx
    }

    Local<Value> arr = node::FillGlobalStatsArray(env,
        static_cast<const uv_stat_t*>(req_wrap_sync.req.ptr));
    args.GetReturnValue().Set(arr);
  }
}

static void Symlink(const FunctionCallbackInfo<Value>& args) {
  Environment* env = Environment::GetCurrent(args);

  int argc = args.Length();
  CHECK_GE(argc, 4);

  BufferValue target(env->isolate(), args[0]);
  CHECK_NE(*target, nullptr);
  BufferValue path(env->isolate(), args[1]);
  CHECK_NE(*path, nullptr);

  CHECK(args[2]->IsInt32());
  int flags = args[2].As<Int32>()->Value();

  FSReqBase* req_wrap_async = GetReqWrap(env, args[3]);
  if (req_wrap_async != nullptr) {  // symlink(target, path, flags, req)
    AsyncDestCall(env, req_wrap_async, args, "symlink", *path, path.length(),
                  UTF8, AfterNoArgs, uv_fs_symlink, *target, *path, flags);
  } else {  // symlink(target, path, flags, undefinec, ctx)
    CHECK_EQ(argc, 5);
    FSReqWrapSync req_wrap_sync;
    FS_SYNC_TRACE_BEGIN(symlink);
    SyncCall(env, args[4], &req_wrap_sync, "symlink",
             uv_fs_symlink, *target, *path, flags);
    FS_SYNC_TRACE_END(symlink);
  }
}

static void Link(const FunctionCallbackInfo<Value>& args) {
  Environment* env = Environment::GetCurrent(args);

  int argc = args.Length();
  CHECK_GE(argc, 3);

  BufferValue src(env->isolate(), args[0]);
  CHECK_NE(*src, nullptr);

  BufferValue dest(env->isolate(), args[1]);
  CHECK_NE(*dest, nullptr);

  FSReqBase* req_wrap_async = GetReqWrap(env, args[2]);
  if (req_wrap_async != nullptr) {  // link(src, dest, req)
    AsyncDestCall(env, req_wrap_async, args, "link", *dest, dest.length(), UTF8,
                  AfterNoArgs, uv_fs_link, *src, *dest);
  } else {  // link(src, dest)
    CHECK_EQ(argc, 4);
    FSReqWrapSync req_wrap_sync;
    FS_SYNC_TRACE_BEGIN(link);
    SyncCall(env, args[3], &req_wrap_sync, "link",
             uv_fs_link, *src, *dest);
    FS_SYNC_TRACE_END(link);
  }
}

static void ReadLink(const FunctionCallbackInfo<Value>& args) {
  Environment* env = Environment::GetCurrent(args);

  int argc = args.Length();
  CHECK_GE(argc, 3);

  BufferValue path(env->isolate(), args[0]);
  CHECK_NE(*path, nullptr);

  const enum encoding encoding = ParseEncoding(env->isolate(), args[1], UTF8);

  FSReqBase* req_wrap_async = GetReqWrap(env, args[2]);
  if (req_wrap_async != nullptr) {  // readlink(path, encoding, req)
    AsyncCall(env, req_wrap_async, args, "readlink", encoding, AfterStringPtr,
              uv_fs_readlink, *path);
  } else {
    CHECK_EQ(argc, 4);
    FSReqWrapSync req_wrap_sync;
    FS_SYNC_TRACE_BEGIN(readlink);
    int err = SyncCall(env, args[3], &req_wrap_sync, "readlink",
                       uv_fs_readlink, *path);
    FS_SYNC_TRACE_END(readlink);
    if (err < 0) {
      return;  // syscall failed, no need to continue, error info is in ctx
    }
    const char* link_path = static_cast<const char*>(req_wrap_sync.req.ptr);

    Local<Value> error;
    MaybeLocal<Value> rc = StringBytes::Encode(env->isolate(),
                                               link_path,
                                               encoding,
                                               &error);
    if (rc.IsEmpty()) {
      Local<Object> ctx = args[3].As<Object>();
      ctx->Set(env->context(), env->error_string(), error).FromJust();
      return;
    }

    args.GetReturnValue().Set(rc.ToLocalChecked());
  }
}

static void Rename(const FunctionCallbackInfo<Value>& args) {
  Environment* env = Environment::GetCurrent(args);

  int argc = args.Length();
  CHECK_GE(argc, 3);

  BufferValue old_path(env->isolate(), args[0]);
  CHECK_NE(*old_path, nullptr);
  BufferValue new_path(env->isolate(), args[1]);
  CHECK_NE(*new_path, nullptr);

  FSReqBase* req_wrap_async = GetReqWrap(env, args[2]);
  if (req_wrap_async != nullptr) {
    AsyncDestCall(env, req_wrap_async, args, "rename", *new_path,
                  new_path.length(), UTF8, AfterNoArgs, uv_fs_rename,
                  *old_path, *new_path);
  } else {
    CHECK_EQ(argc, 4);
    FSReqWrapSync req_wrap_sync;
    FS_SYNC_TRACE_BEGIN(rename);
    SyncCall(env, args[3], &req_wrap_sync, "rename", uv_fs_rename,
             *old_path, *new_path);
    FS_SYNC_TRACE_END(rename);
  }
}

static void FTruncate(const FunctionCallbackInfo<Value>& args) {
  Environment* env = Environment::GetCurrent(args);

  const int argc = args.Length();
  CHECK_GE(argc, 3);

  CHECK(args[0]->IsInt32());
  const int fd = args[0].As<Int32>()->Value();

  CHECK(args[1]->IsNumber());
  const int64_t len = args[1].As<Integer>()->Value();

  FSReqBase* req_wrap_async = GetReqWrap(env, args[2]);
  if (req_wrap_async != nullptr) {
    AsyncCall(env, req_wrap_async, args, "ftruncate", UTF8, AfterNoArgs,
              uv_fs_ftruncate, fd, len);
  } else {
    CHECK_EQ(argc, 4);
    FSReqWrapSync req_wrap_sync;
    FS_SYNC_TRACE_BEGIN(ftruncate);
    SyncCall(env, args[3], &req_wrap_sync, "ftruncate", uv_fs_ftruncate, fd,
             len);
    FS_SYNC_TRACE_END(ftruncate);
  }
}

static void Fdatasync(const FunctionCallbackInfo<Value>& args) {
  Environment* env = Environment::GetCurrent(args);

  const int argc = args.Length();
  CHECK_GE(argc, 2);

  CHECK(args[0]->IsInt32());
  const int fd = args[0].As<Int32>()->Value();

  FSReqBase* req_wrap_async = GetReqWrap(env, args[1]);
  if (req_wrap_async != nullptr) {
    AsyncCall(env, req_wrap_async, args, "fdatasync", UTF8, AfterNoArgs,
              uv_fs_fdatasync, fd);
  } else {
    CHECK_EQ(argc, 3);
    FSReqWrapSync req_wrap_sync;
    FS_SYNC_TRACE_BEGIN(fdatasync);
    SyncCall(env, args[2], &req_wrap_sync, "fdatasync", uv_fs_fdatasync, fd);
    FS_SYNC_TRACE_END(fdatasync);
  }
}

static void Fsync(const FunctionCallbackInfo<Value>& args) {
  Environment* env = Environment::GetCurrent(args);

  const int argc = args.Length();
  CHECK_GE(argc, 2);

  CHECK(args[0]->IsInt32());
  const int fd = args[0].As<Int32>()->Value();

  FSReqBase* req_wrap_async = GetReqWrap(env, args[1]);
  if (req_wrap_async != nullptr) {
    AsyncCall(env, req_wrap_async, args, "fsync", UTF8, AfterNoArgs,
              uv_fs_fsync, fd);
  } else {
    CHECK_EQ(argc, 3);
    FSReqWrapSync req_wrap_sync;
    FS_SYNC_TRACE_BEGIN(fsync);
    SyncCall(env, args[2], &req_wrap_sync, "fsync", uv_fs_fsync, fd);
    FS_SYNC_TRACE_END(fsync);
  }
}

static void Unlink(const FunctionCallbackInfo<Value>& args) {
  Environment* env = Environment::GetCurrent(args);

  const int argc = args.Length();
  CHECK_GE(argc, 2);

  BufferValue path(env->isolate(), args[0]);
  CHECK_NE(*path, nullptr);

  FSReqBase* req_wrap_async = GetReqWrap(env, args[1]);
  if (req_wrap_async != nullptr) {
    AsyncCall(env, req_wrap_async, args, "unlink", UTF8, AfterNoArgs,
              uv_fs_unlink, *path);
  } else {
    CHECK_EQ(argc, 3);
    FSReqWrapSync req_wrap_sync;
    FS_SYNC_TRACE_BEGIN(unlink);
    SyncCall(env, args[2], &req_wrap_sync, "unlink", uv_fs_unlink, *path);
    FS_SYNC_TRACE_END(unlink);
  }
}

static void RMDir(const FunctionCallbackInfo<Value>& args) {
  Environment* env = Environment::GetCurrent(args);

  const int argc = args.Length();
  CHECK_GE(argc, 2);

  BufferValue path(env->isolate(), args[0]);
  CHECK_NE(*path, nullptr);

  FSReqBase* req_wrap_async = GetReqWrap(env, args[1]);  // rmdir(path, req)
  if (req_wrap_async != nullptr) {
    AsyncCall(env, req_wrap_async, args, "rmdir", UTF8, AfterNoArgs,
              uv_fs_rmdir, *path);
  } else {  // rmdir(path, undefined, ctx)
    CHECK_EQ(argc, 3);
    FSReqWrapSync req_wrap_sync;
    FS_SYNC_TRACE_BEGIN(rmdir);
    SyncCall(env, args[2], &req_wrap_sync, "rmdir",
             uv_fs_rmdir, *path);
    FS_SYNC_TRACE_END(rmdir);
  }
}

static void MKDir(const FunctionCallbackInfo<Value>& args) {
  Environment* env = Environment::GetCurrent(args);

  const int argc = args.Length();
  CHECK_GE(argc, 3);

  BufferValue path(env->isolate(), args[0]);
  CHECK_NE(*path, nullptr);

  CHECK(args[1]->IsInt32());
  const int mode = args[1].As<Int32>()->Value();

  FSReqBase* req_wrap_async = GetReqWrap(env, args[2]);
  if (req_wrap_async != nullptr) {  // mkdir(path, mode, req)
    AsyncCall(env, req_wrap_async, args, "mkdir", UTF8, AfterNoArgs,
              uv_fs_mkdir, *path, mode);
  } else {  // mkdir(path, mode, undefined, ctx)
    CHECK_EQ(argc, 4);
    FSReqWrapSync req_wrap_sync;
    FS_SYNC_TRACE_BEGIN(mkdir);
    SyncCall(env, args[3], &req_wrap_sync, "mkdir",
             uv_fs_mkdir, *path, mode);
    FS_SYNC_TRACE_END(mkdir);
  }
}

static void RealPath(const FunctionCallbackInfo<Value>& args) {
  Environment* env = Environment::GetCurrent(args);

  const int argc = args.Length();
  CHECK_GE(argc, 3);

  BufferValue path(env->isolate(), args[0]);
  CHECK_NE(*path, nullptr);

  const enum encoding encoding = ParseEncoding(env->isolate(), args[1], UTF8);

  FSReqBase* req_wrap_async = GetReqWrap(env, args[2]);
  if (req_wrap_async != nullptr) {  // realpath(path, encoding, req)
    AsyncCall(env, req_wrap_async, args, "realpath", encoding, AfterStringPtr,
              uv_fs_realpath, *path);
  } else {  // realpath(path, encoding, undefined, ctx)
    CHECK_EQ(argc, 4);
    FSReqWrapSync req_wrap_sync;
    FS_SYNC_TRACE_BEGIN(realpath);
    int err = SyncCall(env, args[3], &req_wrap_sync, "realpath",
                       uv_fs_realpath, *path);
    FS_SYNC_TRACE_END(realpath);
    if (err < 0) {
      return;  // syscall failed, no need to continue, error info is in ctx
    }

    const char* link_path = static_cast<const char*>(req_wrap_sync.req.ptr);

    Local<Value> error;
    MaybeLocal<Value> rc = StringBytes::Encode(env->isolate(),
                                               link_path,
                                               encoding,
                                               &error);
    if (rc.IsEmpty()) {
      Local<Object> ctx = args[3].As<Object>();
      ctx->Set(env->context(), env->error_string(), error).FromJust();
      return;
    }

    args.GetReturnValue().Set(rc.ToLocalChecked());
  }
}

static void ReadDir(const FunctionCallbackInfo<Value>& args) {
  Environment* env = Environment::GetCurrent(args);

  const int argc = args.Length();
  CHECK_GE(argc, 3);

  BufferValue path(env->isolate(), args[0]);
  CHECK_NE(*path, nullptr);

  const enum encoding encoding = ParseEncoding(env->isolate(), args[1], UTF8);

  FSReqBase* req_wrap_async = GetReqWrap(env, args[2]);
  if (req_wrap_async != nullptr) {  // readdir(path, encoding, req)
    AsyncCall(env, req_wrap_async, args, "scandir", encoding, AfterScanDir,
              uv_fs_scandir, *path, 0 /*flags*/);
  } else {  // readdir(path, encoding, undefined, ctx)
    CHECK_EQ(argc, 4);
    FSReqWrapSync req_wrap_sync;
    FS_SYNC_TRACE_BEGIN(readdir);
    int err = SyncCall(env, args[3], &req_wrap_sync, "scandir",
                       uv_fs_scandir, *path, 0 /*flags*/);
    FS_SYNC_TRACE_END(readdir);
    if (err < 0) {
      return;  // syscall failed, no need to continue, error info is in ctx
    }

    CHECK_GE(req_wrap_sync.req.result, 0);
    int r;
    Local<Array> names = Array::New(env->isolate(), 0);
    Local<Function> fn = env->push_values_to_array_function();
    Local<Value> name_v[NODE_PUSH_VAL_TO_ARRAY_MAX];
    size_t name_idx = 0;

    for (int i = 0; ; i++) {
      uv_dirent_t ent;

      r = uv_fs_scandir_next(&(req_wrap_sync.req), &ent);
      if (r == UV_EOF)
        break;
      if (r != 0) {
        Local<Object> ctx = args[3].As<Object>();
        ctx->Set(env->context(), env->errno_string(),
                 Integer::New(env->isolate(), r)).FromJust();
        ctx->Set(env->context(), env->syscall_string(),
                 OneByteString(env->isolate(), "readdir")).FromJust();
        return;
      }

      Local<Value> error;
      MaybeLocal<Value> filename = StringBytes::Encode(env->isolate(),
                                                       ent.name,
                                                       encoding,
                                                       &error);
      if (filename.IsEmpty()) {
        Local<Object> ctx = args[3].As<Object>();
        ctx->Set(env->context(), env->error_string(), error).FromJust();
        return;
      }

      name_v[name_idx++] = filename.ToLocalChecked();

      if (name_idx >= arraysize(name_v)) {
        MaybeLocal<Value> ret = fn->Call(env->context(), names, name_idx,
                                         name_v);
        if (ret.IsEmpty()) {
          return;
        }
        name_idx = 0;
      }
    }

    if (name_idx > 0) {
      MaybeLocal<Value> ret = fn->Call(env->context(), names, name_idx, name_v);
      if (ret.IsEmpty()) {
        return;
      }
    }

    args.GetReturnValue().Set(names);
  }
}

static void Open(const FunctionCallbackInfo<Value>& args) {
  Environment* env = Environment::GetCurrent(args);

  const int argc = args.Length();
  CHECK_GE(argc, 3);

  BufferValue path(env->isolate(), args[0]);
  CHECK_NE(*path, nullptr);

  CHECK(args[1]->IsInt32());
  const int flags = args[1].As<Int32>()->Value();

  CHECK(args[2]->IsInt32());
  const int mode = args[2].As<Int32>()->Value();

  FSReqBase* req_wrap_async = GetReqWrap(env, args[3]);
  if (req_wrap_async != nullptr) {  // open(path, flags, mode, req)
    AsyncCall(env, req_wrap_async, args, "open", UTF8, AfterInteger,
              uv_fs_open, *path, flags, mode);
  } else {  // open(path, flags, mode, undefined, ctx)
    CHECK_EQ(argc, 5);
    FSReqWrapSync req_wrap_sync;
    FS_SYNC_TRACE_BEGIN(open);
    int result = SyncCall(env, args[4], &req_wrap_sync, "open",
                          uv_fs_open, *path, flags, mode);
    FS_SYNC_TRACE_END(open);
    args.GetReturnValue().Set(result);
  }
}

static void OpenFileHandle(const FunctionCallbackInfo<Value>& args) {
  Environment* env = Environment::GetCurrent(args);

  const int argc = args.Length();
  CHECK_GE(argc, 3);

  BufferValue path(env->isolate(), args[0]);
  CHECK_NE(*path, nullptr);

  CHECK(args[1]->IsInt32());
  const int flags = args[1].As<Int32>()->Value();

  CHECK(args[2]->IsInt32());
  const int mode = args[2].As<Int32>()->Value();

  FSReqBase* req_wrap_async = GetReqWrap(env, args[3]);
  if (req_wrap_async != nullptr) {  // openFileHandle(path, flags, mode, req)
    AsyncCall(env, req_wrap_async, args, "open", UTF8, AfterOpenFileHandle,
              uv_fs_open, *path, flags, mode);
  } else {  // openFileHandle(path, flags, mode, undefined, ctx)
    CHECK_EQ(argc, 5);
    FSReqWrapSync req_wrap_sync;
    FS_SYNC_TRACE_BEGIN(open);
    int result = SyncCall(env, args[4], &req_wrap_sync, "open",
                          uv_fs_open, *path, flags, mode);
    FS_SYNC_TRACE_END(open);
    if (result < 0) {
      return;  // syscall failed, no need to continue, error info is in ctx
    }
    HandleScope scope(env->isolate());
    FileHandle* fd = new FileHandle(env, result);
    args.GetReturnValue().Set(fd->object());
  }
}

static void CopyFile(const FunctionCallbackInfo<Value>& args) {
  Environment* env = Environment::GetCurrent(args);

  const int argc = args.Length();
  CHECK_GE(argc, 3);

  BufferValue src(env->isolate(), args[0]);
  CHECK_NE(*src, nullptr);

  BufferValue dest(env->isolate(), args[1]);
  CHECK_NE(*dest, nullptr);

  CHECK(args[2]->IsInt32());
  const int flags = args[2].As<Int32>()->Value();

  FSReqBase* req_wrap_async = GetReqWrap(env, args[3]);
  if (req_wrap_async != nullptr) {  // copyFile(src, dest, flags, req)
    AsyncDestCall(env, req_wrap_async, args, "copyfile",
                  *dest, dest.length(), UTF8, AfterNoArgs,
                  uv_fs_copyfile, *src, *dest, flags);
  } else {  // copyFile(src, dest, flags, undefined, ctx)
    CHECK_EQ(argc, 5);
    FSReqWrapSync req_wrap_sync;
    FS_SYNC_TRACE_BEGIN(copyfile);
    SyncCall(env, args[4], &req_wrap_sync, "copyfile",
             uv_fs_copyfile, *src, *dest, flags);
    FS_SYNC_TRACE_END(copyfile);
  }
}


// Wrapper for write(2).
//
// bytesWritten = write(fd, buffer, offset, length, position, callback)
// 0 fd        integer. file descriptor
// 1 buffer    the data to write
// 2 offset    where in the buffer to start from
// 3 length    how much to write
// 4 position  if integer, position to write at in the file.
//             if null, write from the current position
static void WriteBuffer(const FunctionCallbackInfo<Value>& args) {
  Environment* env = Environment::GetCurrent(args);

  const int argc = args.Length();
  CHECK_GE(argc, 4);

  CHECK(args[0]->IsInt32());
  const int fd = args[0].As<Int32>()->Value();

  CHECK(Buffer::HasInstance(args[1]));
  Local<Object> buffer_obj = args[1].As<Object>();
  char* buffer_data = Buffer::Data(buffer_obj);
  size_t buffer_length = Buffer::Length(buffer_obj);

  CHECK(args[2]->IsInt32());
  const size_t off = static_cast<size_t>(args[2].As<Int32>()->Value());
  CHECK_LE(off, buffer_length);

  CHECK(args[3]->IsInt32());
  const size_t len = static_cast<size_t>(args[3].As<Int32>()->Value());
  CHECK(Buffer::IsWithinBounds(off, len, buffer_length));
  CHECK_LE(len, buffer_length);
  CHECK_GE(off + len, off);

  const int64_t pos = GET_OFFSET(args[4]);

  char* buf = buffer_data + off;
  uv_buf_t uvbuf = uv_buf_init(const_cast<char*>(buf), len);

  FSReqBase* req_wrap_async = GetReqWrap(env, args[5]);
  if (req_wrap_async != nullptr) {  // write(fd, buffer, off, len, pos, req)
    AsyncCall(env, req_wrap_async, args, "write", UTF8, AfterInteger,
              uv_fs_write, fd, &uvbuf, 1, pos);
  } else {  // write(fd, buffer, off, len, pos, undefined, ctx)
    CHECK_EQ(argc, 7);
    FSReqWrapSync req_wrap_sync;
    FS_SYNC_TRACE_BEGIN(write);
    int bytesWritten = SyncCall(env, args[6], &req_wrap_sync, "write",
                                uv_fs_write, fd, &uvbuf, 1, pos);
    FS_SYNC_TRACE_END(write, "bytesWritten", bytesWritten);
    args.GetReturnValue().Set(bytesWritten);
  }
}


// Wrapper for writev(2).
//
// bytesWritten = writev(fd, chunks, position, callback)
// 0 fd        integer. file descriptor
// 1 chunks    array of buffers to write
// 2 position  if integer, position to write at in the file.
//             if null, write from the current position
static void WriteBuffers(const FunctionCallbackInfo<Value>& args) {
  Environment* env = Environment::GetCurrent(args);

  const int argc = args.Length();
  CHECK_GE(argc, 3);

  CHECK(args[0]->IsInt32());
  const int fd = args[0].As<Int32>()->Value();

  CHECK(args[1]->IsArray());
  Local<Array> chunks = args[1].As<Array>();

  int64_t pos = GET_OFFSET(args[2]);

  MaybeStackBuffer<uv_buf_t> iovs(chunks->Length());

  for (uint32_t i = 0; i < iovs.length(); i++) {
    Local<Value> chunk = chunks->Get(i);
    CHECK(Buffer::HasInstance(chunk));
    iovs[i] = uv_buf_init(Buffer::Data(chunk), Buffer::Length(chunk));
  }

  FSReqBase* req_wrap_async = GetReqWrap(env, args[3]);
  if (req_wrap_async != nullptr) {  // writeBuffers(fd, chunks, pos, req)
    AsyncCall(env, req_wrap_async, args, "write", UTF8, AfterInteger,
              uv_fs_write, fd, *iovs, iovs.length(), pos);
  } else {  // writeBuffers(fd, chunks, pos, undefined, ctx)
    CHECK_EQ(argc, 5);
    FSReqWrapSync req_wrap_sync;
    FS_SYNC_TRACE_BEGIN(write);
    int bytesWritten = SyncCall(env, args[4], &req_wrap_sync, "write",
                                uv_fs_write, fd, *iovs, iovs.length(), pos);
    FS_SYNC_TRACE_END(write, "bytesWritten", bytesWritten);
    args.GetReturnValue().Set(bytesWritten);
  }
}


// Wrapper for write(2).
//
// bytesWritten = write(fd, string, position, enc, callback)
// 0 fd        integer. file descriptor
// 1 string    non-buffer values are converted to strings
// 2 position  if integer, position to write at in the file.
//             if null, write from the current position
// 3 enc       encoding of string
static void WriteString(const FunctionCallbackInfo<Value>& args) {
  Environment* env = Environment::GetCurrent(args);

  const int argc = args.Length();
  CHECK_GE(argc, 4);

  CHECK(args[0]->IsInt32());
  const int fd = args[0].As<Int32>()->Value();

  const int64_t pos = GET_OFFSET(args[2]);

  const auto enc = ParseEncoding(env->isolate(), args[3], UTF8);

  Local<Value> value = args[1];
  char* buf = nullptr;
  size_t len;

  FSReqBase* req_wrap_async = GetReqWrap(env, args[4]);
  const bool is_async = req_wrap_async != nullptr;

  // Avoid copying the string when it is externalized but only when:
  // 1. The target encoding is compatible with the string's encoding, and
  // 2. The write is synchronous, otherwise the string might get neutered
  //    while the request is in flight, and
  // 3. For UCS2, when the host system is little-endian.  Big-endian systems
  //    need to call StringBytes::Write() to ensure proper byte swapping.
  // The const_casts are conceptually sound: memory is read but not written.
  if (!is_async && value->IsString()) {
    auto string = value.As<String>();
    if ((enc == ASCII || enc == LATIN1) && string->IsExternalOneByte()) {
      auto ext = string->GetExternalOneByteStringResource();
      buf = const_cast<char*>(ext->data());
      len = ext->length();
    } else if (enc == UCS2 && IsLittleEndian() && string->IsExternal()) {
      auto ext = string->GetExternalStringResource();
      buf = reinterpret_cast<char*>(const_cast<uint16_t*>(ext->data()));
      len = ext->length() * sizeof(*ext->data());
    }
  }

  if (is_async) {  // write(fd, string, pos, enc, req)
    CHECK_NE(req_wrap_async, nullptr);
    len = StringBytes::StorageSize(env->isolate(), value, enc);
    FSReqBase::FSReqBuffer& stack_buffer =
        req_wrap_async->Init("write", len, enc);
    // StorageSize may return too large a char, so correct the actual length
    // by the write size
    len = StringBytes::Write(env->isolate(), *stack_buffer, len, args[1], enc);
    stack_buffer.SetLengthAndZeroTerminate(len);
    uv_buf_t uvbuf = uv_buf_init(*stack_buffer, len);
    int err = uv_fs_write(env->event_loop(), req_wrap_async->req(),
                          fd, &uvbuf, 1, pos, AfterInteger);
    req_wrap_async->Dispatched();
    if (err < 0) {
      uv_fs_t* uv_req = req_wrap_async->req();
      uv_req->result = err;
      uv_req->path = nullptr;
      AfterInteger(uv_req);  // after may delete req_wrap_async if there is
                             // an error
    } else {
      req_wrap_async->SetReturnValue(args);
    }
  } else {  // write(fd, string, pos, enc, undefined, ctx)
    CHECK_EQ(argc, 6);
    FSReqWrapSync req_wrap_sync;
    FSReqBase::FSReqBuffer stack_buffer;
    if (buf == nullptr) {
      len = StringBytes::StorageSize(env->isolate(), value, enc);
      stack_buffer.AllocateSufficientStorage(len + 1);
      // StorageSize may return too large a char, so correct the actual length
      // by the write size
      len = StringBytes::Write(env->isolate(), *stack_buffer,
                               len, args[1], enc);
      stack_buffer.SetLengthAndZeroTerminate(len);
      buf = *stack_buffer;
    }
    uv_buf_t uvbuf = uv_buf_init(buf, len);
    FS_SYNC_TRACE_BEGIN(write);
    int bytesWritten = SyncCall(env, args[5], &req_wrap_sync, "write",
                                uv_fs_write, fd, &uvbuf, 1, pos);
    FS_SYNC_TRACE_END(write, "bytesWritten", bytesWritten);
    args.GetReturnValue().Set(bytesWritten);
  }
}


/*
 * Wrapper for read(2).
 *
 * bytesRead = fs.read(fd, buffer, offset, length, position)
 *
 * 0 fd        int32. file descriptor
 * 1 buffer    instance of Buffer
 * 2 offset    int32. offset to start reading into inside buffer
 * 3 length    int32. length to read
 * 4 position  int64. file position - -1 for current position
 */
static void Read(const FunctionCallbackInfo<Value>& args) {
  Environment* env = Environment::GetCurrent(args);

  const int argc = args.Length();
  CHECK_GE(argc, 5);

  CHECK(args[0]->IsInt32());
  const int fd = args[0].As<Int32>()->Value();

  CHECK(Buffer::HasInstance(args[1]));
  Local<Object> buffer_obj = args[1].As<Object>();
  char* buffer_data = Buffer::Data(buffer_obj);
  size_t buffer_length = Buffer::Length(buffer_obj);

  CHECK(args[2]->IsInt32());
  const size_t off = static_cast<size_t>(args[2].As<Int32>()->Value());
  CHECK_LT(off, buffer_length);

  CHECK(args[3]->IsInt32());
  const size_t len = static_cast<size_t>(args[3].As<Int32>()->Value());
  CHECK(Buffer::IsWithinBounds(off, len, buffer_length));

  CHECK(args[4]->IsNumber());
  const int64_t pos = args[4].As<Integer>()->Value();

  char* buf = buffer_data + off;
  uv_buf_t uvbuf = uv_buf_init(const_cast<char*>(buf), len);

  FSReqBase* req_wrap_async = GetReqWrap(env, args[5]);
  if (req_wrap_async != nullptr) {  // read(fd, buffer, offset, len, pos, req)
    AsyncCall(env, req_wrap_async, args, "read", UTF8, AfterInteger,
              uv_fs_read, fd, &uvbuf, 1, pos);
  } else {  // read(fd, buffer, offset, len, pos, undefined, ctx)
    CHECK_EQ(argc, 7);
    FSReqWrapSync req_wrap_sync;
    FS_SYNC_TRACE_BEGIN(read);
    const int bytesRead = SyncCall(env, args[6], &req_wrap_sync, "read",
                                   uv_fs_read, fd, &uvbuf, 1, pos);
    FS_SYNC_TRACE_END(read, "bytesRead", bytesRead);
    args.GetReturnValue().Set(bytesRead);
  }
}


/* fs.chmod(path, mode);
 * Wrapper for chmod(1) / EIO_CHMOD
 */
static void Chmod(const FunctionCallbackInfo<Value>& args) {
  Environment* env = Environment::GetCurrent(args);

  const int argc = args.Length();
  CHECK_GE(argc, 2);

  BufferValue path(env->isolate(), args[0]);
  CHECK_NE(*path, nullptr);

  CHECK(args[1]->IsInt32());
  int mode = args[1].As<Int32>()->Value();

  FSReqBase* req_wrap_async = GetReqWrap(env, args[2]);
  if (req_wrap_async != nullptr) {  // chmod(path, mode, req)
    AsyncCall(env, req_wrap_async, args, "chmod", UTF8, AfterNoArgs,
              uv_fs_chmod, *path, mode);
  } else {  // chmod(path, mode, undefined, ctx)
    CHECK_EQ(argc, 4);
    FSReqWrapSync req_wrap_sync;
    FS_SYNC_TRACE_BEGIN(chmod);
    SyncCall(env, args[3], &req_wrap_sync, "chmod",
             uv_fs_chmod, *path, mode);
    FS_SYNC_TRACE_END(chmod);
  }
}


/* fs.fchmod(fd, mode);
 * Wrapper for fchmod(1) / EIO_FCHMOD
 */
static void FChmod(const FunctionCallbackInfo<Value>& args) {
  Environment* env = Environment::GetCurrent(args);

  const int argc = args.Length();
  CHECK_GE(argc, 2);

  CHECK(args[0]->IsInt32());
  const int fd = args[0].As<Int32>()->Value();

  CHECK(args[1]->IsInt32());
  const int mode = args[1].As<Int32>()->Value();

  FSReqBase* req_wrap_async = GetReqWrap(env, args[2]);
  if (req_wrap_async != nullptr) {  // fchmod(fd, mode, req)
    AsyncCall(env, req_wrap_async, args, "fchmod", UTF8, AfterNoArgs,
              uv_fs_fchmod, fd, mode);
  } else {  // fchmod(fd, mode, undefined, ctx)
    CHECK_EQ(argc, 4);
    FSReqWrapSync req_wrap_sync;
    FS_SYNC_TRACE_BEGIN(fchmod);
    SyncCall(env, args[3], &req_wrap_sync, "fchmod",
             uv_fs_fchmod, fd, mode);
    FS_SYNC_TRACE_END(fchmod);
  }
}


/* fs.chown(path, uid, gid);
 * Wrapper for chown(1) / EIO_CHOWN
 */
static void Chown(const FunctionCallbackInfo<Value>& args) {
  Environment* env = Environment::GetCurrent(args);

  const int argc = args.Length();
  CHECK_GE(argc, 3);

  BufferValue path(env->isolate(), args[0]);
  CHECK_NE(*path, nullptr);

  CHECK(args[1]->IsUint32());
  const uv_uid_t uid = static_cast<uv_uid_t>(args[1].As<Uint32>()->Value());

  CHECK(args[2]->IsUint32());
  const uv_gid_t gid = static_cast<uv_gid_t>(args[2].As<Uint32>()->Value());

  FSReqBase* req_wrap_async = GetReqWrap(env, args[3]);
  if (req_wrap_async != nullptr) {  // chown(path, uid, gid, req)
    AsyncCall(env, req_wrap_async, args, "chown", UTF8, AfterNoArgs,
              uv_fs_chown, *path, uid, gid);
  } else {  // chown(path, uid, gid, undefined, ctx)
    CHECK_EQ(argc, 5);
    FSReqWrapSync req_wrap_sync;
    FS_SYNC_TRACE_BEGIN(chown);
    SyncCall(env, args[4], &req_wrap_sync, "chown",
             uv_fs_chown, *path, uid, gid);
    FS_SYNC_TRACE_END(chown);
  }
}


/* fs.fchown(fd, uid, gid);
 * Wrapper for fchown(1) / EIO_FCHOWN
 */
static void FChown(const FunctionCallbackInfo<Value>& args) {
  Environment* env = Environment::GetCurrent(args);

  const int argc = args.Length();
  CHECK_GE(argc, 3);

  CHECK(args[0]->IsInt32());
  const int fd = args[0].As<Int32>()->Value();

  CHECK(args[1]->IsUint32());
  const uv_uid_t uid = static_cast<uv_uid_t>(args[1].As<Uint32>()->Value());

  CHECK(args[2]->IsUint32());
  const uv_gid_t gid = static_cast<uv_gid_t>(args[2].As<Uint32>()->Value());

  FSReqBase* req_wrap_async = GetReqWrap(env, args[3]);
  if (req_wrap_async != nullptr) {  // fchown(fd, uid, gid, req)
    AsyncCall(env, req_wrap_async, args, "fchown", UTF8, AfterNoArgs,
              uv_fs_fchown, fd, uid, gid);
  } else {  // fchown(fd, uid, gid, undefined, ctx)
    CHECK_EQ(argc, 5);
    FSReqWrapSync req_wrap_sync;
    FS_SYNC_TRACE_BEGIN(fchown);
    SyncCall(env, args[4], &req_wrap_sync, "fchown",
             uv_fs_fchown, fd, uid, gid);
    FS_SYNC_TRACE_END(fchown);
  }
}


static void UTimes(const FunctionCallbackInfo<Value>& args) {
  Environment* env = Environment::GetCurrent(args);

  const int argc = args.Length();
  CHECK_GE(argc, 3);

  BufferValue path(env->isolate(), args[0]);
  CHECK_NE(*path, nullptr);

  CHECK(args[1]->IsNumber());
  const double atime = args[1].As<Number>()->Value();

  CHECK(args[2]->IsNumber());
  const double mtime = args[2].As<Number>()->Value();

  FSReqBase* req_wrap_async = GetReqWrap(env, args[3]);
  if (req_wrap_async != nullptr) {  // utimes(path, atime, mtime, req)
    AsyncCall(env, req_wrap_async, args, "utime", UTF8, AfterNoArgs,
              uv_fs_utime, *path, atime, mtime);
  } else {  // utimes(path, atime, mtime, undefined, ctx)
    CHECK_EQ(argc, 5);
    FSReqWrapSync req_wrap_sync;
    FS_SYNC_TRACE_BEGIN(utimes);
    SyncCall(env, args[4], &req_wrap_sync, "utime",
             uv_fs_utime, *path, atime, mtime);
    FS_SYNC_TRACE_END(utimes);
  }
}

static void FUTimes(const FunctionCallbackInfo<Value>& args) {
  Environment* env = Environment::GetCurrent(args);

  const int argc = args.Length();
  CHECK_GE(argc, 3);

  CHECK(args[0]->IsInt32());
  const int fd = args[0].As<Int32>()->Value();

  CHECK(args[1]->IsNumber());
  const double atime = args[1].As<Number>()->Value();

  CHECK(args[2]->IsNumber());
  const double mtime = args[2].As<Number>()->Value();

  FSReqBase* req_wrap_async = GetReqWrap(env, args[3]);
  if (req_wrap_async != nullptr) {  // futimes(fd, atime, mtime, req)
    AsyncCall(env, req_wrap_async, args, "futime", UTF8, AfterNoArgs,
              uv_fs_futime, fd, atime, mtime);
  } else {  // futimes(fd, atime, mtime, undefined, ctx)
    CHECK_EQ(argc, 5);
    FSReqWrapSync req_wrap_sync;
    FS_SYNC_TRACE_BEGIN(futimes);
    SyncCall(env, args[4], &req_wrap_sync, "futime",
             uv_fs_futime, fd, atime, mtime);
    FS_SYNC_TRACE_END(futimes);
  }
}

static void Mkdtemp(const FunctionCallbackInfo<Value>& args) {
  Environment* env = Environment::GetCurrent(args);

  const int argc = args.Length();
  CHECK_GE(argc, 2);

  BufferValue tmpl(env->isolate(), args[0]);
  CHECK_NE(*tmpl, nullptr);

  const enum encoding encoding = ParseEncoding(env->isolate(), args[1], UTF8);

  FSReqBase* req_wrap_async = GetReqWrap(env, args[2]);
  if (req_wrap_async != nullptr) {  // mkdtemp(tmpl, encoding, req)
    AsyncCall(env, req_wrap_async, args, "mkdtemp", encoding, AfterStringPath,
              uv_fs_mkdtemp, *tmpl);
  } else {  // mkdtemp(tmpl, encoding, undefined, ctx)
    CHECK_EQ(argc, 4);
    FSReqWrapSync req_wrap_sync;
    FS_SYNC_TRACE_BEGIN(mkdtemp);
    SyncCall(env, args[3], &req_wrap_sync, "mkdtemp",
             uv_fs_mkdtemp, *tmpl);
    FS_SYNC_TRACE_END(mkdtemp);
    const char* path = static_cast<const char*>(req_wrap_sync.req.path);

    Local<Value> error;
    MaybeLocal<Value> rc =
        StringBytes::Encode(env->isolate(), path, encoding, &error);
    if (rc.IsEmpty()) {
      Local<Object> ctx = args[3].As<Object>();
      ctx->Set(env->context(), env->error_string(), error).FromJust();
      return;
    }
    args.GetReturnValue().Set(rc.ToLocalChecked());
  }
}

void Initialize(Local<Object> target,
                Local<Value> unused,
                Local<Context> context,
                void* priv) {
  Environment* env = Environment::GetCurrent(context);

  env->SetMethod(target, "access", Access);
  env->SetMethod(target, "close", Close);
  env->SetMethod(target, "open", Open);
  env->SetMethod(target, "openFileHandle", OpenFileHandle);
  env->SetMethod(target, "read", Read);
  env->SetMethod(target, "fdatasync", Fdatasync);
  env->SetMethod(target, "fsync", Fsync);
  env->SetMethod(target, "rename", Rename);
  env->SetMethod(target, "ftruncate", FTruncate);
  env->SetMethod(target, "rmdir", RMDir);
  env->SetMethod(target, "mkdir", MKDir);
  env->SetMethod(target, "readdir", ReadDir);
  env->SetMethod(target, "internalModuleReadJSON", InternalModuleReadJSON);
  env->SetMethod(target, "internalModuleStat", InternalModuleStat);
  env->SetMethod(target, "stat", Stat);
  env->SetMethod(target, "statNoException", StatNoException);
  env->SetMethod(target, "lstat", LStat);
  env->SetMethod(target, "lstatNoException", LStatNoException);
  env->SetMethod(target, "fstat", FStat);
  env->SetMethod(target, "link", Link);
  env->SetMethod(target, "symlink", Symlink);
  env->SetMethod(target, "readlink", ReadLink);
  env->SetMethod(target, "unlink", Unlink);
  env->SetMethod(target, "writeBuffer", WriteBuffer);
  env->SetMethod(target, "writeBuffers", WriteBuffers);
  env->SetMethod(target, "writeString", WriteString);
  env->SetMethod(target, "realpath", RealPath);
  env->SetMethod(target, "copyFile", CopyFile);

  env->SetMethod(target, "chmod", Chmod);
  env->SetMethod(target, "fchmod", FChmod);
  // env->SetMethod(target, "lchmod", LChmod);

  env->SetMethod(target, "chown", Chown);
  env->SetMethod(target, "fchown", FChown);
  // env->SetMethod(target, "lchown", LChown);

  env->SetMethod(target, "utimes", UTimes);
  env->SetMethod(target, "futimes", FUTimes);

  env->SetMethod(target, "mkdtemp", Mkdtemp);

  target->Set(env->context(),
              FIXED_ONE_BYTE_STRING(env->isolate(), "kFsStatsFieldsLength"),
              Integer::New(env->isolate(), env->kFsStatsFieldsLength))
        .FromJust();

  target->Set(context,
              FIXED_ONE_BYTE_STRING(env->isolate(), "statValues"),
              env->fs_stats_field_array()->GetJSArray()).FromJust();

  StatWatcher::Initialize(env, target);

  // Create FunctionTemplate for FSReqWrap
  Local<FunctionTemplate> fst =
      FunctionTemplate::New(env->isolate(), NewFSReqWrap);
  fst->InstanceTemplate()->SetInternalFieldCount(1);
  AsyncWrap::AddWrapMethods(env, fst);
  Local<String> wrapString =
      FIXED_ONE_BYTE_STRING(env->isolate(), "FSReqWrap");
  fst->SetClassName(wrapString);
  target->Set(context, wrapString, fst->GetFunction()).FromJust();

  // Create FunctionTemplate for FileHandleReadWrap. There’s no need
  // to do anything in the constructor, so we only store the instance template.
  Local<FunctionTemplate> fh_rw = FunctionTemplate::New(env->isolate());
  fh_rw->InstanceTemplate()->SetInternalFieldCount(1);
  AsyncWrap::AddWrapMethods(env, fh_rw);
  Local<String> fhWrapString =
      FIXED_ONE_BYTE_STRING(env->isolate(), "FileHandleReqWrap");
  fh_rw->SetClassName(fhWrapString);
  env->set_filehandlereadwrap_template(
      fst->InstanceTemplate());

  // Create Function Template for FSReqPromise
  Local<FunctionTemplate> fpt = FunctionTemplate::New(env->isolate());
  AsyncWrap::AddWrapMethods(env, fpt);
  Local<String> promiseString =
      FIXED_ONE_BYTE_STRING(env->isolate(), "FSReqPromise");
  fpt->SetClassName(promiseString);
  Local<ObjectTemplate> fpo = fpt->InstanceTemplate();
  fpo->SetInternalFieldCount(1);
  env->set_fsreqpromise_constructor_template(fpo);

  // Create FunctionTemplate for FileHandle
  Local<FunctionTemplate> fd = env->NewFunctionTemplate(FileHandle::New);
  AsyncWrap::AddWrapMethods(env, fd);
  env->SetProtoMethod(fd, "close", FileHandle::Close);
  env->SetProtoMethod(fd, "releaseFD", FileHandle::ReleaseFD);
  Local<ObjectTemplate> fdt = fd->InstanceTemplate();
  fdt->SetInternalFieldCount(1);
  Local<String> handleString =
       FIXED_ONE_BYTE_STRING(env->isolate(), "FileHandle");
  fd->SetClassName(handleString);
  StreamBase::AddMethods<FileHandle>(env, fd, StreamBase::kFlagNone);
  target->Set(context, handleString, fd->GetFunction()).FromJust();
  env->set_fd_constructor_template(fdt);

  // Create FunctionTemplate for FileHandle::CloseReq
  Local<FunctionTemplate> fdclose = FunctionTemplate::New(env->isolate());
  fdclose->SetClassName(FIXED_ONE_BYTE_STRING(env->isolate(),
                        "FileHandleCloseReq"));
  AsyncWrap::AddWrapMethods(env, fdclose);
  Local<ObjectTemplate> fdcloset = fdclose->InstanceTemplate();
  fdcloset->SetInternalFieldCount(1);
  env->set_fdclose_constructor_template(fdcloset);

  Local<Symbol> use_promises_symbol =
    Symbol::New(env->isolate(),
                FIXED_ONE_BYTE_STRING(env->isolate(), "use promises"));
  env->set_fs_use_promises_symbol(use_promises_symbol);
  target->Set(env->context(),
              FIXED_ONE_BYTE_STRING(env->isolate(), "kUsePromises"),
              use_promises_symbol).FromJust();
}

}  // namespace fs

}  // end namespace node

NODE_BUILTIN_MODULE_CONTEXT_AWARE(fs, node::fs::Initialize)
