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

  const makeLocalError = (code, category, message, details) => {
    const error = new Error(message);
    error.name = 'BuaError';
    error.code = code;
    error.category = category;
    if (details !== undefined) {
      error.details = details;
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

  const freeze = (value) => Object.freeze(value);
  const API2_EVENT_TYPES = freeze([
    'task_state_changed',
    'tab_changed',
    'page_changed',
    'user_takeover',
  ]);
  const API2_ACTIONS = freeze([
    'history',
    'click',
    'type',
    'scroll',
    'scroll_to',
    'move_mouse',
    'drag',
    'select',
    'wait',
  ]);

  const mapCapabilities = (capabilities) => {
    const native = capabilities || {};
    const nativeActions = new Set((native.page && native.page.actions) || []);
    const hasNativeActions = nativeActions.size > 0;
    const supportsAction = (action) => {
      if (!hasNativeActions) {
        return true;
      }
      return nativeActions.has(action);
    };
    const task = native.task || {};
    const tabs = native.tabs || {};
    const page = native.page || {};
    const events = freeze(Array.from(new Set([
      ...API2_EVENT_TYPES,
      ...(native.events || []),
    ])));
    return freeze({
      backend: native.backend || {
        name: 'bua-chromium',
        protocol: 'bua-mojo',
      },
      task: freeze({
        start: task.start !== false,
        pause: task.pause !== false,
        resume: task.resume !== false,
        cancel: task.cancel !== false,
        stop: task.stop !== false,
      }),
      tabs: freeze({
        create: tabs.create !== false,
        list: tabs.list !== false,
        current: tabs.current !== false,
        activate: tabs.activate !== false,
        close: tabs.close !== false,
      }),
      page: freeze({
        navigate: page.navigate !== false,
        snapshot: page.snapshot !== false,
        screenshot: page.screenshot !== false,
        act: page.act !== false,
        actions: freeze(API2_ACTIONS.filter(supportsAction)),
      }),
      events,
    });
  };

  const mapTab = (target, activeOverride) => {
    if (!target || target.kind === 'no_target') {
      const error = new Error(target && target.message ? target.message : 'No BUA tab is available');
      error.name = 'BuaError';
      error.code = target && target.reason ? target.reason : 'no_target';
      error.category = 'tab_state';
      error.details = {target};
      throw error;
    }
    const tab = {
      id: String(target.tabId || target.id || ''),
      title: target.title || '',
      url: target.url || '',
      active: Boolean(activeOverride !== undefined ? activeOverride : target.active),
    };
    if (target.loading !== undefined) {
      tab.loading = target.loading;
    }
    return freeze(tab);
  };

  const mapTaskState = (state) => freeze({
    id: String(state && (state.id || state.taskId) || 'task'),
    status: state && state.status === 'acting' ? 'running' : (state && state.status || 'idle'),
    startedAtMs: state && state.startedAtMs,
    updatedAtMs: Number(state && (state.updatedAtMs || state.updated) || Date.now()),
    reason: state && state.reason,
  });

  const mapTaskResumeResult = (result) => {
    if (result && result.state) {
      return freeze({
        state: mapTaskState(result.state),
        snapshot: result.snapshot ? mapSnapshot(result.snapshot) : undefined,
      });
    }
    return freeze({state: mapTaskState(result)});
  };

  const mapTaskStopResult = (result) =>
      result && result.state ? mapTaskState(result.state) : mapTaskState(result);

  const mapScreenshot = (screenshot) => {
    if (!screenshot) {
      return undefined;
    }
    if (screenshot.dataBase64) {
      return freeze({
        mimeType: screenshot.mimeType || 'image/png',
        dataBase64: screenshot.dataBase64,
        width: screenshot.widthPixels || screenshot.width,
        height: screenshot.heightPixels || screenshot.height,
      });
    }
    if (screenshot.uri) {
      const match = /^data:([^;,]+)?;base64,(.*)$/s.exec(String(screenshot.uri));
      if (match) {
        return freeze({
          mimeType: match[1] || screenshot.mimeType || 'image/png',
          dataBase64: match[2],
          width: screenshot.widthPixels || screenshot.width,
          height: screenshot.heightPixels || screenshot.height,
        });
      }
    }
    return freeze({
      mimeType: screenshot.mimeType || 'image/png',
      dataBase64: '',
      width: screenshot.widthPixels || screenshot.width,
      height: screenshot.heightPixels || screenshot.height,
    });
  };

  const collectText = (node, out = []) => {
    if (!node) {
      return out;
    }
    if (node.text) {
      out.push(String(node.text));
    } else if ((!node.children || !node.children.length) && node.name) {
      out.push(String(node.name));
    }
    for (const child of node.children || []) {
      collectText(child, out);
    }
    return out;
  };

  const mapSnapshot = (snapshot) => {
    const page = snapshot && (snapshot.page || snapshot.mainFrame) || {};
    const target = snapshot && snapshot.target || {};
    const text = typeof (snapshot && snapshot.text) === 'string' ? snapshot.text :
        (snapshot && snapshot.text && snapshot.text.innerText) ||
        collectText(snapshot && snapshot.content).join('\n');
    const viewport = snapshot && snapshot.viewport ? {
      width: Number(snapshot.viewport.width || 0),
      height: Number(snapshot.viewport.height || 0),
      deviceScaleFactor: snapshot.viewport.deviceScaleFactor,
    } : (page.viewportWidth && page.viewportHeight ? {
      width: Number(page.viewportWidth),
      height: Number(page.viewportHeight),
    } : undefined);
    return freeze({
      url: String(page.url || target.url || snapshot && snapshot.url || ''),
      title: page.title || target.title || snapshot && snapshot.title,
      capturedAtMs: Number(snapshot && (snapshot.capturedAtMs || snapshot.updatedAtMs) || Date.now()),
      viewport,
      content: snapshot && snapshot.content,
      text,
      screenshot: mapScreenshot(snapshot && snapshot.screenshot),
    });
  };

  const buildSession = async (options) => {
    const created = await request('createSession', {options});
    const sessionId = created && created.id ? created.id :
        (options && options.id ? options.id : `bua-${Date.now()}-${Math.random().toString(36).slice(2)}`);
    const capabilities = mapCapabilities(created && created.capabilities ?
        created.capabilities : await request('capabilities', {}));
    let latestSnapshot;
    const listeners = new Map(API2_EVENT_TYPES.map((type) => [type, new Set()]));

    const withSession = (payload) => Object.assign({sessionId}, payload || {});
    const emitEvent = (type, value) => {
      const handlers = listeners.get(type);
      if (!handlers || handlers.size === 0) {
        return;
      }
      const event = freeze({
        type,
        timestampMs: Date.now(),
        value,
      });
      for (const handler of Array.from(handlers)) {
        try {
          handler(event);
        } catch (_error) {
        }
      }
    };
    const subscribe = (type, handler) => {
      if (!listeners.has(type)) {
        throw makeLocalError(
            'unsupported_event',
            'unsupported',
            `Unsupported BUA event type: ${type}`,
            {type});
      }
      if (typeof handler !== 'function') {
        throw makeLocalError(
            'invalid_event_handler',
            'validation',
            'BUA event handler must be a function.',
            {type});
      }
      const handlers = listeners.get(type);
      handlers.add(handler);
      return freeze({
        unsubscribe: () => {
          handlers.delete(handler);
          return Promise.resolve();
        },
      });
    };
    const task = freeze({
      start: (taskOptions) =>
          request('task.start', withSession({options: taskOptions}))
              .then((state) => {
                const mapped = mapTaskState(state);
                emitEvent('task_state_changed', mapped);
                return mapped;
              }),
      state: () => request('task.state', withSession()).then(mapTaskState),
      pause: (reason) =>
          request('task.pause', withSession({reason})).then((state) => {
            const mapped = mapTaskState(state);
            emitEvent('task_state_changed', mapped);
            return mapped;
          }),
      resume: (resumeOptions) =>
          request('task.resume', withSession({options: resumeOptions}))
              .then((result) => {
                const mapped = mapTaskResumeResult(result);
                emitEvent('task_state_changed', mapped.state);
                if (mapped.snapshot) {
                  emitEvent('page_changed', mapped.snapshot);
                }
                return mapped;
              }),
      cancel: (reason) => request('task.cancelActions', withSession({reason})),
      stop: (reason) =>
          request('task.stop', withSession({reason})).then((state) => {
            const mapped = mapTaskStopResult(state);
            emitEvent('task_state_changed', mapped);
            return mapped;
          }),
    });
    const tabs = freeze({
      create: (tabOptions) => request('tabs.create', withSession({
        options: tabOptions || {},
      })).then((target) => {
        const tab = mapTab(target, tabOptions && tabOptions.activate);
        emitEvent('tab_changed', tab);
        return tab;
      }),
      list: (includeClosed) => request('tabs.list', withSession({
        includeClosed: Boolean(includeClosed),
      })).then((targets) => freeze((targets || []).map((target) => mapTab(target)))),
      current: () =>
          request('tabs.current', withSession()).then((target) => mapTab(target, true)),
      activate: (tabId) =>
          request('tabs.activate', withSession({tabId}))
              .then((target) => {
                const tab = mapTab(target, true);
                emitEvent('tab_changed', tab);
                return tab;
              }),
      close: (tabId) =>
          request('tabs.close', withSession({tabId})).then((closed) => {
            const didClose = Boolean(closed);
            if (didClose) {
              emitEvent('tab_changed', freeze({id: String(tabId), active: false}));
            }
            return didClose;
          }),
    });
    const events = freeze({
      subscribe: (type, handler) => Promise.resolve(subscribe(type, handler)),
    });
    const page = freeze({
      navigate: (url) => request('page.navigate', withSession({
        url: String(url || ''),
      })).then((result) => {
        const snapshot = result && result.snapshot ? mapSnapshot(result.snapshot) : undefined;
        const navigation = freeze({
          ok: Boolean(!result || result.ok !== false),
          requestedUrl: String(url || ''),
          finalUrl: result && result.finalUrl || snapshot && snapshot.url ||
              result && result.target && result.target.url,
          error: result && result.ok === false ? {
            code: result.code || 'navigation_failed',
            category: result.category || 'navigation',
            message: result.message || 'Navigation failed.',
          } : undefined,
          snapshot,
        });
        if (snapshot) {
          latestSnapshot = snapshot;
          emitEvent('page_changed', snapshot);
        }
        return navigation;
      }).catch((error) => freeze({
        ok: false,
        requestedUrl: String(url || ''),
        error: {
          code: error.code || error.name || 'navigation_failed',
          category: error.category || 'navigation',
          message: error.message || String(error),
          details: error.details,
        },
      })),
      snapshot: (snapshotOptions) => request('page.snapshot', withSession({
        options: snapshotOptions || {},
      })).then((snapshot) => {
        latestSnapshot = mapSnapshot(snapshot);
        return latestSnapshot;
      }),
      screenshot: (screenshotOptions) => request('page.screenshot', withSession({
        options: screenshotOptions || {},
      })).then((result) => {
        const screenshot = mapScreenshot(result && result.screenshot || result);
        if (!screenshot) {
          throw new Error('BUA snapshot did not return a screenshot.');
        }
        return screenshot;
      }),
      act: (actions, actOptions) => request('page.act', withSession({
        actions: Array.isArray(actions) ? actions : [actions],
        options: actOptions,
      })).then((result) => {
        const snapshot = result && result.snapshot ? mapSnapshot(result.snapshot) : undefined;
        if (snapshot) {
          latestSnapshot = snapshot;
          emitEvent('page_changed', snapshot);
        }
        return freeze({
          ok: Boolean(!result || result.ok !== false),
          error: result && result.ok === false ? {
            code: result.code || 'action_failed',
            category: result.category || 'input',
            message: result.message || 'BUA action failed.',
          } : undefined,
          effects: result && result.effects,
          snapshot,
        });
      }),
    });

    return freeze({
      id: sessionId,
      capabilities,
      task,
      tabs,
      page,
      events,
      close: (reason) => request('session.close', withSession({reason})),
    });
  };

  const client = freeze({
    capabilities: () => request('capabilities', {}).then(mapCapabilities),
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
