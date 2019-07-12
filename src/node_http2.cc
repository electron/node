#include "aliased_buffer.h"
#include "debug_utils.h"
#include "node.h"
#include "node_buffer.h"
#include "node_http2.h"
#include "node_http2_state.h"
#include "node_internals.h"
#include "node_perf.h"

#include <algorithm>

namespace node {

using v8::ArrayBuffer;
using v8::Boolean;
using v8::Context;
using v8::Float64Array;
using v8::Function;
using v8::Integer;
using v8::Number;
using v8::ObjectTemplate;
using v8::String;
using v8::Uint32;
using v8::Uint32Array;
using v8::Undefined;

using node::performance::PerformanceEntry;
namespace http2 {

namespace {

const char zero_bytes_256[256] = {};

inline Http2Stream* GetStream(Http2Session* session,
                              int32_t id,
                              nghttp2_data_source* source) {
  Http2Stream* stream = static_cast<Http2Stream*>(source->ptr);
  if (stream == nullptr)
    stream = session->FindStream(id);
  CHECK_NOT_NULL(stream);
  CHECK_EQ(id, stream->id());
  return stream;
}

}  // anonymous namespace

// These configure the callbacks required by nghttp2 itself. There are
// two sets of callback functions, one that is used if a padding callback
// is set, and other that does not include the padding callback.
const Http2Session::Callbacks Http2Session::callback_struct_saved[2] = {
    Callbacks(false),
    Callbacks(true)};

// The Http2Scope object is used to queue a write to the i/o stream. It is
// used whenever any action is take on the underlying nghttp2 API that may
// push data into nghttp2 outbound data queue.
//
// For example:
//
// Http2Scope h2scope(session);
// nghttp2_submit_ping(**session, ... );
//
// When the Http2Scope passes out of scope and is deconstructed, it will
// call Http2Session::MaybeScheduleWrite().
Http2Scope::Http2Scope(Http2Stream* stream) : Http2Scope(stream->session()) {}

Http2Scope::Http2Scope(Http2Session* session) {
  if (session == nullptr)
    return;

  if (session->flags_ & (SESSION_STATE_HAS_SCOPE |
                         SESSION_STATE_WRITE_SCHEDULED)) {
    // There is another scope further below on the stack, or it is already
    // known that a write is scheduled. In either case, there is nothing to do.
    return;
  }
  session->flags_ |= SESSION_STATE_HAS_SCOPE;
  session_ = session;

  // Always keep the session object alive for at least as long as
  // this scope is active.
  session_handle_ = session->object();
  CHECK(!session_handle_.IsEmpty());
}

Http2Scope::~Http2Scope() {
  if (session_ == nullptr)
    return;

  session_->flags_ &= ~SESSION_STATE_HAS_SCOPE;
  session_->MaybeScheduleWrite();
}

// The Http2Options object is used during the construction of Http2Session
// instances to configure an appropriate nghttp2_options struct. The class
// uses a single TypedArray instance that is shared with the JavaScript side
// to more efficiently pass values back and forth.
Http2Options::Http2Options(Environment* env) {
  nghttp2_option_new(&options_);

  // We manually handle flow control within a session in order to
  // implement backpressure -- that is, we only send WINDOW_UPDATE
  // frames to the remote peer as data is actually consumed by user
  // code. This ensures that the flow of data over the connection
  // does not move too quickly and limits the amount of data we
  // are required to buffer.
  nghttp2_option_set_no_auto_window_update(options_, 1);

  // Enable built in support for ALTSVC frames. Once we add support for
  // other non-built in extension frames, this will need to be handled
  // a bit differently. For now, let's let nghttp2 take care of it.
  nghttp2_option_set_builtin_recv_extension_type(options_, NGHTTP2_ALTSVC);

  AliasedBuffer<uint32_t, Uint32Array>& buffer =
      env->http2_state()->options_buffer;
  uint32_t flags = buffer[IDX_OPTIONS_FLAGS];

  if (flags & (1 << IDX_OPTIONS_MAX_DEFLATE_DYNAMIC_TABLE_SIZE)) {
    nghttp2_option_set_max_deflate_dynamic_table_size(
        options_,
        buffer[IDX_OPTIONS_MAX_DEFLATE_DYNAMIC_TABLE_SIZE]);
  }

  if (flags & (1 << IDX_OPTIONS_MAX_RESERVED_REMOTE_STREAMS)) {
    nghttp2_option_set_max_reserved_remote_streams(
        options_,
        buffer[IDX_OPTIONS_MAX_RESERVED_REMOTE_STREAMS]);
  }

  if (flags & (1 << IDX_OPTIONS_MAX_SEND_HEADER_BLOCK_LENGTH)) {
    nghttp2_option_set_max_send_header_block_length(
        options_,
        buffer[IDX_OPTIONS_MAX_SEND_HEADER_BLOCK_LENGTH]);
  }

  // Recommended default
  nghttp2_option_set_peer_max_concurrent_streams(options_, 100);
  if (flags & (1 << IDX_OPTIONS_PEER_MAX_CONCURRENT_STREAMS)) {
    nghttp2_option_set_peer_max_concurrent_streams(
        options_,
        buffer[IDX_OPTIONS_PEER_MAX_CONCURRENT_STREAMS]);
  }

  // The padding strategy sets the mechanism by which we determine how much
  // additional frame padding to apply to DATA and HEADERS frames. Currently
  // this is set on a per-session basis, but eventually we may switch to
  // a per-stream setting, giving users greater control
  if (flags & (1 << IDX_OPTIONS_PADDING_STRATEGY)) {
    padding_strategy_type strategy =
        static_cast<padding_strategy_type>(
            buffer.GetValue(IDX_OPTIONS_PADDING_STRATEGY));
    SetPaddingStrategy(strategy);
  }

  // The max header list pairs option controls the maximum number of
  // header pairs the session may accept. This is a hard limit.. that is,
  // if the remote peer sends more than this amount, the stream will be
  // automatically closed with an RST_STREAM.
  if (flags & (1 << IDX_OPTIONS_MAX_HEADER_LIST_PAIRS)) {
    SetMaxHeaderPairs(buffer[IDX_OPTIONS_MAX_HEADER_LIST_PAIRS]);
  }

  // The HTTP2 specification places no limits on the number of HTTP2
  // PING frames that can be sent. In order to prevent PINGS from being
  // abused as an attack vector, however, we place a strict upper limit
  // on the number of unacknowledged PINGS that can be sent at any given
  // time.
  if (flags & (1 << IDX_OPTIONS_MAX_OUTSTANDING_PINGS)) {
    SetMaxOutstandingPings(buffer[IDX_OPTIONS_MAX_OUTSTANDING_PINGS]);
  }

  // The HTTP2 specification places no limits on the number of HTTP2
  // SETTINGS frames that can be sent. In order to prevent PINGS from being
  // abused as an attack vector, however, we place a strict upper limit
  // on the number of unacknowledged SETTINGS that can be sent at any given
  // time.
  if (flags & (1 << IDX_OPTIONS_MAX_OUTSTANDING_SETTINGS)) {
    SetMaxOutstandingSettings(buffer[IDX_OPTIONS_MAX_OUTSTANDING_SETTINGS]);
  }

  // The HTTP2 specification places no limits on the amount of memory
  // that a session can consume. In order to prevent abuse, we place a
  // cap on the amount of memory a session can consume at any given time.
  // this is a credit based system. Existing streams may cause the limit
  // to be temporarily exceeded but once over the limit, new streams cannot
  // created.
  // Important: The maxSessionMemory option in javascript is expressed in
  //            terms of MB increments (i.e. the value 1 == 1 MB)
  if (flags & (1 << IDX_OPTIONS_MAX_SESSION_MEMORY)) {
    SetMaxSessionMemory(buffer[IDX_OPTIONS_MAX_SESSION_MEMORY] * 1e6);
  }
}

void Http2Session::Http2Settings::Init() {
  AliasedBuffer<uint32_t, Uint32Array>& buffer =
      env()->http2_state()->settings_buffer;
  uint32_t flags = buffer[IDX_SETTINGS_COUNT];

  size_t n = 0;

#define GRABSETTING(N, trace)                                                 \
  if (flags & (1 << IDX_SETTINGS_##N)) {                                      \
    uint32_t val = buffer[IDX_SETTINGS_##N];                                  \
    if (session_ != nullptr)                                                  \
      Debug(session_, "setting " trace ": %d\n", val);                        \
    entries_[n++] =                                                           \
        nghttp2_settings_entry {NGHTTP2_SETTINGS_##N, val};                   \
  }

  GRABSETTING(HEADER_TABLE_SIZE, "header table size");
  GRABSETTING(MAX_CONCURRENT_STREAMS, "max concurrent streams");
  GRABSETTING(MAX_FRAME_SIZE, "max frame size");
  GRABSETTING(INITIAL_WINDOW_SIZE, "initial window size");
  GRABSETTING(MAX_HEADER_LIST_SIZE, "max header list size");
  GRABSETTING(ENABLE_PUSH, "enable push");

#undef GRABSETTING

  count_ = n;
}

Http2Session::Http2Settings::Http2Settings(
    Environment* env)
        : AsyncWrap(env,
                    env->http2settings_constructor_template()
                        ->NewInstance(env->context())
                            .ToLocalChecked(),
                    AsyncWrap::PROVIDER_HTTP2SETTINGS),
          session_(nullptr),
          startTime_(0) {
  Init();
}

// The Http2Settings class is used to configure a SETTINGS frame that is
// to be sent to the connected peer. The settings are set using a TypedArray
// that is shared with the JavaScript side.
Http2Session::Http2Settings::Http2Settings(
    Http2Session* session)
        : AsyncWrap(session->env(),
                    session->env()->http2settings_constructor_template()
                        ->NewInstance(session->env()->context())
                            .ToLocalChecked(),
                    AsyncWrap::PROVIDER_HTTP2SETTINGS),
          session_(session),
          startTime_(uv_hrtime()) {
  Init();
}

// Generates a Buffer that contains the serialized payload of a SETTINGS
// frame. This can be used, for instance, to create the Base64-encoded
// content of an Http2-Settings header field.
Local<Value> Http2Session::Http2Settings::Pack() {
  const size_t len = count_ * 6;
  Local<Value> buf = Buffer::New(env(), len).ToLocalChecked();
  ssize_t ret =
      nghttp2_pack_settings_payload(
        reinterpret_cast<uint8_t*>(Buffer::Data(buf)), len,
        &entries_[0], count_);
  if (ret >= 0)
    return buf;
  else
    return Undefined(env()->isolate());
}

// Updates the shared TypedArray with the current remote or local settings for
// the session.
void Http2Session::Http2Settings::Update(Environment* env,
                                         Http2Session* session,
                                         get_setting fn) {
  AliasedBuffer<uint32_t, Uint32Array>& buffer =
      env->http2_state()->settings_buffer;
  buffer[IDX_SETTINGS_HEADER_TABLE_SIZE] =
      fn(**session, NGHTTP2_SETTINGS_HEADER_TABLE_SIZE);
  buffer[IDX_SETTINGS_MAX_CONCURRENT_STREAMS] =
      fn(**session, NGHTTP2_SETTINGS_MAX_CONCURRENT_STREAMS);
  buffer[IDX_SETTINGS_INITIAL_WINDOW_SIZE] =
      fn(**session, NGHTTP2_SETTINGS_INITIAL_WINDOW_SIZE);
  buffer[IDX_SETTINGS_MAX_FRAME_SIZE] =
      fn(**session, NGHTTP2_SETTINGS_MAX_FRAME_SIZE);
  buffer[IDX_SETTINGS_MAX_HEADER_LIST_SIZE] =
      fn(**session, NGHTTP2_SETTINGS_MAX_HEADER_LIST_SIZE);
  buffer[IDX_SETTINGS_ENABLE_PUSH] =
      fn(**session, NGHTTP2_SETTINGS_ENABLE_PUSH);
}

// Initializes the shared TypedArray with the default settings values.
void Http2Session::Http2Settings::RefreshDefaults(Environment* env) {
  AliasedBuffer<uint32_t, Uint32Array>& buffer =
      env->http2_state()->settings_buffer;

  buffer[IDX_SETTINGS_HEADER_TABLE_SIZE] =
      DEFAULT_SETTINGS_HEADER_TABLE_SIZE;
  buffer[IDX_SETTINGS_ENABLE_PUSH] =
      DEFAULT_SETTINGS_ENABLE_PUSH;
  buffer[IDX_SETTINGS_INITIAL_WINDOW_SIZE] =
      DEFAULT_SETTINGS_INITIAL_WINDOW_SIZE;
  buffer[IDX_SETTINGS_MAX_FRAME_SIZE] =
      DEFAULT_SETTINGS_MAX_FRAME_SIZE;
  buffer[IDX_SETTINGS_MAX_HEADER_LIST_SIZE] =
      DEFAULT_SETTINGS_MAX_HEADER_LIST_SIZE;
  buffer[IDX_SETTINGS_COUNT] =
    (1 << IDX_SETTINGS_HEADER_TABLE_SIZE) |
    (1 << IDX_SETTINGS_ENABLE_PUSH) |
    (1 << IDX_SETTINGS_INITIAL_WINDOW_SIZE) |
    (1 << IDX_SETTINGS_MAX_FRAME_SIZE) |
    (1 << IDX_SETTINGS_MAX_HEADER_LIST_SIZE);
}


void Http2Session::Http2Settings::Send() {
  Http2Scope h2scope(session_);
  CHECK_EQ(nghttp2_submit_settings(**session_, NGHTTP2_FLAG_NONE,
                                   &entries_[0], count_), 0);
}

void Http2Session::Http2Settings::Done(bool ack) {
  uint64_t end = uv_hrtime();
  double duration = (end - startTime_) / 1e6;

  Local<Value> argv[2] = {
    Boolean::New(env()->isolate(), ack),
    Number::New(env()->isolate(), duration)
  };
  MakeCallback(env()->ondone_string(), arraysize(argv), argv);
  delete this;
}

// The Http2Priority class initializes an appropriate nghttp2_priority_spec
// struct used when either creating a stream or updating its priority
// settings.
Http2Priority::Http2Priority(Environment* env,
                             Local<Value> parent,
                             Local<Value> weight,
                             Local<Value> exclusive) {
  Local<Context> context = env->context();
  int32_t parent_ = parent->Int32Value(context).ToChecked();
  int32_t weight_ = weight->Int32Value(context).ToChecked();
  bool exclusive_ = exclusive->BooleanValue(context).ToChecked();
  Debug(env, DebugCategory::HTTP2STREAM,
        "Http2Priority: parent: %d, weight: %d, exclusive: %d\n",
        parent_, weight_, exclusive_);
  nghttp2_priority_spec_init(&spec, parent_, weight_, exclusive_ ? 1 : 0);
}


const char* Http2Session::TypeName() const {
  switch (session_type_) {
    case NGHTTP2_SESSION_SERVER: return "server";
    case NGHTTP2_SESSION_CLIENT: return "client";
    default:
      // This should never happen
      ABORT();
  }
}

// The Headers class initializes a proper array of nghttp2_nv structs
// containing the header name value pairs.
Headers::Headers(Isolate* isolate,
                 Local<Context> context,
                 Local<Array> headers) {
  Local<Value> header_string = headers->Get(context, 0).ToLocalChecked();
  Local<Value> header_count = headers->Get(context, 1).ToLocalChecked();
  count_ = header_count.As<Uint32>()->Value();
  int header_string_len = header_string.As<String>()->Length();

  if (count_ == 0) {
    CHECK_EQ(header_string_len, 0);
    return;
  }

  // Allocate a single buffer with count_ nghttp2_nv structs, followed
  // by the raw header data as passed from JS. This looks like:
  // | possible padding | nghttp2_nv | nghttp2_nv | ... | header contents |
  buf_.AllocateSufficientStorage((alignof(nghttp2_nv) - 1) +
                                 count_ * sizeof(nghttp2_nv) +
                                 header_string_len);
  // Make sure the start address is aligned appropriately for an nghttp2_nv*.
  char* start = reinterpret_cast<char*>(
      ROUND_UP(reinterpret_cast<uintptr_t>(*buf_), alignof(nghttp2_nv)));
  char* header_contents = start + (count_ * sizeof(nghttp2_nv));
  nghttp2_nv* const nva = reinterpret_cast<nghttp2_nv*>(start);

  CHECK_LE(header_contents + header_string_len, *buf_ + buf_.length());
  CHECK_EQ(header_string.As<String>()->WriteOneByte(
               isolate,
               reinterpret_cast<uint8_t*>(header_contents),
               0,
               header_string_len,
               String::NO_NULL_TERMINATION),
           header_string_len);

  size_t n = 0;
  char* p;
  for (p = header_contents; p < header_contents + header_string_len; n++) {
    if (n >= count_) {
      // This can happen if a passed header contained a null byte. In that
      // case, just provide nghttp2 with an invalid header to make it reject
      // the headers list.
      static uint8_t zero = '\0';
      nva[0].name = nva[0].value = &zero;
      nva[0].namelen = nva[0].valuelen = 1;
      count_ = 1;
      return;
    }

    nva[n].flags = NGHTTP2_NV_FLAG_NONE;
    nva[n].name = reinterpret_cast<uint8_t*>(p);
    nva[n].namelen = strlen(p);
    p += nva[n].namelen + 1;
    nva[n].value = reinterpret_cast<uint8_t*>(p);
    nva[n].valuelen = strlen(p);
    p += nva[n].valuelen + 1;
  }
}


// Sets the various callback functions that nghttp2 will use to notify us
// about significant events while processing http2 stuff.
Http2Session::Callbacks::Callbacks(bool kHasGetPaddingCallback) {
  CHECK_EQ(nghttp2_session_callbacks_new(&callbacks), 0);

  nghttp2_session_callbacks_set_on_begin_headers_callback(
    callbacks, OnBeginHeadersCallback);
  nghttp2_session_callbacks_set_on_header_callback2(
    callbacks, OnHeaderCallback);
  nghttp2_session_callbacks_set_on_frame_recv_callback(
    callbacks, OnFrameReceive);
  nghttp2_session_callbacks_set_on_stream_close_callback(
    callbacks, OnStreamClose);
  nghttp2_session_callbacks_set_on_data_chunk_recv_callback(
    callbacks, OnDataChunkReceived);
  nghttp2_session_callbacks_set_on_frame_not_send_callback(
    callbacks, OnFrameNotSent);
  nghttp2_session_callbacks_set_on_invalid_header_callback2(
    callbacks, OnInvalidHeader);
  nghttp2_session_callbacks_set_error_callback(
    callbacks, OnNghttpError);
  nghttp2_session_callbacks_set_send_data_callback(
    callbacks, OnSendData);
  nghttp2_session_callbacks_set_on_invalid_frame_recv_callback(
    callbacks, OnInvalidFrame);
  nghttp2_session_callbacks_set_on_frame_send_callback(
    callbacks, OnFrameSent);

  if (kHasGetPaddingCallback) {
    nghttp2_session_callbacks_set_select_padding_callback(
      callbacks, OnSelectPadding);
  }
}


Http2Session::Callbacks::~Callbacks() {
  nghttp2_session_callbacks_del(callbacks);
}

// Track memory allocated by nghttp2 using a custom allocator.
class Http2Session::MemoryAllocatorInfo {
 public:
  explicit MemoryAllocatorInfo(Http2Session* session)
      : info({ session, H2Malloc, H2Free, H2Calloc, H2Realloc }) {}

  static void* H2Malloc(size_t size, void* user_data) {
    return H2Realloc(nullptr, size, user_data);
  }

  static void* H2Calloc(size_t nmemb, size_t size, void* user_data) {
    size_t real_size = MultiplyWithOverflowCheck(nmemb, size);
    void* mem = H2Malloc(real_size, user_data);
    if (mem != nullptr)
      memset(mem, 0, real_size);
    return mem;
  }

  static void H2Free(void* ptr, void* user_data) {
    if (ptr == nullptr) return;  // free(null); happens quite often.
    void* result = H2Realloc(ptr, 0, user_data);
    CHECK_EQ(result, nullptr);
  }

  static void* H2Realloc(void* ptr, size_t size, void* user_data) {
    Http2Session* session = static_cast<Http2Session*>(user_data);
    size_t previous_size = 0;
    char* original_ptr = nullptr;

    // We prepend each allocated buffer with a size_t containing the full
    // size of the allocation.
    if (size > 0) size += sizeof(size_t);

    if (ptr != nullptr) {
      // We are free()ing or re-allocating.
      original_ptr = static_cast<char*>(ptr) - sizeof(size_t);
      previous_size = *reinterpret_cast<size_t*>(original_ptr);
      // This means we called StopTracking() on this pointer before.
      if (previous_size == 0) {
        // Fall back to the standard Realloc() function.
        char* ret = UncheckedRealloc(original_ptr, size);
        if (ret != nullptr)
          ret += sizeof(size_t);
        return ret;
      }
    }
    CHECK_GE(session->current_nghttp2_memory_, previous_size);

    // TODO(addaleax): Add the following, and handle NGHTTP2_ERR_NOMEM properly
    // everywhere:
    //
    // if (size > previous_size &&
    //     !session->IsAvailableSessionMemory(size - previous_size)) {
    //  return nullptr;
    //}

    char* mem = UncheckedRealloc(original_ptr, size);

    if (mem != nullptr) {
      // Adjust the memory info counter.
      session->current_nghttp2_memory_ += size - previous_size;
      *reinterpret_cast<size_t*>(mem) = size;
      mem += sizeof(size_t);
    } else if (size == 0) {
      session->current_nghttp2_memory_ -= previous_size;
    }

    return mem;
  }

  static void StopTracking(Http2Session* session, void* ptr) {
    size_t* original_ptr = reinterpret_cast<size_t*>(
        static_cast<char*>(ptr) - sizeof(size_t));
    session->current_nghttp2_memory_ -= *original_ptr;
    *original_ptr = 0;
  }

  inline nghttp2_mem* operator*() { return &info; }

  nghttp2_mem info;
};

void Http2Session::StopTrackingRcbuf(nghttp2_rcbuf* buf) {
  MemoryAllocatorInfo::StopTracking(this, buf);
}

Http2Session::Http2Session(Environment* env,
                           Local<Object> wrap,
                           nghttp2_session_type type)
    : AsyncWrap(env, wrap, AsyncWrap::PROVIDER_HTTP2SESSION),
      session_type_(type) {
  MakeWeak();
  statistics_.start_time = uv_hrtime();

  // Capture the configuration options for this session
  Http2Options opts(env);

  max_session_memory_ = opts.GetMaxSessionMemory();

  uint32_t maxHeaderPairs = opts.GetMaxHeaderPairs();
  max_header_pairs_ =
      type == NGHTTP2_SESSION_SERVER
          ? std::max(maxHeaderPairs, 4U)     // minimum # of request headers
          : std::max(maxHeaderPairs, 1U);    // minimum # of response headers

  max_outstanding_pings_ = opts.GetMaxOutstandingPings();
  max_outstanding_settings_ = opts.GetMaxOutstandingSettings();

  padding_strategy_ = opts.GetPaddingStrategy();

  bool hasGetPaddingCallback =
      padding_strategy_ != PADDING_STRATEGY_NONE;

  nghttp2_session_callbacks* callbacks
      = callback_struct_saved[hasGetPaddingCallback ? 1 : 0].callbacks;

  auto fn = type == NGHTTP2_SESSION_SERVER ?
      nghttp2_session_server_new3 :
      nghttp2_session_client_new3;

  MemoryAllocatorInfo allocator_info(this);

  // This should fail only if the system is out of memory, which
  // is going to cause lots of other problems anyway, or if any
  // of the options are out of acceptable range, which we should
  // be catching before it gets this far. Either way, crash if this
  // fails.
  CHECK_EQ(fn(&session_, callbacks, this, *opts, *allocator_info), 0);

  outgoing_storage_.reserve(4096);
  outgoing_buffers_.reserve(32);
}

Http2Session::~Http2Session() {
  CHECK_EQ(flags_ & SESSION_STATE_HAS_SCOPE, 0);
  Debug(this, "freeing nghttp2 session");
  for (const auto& stream : streams_)
    stream.second->session_ = nullptr;
  nghttp2_session_del(session_);
  CHECK_EQ(current_nghttp2_memory_, 0);
}

std::string Http2Session::diagnostic_name() const {
  return std::string("Http2Session ") + TypeName() + " (" +
      std::to_string(static_cast<int64_t>(get_async_id())) + ")";
}

inline bool HasHttp2Observer(Environment* env) {
  AliasedBuffer<uint32_t, Uint32Array>& observers =
      env->performance_state()->observers;
  return observers[performance::NODE_PERFORMANCE_ENTRY_TYPE_HTTP2] != 0;
}

void Http2Stream::EmitStatistics() {
  if (!HasHttp2Observer(env()))
    return;
  Http2StreamPerformanceEntry* entry =
    new Http2StreamPerformanceEntry(env(), id_, statistics_);
  env()->SetImmediate([](Environment* env, void* data) {
    // This takes ownership, the entry is destroyed at the end of this scope.
    std::unique_ptr<Http2StreamPerformanceEntry> entry {
        static_cast<Http2StreamPerformanceEntry*>(data) };
    if (!HasHttp2Observer(env))
      return;
    HandleScope handle_scope(env->isolate());
    AliasedBuffer<double, Float64Array>& buffer =
        env->http2_state()->stream_stats_buffer;
    buffer[IDX_STREAM_STATS_ID] = entry->id();
    if (entry->first_byte() != 0) {
      buffer[IDX_STREAM_STATS_TIMETOFIRSTBYTE] =
          (entry->first_byte() - entry->startTimeNano()) / 1e6;
    } else {
      buffer[IDX_STREAM_STATS_TIMETOFIRSTBYTE] = 0;
    }
    if (entry->first_header() != 0) {
      buffer[IDX_STREAM_STATS_TIMETOFIRSTHEADER] =
          (entry->first_header() - entry->startTimeNano()) / 1e6;
    } else {
      buffer[IDX_STREAM_STATS_TIMETOFIRSTHEADER] = 0;
    }
    if (entry->first_byte_sent() != 0) {
      buffer[IDX_STREAM_STATS_TIMETOFIRSTBYTESENT] =
          (entry->first_byte_sent() - entry->startTimeNano()) / 1e6;
    } else {
      buffer[IDX_STREAM_STATS_TIMETOFIRSTBYTESENT] = 0;
    }
    buffer[IDX_STREAM_STATS_SENTBYTES] = entry->sent_bytes();
    buffer[IDX_STREAM_STATS_RECEIVEDBYTES] = entry->received_bytes();
    entry->Notify(entry->ToObject());
  }, static_cast<void*>(entry));
}

void Http2Session::EmitStatistics() {
  if (!HasHttp2Observer(env()))
    return;
  Http2SessionPerformanceEntry* entry =
    new Http2SessionPerformanceEntry(env(), statistics_, session_type_);
  env()->SetImmediate([](Environment* env, void* data) {
    // This takes ownership, the entr is destroyed at the end of this scope.
    std::unique_ptr<Http2SessionPerformanceEntry> entry {
        static_cast<Http2SessionPerformanceEntry*>(data) };
    if (!HasHttp2Observer(env))
      return;
    HandleScope handle_scope(env->isolate());
    AliasedBuffer<double, Float64Array>& buffer =
        env->http2_state()->session_stats_buffer;
    buffer[IDX_SESSION_STATS_TYPE] = entry->type();
    buffer[IDX_SESSION_STATS_PINGRTT] = entry->ping_rtt() / 1e6;
    buffer[IDX_SESSION_STATS_FRAMESRECEIVED] = entry->frame_count();
    buffer[IDX_SESSION_STATS_FRAMESSENT] = entry->frame_sent();
    buffer[IDX_SESSION_STATS_STREAMCOUNT] = entry->stream_count();
    buffer[IDX_SESSION_STATS_STREAMAVERAGEDURATION] =
        entry->stream_average_duration();
    buffer[IDX_SESSION_STATS_DATA_SENT] = entry->data_sent();
    buffer[IDX_SESSION_STATS_DATA_RECEIVED] = entry->data_received();
    buffer[IDX_SESSION_STATS_MAX_CONCURRENT_STREAMS] =
        entry->max_concurrent_streams();
    entry->Notify(entry->ToObject());
  }, static_cast<void*>(entry));
}

// Closes the session and frees the associated resources
void Http2Session::Close(uint32_t code, bool socket_closed) {
  Debug(this, "closing session");

  if (flags_ & SESSION_STATE_CLOSING)
    return;
  flags_ |= SESSION_STATE_CLOSING;

  // Stop reading on the i/o stream
  if (stream_ != nullptr)
    stream_->ReadStop();

  // If the socket is not closed, then attempt to send a closing GOAWAY
  // frame. There is no guarantee that this GOAWAY will be received by
  // the peer but the HTTP/2 spec recommends sending it anyway. We'll
  // make a best effort.
  if (!socket_closed) {
    Debug(this, "terminating session with code %d", code);
    CHECK_EQ(nghttp2_session_terminate_session(session_, code), 0);
    SendPendingData();
  } else if (stream_ != nullptr) {
    stream_->RemoveStreamListener(this);
  }

  flags_ |= SESSION_STATE_CLOSED;

  // If there are outstanding pings, those will need to be canceled, do
  // so on the next iteration of the event loop to avoid calling out into
  // javascript since this may be called during garbage collection.
  while (!outstanding_pings_.empty()) {
    Http2Session::Http2Ping* ping = PopPing();
    env()->SetImmediate([](Environment* env, void* data) {
      static_cast<Http2Session::Http2Ping*>(data)->Done(false);
    }, static_cast<void*>(ping));
  }

  statistics_.end_time = uv_hrtime();
  EmitStatistics();
}

// Locates an existing known stream by ID. nghttp2 has a similar method
// but this is faster and does not fail if the stream is not found.
inline Http2Stream* Http2Session::FindStream(int32_t id) {
  auto s = streams_.find(id);
  return s != streams_.end() ? s->second : nullptr;
}

inline bool Http2Session::CanAddStream() {
  uint32_t maxConcurrentStreams =
      nghttp2_session_get_local_settings(
          session_, NGHTTP2_SETTINGS_MAX_CONCURRENT_STREAMS);
  size_t maxSize =
      std::min(streams_.max_size(), static_cast<size_t>(maxConcurrentStreams));
  // We can add a new stream so long as we are less than the current
  // maximum on concurrent streams and there's enough available memory
  return streams_.size() < maxSize &&
         IsAvailableSessionMemory(sizeof(Http2Stream));
}

inline void Http2Session::AddStream(Http2Stream* stream) {
  CHECK_GE(++statistics_.stream_count, 0);
  streams_[stream->id()] = stream;
  size_t size = streams_.size();
  if (size > statistics_.max_concurrent_streams)
    statistics_.max_concurrent_streams = size;
  IncrementCurrentSessionMemory(sizeof(*stream));
}


inline void Http2Session::RemoveStream(Http2Stream* stream) {
  if (streams_.empty() || stream == nullptr)
    return;  // Nothing to remove, item was never added?
  streams_.erase(stream->id());
  DecrementCurrentSessionMemory(sizeof(*stream));
}

// Used as one of the Padding Strategy functions. Will attempt to ensure
// that the total frame size, including header bytes, are 8-byte aligned.
// If maxPayloadLen is smaller than the number of bytes necessary to align,
// will return maxPayloadLen instead.
ssize_t Http2Session::OnDWordAlignedPadding(size_t frameLen,
                                            size_t maxPayloadLen) {
  size_t r = (frameLen + 9) % 8;
  if (r == 0) return frameLen;  // If already a multiple of 8, return.

  size_t pad = frameLen + (8 - r);

  // If maxPayloadLen happens to be less than the calculated pad length,
  // use the max instead, even tho this means the frame will not be
  // aligned.
  pad = std::min(maxPayloadLen, pad);
  Debug(this, "using frame size padding: %d", pad);
  return pad;
}

// Used as one of the Padding Strategy functions. Uses the maximum amount
// of padding allowed for the current frame.
ssize_t Http2Session::OnMaxFrameSizePadding(size_t frameLen,
                                            size_t maxPayloadLen) {
  Debug(this, "using max frame size padding: %d", maxPayloadLen);
  return maxPayloadLen;
}

// Used as one of the Padding Strategy functions. Uses a callback to JS land
// to determine the amount of padding for the current frame. This option is
// rather more expensive because of the JS boundary cross. It generally should
// not be the preferred option.
ssize_t Http2Session::OnCallbackPadding(size_t frameLen,
                                        size_t maxPayloadLen) {
  if (frameLen == 0) return 0;
  Debug(this, "using callback to determine padding");
  Isolate* isolate = env()->isolate();
  HandleScope handle_scope(isolate);
  Local<Context> context = env()->context();
  Context::Scope context_scope(context);

  AliasedBuffer<uint32_t, Uint32Array>& buffer =
      env()->http2_state()->padding_buffer;
  buffer[PADDING_BUF_FRAME_LENGTH] = frameLen;
  buffer[PADDING_BUF_MAX_PAYLOAD_LENGTH] = maxPayloadLen;
  buffer[PADDING_BUF_RETURN_VALUE] = frameLen;
  MakeCallback(env()->ongetpadding_string(), 0, nullptr);
  uint32_t retval = buffer[PADDING_BUF_RETURN_VALUE];
  retval = std::min(retval, static_cast<uint32_t>(maxPayloadLen));
  retval = std::max(retval, static_cast<uint32_t>(frameLen));
  Debug(this, "using padding size %d", retval);
  return retval;
}


// Write data received from the i/o stream to the underlying nghttp2_session.
// On each call to nghttp2_session_mem_recv, nghttp2 will begin calling the
// various callback functions. Each of these will typically result in a call
// out to JavaScript so this particular function is rather hot and can be
// quite expensive. This is a potential performance optimization target later.
ssize_t Http2Session::Write(const uv_buf_t* bufs, size_t nbufs) {
  size_t total = 0;
  // Note that nghttp2_session_mem_recv is a synchronous operation that
  // will trigger a number of other callbacks. Those will, in turn have
  // multiple side effects.
  for (size_t n = 0; n < nbufs; n++) {
    Debug(this, "receiving %d bytes [wants data? %d]",
          bufs[n].len,
          nghttp2_session_want_read(session_));
    ssize_t ret =
      nghttp2_session_mem_recv(session_,
                               reinterpret_cast<uint8_t*>(bufs[n].base),
                               bufs[n].len);
    CHECK_NE(ret, NGHTTP2_ERR_NOMEM);

    if (ret < 0)
      return ret;

    total += ret;
  }
  // Send any data that was queued up while processing the received data.
  if (!IsDestroyed()) {
    SendPendingData();
  }
  return total;
}


inline int32_t GetFrameID(const nghttp2_frame* frame) {
  // If this is a push promise, we want to grab the id of the promised stream
  return (frame->hd.type == NGHTTP2_PUSH_PROMISE) ?
      frame->push_promise.promised_stream_id :
      frame->hd.stream_id;
}


// Called by nghttp2 at the start of receiving a HEADERS frame. We use this
// callback to determine if a new stream is being created or if we are simply
// adding a new block of headers to an existing stream. The header pairs
// themselves are set in the OnHeaderCallback
int Http2Session::OnBeginHeadersCallback(nghttp2_session* handle,
                                         const nghttp2_frame* frame,
                                         void* user_data) {
  Http2Session* session = static_cast<Http2Session*>(user_data);
  int32_t id = GetFrameID(frame);
  Debug(session, "beginning headers for stream %d", id);

  Http2Stream* stream = session->FindStream(id);
  if (stream == nullptr) {
    if (!session->CanAddStream()) {
      // Too many concurrent streams being opened
      nghttp2_submit_rst_stream(**session, NGHTTP2_FLAG_NONE, id,
                                NGHTTP2_ENHANCE_YOUR_CALM);
      return NGHTTP2_ERR_TEMPORAL_CALLBACK_FAILURE;
    }
    new Http2Stream(session, id, frame->headers.cat);
  } else if (!stream->IsDestroyed()) {
    stream->StartHeaders(frame->headers.cat);
  }
  return 0;
}

// Called by nghttp2 for each header name/value pair in a HEADERS block.
// This had to have been preceded by a call to OnBeginHeadersCallback so
// the Http2Stream is guaranteed to already exist.
int Http2Session::OnHeaderCallback(nghttp2_session* handle,
                                   const nghttp2_frame* frame,
                                   nghttp2_rcbuf* name,
                                   nghttp2_rcbuf* value,
                                   uint8_t flags,
                                   void* user_data) {
  Http2Session* session = static_cast<Http2Session*>(user_data);
  int32_t id = GetFrameID(frame);
  Http2Stream* stream = session->FindStream(id);
  // If stream is null at this point, either something odd has happened
  // or the stream was closed locally while header processing was occurring.
  // either way, do not proceed and close the stream.
  if (stream == nullptr)
    return NGHTTP2_ERR_TEMPORAL_CALLBACK_FAILURE;

  // If the stream has already been destroyed, ignore.
  if (!stream->IsDestroyed() && !stream->AddHeader(name, value, flags)) {
    // This will only happen if the connected peer sends us more
    // than the allowed number of header items at any given time
    stream->SubmitRstStream(NGHTTP2_ENHANCE_YOUR_CALM);
    return NGHTTP2_ERR_TEMPORAL_CALLBACK_FAILURE;
  }
  return 0;
}


// Called by nghttp2 when a complete HTTP2 frame has been received. There are
// only a handful of frame types tha we care about handling here.
int Http2Session::OnFrameReceive(nghttp2_session* handle,
                                 const nghttp2_frame* frame,
                                 void* user_data) {
  Http2Session* session = static_cast<Http2Session*>(user_data);
  session->statistics_.frame_count++;
  Debug(session, "complete frame received: type: %d",
        frame->hd.type);
  switch (frame->hd.type) {
    case NGHTTP2_DATA:
      session->HandleDataFrame(frame);
      break;
    case NGHTTP2_PUSH_PROMISE:
      // Intentional fall-through, handled just like headers frames
    case NGHTTP2_HEADERS:
      session->HandleHeadersFrame(frame);
      break;
    case NGHTTP2_SETTINGS:
      session->HandleSettingsFrame(frame);
      break;
    case NGHTTP2_PRIORITY:
      session->HandlePriorityFrame(frame);
      break;
    case NGHTTP2_GOAWAY:
      session->HandleGoawayFrame(frame);
      break;
    case NGHTTP2_PING:
      session->HandlePingFrame(frame);
      break;
    case NGHTTP2_ALTSVC:
      session->HandleAltSvcFrame(frame);
      break;
    default:
      break;
  }
  return 0;
}

int Http2Session::OnInvalidFrame(nghttp2_session* handle,
                                 const nghttp2_frame* frame,
                                 int lib_error_code,
                                 void* user_data) {
  Http2Session* session = static_cast<Http2Session*>(user_data);

  Debug(session, "invalid frame received, code: %d", lib_error_code);

  // If the error is fatal or if error code is ERR_STREAM_CLOSED... emit error
  if (nghttp2_is_fatal(lib_error_code) ||
      lib_error_code == NGHTTP2_ERR_STREAM_CLOSED) {
    Environment* env = session->env();
    Isolate* isolate = env->isolate();
    HandleScope scope(isolate);
    Local<Context> context = env->context();
    Context::Scope context_scope(context);
    Local<Value> arg = Integer::New(isolate, lib_error_code);
    session->MakeCallback(env->error_string(), 1, &arg);
  }
  return 0;
}

// If nghttp2 is unable to send a queued up frame, it will call this callback
// to let us know. If the failure occurred because we are in the process of
// closing down the session or stream, we go ahead and ignore it. We don't
// really care about those and there's nothing we can reasonably do about it
// anyway. Other types of failures are reported up to JavaScript. This should
// be exceedingly rare.
int Http2Session::OnFrameNotSent(nghttp2_session* handle,
                                 const nghttp2_frame* frame,
                                 int error_code,
                                 void* user_data) {
  Http2Session* session = static_cast<Http2Session*>(user_data);
  Environment* env = session->env();
  Debug(session, "frame type %d was not sent, code: %d",
        frame->hd.type, error_code);
  // Do not report if the frame was not sent due to the session closing
  if (error_code != NGHTTP2_ERR_SESSION_CLOSING &&
      error_code != NGHTTP2_ERR_STREAM_CLOSED &&
      error_code != NGHTTP2_ERR_STREAM_CLOSING) {
    Isolate* isolate = env->isolate();
    HandleScope scope(isolate);
    Local<Context> context = env->context();
    Context::Scope context_scope(context);

    Local<Value> argv[3] = {
      Integer::New(isolate, frame->hd.stream_id),
      Integer::New(isolate, frame->hd.type),
      Integer::New(isolate, error_code)
    };
    session->MakeCallback(env->onframeerror_string(), arraysize(argv), argv);
  }
  return 0;
}

int Http2Session::OnFrameSent(nghttp2_session* handle,
                              const nghttp2_frame* frame,
                              void* user_data) {
  Http2Session* session = static_cast<Http2Session*>(user_data);
  session->statistics_.frame_sent += 1;
  return 0;
}

// Called by nghttp2 when a stream closes.
int Http2Session::OnStreamClose(nghttp2_session* handle,
                                int32_t id,
                                uint32_t code,
                                void* user_data) {
  Http2Session* session = static_cast<Http2Session*>(user_data);
  Environment* env = session->env();
  Isolate* isolate = env->isolate();
  HandleScope scope(isolate);
  Local<Context> context = env->context();
  Context::Scope context_scope(context);
  Debug(session, "stream %d closed with code: %d", id, code);
  Http2Stream* stream = session->FindStream(id);
  // Intentionally ignore the callback if the stream does not exist or has
  // already been destroyed
  if (stream != nullptr && !stream->IsDestroyed()) {
    stream->Close(code);
    // It is possible for the stream close to occur before the stream is
    // ever passed on to the javascript side. If that happens, skip straight
    // to destroying the stream. We can check this by looking for the
    // onstreamclose function. If it exists, then the stream has already
    // been passed on to javascript.
    Local<Value> fn =
        stream->object()->Get(context, env->onstreamclose_string())
            .ToLocalChecked();
    if (fn->IsFunction()) {
      Local<Value> argv[] = {
        Integer::NewFromUnsigned(isolate, code)
      };
      stream->MakeCallback(fn.As<Function>(), arraysize(argv), argv);
    } else {
      stream->Destroy();
    }
  }
  return 0;
}

// Called by nghttp2 when an invalid header has been received. For now, we
// ignore these. If this callback was not provided, nghttp2 would handle
// invalid headers strictly and would shut down the stream. We are intentionally
// being more lenient here although we may want to revisit this choice later.
int Http2Session::OnInvalidHeader(nghttp2_session* session,
                                  const nghttp2_frame* frame,
                                  nghttp2_rcbuf* name,
                                  nghttp2_rcbuf* value,
                                  uint8_t flags,
                                  void* user_data) {
  // Ignore invalid header fields by default.
  return 0;
}

// When nghttp2 receives a DATA frame, it will deliver the data payload to
// us in discrete chunks. We push these into a linked list stored in the
// Http2Sttream which is flushed out to JavaScript as quickly as possible.
// This can be a particularly hot path.
int Http2Session::OnDataChunkReceived(nghttp2_session* handle,
                                      uint8_t flags,
                                      int32_t id,
                                      const uint8_t* data,
                                      size_t len,
                                      void* user_data) {
  Http2Session* session = static_cast<Http2Session*>(user_data);
  Debug(session, "buffering data chunk for stream %d, size: "
        "%d, flags: %d", id, len, flags);
  Environment* env = session->env();
  HandleScope scope(env->isolate());
  // We should never actually get a 0-length chunk so this check is
  // only a precaution at this point.
  if (len > 0) {
    // Notify nghttp2 that we've consumed a chunk of data on the connection
    // so that it can send a WINDOW_UPDATE frame. This is a critical part of
    // the flow control process in http2
    CHECK_EQ(nghttp2_session_consume_connection(handle, len), 0);
    Http2Stream* stream = session->FindStream(id);
    // If the stream has been destroyed, ignore this chunk
    if (stream->IsDestroyed())
      return 0;

    stream->statistics_.received_bytes += len;

    // Repeatedly ask the stream's owner for memory, and copy the read data
    // into those buffers.
    // The typical case is actually the exception here; Http2StreamListeners
    // know about the HTTP2 session associated with this stream, so they know
    // about the larger from-socket read buffer, so they do not require copying.
    do {
      uv_buf_t buf = stream->EmitAlloc(len);
      ssize_t avail = len;
      if (static_cast<ssize_t>(buf.len) < avail)
        avail = buf.len;

      // `buf.base == nullptr` is the default Http2StreamListener's way
      // of saying that it wants a pointer to the raw original.
      // Since it has access to the original socket buffer from which the data
      // was read in the first place, it can use that to minimize ArrayBuffer
      // allocations.
      if (LIKELY(buf.base == nullptr))
        buf.base = reinterpret_cast<char*>(const_cast<uint8_t*>(data));
      else
        memcpy(buf.base, data, avail);
      data += avail;
      len -= avail;
      stream->EmitRead(avail, buf);

      // If the stream owner (e.g. the JS Http2Stream) wants more data, just
      // tell nghttp2 that all data has been consumed. Otherwise, defer until
      // more data is being requested.
      if (stream->IsReading())
        nghttp2_session_consume_stream(handle, id, avail);
      else
        stream->inbound_consumed_data_while_paused_ += avail;
    } while (len != 0);
  }
  return 0;
}

// Called by nghttp2 when it needs to determine how much padding to use in
// a DATA or HEADERS frame.
ssize_t Http2Session::OnSelectPadding(nghttp2_session* handle,
                                      const nghttp2_frame* frame,
                                      size_t maxPayloadLen,
                                      void* user_data) {
  Http2Session* session = static_cast<Http2Session*>(user_data);
  ssize_t padding = frame->hd.length;

  switch (session->padding_strategy_) {
    case PADDING_STRATEGY_NONE:
      // Fall-through
      break;
    case PADDING_STRATEGY_MAX:
      padding = session->OnMaxFrameSizePadding(padding, maxPayloadLen);
      break;
    case PADDING_STRATEGY_ALIGNED:
      padding = session->OnDWordAlignedPadding(padding, maxPayloadLen);
      break;
    case PADDING_STRATEGY_CALLBACK:
      padding = session->OnCallbackPadding(padding, maxPayloadLen);
      break;
  }
  return padding;
}

#define BAD_PEER_MESSAGE "Remote peer returned unexpected data while we "     \
                         "expected SETTINGS frame.  Perhaps, peer does not "  \
                         "support HTTP/2 properly."

// We use this currently to determine when an attempt is made to use the http2
// protocol with a non-http2 peer.
int Http2Session::OnNghttpError(nghttp2_session* handle,
                                const char* message,
                                size_t len,
                                void* user_data) {
  // Unfortunately, this is currently the only way for us to know if
  // the session errored because the peer is not an http2 peer.
  Http2Session* session = static_cast<Http2Session*>(user_data);
  Debug(session, "Error '%.*s'", len, message);
  if (strncmp(message, BAD_PEER_MESSAGE, len) == 0) {
    Environment* env = session->env();
    Isolate* isolate = env->isolate();
    HandleScope scope(isolate);
    Local<Context> context = env->context();
    Context::Scope context_scope(context);
    Local<Value> arg = Integer::New(isolate, NGHTTP2_ERR_PROTO);
    session->MakeCallback(env->error_string(), 1, &arg);
  }
  return 0;
}

uv_buf_t Http2StreamListener::OnStreamAlloc(size_t size) {
  // See the comments in Http2Session::OnDataChunkReceived
  // (which is the only possible call site for this method).
  return uv_buf_init(nullptr, size);
}

void Http2StreamListener::OnStreamRead(ssize_t nread, const uv_buf_t& buf) {
  Http2Stream* stream = static_cast<Http2Stream*>(stream_);
  Http2Session* session = stream->session();
  Environment* env = stream->env();
  HandleScope handle_scope(env->isolate());
  Context::Scope context_scope(env->context());

  if (nread < 0) {
    PassReadErrorToPreviousListener(nread);
    return;
  }

  CHECK(!session->stream_buf_ab_.IsEmpty());

  // There is a single large array buffer for the entire data read from the
  // network; create a slice of that array buffer and emit it as the
  // received data buffer.
  size_t offset = buf.base - session->stream_buf_.base;

  // Verify that the data offset is inside the current read buffer.
  CHECK_LE(offset, session->stream_buf_.len);
  CHECK_LE(offset + buf.len, session->stream_buf_.len);

  Local<Object> buffer =
      Buffer::New(env, session->stream_buf_ab_, offset, nread).ToLocalChecked();

  stream->CallJSOnreadMethod(nread, buffer);
}


// Called by OnFrameReceived to notify JavaScript land that a complete
// HEADERS frame has been received and processed. This method converts the
// received headers into a JavaScript array and pushes those out to JS.
void Http2Session::HandleHeadersFrame(const nghttp2_frame* frame) {
  Isolate* isolate = env()->isolate();
  HandleScope scope(isolate);
  Local<Context> context = env()->context();
  Context::Scope context_scope(context);

  int32_t id = GetFrameID(frame);
  Debug(this, "handle headers frame for stream %d", id);
  Http2Stream* stream = FindStream(id);

  // If the stream has already been destroyed, ignore.
  if (stream->IsDestroyed())
    return;

  std::vector<nghttp2_header> headers(stream->move_headers());

  Local<String> name_str;
  Local<String> value_str;

  Local<Array> holder = Array::New(isolate);
  Local<Function> fn = env()->push_values_to_array_function();
  Local<Value> argv[NODE_PUSH_VAL_TO_ARRAY_MAX * 2];

  // The headers are passed in above as a queue of nghttp2_header structs.
  // The following converts that into a JS array with the structure:
  // [name1, value1, name2, value2, name3, value3, name3, value4] and so on.
  // That array is passed up to the JS layer and converted into an Object form
  // like {name1: value1, name2: value2, name3: [value3, value4]}. We do it
  // this way for performance reasons (it's faster to generate and pass an
  // array than it is to generate and pass the object).
  size_t n = 0;
  while (n < headers.size()) {
    size_t j = 0;
    while (n < headers.size() && j < arraysize(argv) / 2) {
      nghttp2_header item = headers[n++];
      // The header name and value are passed as external one-byte strings
      name_str =
          ExternalHeader::New<true>(this, item.name).ToLocalChecked();
      value_str =
          ExternalHeader::New<false>(this, item.value).ToLocalChecked();
      argv[j * 2] = name_str;
      argv[j * 2 + 1] = value_str;
      j++;
    }
    // For performance, we pass name and value pairs to array.protototype.push
    // in batches of size NODE_PUSH_VAL_TO_ARRAY_MAX * 2 until there are no
    // more items to push.
    if (j > 0) {
      fn->Call(env()->context(), holder, j * 2, argv).ToLocalChecked();
    }
  }

  Local<Value> args[5] = {
    stream->object(),
    Integer::New(isolate, id),
    Integer::New(isolate, stream->headers_category()),
    Integer::New(isolate, frame->hd.flags),
    holder
  };
  MakeCallback(env()->onheaders_string(), arraysize(args), args);
}


// Called by OnFrameReceived when a complete PRIORITY frame has been
// received. Notifies JS land about the priority change. Note that priorities
// are considered advisory only, so this has no real effect other than to
// simply let user code know that the priority has changed.
void Http2Session::HandlePriorityFrame(const nghttp2_frame* frame) {
  Isolate* isolate = env()->isolate();
  HandleScope scope(isolate);
  Local<Context> context = env()->context();
  Context::Scope context_scope(context);

  nghttp2_priority priority_frame = frame->priority;
  int32_t id = GetFrameID(frame);
  Debug(this, "handle priority frame for stream %d", id);
  // Priority frame stream ID should never be <= 0. nghttp2 handles this for us
  nghttp2_priority_spec spec = priority_frame.pri_spec;

  Local<Value> argv[4] = {
    Integer::New(isolate, id),
    Integer::New(isolate, spec.stream_id),
    Integer::New(isolate, spec.weight),
    Boolean::New(isolate, spec.exclusive)
  };
  MakeCallback(env()->onpriority_string(), arraysize(argv), argv);
}


// Called by OnFrameReceived when a complete DATA frame has been received.
// If we know that this was the last DATA frame (because the END_STREAM flag
// is set), then we'll terminate the readable side of the StreamBase.
void Http2Session::HandleDataFrame(const nghttp2_frame* frame) {
  int32_t id = GetFrameID(frame);
  Debug(this, "handling data frame for stream %d", id);
  Http2Stream* stream = FindStream(id);

  if (!stream->IsDestroyed() && frame->hd.flags & NGHTTP2_FLAG_END_STREAM)
    stream->EmitRead(UV_EOF);
}


// Called by OnFrameReceived when a complete GOAWAY frame has been received.
void Http2Session::HandleGoawayFrame(const nghttp2_frame* frame) {
  Isolate* isolate = env()->isolate();
  HandleScope scope(isolate);
  Local<Context> context = env()->context();
  Context::Scope context_scope(context);

  nghttp2_goaway goaway_frame = frame->goaway;
  Debug(this, "handling goaway frame");

  Local<Value> argv[3] = {
    Integer::NewFromUnsigned(isolate, goaway_frame.error_code),
    Integer::New(isolate, goaway_frame.last_stream_id),
    Undefined(isolate)
  };

  size_t length = goaway_frame.opaque_data_len;
  if (length > 0) {
    argv[2] = Buffer::Copy(isolate,
                           reinterpret_cast<char*>(goaway_frame.opaque_data),
                           length).ToLocalChecked();
  }

  MakeCallback(env()->ongoawaydata_string(), arraysize(argv), argv);
}

// Called by OnFrameReceived when a complete ALTSVC frame has been received.
void Http2Session::HandleAltSvcFrame(const nghttp2_frame* frame) {
  Isolate* isolate = env()->isolate();
  HandleScope scope(isolate);
  Local<Context> context = env()->context();
  Context::Scope context_scope(context);

  int32_t id = GetFrameID(frame);

  nghttp2_extension ext = frame->ext;
  nghttp2_ext_altsvc* altsvc = static_cast<nghttp2_ext_altsvc*>(ext.payload);
  Debug(this, "handling altsvc frame");

  Local<Value> argv[3] = {
    Integer::New(isolate, id),
    String::NewFromOneByte(isolate,
                           altsvc->origin,
                           v8::NewStringType::kNormal,
                           altsvc->origin_len).ToLocalChecked(),
    String::NewFromOneByte(isolate,
                           altsvc->field_value,
                           v8::NewStringType::kNormal,
                           altsvc->field_value_len).ToLocalChecked(),
  };

  MakeCallback(env()->onaltsvc_string(), arraysize(argv), argv);
}

// Called by OnFrameReceived when a complete PING frame has been received.
void Http2Session::HandlePingFrame(const nghttp2_frame* frame) {
  bool ack = frame->hd.flags & NGHTTP2_FLAG_ACK;
  if (ack) {
    Http2Ping* ping = PopPing();
    if (ping != nullptr) {
      ping->Done(true, frame->ping.opaque_data);
    } else {
      // PING Ack is unsolicited. Treat as a connection error. The HTTP/2
      // spec does not require this, but there is no legitimate reason to
      // receive an unsolicited PING ack on a connection. Either the peer
      // is buggy or malicious, and we're not going to tolerate such
      // nonsense.
      Isolate* isolate = env()->isolate();
      HandleScope scope(isolate);
      Local<Context> context = env()->context();
      Context::Scope context_scope(context);
      Local<Value> arg = Integer::New(isolate, NGHTTP2_ERR_PROTO);
      MakeCallback(env()->error_string(), 1, &arg);
    }
  }
}

// Called by OnFrameReceived when a complete SETTINGS frame has been received.
void Http2Session::HandleSettingsFrame(const nghttp2_frame* frame) {
  bool ack = frame->hd.flags & NGHTTP2_FLAG_ACK;
  if (ack) {
    // If this is an acknowledgement, we should have an Http2Settings
    // object for it.
    Http2Settings* settings = PopSettings();
    if (settings != nullptr) {
      settings->Done(true);
    } else {
      // SETTINGS Ack is unsolicited. Treat as a connection error. The HTTP/2
      // spec does not require this, but there is no legitimate reason to
      // receive an unsolicited SETTINGS ack on a connection. Either the peer
      // is buggy or malicious, and we're not going to tolerate such
      // nonsense.
      // Note that nghttp2 currently prevents this from happening for SETTINGS
      // frames, so this block is purely defensive just in case that behavior
      // changes. Specifically, unlike unsolicited PING acks, unsolicited
      // SETTINGS acks should *never* make it this far.
      Isolate* isolate = env()->isolate();
      HandleScope scope(isolate);
      Local<Context> context = env()->context();
      Context::Scope context_scope(context);
      Local<Value> arg = Integer::New(isolate, NGHTTP2_ERR_PROTO);
      MakeCallback(env()->error_string(), 1, &arg);
    }
  } else {
    // Otherwise, notify the session about a new settings
    MakeCallback(env()->onsettings_string(), 0, nullptr);
  }
}

// Callback used when data has been written to the stream.
void Http2Session::OnStreamAfterWrite(WriteWrap* w, int status) {
  Debug(this, "write finished with status %d", status);

  // Inform all pending writes about their completion.
  ClearOutgoing(status);

  if (!(flags_ & SESSION_STATE_WRITE_SCHEDULED)) {
    // Schedule a new write if nghttp2 wants to send data.
    MaybeScheduleWrite();
  }
}

// If the underlying nghttp2_session struct has data pending in its outbound
// queue, MaybeScheduleWrite will schedule a SendPendingData() call to occur
// on the next iteration of the Node.js event loop (using the SetImmediate
// queue), but only if a write has not already been scheduled.
void Http2Session::MaybeScheduleWrite() {
  CHECK_EQ(flags_ & SESSION_STATE_WRITE_SCHEDULED, 0);
  if (session_ != nullptr && nghttp2_session_want_write(session_)) {
    HandleScope handle_scope(env()->isolate());
    Debug(this, "scheduling write");
    flags_ |= SESSION_STATE_WRITE_SCHEDULED;
    env()->SetImmediate([](Environment* env, void* data) {
      Http2Session* session = static_cast<Http2Session*>(data);
      if (session->session_ == nullptr ||
          !(session->flags_ & SESSION_STATE_WRITE_SCHEDULED)) {
        // This can happen e.g. when a stream was reset before this turn
        // of the event loop, in which case SendPendingData() is called early,
        // or the session was destroyed in the meantime.
        return;
      }

      // Sending data may call arbitrary JS code, so keep track of
      // async context.
      HandleScope handle_scope(env->isolate());
      InternalCallbackScope callback_scope(session);
      session->SendPendingData();
    }, static_cast<void*>(this), object());
  }
}

void Http2Session::MaybeStopReading() {
  int want_read = nghttp2_session_want_read(session_);
  Debug(this, "wants read? %d", want_read);
  if (want_read == 0)
    stream_->ReadStop();
}

// Unset the sending state, finish up all current writes, and reset
// storage for data and metadata that was associated with these writes.
void Http2Session::ClearOutgoing(int status) {
  CHECK_NE(flags_ & SESSION_STATE_SENDING, 0);

  flags_ &= ~SESSION_STATE_SENDING;

  if (outgoing_buffers_.size() > 0) {
    outgoing_storage_.clear();

    std::vector<nghttp2_stream_write> current_outgoing_buffers_;
    current_outgoing_buffers_.swap(outgoing_buffers_);
    for (const nghttp2_stream_write& wr : current_outgoing_buffers_) {
      WriteWrap* wrap = wr.req_wrap;
      if (wrap != nullptr)
        wrap->Done(status);
    }
  }

  // Now that we've finished sending queued data, if there are any pending
  // RstStreams we should try sending again and then flush them one by one.
  if (pending_rst_streams_.size() > 0) {
    std::vector<int32_t> current_pending_rst_streams;
    pending_rst_streams_.swap(current_pending_rst_streams);

    SendPendingData();

    for (int32_t stream_id : current_pending_rst_streams) {
      Http2Stream* stream = FindStream(stream_id);
      if (stream != nullptr)
        stream->FlushRstStream();
    }
  }
}

// Queue a given block of data for sending. This always creates a copy,
// so it is used for the cases in which nghttp2 requests sending of a
// small chunk of data.
void Http2Session::CopyDataIntoOutgoing(const uint8_t* src, size_t src_length) {
  size_t offset = outgoing_storage_.size();
  outgoing_storage_.resize(offset + src_length);
  memcpy(&outgoing_storage_[offset], src, src_length);

  // Store with a base of `nullptr` initially, since future resizes
  // of the outgoing_buffers_ vector may invalidate the pointer.
  // The correct base pointers will be set later, before writing to the
  // underlying socket.
  outgoing_buffers_.emplace_back(nghttp2_stream_write {
    uv_buf_init(nullptr, src_length)
  });
}

// Prompts nghttp2 to begin serializing it's pending data and pushes each
// chunk out to the i/o socket to be sent. This is a particularly hot method
// that will generally be called at least twice be event loop iteration.
// This is a potential performance optimization target later.
// Returns non-zero value if a write is already in progress.
uint8_t Http2Session::SendPendingData() {
  Debug(this, "sending pending data");
  // Do not attempt to send data on the socket if the destroying flag has
  // been set. That means everything is shutting down and the socket
  // will not be usable.
  if (IsDestroyed())
    return 0;
  flags_ &= ~SESSION_STATE_WRITE_SCHEDULED;

  // SendPendingData should not be called recursively.
  if (flags_ & SESSION_STATE_SENDING)
    return 1;
  // This is cleared by ClearOutgoing().
  flags_ |= SESSION_STATE_SENDING;

  ssize_t src_length;
  const uint8_t* src;

  CHECK_EQ(outgoing_buffers_.size(), 0);
  CHECK_EQ(outgoing_storage_.size(), 0);

  // Part One: Gather data from nghttp2

  while ((src_length = nghttp2_session_mem_send(session_, &src)) > 0) {
    Debug(this, "nghttp2 has %d bytes to send", src_length);
    CopyDataIntoOutgoing(src, src_length);
  }

  CHECK_NE(src_length, NGHTTP2_ERR_NOMEM);

  if (stream_ == nullptr) {
    // It would seem nice to bail out earlier, but `nghttp2_session_mem_send()`
    // does take care of things like closing the individual streams after
    // a socket has been torn down, so we still need to call it.
    ClearOutgoing(UV_ECANCELED);
    return 0;
  }

  // Part Two: Pass Data to the underlying stream

  size_t count = outgoing_buffers_.size();
  if (count == 0) {
    ClearOutgoing(0);
    return 0;
  }
  MaybeStackBuffer<uv_buf_t, 32> bufs;
  bufs.AllocateSufficientStorage(count);

  // Set the buffer base pointers for copied data that ended up in the
  // sessions's own storage since it might have shifted around during gathering.
  // (Those are marked by having .base == nullptr.)
  size_t offset = 0;
  size_t i = 0;
  for (const nghttp2_stream_write& write : outgoing_buffers_) {
    statistics_.data_sent += write.buf.len;
    if (write.buf.base == nullptr) {
      bufs[i++] = uv_buf_init(
          reinterpret_cast<char*>(outgoing_storage_.data() + offset),
          write.buf.len);
      offset += write.buf.len;
    } else {
      bufs[i++] = write.buf;
    }
  }

  chunks_sent_since_last_write_++;

  StreamWriteResult res = underlying_stream()->Write(*bufs, count);
  if (!res.async) {
    ClearOutgoing(res.err);
  }

  MaybeStopReading();

  return 0;
}


// This callback is called from nghttp2 when it wants to send DATA frames for a
// given Http2Stream, when we set the `NGHTTP2_DATA_FLAG_NO_COPY` flag earlier
// in the Http2Stream::Provider::Stream::OnRead callback.
// We take the write information directly out of the stream's data queue.
int Http2Session::OnSendData(
      nghttp2_session* session_,
      nghttp2_frame* frame,
      const uint8_t* framehd,
      size_t length,
      nghttp2_data_source* source,
      void* user_data) {
  Http2Session* session = static_cast<Http2Session*>(user_data);
  Http2Stream* stream = GetStream(session, frame->hd.stream_id, source);

  // Send the frame header + a byte that indicates padding length.
  session->CopyDataIntoOutgoing(framehd, 9);
  if (frame->data.padlen > 0) {
    uint8_t padding_byte = frame->data.padlen - 1;
    CHECK_EQ(padding_byte, frame->data.padlen - 1);
    session->CopyDataIntoOutgoing(&padding_byte, 1);
  }

  Debug(session, "nghttp2 has %d bytes to send directly", length);
  while (length > 0) {
    // nghttp2 thinks that there is data available (length > 0), which means
    // we told it so, which means that we *should* have data available.
    CHECK(!stream->queue_.empty());

    nghttp2_stream_write& write = stream->queue_.front();
    if (write.buf.len <= length) {
      // This write does not suffice by itself, so we can consume it completely.
      length -= write.buf.len;
      session->outgoing_buffers_.emplace_back(std::move(write));
      stream->queue_.pop();
      continue;
    }

    // Slice off `length` bytes of the first write in the queue.
    session->outgoing_buffers_.emplace_back(nghttp2_stream_write {
      uv_buf_init(write.buf.base, length)
    });
    write.buf.base += length;
    write.buf.len -= length;
    break;
  }

  if (frame->data.padlen > 0) {
    // Send padding if that was requested.
    session->outgoing_buffers_.emplace_back(nghttp2_stream_write {
      uv_buf_init(const_cast<char*>(zero_bytes_256), frame->data.padlen - 1)
    });
  }

  return 0;
}

// Creates a new Http2Stream and submits a new http2 request.
Http2Stream* Http2Session::SubmitRequest(
    nghttp2_priority_spec* prispec,
    nghttp2_nv* nva,
    size_t len,
    int32_t* ret,
    int options) {
  Debug(this, "submitting request");
  Http2Scope h2scope(this);
  Http2Stream* stream = nullptr;
  Http2Stream::Provider::Stream prov(options);
  *ret = nghttp2_submit_request(session_, prispec, nva, len, *prov, nullptr);
  CHECK_NE(*ret, NGHTTP2_ERR_NOMEM);
  if (*ret > 0)
    stream = new Http2Stream(this, *ret, NGHTTP2_HCAT_HEADERS, options);
  return stream;
}

uv_buf_t Http2Session::OnStreamAlloc(size_t suggested_size) {
  return env()->AllocateManaged(suggested_size).release();
}

// Callback used to receive inbound data from the i/o stream
void Http2Session::OnStreamRead(ssize_t nread, const uv_buf_t& buf_) {
  HandleScope handle_scope(env()->isolate());
  Context::Scope context_scope(env()->context());
  Http2Scope h2scope(this);
  CHECK_NOT_NULL(stream_);
  Debug(this, "receiving %d bytes", nread);
  CHECK(stream_buf_ab_.IsEmpty());
  AllocatedBuffer buf(env(), buf_);

  if (nread <= 0) {
    if (nread < 0) {
      PassReadErrorToPreviousListener(nread);
    }
  } else {
    // Only pass data on if nread > 0

    // Shrink to the actual amount of used data.
    buf.Resize(nread);

    IncrementCurrentSessionMemory(nread);

    // Makre sure that there was no read previously active.
    CHECK_NULL(stream_buf_.base);
    CHECK_EQ(stream_buf_.len, 0);

    // Remember the current buffer, so that OnDataChunkReceived knows the
    // offset of a DATA frame's data into the socket read buffer.
    stream_buf_ = uv_buf_init(buf.data(), nread);

    Isolate* isolate = env()->isolate();

    // Create an array buffer for the read data. DATA frames will be emitted
    // as slices of this array buffer to avoid having to copy memory.
    stream_buf_ab_ = buf.ToArrayBuffer();

    statistics_.data_received += nread;
    ssize_t ret = Write(&stream_buf_, 1);

    if (ret < 0) {
      Debug(this, "fatal error receiving data: %d", ret);

      Local<Value> argv[] = {
        Integer::New(isolate, ret),
      };
      MakeCallback(env()->error_string(), arraysize(argv), argv);
    } else {
      MaybeStopReading();
    }
  }

  // Since we are finished handling this write, reset the stream buffer.
  // The memory has either been free()d or was handed over to V8.
  DecrementCurrentSessionMemory(nread);

  stream_buf_ab_ = Local<ArrayBuffer>();
  stream_buf_ = uv_buf_init(nullptr, 0);
}

bool Http2Session::HasWritesOnSocketForStream(Http2Stream* stream) {
  for (const nghttp2_stream_write& wr : outgoing_buffers_) {
    if (wr.req_wrap != nullptr && wr.req_wrap->stream() == stream)
      return true;
  }
  return false;
}

// Every Http2Session session is tightly bound to a single i/o StreamBase
// (typically a net.Socket or tls.TLSSocket). The lifecycle of the two is
// tightly coupled with all data transfer between the two happening at the
// C++ layer via the StreamBase API.
void Http2Session::Consume(Local<External> external) {
  StreamBase* stream = static_cast<StreamBase*>(external->Value());
  stream->PushStreamListener(this);
  Debug(this, "i/o stream consumed");
}


Http2Stream::Http2Stream(
    Http2Session* session,
    int32_t id,
    nghttp2_headers_category category,
    int options) : AsyncWrap(session->env(),
                             session->env()->http2stream_constructor_template()
                                 ->NewInstance(session->env()->context())
                                     .ToLocalChecked(),
                             AsyncWrap::PROVIDER_HTTP2STREAM),
                   StreamBase(session->env()),
                   session_(session),
                   id_(id),
                   current_headers_category_(category) {
  MakeWeak();
  statistics_.start_time = uv_hrtime();

  // Limit the number of header pairs
  max_header_pairs_ = session->GetMaxHeaderPairs();
  if (max_header_pairs_ == 0)
  max_header_pairs_ = DEFAULT_MAX_HEADER_LIST_PAIRS;
  current_headers_.reserve(max_header_pairs_);

  // Limit the number of header octets
  max_header_length_ =
      std::min(
        nghttp2_session_get_local_settings(
          session->session(),
          NGHTTP2_SETTINGS_MAX_HEADER_LIST_SIZE),
      MAX_MAX_HEADER_LIST_SIZE);

  if (options & STREAM_OPTION_GET_TRAILERS)
    flags_ |= NGHTTP2_STREAM_FLAG_TRAILERS;

  PushStreamListener(&stream_listener_);

  if (options & STREAM_OPTION_EMPTY_PAYLOAD)
    Shutdown();
  session->AddStream(this);
}


Http2Stream::~Http2Stream() {
  for (nghttp2_header& header : current_headers_) {
    nghttp2_rcbuf_decref(header.name);
    nghttp2_rcbuf_decref(header.value);
  }

  if (session_ == nullptr)
    return;
  Debug(this, "tearing down stream");
  session_->RemoveStream(this);
  session_ = nullptr;
}

std::string Http2Stream::diagnostic_name() const {
  return "HttpStream " + std::to_string(id()) + " (" +
      std::to_string(static_cast<int64_t>(get_async_id())) + ") [" +
      session()->diagnostic_name() + "]";
}

// Notify the Http2Stream that a new block of HEADERS is being processed.
void Http2Stream::StartHeaders(nghttp2_headers_category category) {
  Debug(this, "starting headers, category: %d", id_, category);
  CHECK(!this->IsDestroyed());
  current_headers_length_ = 0;
  current_headers_.clear();
  current_headers_category_ = category;
}


nghttp2_stream* Http2Stream::operator*() {
  return nghttp2_session_find_stream(**session_, id_);
}

void Http2Stream::Close(int32_t code) {
  CHECK(!this->IsDestroyed());
  flags_ |= NGHTTP2_STREAM_FLAG_CLOSED;
  code_ = code;
  Debug(this, "closed with code %d", code);
}

int Http2Stream::DoShutdown(ShutdownWrap* req_wrap) {
  if (IsDestroyed())
    return UV_EPIPE;

  {
    Http2Scope h2scope(this);
    flags_ |= NGHTTP2_STREAM_FLAG_SHUT;
    CHECK_NE(nghttp2_session_resume_data(**session_, id_),
             NGHTTP2_ERR_NOMEM);
    Debug(this, "writable side shutdown");
  }
  req_wrap->Done(0);
  return 0;
}

// Destroy the Http2Stream and render it unusable. Actual resources for the
// Stream will not be freed until the next tick of the Node.js event loop
// using the SetImmediate queue.
void Http2Stream::Destroy() {
  // Do nothing if this stream instance is already destroyed
  if (IsDestroyed())
    return;
  if (session_->HasPendingRstStream(id_))
    FlushRstStream();
  flags_ |= NGHTTP2_STREAM_FLAG_DESTROYED;

  Debug(this, "destroying stream");

  // Wait until the start of the next loop to delete because there
  // may still be some pending operations queued for this stream.
  env()->SetImmediate([](Environment* env, void* data) {
    Http2Stream* stream = static_cast<Http2Stream*>(data);
    // Free any remaining outgoing data chunks here. This should be done
    // here because it's possible for destroy to have been called while
    // we still have queued outbound writes.
    while (!stream->queue_.empty()) {
      nghttp2_stream_write& head = stream->queue_.front();
      if (head.req_wrap != nullptr)
        head.req_wrap->Done(UV_ECANCELED);
      stream->queue_.pop();
    }

    // We can destroy the stream now if there are no writes for it
    // already on the socket. Otherwise, we'll wait for the garbage collector
    // to take care of cleaning up.
    if (stream->session() == nullptr ||
        !stream->session()->HasWritesOnSocketForStream(stream))
      delete stream;
  }, this, this->object());

  statistics_.end_time = uv_hrtime();
  session_->statistics_.stream_average_duration =
      ((statistics_.end_time - statistics_.start_time) /
          session_->statistics_.stream_count) / 1e6;
  EmitStatistics();
}


// Initiates a response on the Http2Stream using data provided via the
// StreamBase Streams API.
int Http2Stream::SubmitResponse(nghttp2_nv* nva, size_t len, int options) {
  CHECK(!this->IsDestroyed());
  Http2Scope h2scope(this);
  Debug(this, "submitting response");
  if (options & STREAM_OPTION_GET_TRAILERS)
    flags_ |= NGHTTP2_STREAM_FLAG_TRAILERS;

  if (!IsWritable())
    options |= STREAM_OPTION_EMPTY_PAYLOAD;

  Http2Stream::Provider::Stream prov(this, options);
  int ret = nghttp2_submit_response(**session_, id_, nva, len, *prov);
  CHECK_NE(ret, NGHTTP2_ERR_NOMEM);
  return ret;
}


// Submit informational headers for a stream.
int Http2Stream::SubmitInfo(nghttp2_nv* nva, size_t len) {
  CHECK(!this->IsDestroyed());
  Http2Scope h2scope(this);
  Debug(this, "sending %d informational headers", len);
  int ret = nghttp2_submit_headers(**session_,
                                   NGHTTP2_FLAG_NONE,
                                   id_, nullptr,
                                   nva, len, nullptr);
  CHECK_NE(ret, NGHTTP2_ERR_NOMEM);
  return ret;
}

void Http2Stream::OnTrailers() {
  Debug(this, "let javascript know we are ready for trailers");
  CHECK(!this->IsDestroyed());
  Isolate* isolate = env()->isolate();
  HandleScope scope(isolate);
  Local<Context> context = env()->context();
  Context::Scope context_scope(context);
  flags_ &= ~NGHTTP2_STREAM_FLAG_TRAILERS;
  MakeCallback(env()->ontrailers_string(), 0, nullptr);
}

// Submit informational headers for a stream.
int Http2Stream::SubmitTrailers(nghttp2_nv* nva, size_t len) {
  CHECK(!this->IsDestroyed());
  Http2Scope h2scope(this);
  Debug(this, "sending %d trailers", len);
  int ret;
  // Sending an empty trailers frame poses problems in Safari, Edge & IE.
  // Instead we can just send an empty data frame with NGHTTP2_FLAG_END_STREAM
  // to indicate that the stream is ready to be closed.
  if (len == 0) {
    Http2Stream::Provider::Stream prov(this, 0);
    ret = nghttp2_submit_data(**session_, NGHTTP2_FLAG_END_STREAM, id_, *prov);
  } else {
    ret = nghttp2_submit_trailer(**session_, id_, nva, len);
  }
  CHECK_NE(ret, NGHTTP2_ERR_NOMEM);
  return ret;
}

// Submit a PRIORITY frame to the connected peer.
int Http2Stream::SubmitPriority(nghttp2_priority_spec* prispec,
                                bool silent) {
  CHECK(!this->IsDestroyed());
  Http2Scope h2scope(this);
  Debug(this, "sending priority spec");
  int ret = silent ?
      nghttp2_session_change_stream_priority(**session_,
                                             id_, prispec) :
      nghttp2_submit_priority(**session_,
                              NGHTTP2_FLAG_NONE,
                              id_, prispec);
  CHECK_NE(ret, NGHTTP2_ERR_NOMEM);
  return ret;
}

// Closes the Http2Stream by submitting an RST_STREAM frame to the connected
// peer.
void Http2Stream::SubmitRstStream(const uint32_t code) {
  CHECK(!this->IsDestroyed());
  code_ = code;
  // If possible, force a purge of any currently pending data here to make sure
  // it is sent before closing the stream. If it returns non-zero then we need
  // to wait until the current write finishes and try again to avoid nghttp2
  // behaviour where it prioritizes RstStream over everything else.
  if (session_->SendPendingData() != 0) {
    session_->AddPendingRstStream(id_);
    return;
  }

  FlushRstStream();
}

void Http2Stream::FlushRstStream() {
  if (IsDestroyed())
    return;
  Http2Scope h2scope(this);
  CHECK_EQ(nghttp2_submit_rst_stream(**session_, NGHTTP2_FLAG_NONE,
                                     id_, code_), 0);
}


// Submit a push promise and create the associated Http2Stream if successful.
Http2Stream* Http2Stream::SubmitPushPromise(nghttp2_nv* nva,
                                            size_t len,
                                            int32_t* ret,
                                            int options) {
  CHECK(!this->IsDestroyed());
  Http2Scope h2scope(this);
  Debug(this, "sending push promise");
  *ret = nghttp2_submit_push_promise(**session_, NGHTTP2_FLAG_NONE,
                                     id_, nva, len, nullptr);
  CHECK_NE(*ret, NGHTTP2_ERR_NOMEM);
  Http2Stream* stream = nullptr;
  if (*ret > 0)
    stream = new Http2Stream(session_, *ret, NGHTTP2_HCAT_HEADERS, options);

  return stream;
}

// Switch the StreamBase into flowing mode to begin pushing chunks of data
// out to JS land.
int Http2Stream::ReadStart() {
  Http2Scope h2scope(this);
  CHECK(!this->IsDestroyed());
  flags_ |= NGHTTP2_STREAM_FLAG_READ_START;
  flags_ &= ~NGHTTP2_STREAM_FLAG_READ_PAUSED;

  Debug(this, "reading starting");

  // Tell nghttp2 about our consumption of the data that was handed
  // off to JS land.
  nghttp2_session_consume_stream(**session_,
                                 id_,
                                 inbound_consumed_data_while_paused_);
  inbound_consumed_data_while_paused_ = 0;

  return 0;
}

// Switch the StreamBase into paused mode.
int Http2Stream::ReadStop() {
  CHECK(!this->IsDestroyed());
  if (!IsReading())
    return 0;
  flags_ |= NGHTTP2_STREAM_FLAG_READ_PAUSED;
  Debug(this, "reading stopped");
  return 0;
}

// The Http2Stream class is a subclass of StreamBase. The DoWrite method
// receives outbound chunks of data to send as outbound DATA frames. These
// are queued in an internal linked list of uv_buf_t structs that are sent
// when nghttp2 is ready to serialize the data frame.
//
// Queue the given set of uv_but_t handles for writing to an
// nghttp2_stream. The WriteWrap's Done callback will be invoked once the
// chunks of data have been flushed to the underlying nghttp2_session.
// Note that this does *not* mean that the data has been flushed
// to the socket yet.
int Http2Stream::DoWrite(WriteWrap* req_wrap,
                         uv_buf_t* bufs,
                         size_t nbufs,
                         uv_stream_t* send_handle) {
  CHECK_NULL(send_handle);
  Http2Scope h2scope(this);
  if (!IsWritable() || IsDestroyed()) {
    req_wrap->Done(UV_EOF);
    return 0;
  }
  Debug(this, "queuing %d buffers to send", id_, nbufs);
  for (size_t i = 0; i < nbufs; ++i) {
    // Store the req_wrap on the last write info in the queue, so that it is
    // only marked as finished once all buffers associated with it are finished.
    queue_.emplace(nghttp2_stream_write {
      i == nbufs - 1 ? req_wrap : nullptr,
      bufs[i]
    });
    IncrementAvailableOutboundLength(bufs[i].len);
  }
  CHECK_NE(nghttp2_session_resume_data(**session_, id_), NGHTTP2_ERR_NOMEM);
  return 0;
}

// Ads a header to the Http2Stream. Note that the header name and value are
// provided using a buffer structure provided by nghttp2 that allows us to
// avoid unnecessary memcpy's. Those buffers are ref counted. The ref count
// is incremented here and are decremented when the header name and values
// are garbage collected later.
bool Http2Stream::AddHeader(nghttp2_rcbuf* name,
                            nghttp2_rcbuf* value,
                            uint8_t flags) {
  CHECK(!this->IsDestroyed());
  if (this->statistics_.first_header == 0)
    this->statistics_.first_header = uv_hrtime();
  size_t length = nghttp2_rcbuf_get_buf(name).len +
                  nghttp2_rcbuf_get_buf(value).len + 32;
  // A header can only be added if we have not exceeded the maximum number
  // of headers and the session has memory available for it.
  if (!session_->IsAvailableSessionMemory(length) ||
      current_headers_.size() == max_header_pairs_ ||
      current_headers_length_ + length > max_header_length_) {
    return false;
  }
  nghttp2_header header;
  header.name = name;
  header.value = value;
  header.flags = flags;
  current_headers_.push_back(header);
  nghttp2_rcbuf_incref(name);
  nghttp2_rcbuf_incref(value);
  current_headers_length_ += length;
  return true;
}

// A Provider is the thing that provides outbound DATA frame data.
Http2Stream::Provider::Provider(Http2Stream* stream, int options) {
  CHECK(!stream->IsDestroyed());
  provider_.source.ptr = stream;
  empty_ = options & STREAM_OPTION_EMPTY_PAYLOAD;
}

Http2Stream::Provider::Provider(int options) {
  provider_.source.ptr = nullptr;
  empty_ = options & STREAM_OPTION_EMPTY_PAYLOAD;
}

Http2Stream::Provider::~Provider() {
  provider_.source.ptr = nullptr;
}

// The Stream Provider pulls data from a linked list of uv_buf_t structs
// built via the StreamBase API and the Streams js API.
Http2Stream::Provider::Stream::Stream(int options)
    : Http2Stream::Provider(options) {
  provider_.read_callback = Http2Stream::Provider::Stream::OnRead;
}

Http2Stream::Provider::Stream::Stream(Http2Stream* stream, int options)
    : Http2Stream::Provider(stream, options) {
  provider_.read_callback = Http2Stream::Provider::Stream::OnRead;
}

ssize_t Http2Stream::Provider::Stream::OnRead(nghttp2_session* handle,
                                              int32_t id,
                                              uint8_t* buf,
                                              size_t length,
                                              uint32_t* flags,
                                              nghttp2_data_source* source,
                                              void* user_data) {
  Http2Session* session = static_cast<Http2Session*>(user_data);
  Debug(session, "reading outbound data for stream %d", id);
  Http2Stream* stream = GetStream(session, id, source);
  if (stream->statistics_.first_byte_sent == 0)
    stream->statistics_.first_byte_sent = uv_hrtime();
  CHECK_EQ(id, stream->id());

  size_t amount = 0;          // amount of data being sent in this data frame.

  // Remove all empty chunks from the head of the queue.
  // This is done here so that .write('', cb) is still a meaningful way to
  // find out when the HTTP2 stream wants to consume data, and because the
  // StreamBase API allows empty input chunks.
  while (!stream->queue_.empty() && stream->queue_.front().buf.len == 0) {
    WriteWrap* finished = stream->queue_.front().req_wrap;
    stream->queue_.pop();
    if (finished != nullptr)
      finished->Done(0);
  }

  if (!stream->queue_.empty()) {
    Debug(session, "stream %d has pending outbound data", id);
    amount = std::min(stream->available_outbound_length_, length);
    Debug(session, "sending %d bytes for data frame on stream %d", amount, id);
    if (amount > 0) {
      // Just return the length, let Http2Session::OnSendData take care of
      // actually taking the buffers out of the queue.
      *flags |= NGHTTP2_DATA_FLAG_NO_COPY;
      stream->DecrementAvailableOutboundLength(amount);
    }
  }

  if (amount == 0 && stream->IsWritable()) {
    CHECK(stream->queue_.empty());
    Debug(session, "deferring stream %d", id);
    stream->EmitWantsWrite(length);
    if (stream->available_outbound_length_ > 0 || !stream->IsWritable()) {
      // EmitWantsWrite() did something interesting synchronously, restart:
      return OnRead(handle, id, buf, length, flags, source, user_data);
    }
    return NGHTTP2_ERR_DEFERRED;
  }

  if (stream->queue_.empty() && !stream->IsWritable()) {
    Debug(session, "no more data for stream %d", id);
    *flags |= NGHTTP2_DATA_FLAG_EOF;
    if (stream->HasTrailers()) {
      *flags |= NGHTTP2_DATA_FLAG_NO_END_STREAM;
      stream->OnTrailers();
    }
  }

  stream->statistics_.sent_bytes += amount;
  return amount;
}

inline void Http2Stream::IncrementAvailableOutboundLength(size_t amount) {
  available_outbound_length_ += amount;
  session_->IncrementCurrentSessionMemory(amount);
}

inline void Http2Stream::DecrementAvailableOutboundLength(size_t amount) {
  available_outbound_length_ -= amount;
  session_->DecrementCurrentSessionMemory(amount);
}


// Implementation of the JavaScript API

// Fetches the string description of a nghttp2 error code and passes that
// back to JS land
void HttpErrorString(const FunctionCallbackInfo<Value>& args) {
  Environment* env = Environment::GetCurrent(args);
  uint32_t val = args[0]->Uint32Value(env->context()).ToChecked();
  args.GetReturnValue().Set(
      String::NewFromOneByte(
          env->isolate(),
          reinterpret_cast<const uint8_t*>(nghttp2_strerror(val)),
          v8::NewStringType::kInternalized).ToLocalChecked());
}


// Serializes the settings object into a Buffer instance that
// would be suitable, for instance, for creating the Base64
// output for an HTTP2-Settings header field.
void PackSettings(const FunctionCallbackInfo<Value>& args) {
  Environment* env = Environment::GetCurrent(args);
  Http2Session::Http2Settings settings(env);
  args.GetReturnValue().Set(settings.Pack());
}

// A TypedArray instance is shared between C++ and JS land to contain the
// default SETTINGS. RefreshDefaultSettings updates that TypedArray with the
// default values.
void RefreshDefaultSettings(const FunctionCallbackInfo<Value>& args) {
  Environment* env = Environment::GetCurrent(args);
  Http2Session::Http2Settings::RefreshDefaults(env);
}

// Sets the next stream ID the Http2Session. If successful, returns true.
void Http2Session::SetNextStreamID(const FunctionCallbackInfo<Value>& args) {
  Environment* env = Environment::GetCurrent(args);
  Http2Session* session;
  ASSIGN_OR_RETURN_UNWRAP(&session, args.Holder());
  int32_t id = args[0]->Int32Value(env->context()).ToChecked();
  if (nghttp2_session_set_next_stream_id(**session, id) < 0) {
    Debug(session, "failed to set next stream id to %d", id);
    return args.GetReturnValue().Set(false);
  }
  args.GetReturnValue().Set(true);
  Debug(session, "set next stream id to %d", id);
}

// A TypedArray instance is shared between C++ and JS land to contain the
// SETTINGS (either remote or local). RefreshSettings updates the current
// values established for each of the settings so those can be read in JS land.
template <get_setting fn>
void Http2Session::RefreshSettings(const FunctionCallbackInfo<Value>& args) {
  Environment* env = Environment::GetCurrent(args);
  Http2Session* session;
  ASSIGN_OR_RETURN_UNWRAP(&session, args.Holder());
  Http2Settings::Update(env, session, fn);
  Debug(session, "settings refreshed for session");
}

// A TypedArray instance is shared between C++ and JS land to contain state
// information of the current Http2Session. This updates the values in the
// TypedArray so those can be read in JS land.
void Http2Session::RefreshState(const FunctionCallbackInfo<Value>& args) {
  Environment* env = Environment::GetCurrent(args);
  Http2Session* session;
  ASSIGN_OR_RETURN_UNWRAP(&session, args.Holder());
  Debug(session, "refreshing state");

  AliasedBuffer<double, Float64Array>& buffer =
      env->http2_state()->session_state_buffer;

  nghttp2_session* s = **session;

  buffer[IDX_SESSION_STATE_EFFECTIVE_LOCAL_WINDOW_SIZE] =
      nghttp2_session_get_effective_local_window_size(s);
  buffer[IDX_SESSION_STATE_EFFECTIVE_RECV_DATA_LENGTH] =
      nghttp2_session_get_effective_recv_data_length(s);
  buffer[IDX_SESSION_STATE_NEXT_STREAM_ID] =
      nghttp2_session_get_next_stream_id(s);
  buffer[IDX_SESSION_STATE_LOCAL_WINDOW_SIZE] =
      nghttp2_session_get_local_window_size(s);
  buffer[IDX_SESSION_STATE_LAST_PROC_STREAM_ID] =
      nghttp2_session_get_last_proc_stream_id(s);
  buffer[IDX_SESSION_STATE_REMOTE_WINDOW_SIZE] =
      nghttp2_session_get_remote_window_size(s);
  buffer[IDX_SESSION_STATE_OUTBOUND_QUEUE_SIZE] =
      nghttp2_session_get_outbound_queue_size(s);
  buffer[IDX_SESSION_STATE_HD_DEFLATE_DYNAMIC_TABLE_SIZE] =
      nghttp2_session_get_hd_deflate_dynamic_table_size(s);
  buffer[IDX_SESSION_STATE_HD_INFLATE_DYNAMIC_TABLE_SIZE] =
      nghttp2_session_get_hd_inflate_dynamic_table_size(s);
}


// Constructor for new Http2Session instances.
void Http2Session::New(const FunctionCallbackInfo<Value>& args) {
  Environment* env = Environment::GetCurrent(args);
  CHECK(args.IsConstructCall());
  int val = args[0]->IntegerValue(env->context()).ToChecked();
  nghttp2_session_type type = static_cast<nghttp2_session_type>(val);
  Http2Session* session = new Http2Session(env, args.This(), type);
  session->get_async_id();  // avoid compiler warning
  Debug(session, "session created");
}


// Binds the Http2Session with a StreamBase used for i/o
void Http2Session::Consume(const FunctionCallbackInfo<Value>& args) {
  Http2Session* session;
  ASSIGN_OR_RETURN_UNWRAP(&session, args.Holder());
  CHECK(args[0]->IsExternal());
  session->Consume(args[0].As<External>());
}

// Destroys the Http2Session instance and renders it unusable
void Http2Session::Destroy(const FunctionCallbackInfo<Value>& args) {
  Http2Session* session;
  ASSIGN_OR_RETURN_UNWRAP(&session, args.Holder());
  Debug(session, "destroying session");
  Environment* env = Environment::GetCurrent(args);
  Local<Context> context = env->context();

  uint32_t code = args[0]->Uint32Value(context).ToChecked();
  bool socketDestroyed = args[1]->BooleanValue(context).ToChecked();

  session->Close(code, socketDestroyed);
}

// Submits a new request on the Http2Session and returns either an error code
// or the Http2Stream object.
void Http2Session::Request(const FunctionCallbackInfo<Value>& args) {
  Http2Session* session;
  ASSIGN_OR_RETURN_UNWRAP(&session, args.Holder());
  Environment* env = session->env();
  Local<Context> context = env->context();
  Isolate* isolate = env->isolate();

  Local<Array> headers = args[0].As<Array>();
  int options = args[1]->IntegerValue(context).ToChecked();
  Http2Priority priority(env, args[2], args[3], args[4]);

  Headers list(isolate, context, headers);

  Debug(session, "request submitted");

  int32_t ret = 0;
  Http2Stream* stream =
      session->Http2Session::SubmitRequest(*priority, *list, list.length(),
                                           &ret, options);

  if (ret <= 0) {
    Debug(session, "could not submit request: %s", nghttp2_strerror(ret));
    return args.GetReturnValue().Set(ret);
  }

  Debug(session, "request submitted, new stream id %d", stream->id());
  args.GetReturnValue().Set(stream->object());
}

// Submits a GOAWAY frame to signal that the Http2Session is in the process
// of shutting down. Note that this function does not actually alter the
// state of the Http2Session, it's simply a notification.
void Http2Session::Goaway(uint32_t code,
                          int32_t lastStreamID,
                          uint8_t* data,
                          size_t len) {
  if (IsDestroyed())
    return;

  Http2Scope h2scope(this);
  // the last proc stream id is the most recently created Http2Stream.
  if (lastStreamID <= 0)
    lastStreamID = nghttp2_session_get_last_proc_stream_id(session_);
  Debug(this, "submitting goaway");
  nghttp2_submit_goaway(session_, NGHTTP2_FLAG_NONE,
                        lastStreamID, code, data, len);
}

// Submits a GOAWAY frame to signal that the Http2Session is in the process
// of shutting down. The opaque data argument is an optional TypedArray that
// can be used to send debugging data to the connected peer.
void Http2Session::Goaway(const FunctionCallbackInfo<Value>& args) {
  Environment* env = Environment::GetCurrent(args);
  Local<Context> context = env->context();
  Http2Session* session;
  ASSIGN_OR_RETURN_UNWRAP(&session, args.Holder());

  uint32_t code = args[0]->Uint32Value(context).ToChecked();
  int32_t lastStreamID = args[1]->Int32Value(context).ToChecked();
  Local<Value> opaqueData = args[2];
  uint8_t* data = nullptr;
  size_t length = 0;

  if (Buffer::HasInstance(opaqueData)) {
    data = reinterpret_cast<uint8_t*>(Buffer::Data(opaqueData));
    length = Buffer::Length(opaqueData);
  }

  session->Goaway(code, lastStreamID, data, length);
}

// Update accounting of data chunks. This is used primarily to manage timeout
// logic when using the FD Provider.
void Http2Session::UpdateChunksSent(const FunctionCallbackInfo<Value>& args) {
  Environment* env = Environment::GetCurrent(args);
  Isolate* isolate = env->isolate();
  HandleScope scope(isolate);
  Http2Session* session;
  ASSIGN_OR_RETURN_UNWRAP(&session, args.Holder());

  uint32_t length = session->chunks_sent_since_last_write_;

  session->object()->Set(env->context(),
                         env->chunks_sent_since_last_write_string(),
                         Integer::NewFromUnsigned(isolate, length)).FromJust();

  args.GetReturnValue().Set(length);
}

// Submits an RST_STREAM frame effectively closing the Http2Stream. Note that
// this *WILL* alter the state of the stream, causing the OnStreamClose
// callback to the triggered.
void Http2Stream::RstStream(const FunctionCallbackInfo<Value>& args) {
  Environment* env = Environment::GetCurrent(args);
  Local<Context> context = env->context();
  Http2Stream* stream;
  ASSIGN_OR_RETURN_UNWRAP(&stream, args.Holder());
  uint32_t code = args[0]->Uint32Value(context).ToChecked();
  Debug(stream, "sending rst_stream with code %d", code);
  stream->SubmitRstStream(code);
}

// Initiates a response on the Http2Stream using the StreamBase API to provide
// outbound DATA frames.
void Http2Stream::Respond(const FunctionCallbackInfo<Value>& args) {
  Environment* env = Environment::GetCurrent(args);
  Local<Context> context = env->context();
  Isolate* isolate = env->isolate();
  Http2Stream* stream;
  ASSIGN_OR_RETURN_UNWRAP(&stream, args.Holder());

  Local<Array> headers = args[0].As<Array>();
  int options = args[1]->IntegerValue(context).ToChecked();

  Headers list(isolate, context, headers);

  args.GetReturnValue().Set(
      stream->SubmitResponse(*list, list.length(), options));
  Debug(stream, "response submitted");
}


// Submits informational headers on the Http2Stream
void Http2Stream::Info(const FunctionCallbackInfo<Value>& args) {
  Environment* env = Environment::GetCurrent(args);
  Local<Context> context = env->context();
  Isolate* isolate = env->isolate();
  Http2Stream* stream;
  ASSIGN_OR_RETURN_UNWRAP(&stream, args.Holder());

  Local<Array> headers = args[0].As<Array>();

  Headers list(isolate, context, headers);
  args.GetReturnValue().Set(stream->SubmitInfo(*list, list.length()));
  Debug(stream, "%d informational headers sent", list.length());
}

// Submits trailing headers on the Http2Stream
void Http2Stream::Trailers(const FunctionCallbackInfo<Value>& args) {
  Environment* env = Environment::GetCurrent(args);
  Local<Context> context = env->context();
  Isolate* isolate = env->isolate();
  Http2Stream* stream;
  ASSIGN_OR_RETURN_UNWRAP(&stream, args.Holder());

  Local<Array> headers = args[0].As<Array>();

  Headers list(isolate, context, headers);
  args.GetReturnValue().Set(stream->SubmitTrailers(*list, list.length()));
  Debug(stream, "%d trailing headers sent", list.length());
}

// Grab the numeric id of the Http2Stream
void Http2Stream::GetID(const FunctionCallbackInfo<Value>& args) {
  Http2Stream* stream;
  ASSIGN_OR_RETURN_UNWRAP(&stream, args.Holder());
  args.GetReturnValue().Set(stream->id());
}

// Destroy the Http2Stream, rendering it no longer usable
void Http2Stream::Destroy(const FunctionCallbackInfo<Value>& args) {
  Http2Stream* stream;
  ASSIGN_OR_RETURN_UNWRAP(&stream, args.Holder());
  Debug(stream, "destroying stream");
  stream->Destroy();
}

// Prompt the Http2Stream to begin sending data to the JS land.
void Http2Stream::FlushData(const FunctionCallbackInfo<Value>& args) {
  Http2Stream* stream;
  ASSIGN_OR_RETURN_UNWRAP(&stream, args.Holder());
  stream->ReadStart();
  Debug(stream, "data flushed to js");
}

// Initiate a Push Promise and create the associated Http2Stream
void Http2Stream::PushPromise(const FunctionCallbackInfo<Value>& args) {
  Environment* env = Environment::GetCurrent(args);
  Local<Context> context = env->context();
  Isolate* isolate = env->isolate();
  Http2Stream* parent;
  ASSIGN_OR_RETURN_UNWRAP(&parent, args.Holder());

  Local<Array> headers = args[0].As<Array>();
  int options = args[1]->IntegerValue(context).ToChecked();

  Headers list(isolate, context, headers);

  Debug(parent, "creating push promise");

  int32_t ret = 0;
  Http2Stream* stream = parent->SubmitPushPromise(*list, list.length(),
                                                  &ret, options);
  if (ret <= 0) {
    Debug(parent, "failed to create push stream: %d", ret);
    return args.GetReturnValue().Set(ret);
  }
  Debug(parent, "push stream %d created", stream->id());
  args.GetReturnValue().Set(stream->object());
}

// Send a PRIORITY frame
void Http2Stream::Priority(const FunctionCallbackInfo<Value>& args) {
  Environment* env = Environment::GetCurrent(args);
  Local<Context> context = env->context();
  Http2Stream* stream;
  ASSIGN_OR_RETURN_UNWRAP(&stream, args.Holder());

  Http2Priority priority(env, args[0], args[1], args[2]);
  bool silent = args[3]->BooleanValue(context).ToChecked();

  CHECK_EQ(stream->SubmitPriority(*priority, silent), 0);
  Debug(stream, "priority submitted");
}

// A TypedArray shared by C++ and JS land is used to communicate state
// information about the Http2Stream. This updates the values in that
// TypedArray so that the state can be read by JS.
void Http2Stream::RefreshState(const FunctionCallbackInfo<Value>& args) {
  Environment* env = Environment::GetCurrent(args);
  Http2Stream* stream;
  ASSIGN_OR_RETURN_UNWRAP(&stream, args.Holder());

  Debug(stream, "refreshing state");

  AliasedBuffer<double, Float64Array>& buffer =
      env->http2_state()->stream_state_buffer;

  nghttp2_stream* str = **stream;
  nghttp2_session* s = **(stream->session());

  if (str == nullptr) {
    buffer[IDX_STREAM_STATE] = NGHTTP2_STREAM_STATE_IDLE;
    buffer[IDX_STREAM_STATE_WEIGHT] =
        buffer[IDX_STREAM_STATE_SUM_DEPENDENCY_WEIGHT] =
        buffer[IDX_STREAM_STATE_LOCAL_CLOSE] =
        buffer[IDX_STREAM_STATE_REMOTE_CLOSE] =
        buffer[IDX_STREAM_STATE_LOCAL_WINDOW_SIZE] = 0;
  } else {
    buffer[IDX_STREAM_STATE] =
        nghttp2_stream_get_state(str);
    buffer[IDX_STREAM_STATE_WEIGHT] =
        nghttp2_stream_get_weight(str);
    buffer[IDX_STREAM_STATE_SUM_DEPENDENCY_WEIGHT] =
        nghttp2_stream_get_sum_dependency_weight(str);
    buffer[IDX_STREAM_STATE_LOCAL_CLOSE] =
        nghttp2_session_get_stream_local_close(s, stream->id());
    buffer[IDX_STREAM_STATE_REMOTE_CLOSE] =
        nghttp2_session_get_stream_remote_close(s, stream->id());
    buffer[IDX_STREAM_STATE_LOCAL_WINDOW_SIZE] =
        nghttp2_session_get_stream_local_window_size(s, stream->id());
  }
}

void Http2Session::AltSvc(int32_t id,
                          uint8_t* origin,
                          size_t origin_len,
                          uint8_t* value,
                          size_t value_len) {
  Http2Scope h2scope(this);
  CHECK_EQ(nghttp2_submit_altsvc(session_, NGHTTP2_FLAG_NONE, id,
                                 origin, origin_len, value, value_len), 0);
}

// Submits an AltSvc frame to be sent to the connected peer.
void Http2Session::AltSvc(const FunctionCallbackInfo<Value>& args) {
  Environment* env = Environment::GetCurrent(args);
  Http2Session* session;
  ASSIGN_OR_RETURN_UNWRAP(&session, args.Holder());

  int32_t id = args[0]->Int32Value(env->context()).ToChecked();

  // origin and value are both required to be ASCII, handle them as such.
  Local<String> origin_str = args[1]->ToString(env->context()).ToLocalChecked();
  Local<String> value_str = args[2]->ToString(env->context()).ToLocalChecked();

  size_t origin_len = origin_str->Length();
  size_t value_len = value_str->Length();

  CHECK_LE(origin_len + value_len, 16382);  // Max permitted for ALTSVC
  // Verify that origin len != 0 if stream id == 0, or
  // that origin len == 0 if stream id != 0
  CHECK((origin_len != 0 && id == 0) || (origin_len == 0 && id != 0));

  MaybeStackBuffer<uint8_t> origin(origin_len);
  MaybeStackBuffer<uint8_t> value(value_len);
  origin_str->WriteOneByte(env->isolate(), *origin);
  value_str->WriteOneByte(env->isolate(), *value);

  session->AltSvc(id, *origin, origin_len, *value, value_len);
}

// Submits a PING frame to be sent to the connected peer.
void Http2Session::Ping(const FunctionCallbackInfo<Value>& args) {
  Environment* env = Environment::GetCurrent(args);
  Http2Session* session;
  ASSIGN_OR_RETURN_UNWRAP(&session, args.Holder());

  // A PING frame may have exactly 8 bytes of payload data. If not provided,
  // then the current hrtime will be used as the payload.
  uint8_t* payload = nullptr;
  if (Buffer::HasInstance(args[0])) {
    payload = reinterpret_cast<uint8_t*>(Buffer::Data(args[0]));
    CHECK_EQ(Buffer::Length(args[0]), 8);
  }

  Http2Session::Http2Ping* ping = new Http2Ping(session);
  Local<Object> obj = ping->object();
  obj->Set(env->context(), env->ondone_string(), args[1]).FromJust();

  // To prevent abuse, we strictly limit the number of unacknowledged PING
  // frames that may be sent at any given time. This is configurable in the
  // Options when creating a Http2Session.
  if (!session->AddPing(ping)) {
    ping->Done(false);
    return args.GetReturnValue().Set(false);
  }

  // The Ping itself is an Async resource. When the acknowledgement is received,
  // the callback will be invoked and a notification sent out to JS land. The
  // notification will include the duration of the ping, allowing the round
  // trip to be measured.
  ping->Send(payload);
  args.GetReturnValue().Set(true);
}

// Submits a SETTINGS frame for the Http2Session
void Http2Session::Settings(const FunctionCallbackInfo<Value>& args) {
  Environment* env = Environment::GetCurrent(args);
  Http2Session* session;
  ASSIGN_OR_RETURN_UNWRAP(&session, args.Holder());

  Http2Session::Http2Settings* settings = new Http2Settings(session);
  Local<Object> obj = settings->object();
  obj->Set(env->context(), env->ondone_string(), args[0]).FromJust();

  if (!session->AddSettings(settings)) {
    settings->Done(false);
    return args.GetReturnValue().Set(false);
  }

  settings->Send();
  args.GetReturnValue().Set(true);
}


Http2Session::Http2Ping* Http2Session::PopPing() {
  Http2Ping* ping = nullptr;
  if (!outstanding_pings_.empty()) {
    ping = outstanding_pings_.front();
    outstanding_pings_.pop();
    DecrementCurrentSessionMemory(sizeof(*ping));
  }
  return ping;
}

bool Http2Session::AddPing(Http2Session::Http2Ping* ping) {
  if (outstanding_pings_.size() == max_outstanding_pings_)
    return false;
  outstanding_pings_.push(ping);
  IncrementCurrentSessionMemory(sizeof(*ping));
  return true;
}

Http2Session::Http2Settings* Http2Session::PopSettings() {
  Http2Settings* settings = nullptr;
  if (!outstanding_settings_.empty()) {
    settings = outstanding_settings_.front();
    outstanding_settings_.pop();
    DecrementCurrentSessionMemory(sizeof(*settings));
  }
  return settings;
}

bool Http2Session::AddSettings(Http2Session::Http2Settings* settings) {
  if (outstanding_settings_.size() == max_outstanding_settings_)
    return false;
  outstanding_settings_.push(settings);
  IncrementCurrentSessionMemory(sizeof(*settings));
  return true;
}

Http2Session::Http2Ping::Http2Ping(
    Http2Session* session)
        : AsyncWrap(session->env(),
                    session->env()->http2ping_constructor_template()
                        ->NewInstance(session->env()->context())
                            .ToLocalChecked(),
                    AsyncWrap::PROVIDER_HTTP2PING),
          session_(session),
          startTime_(uv_hrtime()) { }

void Http2Session::Http2Ping::Send(uint8_t* payload) {
  uint8_t data[8];
  if (payload == nullptr) {
    memcpy(&data, &startTime_, arraysize(data));
    payload = data;
  }
  Http2Scope h2scope(session_);
  CHECK_EQ(nghttp2_submit_ping(**session_, NGHTTP2_FLAG_NONE, payload), 0);
}

void Http2Session::Http2Ping::Done(bool ack, const uint8_t* payload) {
  session_->statistics_.ping_rtt = uv_hrtime() - startTime_;
  double duration = session_->statistics_.ping_rtt / 1e6;

  Local<Value> buf = Undefined(env()->isolate());
  if (payload != nullptr) {
    buf = Buffer::Copy(env()->isolate(),
                       reinterpret_cast<const char*>(payload),
                       8).ToLocalChecked();
  }

  Local<Value> argv[3] = {
    Boolean::New(env()->isolate(), ack),
    Number::New(env()->isolate(), duration),
    buf
  };
  MakeCallback(env()->ondone_string(), arraysize(argv), argv);
  delete this;
}


void nghttp2_stream_write::MemoryInfo(MemoryTracker* tracker) const {
  tracker->TrackThis(this);
  if (req_wrap != nullptr)
    tracker->TrackField("req_wrap", req_wrap->GetAsyncWrap());
  tracker->TrackField("buf", buf);
}


void nghttp2_header::MemoryInfo(MemoryTracker* tracker) const {
  tracker->TrackThis(this);
  tracker->TrackFieldWithSize("name", nghttp2_rcbuf_get_buf(name).len);
  tracker->TrackFieldWithSize("value", nghttp2_rcbuf_get_buf(value).len);
}


// Set up the process.binding('http2') binding.
void Initialize(Local<Object> target,
                Local<Value> unused,
                Local<Context> context,
                void* priv) {
  Environment* env = Environment::GetCurrent(context);
  Isolate* isolate = env->isolate();
  HandleScope scope(isolate);

  std::unique_ptr<Http2State> state(new Http2State(isolate));

#define SET_STATE_TYPEDARRAY(name, field)             \
  target->Set(context,                                \
              FIXED_ONE_BYTE_STRING(isolate, (name)), \
              (field)).FromJust()

  // Initialize the buffer used for padding callbacks
  SET_STATE_TYPEDARRAY(
    "paddingBuffer", state->padding_buffer.GetJSArray());
  // Initialize the buffer used to store the session state
  SET_STATE_TYPEDARRAY(
    "sessionState", state->session_state_buffer.GetJSArray());
  // Initialize the buffer used to store the stream state
  SET_STATE_TYPEDARRAY(
    "streamState", state->stream_state_buffer.GetJSArray());
  SET_STATE_TYPEDARRAY(
    "settingsBuffer", state->settings_buffer.GetJSArray());
  SET_STATE_TYPEDARRAY(
    "optionsBuffer", state->options_buffer.GetJSArray());
  SET_STATE_TYPEDARRAY(
    "streamStats", state->stream_stats_buffer.GetJSArray());
  SET_STATE_TYPEDARRAY(
    "sessionStats", state->session_stats_buffer.GetJSArray());
#undef SET_STATE_TYPEDARRAY

  env->set_http2_state(std::move(state));

  NODE_DEFINE_CONSTANT(target, PADDING_BUF_FRAME_LENGTH);
  NODE_DEFINE_CONSTANT(target, PADDING_BUF_MAX_PAYLOAD_LENGTH);
  NODE_DEFINE_CONSTANT(target, PADDING_BUF_RETURN_VALUE);

  // Method to fetch the nghttp2 string description of an nghttp2 error code
  env->SetMethod(target, "nghttp2ErrorString", HttpErrorString);

  Local<String> http2SessionClassName =
    FIXED_ONE_BYTE_STRING(isolate, "Http2Session");

  Local<FunctionTemplate> ping = FunctionTemplate::New(env->isolate());
  ping->SetClassName(FIXED_ONE_BYTE_STRING(env->isolate(), "Http2Ping"));
  AsyncWrap::AddWrapMethods(env, ping);
  Local<ObjectTemplate> pingt = ping->InstanceTemplate();
  pingt->SetInternalFieldCount(1);
  env->set_http2ping_constructor_template(pingt);

  Local<FunctionTemplate> setting = FunctionTemplate::New(env->isolate());
  setting->SetClassName(FIXED_ONE_BYTE_STRING(env->isolate(), "Http2Setting"));
  AsyncWrap::AddWrapMethods(env, setting);
  Local<ObjectTemplate> settingt = setting->InstanceTemplate();
  settingt->SetInternalFieldCount(1);
  env->set_http2settings_constructor_template(settingt);

  Local<FunctionTemplate> stream = FunctionTemplate::New(env->isolate());
  stream->SetClassName(FIXED_ONE_BYTE_STRING(env->isolate(), "Http2Stream"));
  env->SetProtoMethod(stream, "id", Http2Stream::GetID);
  env->SetProtoMethod(stream, "destroy", Http2Stream::Destroy);
  env->SetProtoMethod(stream, "flushData", Http2Stream::FlushData);
  env->SetProtoMethod(stream, "priority", Http2Stream::Priority);
  env->SetProtoMethod(stream, "pushPromise", Http2Stream::PushPromise);
  env->SetProtoMethod(stream, "info", Http2Stream::Info);
  env->SetProtoMethod(stream, "trailers", Http2Stream::Trailers);
  env->SetProtoMethod(stream, "respond", Http2Stream::Respond);
  env->SetProtoMethod(stream, "rstStream", Http2Stream::RstStream);
  env->SetProtoMethod(stream, "refreshState", Http2Stream::RefreshState);
  AsyncWrap::AddWrapMethods(env, stream);
  StreamBase::AddMethods<Http2Stream>(env, stream);
  Local<ObjectTemplate> streamt = stream->InstanceTemplate();
  streamt->SetInternalFieldCount(1);
  env->set_http2stream_constructor_template(streamt);
  target->Set(context,
              FIXED_ONE_BYTE_STRING(env->isolate(), "Http2Stream"),
              stream->GetFunction()).FromJust();

  Local<FunctionTemplate> session =
      env->NewFunctionTemplate(Http2Session::New);
  session->SetClassName(http2SessionClassName);
  session->InstanceTemplate()->SetInternalFieldCount(1);
  AsyncWrap::AddWrapMethods(env, session);
  env->SetProtoMethod(session, "altsvc", Http2Session::AltSvc);
  env->SetProtoMethod(session, "ping", Http2Session::Ping);
  env->SetProtoMethod(session, "consume", Http2Session::Consume);
  env->SetProtoMethod(session, "destroy", Http2Session::Destroy);
  env->SetProtoMethod(session, "goaway", Http2Session::Goaway);
  env->SetProtoMethod(session, "settings", Http2Session::Settings);
  env->SetProtoMethod(session, "request", Http2Session::Request);
  env->SetProtoMethod(session, "setNextStreamID",
                      Http2Session::SetNextStreamID);
  env->SetProtoMethod(session, "updateChunksSent",
                      Http2Session::UpdateChunksSent);
  env->SetProtoMethod(session, "refreshState", Http2Session::RefreshState);
  env->SetProtoMethod(
      session, "localSettings",
      Http2Session::RefreshSettings<nghttp2_session_get_local_settings>);
  env->SetProtoMethod(
      session, "remoteSettings",
      Http2Session::RefreshSettings<nghttp2_session_get_remote_settings>);
  target->Set(context,
              http2SessionClassName,
              session->GetFunction()).FromJust();

  Local<Object> constants = Object::New(isolate);
  Local<Array> name_for_error_code = Array::New(isolate);

#define NODE_NGHTTP2_ERROR_CODES(V)                       \
  V(NGHTTP2_SESSION_SERVER);                              \
  V(NGHTTP2_SESSION_CLIENT);                              \
  V(NGHTTP2_STREAM_STATE_IDLE);                           \
  V(NGHTTP2_STREAM_STATE_OPEN);                           \
  V(NGHTTP2_STREAM_STATE_RESERVED_LOCAL);                 \
  V(NGHTTP2_STREAM_STATE_RESERVED_REMOTE);                \
  V(NGHTTP2_STREAM_STATE_HALF_CLOSED_LOCAL);              \
  V(NGHTTP2_STREAM_STATE_HALF_CLOSED_REMOTE);             \
  V(NGHTTP2_STREAM_STATE_CLOSED);                         \
  V(NGHTTP2_NO_ERROR);                                    \
  V(NGHTTP2_PROTOCOL_ERROR);                              \
  V(NGHTTP2_INTERNAL_ERROR);                              \
  V(NGHTTP2_FLOW_CONTROL_ERROR);                          \
  V(NGHTTP2_SETTINGS_TIMEOUT);                            \
  V(NGHTTP2_STREAM_CLOSED);                               \
  V(NGHTTP2_FRAME_SIZE_ERROR);                            \
  V(NGHTTP2_REFUSED_STREAM);                              \
  V(NGHTTP2_CANCEL);                                      \
  V(NGHTTP2_COMPRESSION_ERROR);                           \
  V(NGHTTP2_CONNECT_ERROR);                               \
  V(NGHTTP2_ENHANCE_YOUR_CALM);                           \
  V(NGHTTP2_INADEQUATE_SECURITY);                         \
  V(NGHTTP2_HTTP_1_1_REQUIRED);                           \

#define V(name)                                                         \
  NODE_DEFINE_CONSTANT(constants, name);                                \
  name_for_error_code->Set(static_cast<int>(name),                      \
                           FIXED_ONE_BYTE_STRING(isolate, #name));
  NODE_NGHTTP2_ERROR_CODES(V)
#undef V

  NODE_DEFINE_HIDDEN_CONSTANT(constants, NGHTTP2_HCAT_REQUEST);
  NODE_DEFINE_HIDDEN_CONSTANT(constants, NGHTTP2_HCAT_RESPONSE);
  NODE_DEFINE_HIDDEN_CONSTANT(constants, NGHTTP2_HCAT_PUSH_RESPONSE);
  NODE_DEFINE_HIDDEN_CONSTANT(constants, NGHTTP2_HCAT_HEADERS);
  NODE_DEFINE_HIDDEN_CONSTANT(constants, NGHTTP2_NV_FLAG_NONE);
  NODE_DEFINE_HIDDEN_CONSTANT(constants, NGHTTP2_NV_FLAG_NO_INDEX);
  NODE_DEFINE_HIDDEN_CONSTANT(constants, NGHTTP2_ERR_DEFERRED);
  NODE_DEFINE_HIDDEN_CONSTANT(constants, NGHTTP2_ERR_STREAM_ID_NOT_AVAILABLE);
  NODE_DEFINE_HIDDEN_CONSTANT(constants, NGHTTP2_ERR_INVALID_ARGUMENT);
  NODE_DEFINE_HIDDEN_CONSTANT(constants, NGHTTP2_ERR_STREAM_CLOSED);
  NODE_DEFINE_CONSTANT(constants, NGHTTP2_ERR_FRAME_SIZE_ERROR);

  NODE_DEFINE_HIDDEN_CONSTANT(constants, STREAM_OPTION_EMPTY_PAYLOAD);
  NODE_DEFINE_HIDDEN_CONSTANT(constants, STREAM_OPTION_GET_TRAILERS);

  NODE_DEFINE_CONSTANT(constants, NGHTTP2_FLAG_NONE);
  NODE_DEFINE_CONSTANT(constants, NGHTTP2_FLAG_END_STREAM);
  NODE_DEFINE_CONSTANT(constants, NGHTTP2_FLAG_END_HEADERS);
  NODE_DEFINE_CONSTANT(constants, NGHTTP2_FLAG_ACK);
  NODE_DEFINE_CONSTANT(constants, NGHTTP2_FLAG_PADDED);
  NODE_DEFINE_CONSTANT(constants, NGHTTP2_FLAG_PRIORITY);

  NODE_DEFINE_CONSTANT(constants, DEFAULT_SETTINGS_HEADER_TABLE_SIZE);
  NODE_DEFINE_CONSTANT(constants, DEFAULT_SETTINGS_ENABLE_PUSH);
  NODE_DEFINE_CONSTANT(constants, DEFAULT_SETTINGS_INITIAL_WINDOW_SIZE);
  NODE_DEFINE_CONSTANT(constants, DEFAULT_SETTINGS_MAX_FRAME_SIZE);
  NODE_DEFINE_CONSTANT(constants, MAX_MAX_FRAME_SIZE);
  NODE_DEFINE_CONSTANT(constants, MIN_MAX_FRAME_SIZE);
  NODE_DEFINE_CONSTANT(constants, MAX_INITIAL_WINDOW_SIZE);
  NODE_DEFINE_CONSTANT(constants, NGHTTP2_DEFAULT_WEIGHT);

  NODE_DEFINE_CONSTANT(constants, NGHTTP2_SETTINGS_HEADER_TABLE_SIZE);
  NODE_DEFINE_CONSTANT(constants, NGHTTP2_SETTINGS_ENABLE_PUSH);
  NODE_DEFINE_CONSTANT(constants, NGHTTP2_SETTINGS_MAX_CONCURRENT_STREAMS);
  NODE_DEFINE_CONSTANT(constants, NGHTTP2_SETTINGS_INITIAL_WINDOW_SIZE);
  NODE_DEFINE_CONSTANT(constants, NGHTTP2_SETTINGS_MAX_FRAME_SIZE);
  NODE_DEFINE_CONSTANT(constants, NGHTTP2_SETTINGS_MAX_HEADER_LIST_SIZE);

  NODE_DEFINE_CONSTANT(constants, PADDING_STRATEGY_NONE);
  NODE_DEFINE_CONSTANT(constants, PADDING_STRATEGY_ALIGNED);
  NODE_DEFINE_CONSTANT(constants, PADDING_STRATEGY_MAX);
  NODE_DEFINE_CONSTANT(constants, PADDING_STRATEGY_CALLBACK);

#define STRING_CONSTANT(NAME, VALUE)                                          \
  NODE_DEFINE_STRING_CONSTANT(constants, "HTTP2_HEADER_" # NAME, VALUE);
HTTP_KNOWN_HEADERS(STRING_CONSTANT)
#undef STRING_CONSTANT

#define STRING_CONSTANT(NAME, VALUE)                                          \
  NODE_DEFINE_STRING_CONSTANT(constants, "HTTP2_METHOD_" # NAME, VALUE);
HTTP_KNOWN_METHODS(STRING_CONSTANT)
#undef STRING_CONSTANT

#define V(name, _) NODE_DEFINE_CONSTANT(constants, HTTP_STATUS_##name);
HTTP_STATUS_CODES(V)
#undef V

  env->SetMethod(target, "refreshDefaultSettings", RefreshDefaultSettings);
  env->SetMethod(target, "packSettings", PackSettings);

  target->Set(context,
              FIXED_ONE_BYTE_STRING(isolate, "constants"),
              constants).FromJust();
  target->Set(context,
              FIXED_ONE_BYTE_STRING(isolate, "nameForErrorCode"),
              name_for_error_code).FromJust();
}
}  // namespace http2
}  // namespace node

NODE_BUILTIN_MODULE_CONTEXT_AWARE(http2, node::http2::Initialize)
