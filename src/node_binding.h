#ifndef SRC_NODE_BINDING_H_
#define SRC_NODE_BINDING_H_

#if defined(NODE_WANT_INTERNALS) && NODE_WANT_INTERNALS

#include "node.h"
#include "node_api.h"
#include "uv.h"
#include "v8.h"

enum {
  NM_F_BUILTIN = 1 << 0,
  NM_F_LINKED = 1 << 1,
  NM_F_INTERNAL = 1 << 2,
};

#define NODE_MODULE_CONTEXT_AWARE_CPP(modname, regfunc, priv, flags)           \
  static node::node_module _module = {                                         \
      NODE_MODULE_VERSION,                                                     \
      flags,                                                                   \
      nullptr,                                                                 \
      __FILE__,                                                                \
      nullptr,                                                                 \
      (node::addon_context_register_func)(regfunc),                            \
      NODE_STRINGIFY(modname),                                                 \
      priv,                                                                    \
      nullptr};                                                                \
  void _register_##modname() { node_module_register(&_module); }

#define NODE_BUILTIN_MODULE_CONTEXT_AWARE(modname, regfunc)                    \
  NODE_MODULE_CONTEXT_AWARE_CPP(modname, regfunc, nullptr, NM_F_BUILTIN)

void napi_module_register_by_symbol(v8::Local<v8::Object> exports,
                                    v8::Local<v8::Value> module,
                                    v8::Local<v8::Context> context,
                                    napi_addon_register_func init);

namespace node {

#define NODE_MODULE_CONTEXT_AWARE_INTERNAL(modname, regfunc)                   \
  NODE_MODULE_CONTEXT_AWARE_CPP(modname, regfunc, nullptr, NM_F_INTERNAL)

// Globals per process
// This is set by node::Init() which is used by embedders
extern bool node_is_initialized;

namespace binding {

// Call _register<module_name> functions for all of
// the built-in modules. Because built-in modules don't
// use the __attribute__((constructor)). Need to
// explicitly call the _register* functions.
void RegisterBuiltinModules();
void GetBinding(const v8::FunctionCallbackInfo<v8::Value>& args);
void GetInternalBinding(const v8::FunctionCallbackInfo<v8::Value>& args);
void GetLinkedBinding(const v8::FunctionCallbackInfo<v8::Value>& args);
void DLOpen(const v8::FunctionCallbackInfo<v8::Value>& args);

NODE_EXTERN node_module* get_linked_module(const char *name);

}  // namespace binding

}  // namespace node

#endif  // defined(NODE_WANT_INTERNALS) && NODE_WANT_INTERNALS
#endif  // SRC_NODE_BINDING_H_
