// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "components/bua/renderer/bua_bindings.h"

#include <memory>
#include <string>
#include <tuple>

#include "base/functional/bind.h"
#include "base/memory/weak_ptr.h"
#include "base/strings/utf_string_conversions.h"
#include "components/bua/public/mojom/bua_host.mojom.h"
#include "content/public/renderer/render_frame.h"
#include "content/public/renderer/render_frame_observer.h"
#include "gin/arguments.h"
#include "gin/object_template_builder.h"
#include "gin/public/wrappable_pointer_tags.h"
#include "gin/wrappable.h"
#include "mojo/public/cpp/bindings/remote.h"
#include "third_party/blink/public/platform/browser_interface_broker_proxy.h"
#include "third_party/blink/public/platform/scheduler/web_agent_group_scheduler.h"
#include "third_party/blink/public/platform/task_type.h"
#include "third_party/blink/public/web/web_local_frame.h"
#include "v8/include/cppgc/allocation.h"
#include "v8/include/cppgc/prefinalizer.h"
#include "v8/include/v8-context.h"
#include "v8/include/v8-cppgc.h"
#include "v8/include/v8-exception.h"
#include "v8/include/v8-function.h"
#include "v8/include/v8-isolate.h"
#include "v8/include/v8-local-handle.h"
#include "v8/include/v8-object.h"
#include "v8/include/v8-persistent-handle.h"
#include "v8/include/v8-primitive.h"

namespace bua {
namespace {

constexpr char kNativeObjectName[] = "__buaNative";

constexpr char kBuaFacadeScript[] = R"JS(
(() => {
  const native = globalThis.__buaNative;
  if (!native || globalThis.bua) {
    return;
  }

  const makeError = (payload) => {
    const data = payload && payload.error ? payload.error : {};
    const error = new Error(data.message || 'BUA request failed');
    error.name = 'BuaError';
    if (data.code) {
      error.code = data.code;
    }
    if (data.category) {
      error.category = data.category;
    }
    if (data.details !== undefined) {
      error.details = data.details;
    }
    return error;
  };

  const request = (method, requestPayload) => new Promise((resolve, reject) => {
    native.request(method, JSON.stringify(requestPayload || {}), (responseJson) => {
      let response;
      try {
        response = JSON.parse(responseJson);
      } catch (error) {
        reject(error);
        return;
      }
      if (response && response.ok) {
        resolve(response.value);
        return;
      }
      reject(makeError(response));
    });
  });

  const subscription = () => Object.freeze({unsubscribe() {}});
  const unsupported = (method, payload) => request(method, payload || {});
  const freeze = (value) => Object.freeze(value);

  const buildSession = async (options) => {
    const created = await request('createSession', {options});
    const sessionId = created && created.id ? created.id :
        (options && options.id ? options.id : `bua-${Date.now()}-${Math.random().toString(36).slice(2)}`);
    const capabilities = created && created.capabilities ?
        created.capabilities : await request('capabilities', {});
    let latestSnapshot;

    const withSession = (payload) => Object.assign({sessionId}, payload || {});
    const task = freeze({
      start: (taskOptions) => request('task.start', withSession({options: taskOptions})),
      state: () => request('task.state', withSession()),
      pause: (reason) => request('task.pause', withSession({reason})),
      resume: (resumeOptions) => request('task.resume', withSession({options: resumeOptions})),
      interrupt: (reason) => request('task.interrupt', withSession({reason})),
      stop: (reason) => request('task.stop', withSession({reason})),
      cancelActions: (reason) => request('task.cancelActions', withSession({reason})),
    });
    const targets = freeze({
      current: () => request('targets.current', withSession()),
      list: (listOptions) => request('targets.list', withSession({options: listOptions})),
      createTab: (tabOptions) => request('targets.createTab', withSession({options: tabOptions})),
      activate: (target) => request('targets.activate', withSession({target})),
      close: (target) => request('targets.close', withSession({target})),
      createWindow: (windowOptions) => unsupported('targets.createWindow', withSession({options: windowOptions})),
      activateWindow: (windowId) => unsupported('targets.activateWindow', withSession({windowId})),
      closeWindow: (windowId) => unsupported('targets.closeWindow', withSession({windowId})),
    });
    const availability = freeze({
      current: () => request('availability.current', withSession()),
      onChange: () => subscription(),
    });
    const requests = freeze({
      next: (requestOptions) => request('requests.next', withSession({options: requestOptions})),
      respond: (id, response) => request('requests.respond', withSession({id, response})),
      onRequest: () => subscription(),
    });
    const events = freeze({
      subscribe: () => subscription(),
    });
    const diagnostics = freeze({
      current: () => request('diagnostics.current', withSession()),
    });

    return freeze({
      id: sessionId,
      capabilities,
      availability,
      task,
      targets,
      requests,
      events,
      diagnostics,
      snapshot: (snapshotOptions) => request('snapshot', withSession({options: snapshotOptions})).then((snapshot) => {
        latestSnapshot = snapshot;
        return snapshot;
      }),
      latestSnapshot: () => latestSnapshot,
      act: (actions, actOptions) => request('act', withSession({
        actions: Array.isArray(actions) ? actions : [actions],
        options: actOptions,
      })).then((result) => {
        if (result && result.snapshot) {
          latestSnapshot = result.snapshot;
        }
        return result;
      }),
      close: (reason) => request('session.close', withSession({reason})),
    });
  };

  const client = freeze({
    capabilities: () => request('capabilities', {}),
    createSession: (options) => buildSession(options),
  });

  Object.defineProperty(globalThis, 'bua', {
    value: client,
    writable: false,
    enumerable: false,
    configurable: false,
  });
})()
)JS";

class BuaNative final : public gin::Wrappable<BuaNative>,
                        public content::RenderFrameObserver {
  CPPGC_USING_PRE_FINALIZER(BuaNative, Dispose);

 public:
  static constexpr gin::WrapperInfo kWrapperInfo = {{gin::kEmbedderNativeGin},
                                                    gin::kBuaNative};

  explicit BuaNative(content::RenderFrame* render_frame)
      : RenderFrameObserver(render_frame) {}

  BuaNative(const BuaNative&) = delete;
  BuaNative& operator=(const BuaNative&) = delete;

  ~BuaNative() override = default;

  void Dispose() {
    Shutdown();
    RenderFrameObserver::Dispose();
  }

 private:
  gin::ObjectTemplateBuilder GetObjectTemplateBuilder(
      v8::Isolate* isolate) override {
    return gin::Wrappable<BuaNative>::GetObjectTemplateBuilder(isolate)
        .SetMethod("request", &BuaNative::Request);
  }

  const gin::WrapperInfo* wrapper_info() const override {
    return &kWrapperInfo;
  }

  void OnDestruct() override { Shutdown(); }

  void Shutdown() {
    host_.reset();
    weak_ptr_factory_.InvalidateWeakPtrs();
  }

  void EnsureHost() {
    if (host_.is_bound() || !render_frame()) {
      return;
    }
    render_frame()->GetBrowserInterfaceBroker().GetInterface(
        host_.BindNewPipeAndPassReceiver(
            render_frame()->GetTaskRunner(blink::TaskType::kInternalDefault)));
  }

  void Request(gin::Arguments* args) {
    std::string method;
    std::string request_json;
    v8::Local<v8::Function> callback;
    if (!args->GetNext(&method) || !args->GetNext(&request_json) ||
        !args->GetNext(&callback)) {
      args->ThrowError();
      return;
    }

    auto request_context = std::make_unique<v8::Global<v8::Context>>(
        args->isolate(), args->isolate()->GetCurrentContext());
    auto callback_handle =
        std::make_unique<v8::Global<v8::Function>>(args->isolate(), callback);

    EnsureHost();
    if (!host_.is_bound()) {
      RunRequestCallback(
          args->isolate(), std::move(request_context),
          std::move(callback_handle),
          R"({"ok":false,"error":{"code":"bua_unavailable","message":"BUA host is unavailable","category":"transport"}})");
      return;
    }

    host_->Request(
        std::move(method), std::move(request_json),
        base::BindOnce(&BuaNative::RunRequestCallback,
                       weak_ptr_factory_.GetWeakPtr(), args->isolate(),
                       std::move(request_context), std::move(callback_handle)));
  }

  void RunRequestCallback(
      v8::Isolate* isolate,
      std::unique_ptr<v8::Global<v8::Context>> request_context,
      std::unique_ptr<v8::Global<v8::Function>> callback,
      const std::string& response_json) {
    if (!render_frame()) {
      return;
    }
    blink::WebLocalFrame* web_frame = render_frame()->GetWebFrame();
    if (!web_frame) {
      return;
    }

    v8::HandleScope handle_scope(isolate);
    v8::Local<v8::Context> context =
        v8::Local<v8::Context>::New(isolate, *request_context);
    if (context.IsEmpty()) {
      return;
    }

    v8::Local<v8::Context> current_context =
        web_frame->MainWorldScriptContext();
    if (current_context.IsEmpty() || current_context != context) {
      return;
    }

    v8::Context::Scope context_scope(context);
    v8::TryCatch try_catch(isolate);
    try_catch.SetVerbose(true);

    v8::Local<v8::Value> argv[] = {
        v8::String::NewFromUtf8(isolate, response_json.c_str(),
                                v8::NewStringType::kNormal,
                                static_cast<int>(response_json.size()))
            .ToLocalChecked()};
    std::ignore = v8::Local<v8::Function>::New(isolate, *callback)
                      ->Call(context, context->Global(), 1, argv);
  }

  mojo::Remote<mojom::BuaHost> host_;
  base::WeakPtrFactory<BuaNative> weak_ptr_factory_{this};
};

bool ShouldInstall(content::RenderFrame* render_frame) {
  if (!render_frame || !render_frame->IsMainFrame()) {
    return false;
  }

  return render_frame->GetWebFrame() != nullptr;
}

}  // namespace

// static
void BuaBindings::Install(content::RenderFrame* render_frame) {
  if (!ShouldInstall(render_frame)) {
    return;
  }

  blink::WebLocalFrame* web_frame = render_frame->GetWebFrame();
  v8::Isolate* isolate = web_frame->GetAgentGroupScheduler()->Isolate();
  v8::HandleScope handle_scope(isolate);
  v8::Local<v8::Context> context = web_frame->MainWorldScriptContext();
  if (context.IsEmpty()) {
    return;
  }

  v8::Context::Scope context_scope(context);

  auto* native = cppgc::MakeGarbageCollected<BuaNative>(
      isolate->GetCppHeap()->GetAllocationHandle(), render_frame);
  v8::Local<v8::Object> wrapper = native->GetWrapper(isolate).ToLocalChecked();
  context->Global()
      ->DefineOwnProperty(
          context, v8::String::NewFromUtf8Literal(isolate, kNativeObjectName),
          wrapper,
          static_cast<v8::PropertyAttribute>(v8::DontEnum | v8::ReadOnly |
                                             v8::DontDelete))
      .Check();

  render_frame->ExecuteJavaScript(
      base::UTF8ToUTF16(std::string_view(kBuaFacadeScript)));
}

}  // namespace bua
