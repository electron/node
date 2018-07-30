#ifndef SRC_INSPECTOR_AGENT_H_
#define SRC_INSPECTOR_AGENT_H_

#include <memory>

#include <stddef.h>

#if !HAVE_INSPECTOR
#error("This header can only be used when inspector is enabled")
#endif

#include "node.h"
#include "node_debug_options.h"
#include "node_persistent.h"
#include "node_platform.h"
#include "v8.h"

namespace v8_inspector {
class StringView;
}  // namespace v8_inspector

namespace node {
// Forward declaration to break recursive dependency chain with src/env.h.
class Environment;
struct ContextInfo;

namespace inspector {
class InspectorIo;
class NodeInspectorClient;

class InspectorSession {
 public:
  InspectorSession(int session_id, std::shared_ptr<NodeInspectorClient> client);
  ~InspectorSession();
  void Dispatch(const v8_inspector::StringView& message);
 private:
  int session_id_;
  std::shared_ptr<NodeInspectorClient> client_;
};

class InspectorSessionDelegate {
 public:
  virtual ~InspectorSessionDelegate() = default;
  virtual void SendMessageToFrontend(const v8_inspector::StringView& message)
                                     = 0;
};

class NODE_EXTERN Agent {
 public:
  explicit Agent(node::Environment* env);
  ~Agent();

  // Create client_, may create io_ if option enabled
  bool Start(const char* path, const DebugOptions& options);
  // Stop and destroy io_
  void Stop();

  bool IsStarted() { return !!client_; }

  // IO thread started, and client connected
  bool IsWaitingForConnect();

  void WaitForDisconnect();
  void FatalException(v8::Local<v8::Value> error,
                      v8::Local<v8::Message> message);

  // Async stack traces instrumentation.
  void AsyncTaskScheduled(const v8_inspector::StringView& taskName, void* task,
                          bool recurring);
  void AsyncTaskCanceled(void* task);
  void AsyncTaskStarted(void* task);
  void AsyncTaskFinished(void* task);
  void AllAsyncTasksCanceled();

  void RegisterAsyncHook(v8::Isolate* isolate,
    v8::Local<v8::Function> enable_function,
    v8::Local<v8::Function> disable_function);
  void EnableAsyncHook();
  void DisableAsyncHook();

  // Called by the WS protocol and JS binding to create inspector sessions.
  // The inspector responds by using the delegate to send messages back.
  std::unique_ptr<InspectorSession> Connect(
      std::unique_ptr<InspectorSessionDelegate> delegate);

  void PauseOnNextJavascriptStatement(const std::string& reason);

  // Returns true as long as there is at least one connected session.
  bool HasConnectedSessions();

  InspectorIo* io() {
    return io_.get();
  }

  // Can only be called from the main thread.
  bool StartIoThread(bool wait_for_connect);

  // Calls StartIoThread() from off the main thread.
  void RequestIoThreadStart();

  DebugOptions& options() { return debug_options_; }
  void ContextCreated(v8::Local<v8::Context> context, const ContextInfo& info);

 private:
  void ToggleAsyncHook(v8::Isolate* isolate,
                       const node::Persistent<v8::Function>& fn);

  node::Environment* parent_env_;
  std::shared_ptr<NodeInspectorClient> client_;
  std::unique_ptr<InspectorIo> io_;
  std::string path_;
  DebugOptions debug_options_;

  bool pending_enable_async_hook_ = false;
  bool pending_disable_async_hook_ = false;
  node::Persistent<v8::Function> enable_async_hook_function_;
  node::Persistent<v8::Function> disable_async_hook_function_;
};

}  // namespace inspector
}  // namespace node

#endif  // SRC_INSPECTOR_AGENT_H_
