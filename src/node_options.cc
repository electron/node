#include <errno.h>
#include "node_internals.h"
#include "node_options-inl.h"

using v8::Boolean;
using v8::Context;
using v8::FunctionCallbackInfo;
using v8::Integer;
using v8::Isolate;
using v8::Local;
using v8::Map;
using v8::Number;
using v8::Object;
using v8::String;
using v8::Undefined;
using v8::Value;

namespace node {
namespace options_parser {

// XXX: If you add an option here, please also add it to doc/node.1 and
// doc/api/cli.md
// TODO(addaleax): Make that unnecessary.

template class EXPORT_TEMPLATE_DEFINE(NODE_EXTERN) OptionsParser<DebugOptions>;

DebugOptionsParser::DebugOptionsParser() {
#if HAVE_INSPECTOR
  AddOption("--inspect-port",
            "set host:port for inspector",
            &DebugOptions::host_port,
            kAllowedInEnvironment);
  AddAlias("--debug-port", "--inspect-port");

  AddOption("--inspect",
            "activate inspector on host:port (default: 127.0.0.1:9229)",
            &DebugOptions::inspector_enabled,
            kAllowedInEnvironment);
  AddAlias("--inspect=", { "--inspect-port", "--inspect" });

  AddOption("--debug", "", &DebugOptions::deprecated_debug);
  AddAlias("--debug=", { "--inspect-port", "--debug" });

  AddOption("--inspect-brk",
            "activate inspector on host:port and break at start of user script",
            &DebugOptions::break_first_line,
            kAllowedInEnvironment);
  Implies("--inspect-brk", "--inspect");
  AddAlias("--inspect-brk=", { "--inspect-port", "--inspect-brk" });

  AddOption("--inspect-brk-node", "", &DebugOptions::break_node_first_line);
  Implies("--inspect-brk-node", "--inspect");
  AddAlias("--inspect-brk-node=", { "--inspect-port", "--inspect-brk-node" });

  AddOption("--debug-brk", "", &DebugOptions::break_first_line);
  Implies("--debug-brk", "--debug");
  AddAlias("--debug-brk=", { "--inspect-port", "--debug-brk" });
#endif
}

DebugOptionsParser DebugOptionsParser::instance;

EnvironmentOptionsParser::EnvironmentOptionsParser() {
  AddOption("--experimental-modules",
            "experimental ES Module support and caching modules",
            &EnvironmentOptions::experimental_modules,
            kAllowedInEnvironment);
  AddOption("--experimental-repl-await",
            "experimental await keyword support in REPL",
            &EnvironmentOptions::experimental_repl_await,
            kAllowedInEnvironment);
  AddOption("--experimental-vm-modules",
            "experimental ES Module support in vm module",
            &EnvironmentOptions::experimental_vm_modules,
            kAllowedInEnvironment);
  AddOption("--experimental-worker",
            "experimental threaded Worker support",
            &EnvironmentOptions::experimental_worker,
            kAllowedInEnvironment);
  AddOption("--expose-internals", "", &EnvironmentOptions::expose_internals);
  // TODO(addaleax): Remove this when adding -/_ canonicalization to the parser.
  AddAlias("--expose_internals", "--expose-internals");
  AddOption("--loader",
            "(with --experimental-modules) use the specified file as a "
            "custom loader",
            &EnvironmentOptions::userland_loader,
            kAllowedInEnvironment);
  AddOption("--no-deprecation",
            "silence deprecation warnings",
            &EnvironmentOptions::no_deprecation,
            kAllowedInEnvironment);
  AddOption("--no-force-async-hooks-checks",
            "disable checks for async_hooks",
            &EnvironmentOptions::no_force_async_hooks_checks,
            kAllowedInEnvironment);
  AddOption("--no-warnings",
            "silence all process warnings",
            &EnvironmentOptions::no_warnings,
            kAllowedInEnvironment);
  AddOption("--pending-deprecation",
            "emit pending deprecation warnings",
            &EnvironmentOptions::pending_deprecation,
            kAllowedInEnvironment);
  AddOption("--preserve-symlinks",
            "preserve symbolic links when resolving",
            &EnvironmentOptions::preserve_symlinks);
  AddOption("--preserve-symlinks-main",
            "preserve symbolic links when resolving the main module",
            &EnvironmentOptions::preserve_symlinks_main);
  AddOption("--prof-process",
            "process V8 profiler output generated using --prof",
            &EnvironmentOptions::prof_process);
  AddOption("--redirect-warnings",
            "write warnings to file instead of stderr",
            &EnvironmentOptions::redirect_warnings,
            kAllowedInEnvironment);
  AddOption("--throw-deprecation",
            "throw an exception on deprecations",
            &EnvironmentOptions::throw_deprecation,
            kAllowedInEnvironment);
  AddOption("--trace-deprecation",
            "show stack traces on deprecations",
            &EnvironmentOptions::trace_deprecation,
            kAllowedInEnvironment);
  AddOption("--trace-sync-io",
            "show stack trace when use of sync IO is detected after the "
            "first tick",
            &EnvironmentOptions::trace_sync_io,
            kAllowedInEnvironment);
  AddOption("--trace-warnings",
            "show stack traces on process warnings",
            &EnvironmentOptions::trace_warnings,
            kAllowedInEnvironment);

  AddOption("--check",
            "syntax check script without executing",
            &EnvironmentOptions::syntax_check_only);
  AddAlias("-c", "--check");
  // This option is only so that we can tell --eval with an empty string from
  // no eval at all. Having it not start with a dash makes it inaccessible
  // from the parser itself, but available for using Implies().
  // TODO(addaleax): When moving --help over to something generated from the
  // programmatic descriptions, this will need some special care.
  // (See also [ssl_openssl_cert_store] below.)
  AddOption("[has_eval_string]", "", &EnvironmentOptions::has_eval_string);
  AddOption("--eval", "evaluate script", &EnvironmentOptions::eval_string);
  Implies("--eval", "[has_eval_string]");
  AddOption("--print",
            "evaluate script and print result",
            &EnvironmentOptions::print_eval);
  AddAlias("-e", "--eval");
  AddAlias("--print <arg>", "-pe");
  AddAlias("-pe", { "--print", "--eval" });
  AddAlias("-p", "--print");
  AddOption("--require",
            "module to preload (option can be repeated)",
            &EnvironmentOptions::preload_modules,
            kAllowedInEnvironment);
  AddAlias("-r", "--require");
  AddOption("--interactive",
            "always enter the REPL even if stdin does not appear "
            "to be a terminal",
            &EnvironmentOptions::force_repl);
  AddAlias("-i", "--interactive");

  AddOption("--napi-modules", "", NoOp {}, kAllowedInEnvironment);
  AddOption("--expose-http2", "", NoOp {}, kAllowedInEnvironment);
  AddOption("--expose_http2", "", NoOp {}, kAllowedInEnvironment);

  Insert(&DebugOptionsParser::instance,
         &EnvironmentOptions::get_debug_options);
}

EnvironmentOptionsParser EnvironmentOptionsParser::instance;

PerIsolateOptionsParser::PerIsolateOptionsParser() {
  AddOption("--track-heap-objects",
            "track heap object allocations for heap snapshots",
            &PerIsolateOptions::track_heap_objects,
            kAllowedInEnvironment);

  // Explicitly add some V8 flags to mark them as allowed in NODE_OPTIONS.
  AddOption("--abort_on_uncaught_exception",
            "aborting instead of exiting causes a core file to be generated "
            "for analysis",
            V8Option{},
            kAllowedInEnvironment);
  AddOption("--max_old_space_size", "", V8Option{}, kAllowedInEnvironment);
  AddOption("--perf_basic_prof", "", V8Option{}, kAllowedInEnvironment);
  AddOption("--perf_prof", "", V8Option{}, kAllowedInEnvironment);
  AddOption("--stack_trace_limit", "", V8Option{}, kAllowedInEnvironment);

  Insert(&EnvironmentOptionsParser::instance,
         &PerIsolateOptions::get_per_env_options);
}

PerIsolateOptionsParser PerIsolateOptionsParser::instance;

PerProcessOptionsParser::PerProcessOptionsParser() {
  AddOption("--title",
            "the process title to use on startup",
            &PerProcessOptions::title,
            kAllowedInEnvironment);
  AddOption("--trace-event-categories",
            "comma separated list of trace event categories to record",
            &PerProcessOptions::trace_event_categories,
            kAllowedInEnvironment);
  AddOption("--trace-event-file-pattern",
            "Template string specifying the filepath for the trace-events "
            "data, it supports ${rotation} and ${pid} log-rotation id.",
            &PerProcessOptions::trace_event_file_pattern,
            kAllowedInEnvironment);
  AddAlias("--trace-events-enabled", {
    "--trace-event-categories", "v8,node,node.async_hooks" });
  AddOption("--v8-pool-size",
            "set V8's thread pool size",
            &PerProcessOptions::v8_thread_pool_size,
            kAllowedInEnvironment);
  AddOption("--zero-fill-buffers",
            "automatically zero-fill all newly allocated Buffer and "
            "SlowBuffer instances",
            &PerProcessOptions::zero_fill_all_buffers,
            kAllowedInEnvironment);

  AddOption("--security-reverts", "", &PerProcessOptions::security_reverts);
  AddOption("--help",
            "print node command line options",
            &PerProcessOptions::print_help);
  AddAlias("-h", "--help");
  AddOption(
      "--version", "print Node.js version", &PerProcessOptions::print_version);
  AddAlias("-v", "--version");
  AddOption("--v8-options",
            "print V8 command line options",
            &PerProcessOptions::print_v8_help);

#ifdef NODE_HAVE_I18N_SUPPORT
  AddOption("--icu-data-dir",
            "set ICU data load path to dir (overrides NODE_ICU_DATA)"
#ifndef NODE_HAVE_SMALL_ICU
            " (note: linked-in ICU data is present)\n"
#endif
            ,
            &PerProcessOptions::icu_data_dir,
            kAllowedInEnvironment);
#endif

#if HAVE_OPENSSL
  AddOption("--openssl-config",
            "load OpenSSL configuration from the specified file "
            "(overrides OPENSSL_CONF)",
            &PerProcessOptions::openssl_config,
            kAllowedInEnvironment);
  AddOption("--tls-cipher-list",
            "use an alternative default TLS cipher list",
            &PerProcessOptions::tls_cipher_list,
            kAllowedInEnvironment);
  AddOption("--use-openssl-ca",
            "use OpenSSL's default CA store"
#if defined(NODE_OPENSSL_CERT_STORE)
            " (default)"
#endif
            ,
            &PerProcessOptions::use_openssl_ca,
            kAllowedInEnvironment);
  AddOption("--use-bundled-ca",
            "use bundled CA store"
#if !defined(NODE_OPENSSL_CERT_STORE)
            " (default)"
#endif
            ,
            &PerProcessOptions::use_bundled_ca,
            kAllowedInEnvironment);
  // Similar to [has_eval_string] above, except that the separation between
  // this and use_openssl_ca only exists for option validation after parsing.
  // This is not ideal.
  AddOption("[ssl_openssl_cert_store]",
            "",
            &PerProcessOptions::ssl_openssl_cert_store);
  Implies("--use-openssl-ca", "[ssl_openssl_cert_store]");
  ImpliesNot("--use-bundled-ca", "[ssl_openssl_cert_store]");
#if NODE_FIPS_MODE
  AddOption("--enable-fips",
            "enable FIPS crypto at startup",
            &PerProcessOptions::enable_fips_crypto,
            kAllowedInEnvironment);
  AddOption("--force-fips",
            "force FIPS crypto (cannot be disabled)",
            &PerProcessOptions::force_fips_crypto,
            kAllowedInEnvironment);
#endif
#endif

  Insert(&PerIsolateOptionsParser::instance,
         &PerProcessOptions::get_per_isolate_options);
}

PerProcessOptionsParser PerProcessOptionsParser::instance;

inline std::string RemoveBrackets(const std::string& host) {
  if (!host.empty() && host.front() == '[' && host.back() == ']')
    return host.substr(1, host.size() - 2);
  else
    return host;
}

inline int ParseAndValidatePort(const std::string& port, std::string* error) {
  char* endptr;
  errno = 0;
  const long result = strtol(port.c_str(), &endptr, 10);  // NOLINT(runtime/int)
  if (errno != 0 || *endptr != '\0'||
      (result != 0 && result < 1024) || result > 65535) {
    *error = "Port must be 0 or in range 1024 to 65535.";
  }
  return static_cast<int>(result);
}

HostPort SplitHostPort(const std::string& arg, std::string* error) {
  // remove_brackets only works if no port is specified
  // so if it has an effect only an IPv6 address was specified.
  std::string host = RemoveBrackets(arg);
  if (host.length() < arg.length())
    return HostPort { host, -1 };

  size_t colon = arg.rfind(':');
  if (colon == std::string::npos) {
    // Either a port number or a host name.  Assume that
    // if it's not all decimal digits, it's a host name.
    for (char c : arg) {
      if (c < '0' || c > '9') {
        return HostPort { arg, -1 };
      }
    }
    return HostPort { "", ParseAndValidatePort(arg, error) };
  }
  // Host and port found:
  return HostPort { RemoveBrackets(arg.substr(0, colon)),
                    ParseAndValidatePort(arg.substr(colon + 1), error) };
}

// Usage: Either:
// - getOptions() to get all options + metadata or
// - getOptions(string) to get the value of a particular option
void GetOptions(const FunctionCallbackInfo<Value>& args) {
  Mutex::ScopedLock lock(per_process_opts_mutex);
  Environment* env = Environment::GetCurrent(args);
  Isolate* isolate = env->isolate();
  Local<Context> context = env->context();

  // Temporarily act as if the current Environment's/IsolateData's options were
  // the default options, i.e. like they are the ones we'd access for global
  // options parsing, so that all options are available from the main parser.
  auto original_per_isolate = per_process_opts->per_isolate;
  per_process_opts->per_isolate = env->isolate_data()->options();
  auto original_per_env = per_process_opts->per_isolate->per_env;
  per_process_opts->per_isolate->per_env = env->options();
  OnScopeLeave on_scope_leave([&]() {
    per_process_opts->per_isolate->per_env = original_per_env;
    per_process_opts->per_isolate = original_per_isolate;
  });

  const auto& parser = PerProcessOptionsParser::instance;

  std::string filter;
  if (args[0]->IsString()) filter = *node::Utf8Value(isolate, args[0]);

  Local<Map> options = Map::New(isolate);
  for (const auto& item : parser.options_) {
    if (!filter.empty() && item.first != filter) continue;

    Local<Value> value;
    const auto& option_info = item.second;
    auto field = option_info.field;
    PerProcessOptions* opts = per_process_opts.get();
    switch (option_info.type) {
      case kNoOp:
      case kV8Option:
        value = Undefined(isolate);
        break;
      case kBoolean:
        value = Boolean::New(isolate, *parser.Lookup<bool>(field, opts));
        break;
      case kInteger:
        value = Number::New(isolate, *parser.Lookup<int64_t>(field, opts));
        break;
      case kString:
        if (!ToV8Value(context, *parser.Lookup<std::string>(field, opts))
                 .ToLocal(&value)) {
          return;
        }
        break;
      case kStringList:
        if (!ToV8Value(context,
                       *parser.Lookup<std::vector<std::string>>(field, opts))
                 .ToLocal(&value)) {
          return;
        }
        break;
      case kHostPort: {
        const HostPort& host_port = *parser.Lookup<HostPort>(field, opts);
        Local<Object> obj = Object::New(isolate);
        Local<Value> host;
        if (!ToV8Value(context, host_port.host_name).ToLocal(&host) ||
            obj->Set(context, env->host_string(), host).IsNothing() ||
            obj->Set(context,
                     env->port_string(),
                     Integer::New(isolate, host_port.port))
                .IsNothing()) {
          return;
        }
        value = obj;
        break;
      }
      default:
        UNREACHABLE();
    }
    CHECK(!value.IsEmpty());

    if (!filter.empty()) {
      args.GetReturnValue().Set(value);
      return;
    }

    Local<Value> name = ToV8Value(context, item.first).ToLocalChecked();
    Local<Object> info = Object::New(isolate);
    Local<Value> help_text;
    if (!ToV8Value(context, option_info.help_text).ToLocal(&help_text) ||
        !info->Set(context, env->help_text_string(), help_text)
             .FromMaybe(false) ||
        !info->Set(context,
                   env->env_var_settings_string(),
                   Integer::New(isolate,
                                static_cast<int>(option_info.env_setting)))
             .FromMaybe(false) ||
        !info->Set(context,
                   env->type_string(),
                   Integer::New(isolate, static_cast<int>(option_info.type)))
             .FromMaybe(false) ||
        info->Set(context, env->value_string(), value).IsNothing() ||
        options->Set(context, name, info).IsEmpty()) {
      return;
    }
  }

  if (!filter.empty()) return;

  Local<Value> aliases;
  if (!ToV8Value(context, parser.aliases_).ToLocal(&aliases)) return;

  Local<Object> ret = Object::New(isolate);
  if (ret->Set(context, env->options_string(), options).IsNothing() ||
      ret->Set(context, env->aliases_string(), aliases).IsNothing()) {
    return;
  }

  args.GetReturnValue().Set(ret);
}

void Initialize(Local<Object> target,
                Local<Value> unused,
                Local<Context> context) {
  Environment* env = Environment::GetCurrent(context);
  Isolate* isolate = env->isolate();
  env->SetMethodNoSideEffect(target, "getOptions", GetOptions);

  Local<Object> env_settings = Object::New(isolate);
  NODE_DEFINE_CONSTANT(env_settings, kAllowedInEnvironment);
  NODE_DEFINE_CONSTANT(env_settings, kDisallowedInEnvironment);
  target
      ->Set(
          context, FIXED_ONE_BYTE_STRING(isolate, "envSettings"), env_settings)
      .FromJust();

  Local<Object> types = Object::New(isolate);
  NODE_DEFINE_CONSTANT(types, kNoOp);
  NODE_DEFINE_CONSTANT(types, kV8Option);
  NODE_DEFINE_CONSTANT(types, kBoolean);
  NODE_DEFINE_CONSTANT(types, kInteger);
  NODE_DEFINE_CONSTANT(types, kString);
  NODE_DEFINE_CONSTANT(types, kHostPort);
  NODE_DEFINE_CONSTANT(types, kStringList);
  target->Set(context, FIXED_ONE_BYTE_STRING(isolate, "types"), types)
      .FromJust();
}

}  // namespace options_parser
}  // namespace node

NODE_MODULE_CONTEXT_AWARE_INTERNAL(options, node::options_parser::Initialize)
