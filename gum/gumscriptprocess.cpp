/*
 * Copyright (C) 2010-2014 Ole André Vadla Ravnås <ole.andre.ravnas@tillitech.com>
 *
 * Licence: wxWindows Library Licence, Version 3.1
 */

#include "gumscriptprocess.h"

#include <string.h>

#if defined (HAVE_I386)
# if GLIB_SIZEOF_VOID_P == 4
#  define GUM_SCRIPT_ARCH "ia32"
# else
#  define GUM_SCRIPT_ARCH "x64"
# endif
#elif defined (HAVE_ARM)
# define GUM_SCRIPT_ARCH "arm"
#elif defined (HAVE_ARM64)
# define GUM_SCRIPT_ARCH "arm64"
#endif

#if defined (HAVE_LINUX)
# define GUM_SCRIPT_PLATFORM "linux"
#elif defined (HAVE_DARWIN)
# define GUM_SCRIPT_PLATFORM "darwin"
#elif defined (G_OS_WIN32)
# define GUM_SCRIPT_PLATFORM "windows"
#endif

using namespace v8;

typedef struct _GumScriptMatchContext GumScriptMatchContext;

struct _GumScriptMatchContext
{
  GumScriptProcess * self;
  Isolate * isolate;
  Local<Function> on_match;
  Local<Function> on_complete;
  Local<Object> receiver;
};

static void gum_script_process_on_get_current_thread_id (
    const FunctionCallbackInfo<Value> & info);
static void gum_script_process_on_enumerate_threads (
    const FunctionCallbackInfo<Value> & info);
static gboolean gum_script_process_thread_match (
    const GumThreadDetails * details, gpointer user_data);
static const gchar * gum_script_thread_state_to_string (GumThreadState state);
static void gum_script_process_on_enumerate_modules (
    const FunctionCallbackInfo<Value> & info);
static gboolean gum_script_process_handle_module_match (
    const GumModuleDetails * details, gpointer user_data);
static void gum_script_process_on_enumerate_ranges (
    const FunctionCallbackInfo<Value> & info);
static gboolean gum_script_process_handle_range_match (
    const GumRangeDetails * details, gpointer user_data);

void
_gum_script_process_init (GumScriptProcess * self,
                          GumScriptCore * core,
                          Handle<ObjectTemplate> scope)
{
  Isolate * isolate = core->isolate;

  self->core = core;

  Local<External> data (External::New (isolate, self));

  Handle<ObjectTemplate> process = ObjectTemplate::New (isolate);
  process->Set (String::NewFromUtf8 (isolate, "arch"),
      String::NewFromUtf8 (isolate, GUM_SCRIPT_ARCH), ReadOnly);
  process->Set (String::NewFromUtf8 (isolate, "platform"),
      String::NewFromUtf8 (isolate, GUM_SCRIPT_PLATFORM), ReadOnly);
  process->Set (String::NewFromUtf8 (isolate, "pointerSize"),
      Number::New (isolate, GLIB_SIZEOF_VOID_P), ReadOnly);
  process->Set (String::NewFromUtf8 (isolate, "getCurrentThreadId"),
      FunctionTemplate::New (isolate,
      gum_script_process_on_get_current_thread_id));
  process->Set (String::NewFromUtf8 (isolate, "enumerateThreads"),
      FunctionTemplate::New (isolate, gum_script_process_on_enumerate_threads,
      data));
  process->Set (String::NewFromUtf8 (isolate, "enumerateModules"),
      FunctionTemplate::New (isolate, gum_script_process_on_enumerate_modules,
      data));
  process->Set (String::NewFromUtf8 (isolate, "enumerateRanges"),
      FunctionTemplate::New (isolate, gum_script_process_on_enumerate_ranges,
      data));
  scope->Set (String::NewFromUtf8 (isolate, "Process"), process);
}

void
_gum_script_process_realize (GumScriptProcess * self)
{
  (void) self;
}

void
_gum_script_process_dispose (GumScriptProcess * self)
{
  (void) self;
}

void
_gum_script_process_finalize (GumScriptProcess * self)
{
  (void) self;
}

static void
gum_script_process_on_get_current_thread_id (
    const FunctionCallbackInfo<Value> & info)
{
  info.GetReturnValue ().Set (static_cast<uint32_t> (
      gum_process_get_current_thread_id ()));
}

static void
gum_script_process_on_enumerate_threads (
    const FunctionCallbackInfo<Value> & info)
{
  GumScriptMatchContext ctx;

  ctx.self = static_cast<GumScriptProcess *> (
      info.Data ().As<External> ()->Value ());
  ctx.isolate = info.GetIsolate ();

  Local<Value> callbacks_value = info[0];
  if (!callbacks_value->IsObject ())
  {
    ctx.isolate->ThrowException (Exception::TypeError (String::NewFromUtf8 (
        ctx.isolate, "Process.enumerateThreads: argument must be "
        "a callback object")));
    return;
  }

  Local<Object> callbacks = Local<Object>::Cast (callbacks_value);
  if (!_gum_script_callbacks_get (callbacks, "onMatch", &ctx.on_match,
      ctx.self->core))
  {
    return;
  }
  if (!_gum_script_callbacks_get (callbacks, "onComplete", &ctx.on_complete,
      ctx.self->core))
  {
    return;
  }

  ctx.receiver = info.This ();

  gum_process_enumerate_threads (gum_script_process_thread_match, &ctx);

  ctx.on_complete->Call (ctx.receiver, 0, 0);
}

static gboolean
gum_script_process_thread_match (const GumThreadDetails * details,
                                 gpointer user_data)
{
  GumScriptMatchContext * ctx =
      static_cast<GumScriptMatchContext *> (user_data);
  Isolate * isolate = ctx->isolate;

  if (gum_script_is_ignoring (details->id))
    return TRUE;

  Local<Object> thread (Object::New (isolate));
  thread->Set (String::NewFromUtf8 (isolate, "id"),
      Number::New (isolate, details->id), ReadOnly);
  thread->Set (String::NewFromUtf8 (isolate, "state"),
      String::NewFromUtf8 (isolate,
      gum_script_thread_state_to_string (details->state)), ReadOnly);
  thread->Set (String::NewFromUtf8 (isolate, "registers"),
      _gum_script_cpu_context_to_object (&details->cpu_context,
      ctx->self->core), ReadOnly);
  Handle<Value> argv[] = { thread };
  Local<Value> result = ctx->on_match->Call (ctx->receiver, 1, argv);

  gboolean proceed = TRUE;
  if (!result.IsEmpty () && result->IsString ())
  {
    String::Utf8Value str (result);
    proceed = (strcmp (*str, "stop") != 0);
  }

  return proceed;
}

static const gchar *
gum_script_thread_state_to_string (GumThreadState state)
{
  switch (state)
  {
    case GUM_THREAD_RUNNING: return "running";
    case GUM_THREAD_STOPPED: return "stopped";
    case GUM_THREAD_WAITING: return "waiting";
    case GUM_THREAD_UNINTERRUPTIBLE: return "uninterruptible";
    case GUM_THREAD_HALTED: return "halted";
    default:
      break;
  }

  g_assert_not_reached ();
}

static void
gum_script_process_on_enumerate_modules (
    const FunctionCallbackInfo<Value> & info)
{
  GumScriptMatchContext ctx;

  ctx.self = static_cast<GumScriptProcess *> (
      info.Data ().As<External> ()->Value ());
  ctx.isolate = info.GetIsolate ();

  Local<Value> callbacks_value = info[0];
  if (!callbacks_value->IsObject ())
  {
    ctx.isolate->ThrowException (Exception::TypeError (String::NewFromUtf8 (
        ctx.isolate, "Process.enumerateModules: argument must be "
        "a callback object")));
    return;
  }

  Local<Object> callbacks = Local<Object>::Cast (callbacks_value);
  if (!_gum_script_callbacks_get (callbacks, "onMatch", &ctx.on_match,
      ctx.self->core))
  {
    return;
  }
  if (!_gum_script_callbacks_get (callbacks, "onComplete", &ctx.on_complete,
      ctx.self->core))
  {
    return;
  }

  ctx.receiver = info.This ();

  gum_process_enumerate_modules (gum_script_process_handle_module_match, &ctx);

  ctx.on_complete->Call (ctx.receiver, 0, 0);

  return;
}

static gboolean
gum_script_process_handle_module_match (const GumModuleDetails * details,
                                        gpointer user_data)
{
  GumScriptMatchContext * ctx =
      static_cast<GumScriptMatchContext *> (user_data);
  Isolate * isolate = ctx->isolate;

  Local<Object> module (Object::New (isolate));
  module->Set (String::NewFromUtf8 (isolate, "name"),
      String::NewFromUtf8 (isolate, details->name), ReadOnly);
  module->Set (String::NewFromUtf8 (isolate, "base"),
      _gum_script_pointer_new (GSIZE_TO_POINTER (details->range->base_address),
      ctx->self->core), ReadOnly);
  module->Set (String::NewFromUtf8 (isolate, "size"),
      Integer::NewFromUnsigned (isolate, details->range->size), ReadOnly);
  module->Set (String::NewFromUtf8 (isolate, "path"),
      String::NewFromUtf8 (isolate, details->path), ReadOnly);

  Handle<Value> argv[] = {
    module
  };
  Local<Value> result = ctx->on_match->Call (ctx->receiver, 1, argv);

  gboolean proceed = TRUE;
  if (!result.IsEmpty () && result->IsString ())
  {
    String::Utf8Value str (result);
    proceed = (strcmp (*str, "stop") != 0);
  }

  return proceed;
}

static void
gum_script_process_on_enumerate_ranges (
    const FunctionCallbackInfo<Value> & info)
{
  GumScriptMatchContext ctx;

  ctx.self = static_cast<GumScriptProcess *> (
      info.Data ().As<External> ()->Value ());
  ctx.isolate = info.GetIsolate ();

  GumPageProtection prot;
  if (!_gum_script_page_protection_get (info[0], &prot, ctx.self->core))
    return;

  Local<Value> callbacks_value = info[1];
  if (!callbacks_value->IsObject ())
  {
    ctx.isolate->ThrowException (Exception::TypeError (String::NewFromUtf8 (
        ctx.isolate, "Process.enumerateRanges: second argument must be "
        "a callback object")));
    return;
  }

  Local<Object> callbacks = Local<Object>::Cast (callbacks_value);
  if (!_gum_script_callbacks_get (callbacks, "onMatch", &ctx.on_match,
      ctx.self->core))
  {
    return;
  }
  if (!_gum_script_callbacks_get (callbacks, "onComplete", &ctx.on_complete,
      ctx.self->core))
  {
    return;
  }

  ctx.receiver = info.This ();

  gum_process_enumerate_ranges (prot, gum_script_process_handle_range_match,
      &ctx);

  ctx.on_complete->Call (ctx.receiver, 0, 0);

  return;
}

static gboolean
gum_script_process_handle_range_match (const GumRangeDetails * details,
                                       gpointer user_data)
{
  GumScriptMatchContext * ctx =
      static_cast<GumScriptMatchContext *> (user_data);
  Isolate * isolate = ctx->isolate;

  char prot_str[4] = "---";
  if ((details->prot & GUM_PAGE_READ) != 0)
    prot_str[0] = 'r';
  if ((details->prot & GUM_PAGE_WRITE) != 0)
    prot_str[1] = 'w';
  if ((details->prot & GUM_PAGE_EXECUTE) != 0)
    prot_str[2] = 'x';

  Local<Object> range (Object::New (isolate));
  range->Set (String::NewFromUtf8 (isolate, "base"),
      _gum_script_pointer_new (GSIZE_TO_POINTER (details->range->base_address),
      ctx->self->core), ReadOnly);
  range->Set (String::NewFromUtf8 (isolate, "size"),
      Integer::NewFromUnsigned (isolate, details->range->size), ReadOnly);
  range->Set (String::NewFromUtf8 (isolate, "protection"),
      String::NewFromUtf8 (isolate, prot_str), ReadOnly);

  Handle<Value> argv[] = {
    range
  };
  Local<Value> result = ctx->on_match->Call (ctx->receiver, 1, argv);

  gboolean proceed = TRUE;
  if (!result.IsEmpty () && result->IsString ())
  {
    String::Utf8Value str (result);
    proceed = (strcmp (*str, "stop") != 0);
  }

  return proceed;
}
