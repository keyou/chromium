// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/bua/bua_document_service.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "base/base64.h"
#include "base/check.h"
#include "base/functional/bind.h"
#include "base/json/json_reader.h"
#include "base/json/json_writer.h"
#include "base/location.h"
#include "base/strings/pattern.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/string_util.h"
#include "base/strings/stringprintf.h"
#include "base/strings/utf_string_conversions.h"
#include "base/task/single_thread_task_runner.h"
#include "base/time/time.h"
#include "base/values.h"
#include "chrome/browser/actor/actor_keyed_service.h"
#include "chrome/browser/actor/actor_proto_conversion.h"
#include "chrome/browser/actor/actor_task_metadata.h"
#include "chrome/browser/actor/tab_observation_strategy.h"
#include "chrome/browser/glic/actor/glic_actor_policy_checker.h"
#include "chrome/browser/glic/host/context/glic_page_context_fetcher.h"
#include "chrome/browser/glic/host/context/glic_tab_data.h"
#include "chrome/browser/glic/host/glic.mojom.h"
#include "chrome/browser/glic/public/glic_keyed_service.h"
#include "chrome/browser/glic/public/glic_keyed_service_factory.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/tab_list/tab_list_interface.h"
#include "chrome/browser/ui/browser_window/public/browser_collection.h"
#include "chrome/browser/ui/browser_window/public/browser_window_interface.h"
#include "chrome/browser/ui/browser_window/public/global_browser_collection.h"
#include "chrome/common/actor/action_result.h"
#include "chrome/common/actor/actor_constants.h"
#include "components/actor/core/task_source_info.h"
#include "components/actor/public/mojom/actor_types.mojom.h"
#include "components/optimization_guide/content/browser/page_content_proto_provider.h"
#include "components/optimization_guide/proto/features/actions_data.pb.h"
#include "components/optimization_guide/proto/features/common_quality_data.pb.h"
#include "components/page_content_annotations/content/page_context_fetcher.h"
#include "components/page_content_annotations/content/page_context_fetcher_options.h"
#include "components/tabs/public/tab_interface.h"
#include "content/public/browser/navigation_controller.h"
#include "content/public/browser/render_frame_host.h"
#include "content/public/browser/render_widget_host.h"
#include "content/public/browser/reload_type.h"
#include "content/public/browser/web_contents.h"
#include "mojo/public/cpp/base/proto_wrapper.h"
#include "third_party/blink/public/common/input/web_mouse_event.h"
#include "ui/gfx/geometry/point_f.h"
#include "url/gurl.h"
#include "url/origin.h"

namespace bua {
namespace {

namespace apc = optimization_guide::proto;

constexpr char kViewportNodeId[] = "viewport";
constexpr int kDefaultInnerTextBytesLimit = 256 * 1024;
constexpr int kDefaultPdfBytesLimit = 2 * 1024 * 1024;
constexpr int kDefaultMaxNodes = 2000;
constexpr int kDefaultWaitTimeoutMs = 5000;
constexpr int kMaxWaitTimeoutMs = 60000;
constexpr int kWaitPollIntervalMs = 250;
constexpr int kDefaultStableForMs = 500;

int64_t NowMs() {
  return base::Time::Now().InMillisecondsSinceUnixEpoch();
}

std::string WriteJson(base::DictValue dict) {
  std::string json;
  CHECK(base::JSONWriter::Write(base::Value(std::move(dict)), &json));
  return json;
}

std::string Success(base::Value value) {
  base::DictValue envelope;
  envelope.Set("ok", true);
  envelope.Set("value", std::move(value));
  return WriteJson(std::move(envelope));
}

std::string SuccessDict(base::DictValue value) {
  return Success(base::Value(std::move(value)));
}

std::string Error(std::string code,
                  std::string message,
                  std::string category = "backend_error") {
  base::DictValue error;
  error.Set("code", std::move(code));
  error.Set("message", std::move(message));
  error.Set("category", std::move(category));

  base::DictValue envelope;
  envelope.Set("ok", false);
  envelope.Set("error", std::move(error));
  return WriteJson(std::move(envelope));
}

base::ListValue StringList(std::initializer_list<std::string_view> values) {
  base::ListValue list;
  for (std::string_view value : values) {
    list.Append(std::string(value));
  }
  return list;
}

std::optional<base::DictValue> ParseRequestDict(
    const std::string& request_json) {
  return base::JSONReader::ReadDict(request_json, base::JSON_PARSE_RFC);
}

std::optional<std::string> FindStringMember(const base::DictValue& dict,
                                            std::string_view key) {
  const std::string* value = dict.FindString(key);
  if (!value || value->empty()) {
    return std::nullopt;
  }
  return *value;
}

bool IsAllowedFrame(content::RenderFrameHost& render_frame_host) {
  return render_frame_host.IsInPrimaryMainFrame();
}

std::string TabIdString(int32_t tab_id) {
  return base::StringPrintf("tab:%d", tab_id);
}

std::string WindowIdString(int32_t window_id) {
  return base::StringPrintf("window:%d", window_id);
}

std::optional<int32_t> ParseTabId(std::string_view id) {
  constexpr std::string_view kPrefix = "tab:";
  if (!id.starts_with(kPrefix)) {
    return std::nullopt;
  }
  int value = 0;
  if (!base::StringToInt(id.substr(kPrefix.size()), &value)) {
    return std::nullopt;
  }
  return value;
}

std::optional<int32_t> ParseWindowId(std::string_view id) {
  constexpr std::string_view kPrefix = "window:";
  if (!id.starts_with(kPrefix)) {
    return std::nullopt;
  }
  int value = 0;
  if (!base::StringToInt(id.substr(kPrefix.size()), &value)) {
    return std::nullopt;
  }
  return value;
}

bool IsEligibleBrowser(BrowserWindowInterface* browser,
                       content::BrowserContext* browser_context) {
  return browser && !browser->IsDeleteScheduled() &&
         browser->GetType() == BrowserWindowInterface::TYPE_NORMAL &&
         browser->GetProfile() == browser_context;
}

BrowserWindowInterface* FindBrowserForProfile(
    content::BrowserContext* browser_context,
    std::optional<int32_t> window_id = std::nullopt,
    bool require_active_tab = false) {
  GlobalBrowserCollection* collection = GlobalBrowserCollection::GetInstance();
  if (!collection) {
    return nullptr;
  }

  BrowserWindowInterface* result = nullptr;
  collection->ForEach(
      [&](BrowserWindowInterface* browser) {
        if (!IsEligibleBrowser(browser, browser_context)) {
          return true;
        }
        if (window_id && browser->GetSessionID().id() != *window_id) {
          return true;
        }
        if (require_active_tab) {
          TabListInterface* tab_list = TabListInterface::From(browser);
          if (!tab_list || tab_list->GetTabCount() <= 0 ||
              !tab_list->GetActiveTab()) {
            return true;
          }
        }
        result = browser;
        return false;
      },
      BrowserCollection::Order::kActivation);
  return result;
}

bool HasBrowserForProfile(content::BrowserContext* browser_context) {
  return !!FindBrowserForProfile(browser_context);
}

bool IsBuaUiTab(tabs::TabInterface* tab,
                content::WebContents* requesting_web_contents) {
  if (!tab) {
    return false;
  }
  if (tab->GetContents() == requesting_web_contents) {
    return true;
  }
  const GURL url = tab->GetURL();
  return url.SchemeIs("chrome") && url.host() == "glic";
}

bool IsUsableDefaultTargetTab(tabs::TabInterface* tab,
                              content::BrowserContext* browser_context,
                              content::WebContents* requesting_web_contents) {
  return tab && tab->GetProfile() == browser_context &&
         !IsBuaUiTab(tab, requesting_web_contents);
}

tabs::TabInterface* FindDefaultTabForProfile(
    content::BrowserContext* browser_context,
    content::WebContents* requesting_web_contents) {
  GlobalBrowserCollection* collection = GlobalBrowserCollection::GetInstance();
  if (!collection) {
    return nullptr;
  }

  tabs::TabInterface* best_tab = nullptr;
  base::Time best_last_active_time;
  collection->ForEach(
      [&](BrowserWindowInterface* browser) {
        if (!IsEligibleBrowser(browser, browser_context)) {
          return true;
        }
        TabListInterface* tab_list = TabListInterface::From(browser);
        if (!tab_list) {
          return true;
        }
        tabs::TabInterface* active_tab = tab_list->GetActiveTab();
        if (IsUsableDefaultTargetTab(active_tab, browser_context,
                                     requesting_web_contents)) {
          best_tab = active_tab;
          return false;
        }
        for (tabs::TabInterface* tab : tab_list->GetAllTabs()) {
          if (!IsUsableDefaultTargetTab(tab, browser_context,
                                        requesting_web_contents)) {
            continue;
          }
          if (!best_tab || tab->GetLastActiveTime() > best_last_active_time) {
            best_tab = tab;
            best_last_active_time = tab->GetLastActiveTime();
          }
        }
        return true;
      },
      BrowserCollection::Order::kActivation);
  return best_tab;
}

base::DictValue BuildNoTargetState(std::string reason, std::string message) {
  base::DictValue state;
  state.Set("kind", "no_target");
  state.Set("reason", std::move(reason));
  state.Set("message", std::move(message));
  return state;
}

std::string NoDefaultTargetError(content::BrowserContext* browser_context,
                                 std::string operation) {
  if (!HasBrowserForProfile(browser_context)) {
    return Error("no_target",
                 operation + " requires a browser window for this profile.",
                 "target_not_found");
  }
  return Error("no_target",
               operation +
                   " requires target.tabId or an available tab in the "
                   "selected browser window.",
               "target_not_found");
}

const base::DictValue* FindRequestTargetRef(const base::DictValue& request) {
  if (const base::DictValue* target = request.FindDict("target")) {
    return target;
  }
  if (const base::DictValue* options = request.FindDict("options")) {
    return options->FindDict("target");
  }
  return nullptr;
}

tabs::TabInterface* ResolveTargetRef(const base::DictValue& target,
                                     tabs::TabInterface* default_tab,
                                     content::BrowserContext* browser_context,
                                     std::string* error_json) {
  if (target.FindBool("current").value_or(false)) {
    if (!default_tab) {
      *error_json = NoDefaultTargetError(browser_context, "target.current");
    }
    return default_tab;
  }

  const std::string* target_id = target.FindString("targetId");
  const std::string* target_tab_id = target.FindString("tabId");
  if (!target_tab_id && target_id && target_id->starts_with("tab:")) {
    target_tab_id = target_id;
  }

  if (target_tab_id) {
    std::optional<int32_t> parsed_tab_id = ParseTabId(*target_tab_id);
    if (!parsed_tab_id) {
      *error_json =
          Error("invalid_request", "target.tabId is invalid.", "input");
      return nullptr;
    }

    tabs::TabInterface* target_tab = tabs::TabHandle(*parsed_tab_id).Get();
    if (!target_tab || target_tab->GetProfile() != browser_context) {
      *error_json =
          Error("target_not_found",
                "target.tabId does not refer to a live tab in this profile.",
                "target_not_found");
      return nullptr;
    }
    return target_tab;
  }

  const std::string* target_window_id = target.FindString("windowId");
  if (!target_window_id && target_id && target_id->starts_with("window:")) {
    target_window_id = target_id;
  }
  if (target_window_id) {
    std::optional<int32_t> parsed_window_id = ParseWindowId(*target_window_id);
    if (!parsed_window_id) {
      *error_json =
          Error("invalid_request", "target.windowId is invalid.", "input");
      return nullptr;
    }

    BrowserWindowInterface* browser =
        FindBrowserForProfile(browser_context, parsed_window_id,
                              /*require_active_tab=*/true);
    if (!browser) {
      *error_json =
          Error("target_not_found",
                "target.windowId does not refer to a browser window with an "
                "active tab in this profile.",
                "target_not_found");
      return nullptr;
    }
    return TabListInterface::From(browser)->GetActiveTab();
  }

  *error_json = Error(
      "invalid_request",
      "target requires tabId, targetId, windowId, or current=true.", "input");
  return nullptr;
}

tabs::TabInterface* ResolveTargetTabOrDefault(
    const base::DictValue& request,
    tabs::TabInterface* default_tab,
    content::BrowserContext* browser_context,
    std::string* error_json) {
  const base::DictValue* target = FindRequestTargetRef(request);
  if (!target) {
    return default_tab;
  }
  return ResolveTargetRef(*target, default_tab, browser_context, error_json);
}

std::optional<int32_t> ResolveActionTabId(
    const base::DictValue& action,
    tabs::TabInterface* default_tab,
    content::BrowserContext* browser_context,
    std::string* error_json) {
  const base::DictValue* target_ref = action.FindDict("targetRef");
  if (!target_ref) {
    if (!default_tab) {
      *error_json = NoDefaultTargetError(browser_context, "Action");
      return std::nullopt;
    }
    return default_tab->GetHandle().raw_value();
  }

  tabs::TabInterface* target_tab =
      ResolveTargetRef(*target_ref, default_tab, browser_context, error_json);
  if (!target_tab) {
    if (error_json->empty()) {
      *error_json = NoDefaultTargetError(browser_context, "Action");
    }
    return std::nullopt;
  }
  return target_tab->GetHandle().raw_value();
}

base::DictValue BuildTargetSnapshotFromTabData(
    const glic::mojom::TabData& tab_data) {
  base::DictValue target;
  target.Set("id", TabIdString(tab_data.tab_id));
  target.Set("kind", "page");
  target.Set("tabId", TabIdString(tab_data.tab_id));
  target.Set("windowId", WindowIdString(tab_data.window_id));
  target.Set("url", tab_data.url.spec());
  if (tab_data.title) {
    target.Set("title", *tab_data.title);
  }
  target.Set("mimeType", tab_data.document_mime_type);
  if (tab_data.is_active_in_window) {
    target.Set("active", *tab_data.is_active_in_window);
  }
  if (tab_data.is_window_active) {
    target.Set("focused", *tab_data.is_window_active);
  }
  target.Set("readable", true);
  if (tab_data.is_media_active) {
    target.Set("mediaActive", *tab_data.is_media_active);
  }
  if (tab_data.is_tab_content_captured) {
    target.Set("captured", *tab_data.is_tab_content_captured);
  }
  target.Set("updatedAtMs", static_cast<double>(NowMs()));
  return target;
}

base::DictValue BuildTargetSnapshotFromTab(tabs::TabInterface* tab) {
  if (tab) {
    glic::mojom::TabDataPtr tab_data = glic::CreateTabData(tab);
    if (tab_data) {
      return BuildTargetSnapshotFromTabData(*tab_data);
    }
  }

  base::DictValue target;
  if (!tab) {
    target.Set("id", "unknown");
    target.Set("kind", "page");
    target.Set("readable", false);
    return target;
  }

  const int32_t tab_id = tab->GetHandle().raw_value();
  target.Set("id", TabIdString(tab_id));
  target.Set("kind", "page");
  target.Set("tabId", TabIdString(tab_id));
  if (BrowserWindowInterface* browser = tab->GetBrowserWindowInterface()) {
    target.Set("windowId", WindowIdString(browser->GetSessionID().id()));
  }
  target.Set("url", tab->GetURL().spec());
  target.Set("title", base::UTF16ToUTF8(tab->GetTitle()));
  if (content::WebContents* contents = tab->GetContents()) {
    target.Set("mimeType", contents->GetContentsMimeType());
    target.Set("readable", true);
  } else {
    target.Set("readable", false);
  }
  target.Set("active", tab->IsActivated());
  target.Set("focused", false);
  target.Set("updatedAtMs", static_cast<double>(NowMs()));
  return target;
}

base::ListValue BuildTargetListForProfile(
    content::BrowserContext* browser_context,
    std::optional<int32_t> window_id,
    bool include_background) {
  base::ListValue tabs;
  GlobalBrowserCollection* collection = GlobalBrowserCollection::GetInstance();
  if (!collection) {
    return tabs;
  }

  collection->ForEach(
      [&](BrowserWindowInterface* browser) {
        if (!IsEligibleBrowser(browser, browser_context)) {
          return true;
        }
        if (window_id && browser->GetSessionID().id() != *window_id) {
          return true;
        }
        TabListInterface* tab_list = TabListInterface::From(browser);
        if (!tab_list) {
          return true;
        }
        for (tabs::TabInterface* tab : tab_list->GetAllTabs()) {
          if (!tab || (!include_background && !tab->IsActivated())) {
            continue;
          }
          tabs.Append(BuildTargetSnapshotFromTab(tab));
        }
        return true;
      },
      BrowserCollection::Order::kActivation);
  return tabs;
}

bool IsCreateTabUrlAllowed(const GURL& url) {
  return url.is_valid() && (url.SchemeIsHTTPOrHTTPS() || url.SchemeIs("about"));
}

tabs::TabInterface* CreateTabForProfile(
    content::BrowserContext* browser_context,
    const GURL& url,
    bool background,
    std::optional<int32_t> window_id,
    std::string* error_json) {
  BrowserWindowInterface* browser =
      FindBrowserForProfile(browser_context, window_id,
                            /*require_active_tab=*/false);
  if (!browser) {
    *error_json =
        Error(window_id ? "target_not_found" : "no_target",
              window_id ? "windowId does not refer to a browser window in "
                          "this profile."
                        : "tabs.create requires an existing browser window "
                          "for this profile.",
              "target_not_found");
    return nullptr;
  }
  TabListInterface* tab_list = TabListInterface::From(browser);
  if (!tab_list) {
    *error_json = Error("backend_unavailable",
                        "Selected browser window does not expose a tab list.",
                        "backend_error");
    return nullptr;
  }
  tabs::TabInterface* tab =
      tab_list->OpenTab(url, /*index=*/-1, /*foreground=*/!background);
  if (!tab) {
    *error_json =
        Error("create_tab_failed", "TabListInterface did not create a tab.",
              "browser_state");
  }
  return tab;
}

bool NavigateActionCanCreateTab(const base::DictValue& action) {
  const std::string kind = FindStringMember(action, "kind").value_or("");
  if (kind != "navigate") {
    return false;
  }
  const base::DictValue* target_ref = action.FindDict("targetRef");
  return !target_ref || target_ref->FindBool("current").value_or(false);
}

std::optional<GURL> NavigateUrlFromAction(const base::DictValue& action,
                                          std::string* error_json) {
  const std::string* url_string = action.FindString("url");
  if (!url_string) {
    *error_json =
        Error("invalid_request", "navigate action requires url.", "input");
    return std::nullopt;
  }
  const GURL url(*url_string);
  if (!url.is_valid() || !url.SchemeIsHTTPOrHTTPS()) {
    *error_json =
        Error("invalid_request",
              "navigate action only accepts valid HTTP(S) URLs.", "navigation");
    return std::nullopt;
  }
  return url;
}

base::DictValue BuildCreatedTabNavigateResult(const base::DictValue& action,
                                              tabs::TabInterface* tab,
                                              base::TimeTicks start_time) {
  base::DictValue result;
  result.Set("ok", true);
  result.Set("status", "succeeded");
  result.Set(
      "actionId",
      FindStringMember(action, "id").value_or(std::string("bua-action-0")));
  result.Set("code", "bua:navigate_created_tab");
  result.Set("category", "navigation");
  result.Set("message",
             "navigate created a new tab because no default tab existed.");
  result.Set("target", BuildTargetSnapshotFromTab(tab));

  base::DictValue timing;
  timing.Set("endedAtMs", static_cast<double>(NowMs()));
  timing.Set(
      "elapsedMs",
      static_cast<int>((base::TimeTicks::Now() - start_time).InMilliseconds()));
  result.Set("timing", std::move(timing));
  return result;
}

base::DictValue BuildReloadActionResult(const base::DictValue& action,
                                        tabs::TabInterface* tab,
                                        base::TimeTicks start_time,
                                        bool ignore_cache) {
  base::DictValue result;
  result.Set("ok", true);
  result.Set("status", "succeeded");
  result.Set("actionId",
             FindStringMember(action, "id").value_or("bua-action-0"));
  result.Set("code", "bua:reload_started");
  result.Set("category", "navigation");
  result.Set("message", ignore_cache ? "Reload bypassing cache started."
                                     : "Reload started.");
  result.Set("target", BuildTargetSnapshotFromTab(tab));

  base::DictValue timing;
  timing.Set("endedAtMs", static_cast<double>(NowMs()));
  timing.Set(
      "elapsedMs",
      static_cast<int>((base::TimeTicks::Now() - start_time).InMilliseconds()));
  result.Set("timing", std::move(timing));
  return result;
}

base::DictValue BuildMiddleClickActionResult(const base::DictValue& action,
                                             tabs::TabInterface* tab,
                                             base::TimeTicks start_time,
                                             const gfx::PointF& point,
                                             int click_count) {
  base::DictValue result;
  result.Set("ok", true);
  result.Set("status", "succeeded");
  result.Set("actionId",
             FindStringMember(action, "id").value_or("bua-action-0"));
  result.Set("code", "bua:middle_click_dispatched");
  result.Set("category", "actuation");
  result.Set("message", "Middle click was dispatched to the selected tab.");
  result.Set("target", BuildTargetSnapshotFromTab(tab));

  base::DictValue point_value;
  point_value.Set("x", point.x());
  point_value.Set("y", point.y());
  result.Set("point", std::move(point_value));
  result.Set("clickCount", click_count);

  base::DictValue timing;
  timing.Set("endedAtMs", static_cast<double>(NowMs()));
  timing.Set(
      "elapsedMs",
      static_cast<int>((base::TimeTicks::Now() - start_time).InMilliseconds()));
  result.Set("timing", std::move(timing));
  return result;
}

base::DictValue BuildCapabilities() {
  base::DictValue backend;
  backend.Set("name", "bua-chromium");
  backend.Set("version", "0.2");
  backend.Set("protocol", "bua-mojo");

  base::DictValue task;
  task.Set("start", true);
  task.Set("pause", true);
  task.Set("resume", true);
  task.Set("cancel", true);
  task.Set("stop", true);

  base::DictValue tabs;
  tabs.Set("create", true);
  tabs.Set("list", true);
  tabs.Set("current", true);
  tabs.Set("activate", true);
  tabs.Set("close", true);

  base::DictValue page;
  page.Set("navigate", true);
  page.Set("snapshot", true);
  page.Set("screenshot", true);
  page.Set("act", true);
  page.Set("actions",
           StringList({"history", "click", "type", "scroll", "scroll_to",
                       "move_mouse", "drag", "select", "wait"}));

  base::DictValue capabilities;
  capabilities.Set("backend", std::move(backend));
  capabilities.Set("task", std::move(task));
  capabilities.Set("tabs", std::move(tabs));
  capabilities.Set("page", std::move(page));
  capabilities.Set("events", base::ListValue());
  return capabilities;
}

using DocumentIdentifierToShortIdMap = std::map<std::string, int>;
using ShortDocumentIdToIdentifierMap = std::map<int, std::string>;

struct BuaPageNodeRef {
  std::string document_identifier;
  int dom_node_id = 0;
};

int RegisterShortDocumentId(
    std::string_view document_identifier,
    DocumentIdentifierToShortIdMap* document_identifier_to_short_id,
    ShortDocumentIdToIdentifierMap* short_document_id_to_identifier,
    int* next_short_document_id) {
  CHECK(document_identifier_to_short_id);
  CHECK(short_document_id_to_identifier);
  CHECK(next_short_document_id);

  std::string document_identifier_string(document_identifier);
  auto existing =
      document_identifier_to_short_id->find(document_identifier_string);
  if (existing != document_identifier_to_short_id->end()) {
    return existing->second;
  }

  const int short_document_id = (*next_short_document_id)++;
  auto [inserted, _] = document_identifier_to_short_id->emplace(
      std::move(document_identifier_string), short_document_id);
  short_document_id_to_identifier->emplace(short_document_id, inserted->first);
  return short_document_id;
}

std::string EncodeBuaPageNodeId(
    std::string_view document_identifier,
    int dom_node_id,
    DocumentIdentifierToShortIdMap* document_identifier_to_short_id,
    ShortDocumentIdToIdentifierMap* short_document_id_to_identifier,
    int* next_short_document_id) {
  const int short_document_id =
      RegisterShortDocumentId(document_identifier, document_identifier_to_short_id,
                              short_document_id_to_identifier,
                              next_short_document_id);
  return base::StringPrintf("%d.%d", short_document_id, dom_node_id);
}

std::optional<BuaPageNodeRef> DecodeBuaPageNodeId(
    std::string_view node_id,
    const ShortDocumentIdToIdentifierMap& short_document_id_to_identifier) {
  size_t separator = node_id.rfind('.');
  if (separator == std::string_view::npos || separator == 0 ||
      separator == node_id.size() - 1) {
    return std::nullopt;
  }

  int short_document_id = 0;
  if (!base::StringToInt(node_id.substr(0, separator), &short_document_id) ||
      short_document_id <= 0) {
    return std::nullopt;
  }
  auto document =
      short_document_id_to_identifier.find(short_document_id);
  if (document == short_document_id_to_identifier.end()) {
    return std::nullopt;
  }

  int dom_node_id = 0;
  if (!base::StringToInt(node_id.substr(separator + 1), &dom_node_id)) {
    return std::nullopt;
  }
  return BuaPageNodeRef{document->second, dom_node_id};
}

std::string OriginFromSecurityOrigin(
    const apc::SecurityOrigin& security_origin) {
  return security_origin.opaque() ? std::string("opaque")
                                  : security_origin.value();
}

base::DictValue BuildRect(const apc::BoundingRect& rect) {
  base::DictValue value;
  value.Set("x", rect.x());
  value.Set("y", rect.y());
  value.Set("width", rect.width());
  value.Set("height", rect.height());
  return value;
}

base::DictValue BuildSize(const apc::BoundingSize& size) {
  base::DictValue value;
  value.Set("width", size.width());
  value.Set("height", size.height());
  return value;
}

std::string SnapshotModeFromRequest(const base::DictValue& request) {
  const base::DictValue* options = request.FindDict("options");
  const std::string* mode = options ? options->FindString("mode") : nullptr;
  return mode && *mode == "interact" ? "interact" : "default";
}

apc::AnnotatedPageContentMode ApcModeForSnapshotMode(
    std::string_view snapshot_mode) {
  return snapshot_mode == "interact"
             ? apc::ANNOTATED_PAGE_CONTENT_MODE_ACTIONABLE_ELEMENTS
             : apc::ANNOTATED_PAGE_CONTENT_MODE_DEFAULT;
}

bool HasVisibleBounds(const apc::ContentAttributes& attributes) {
  return attributes.has_geometry() &&
         attributes.geometry().has_visible_bounding_box() &&
         attributes.geometry().visible_bounding_box().width() > 0 &&
         attributes.geometry().visible_bounding_box().height() > 0;
}

std::string NodeKindForAttributes(const apc::ContentAttributes& attributes) {
  if (attributes.attribute_type() == apc::CONTENT_ATTRIBUTE_FORM_CONTROL &&
      attributes.has_form_control_data()) {
    switch (attributes.form_control_data().form_control_type()) {
      case apc::FORM_CONTROL_TYPE_TEXT_AREA:
        return "textarea";
      case apc::FORM_CONTROL_TYPE_SELECT_ONE:
      case apc::FORM_CONTROL_TYPE_SELECT_MULTIPLE:
        return "select";
      case apc::FORM_CONTROL_TYPE_INPUT_CHECKBOX:
        return "checkbox";
      case apc::FORM_CONTROL_TYPE_INPUT_RADIO:
        return "radio";
      case apc::FORM_CONTROL_TYPE_BUTTON_BUTTON:
      case apc::FORM_CONTROL_TYPE_BUTTON_SUBMIT:
      case apc::FORM_CONTROL_TYPE_BUTTON_RESET:
      case apc::FORM_CONTROL_TYPE_BUTTON_POPOVER:
      case apc::FORM_CONTROL_TYPE_INPUT_BUTTON:
      case apc::FORM_CONTROL_TYPE_INPUT_IMAGE:
      case apc::FORM_CONTROL_TYPE_INPUT_RESET:
      case apc::FORM_CONTROL_TYPE_INPUT_SUBMIT:
        return "button";
      default:
        return "input";
    }
  }

  switch (attributes.attribute_type()) {
    case apc::CONTENT_ATTRIBUTE_ROOT:
    case apc::CONTENT_ATTRIBUTE_IFRAME:
      return "frame";
    case apc::CONTENT_ATTRIBUTE_CONTAINER:
    case apc::CONTENT_ATTRIBUTE_PARAGRAPH:
    case apc::CONTENT_ATTRIBUTE_HEADING:
      return "section";
    case apc::CONTENT_ATTRIBUTE_TEXT:
      return "text";
    case apc::CONTENT_ATTRIBUTE_ANCHOR:
      return "link";
    case apc::CONTENT_ATTRIBUTE_IMAGE:
      return "image";
    case apc::CONTENT_ATTRIBUTE_FORM:
      return "form";
    case apc::CONTENT_ATTRIBUTE_TABLE:
      return "table";
    case apc::CONTENT_ATTRIBUTE_TABLE_ROW:
      return "row";
    case apc::CONTENT_ATTRIBUTE_TABLE_CELL:
      return "cell";
    case apc::CONTENT_ATTRIBUTE_ORDERED_LIST:
    case apc::CONTENT_ATTRIBUTE_UNORDERED_LIST:
      return "list";
    case apc::CONTENT_ATTRIBUTE_LIST_ITEM:
      return "list_item";
    case apc::CONTENT_ATTRIBUTE_VIDEO:
      return "media";
    case apc::CONTENT_ATTRIBUTE_CANVAS:
      return "canvas";
    default:
      return "unknown";
  }
}

std::string RoleForAttributes(const apc::ContentAttributes& attributes) {
  if (attributes.has_aria_role()) {
    return base::NumberToString(static_cast<int>(attributes.aria_role()));
  }
  return NodeKindForAttributes(attributes);
}

void MaybeAddAction(base::ListValue& actions, std::string_view action) {
  for (const base::Value& existing : actions) {
    if (existing.is_string() && existing.GetString() == action) {
      return;
    }
  }
  actions.Append(std::string(action));
}

base::ListValue ActionsForAttributes(const apc::ContentAttributes& attributes) {
  base::ListValue actions;
  if (attributes.has_interaction_info()) {
    const apc::InteractionInfo& interaction = attributes.interaction_info();
    if (interaction.clickability_reasons_size() > 0) {
      MaybeAddAction(actions, "click");
      MaybeAddAction(actions, "hover");
    }
    if (interaction.has_scroller_info()) {
      MaybeAddAction(actions, "scroll");
    }
  }

  if (attributes.attribute_type() == apc::CONTENT_ATTRIBUTE_FORM_CONTROL &&
      attributes.has_form_control_data()) {
    const apc::FormControlData& control = attributes.form_control_data();
    switch (control.form_control_type()) {
      case apc::FORM_CONTROL_TYPE_SELECT_ONE:
      case apc::FORM_CONTROL_TYPE_SELECT_MULTIPLE:
        MaybeAddAction(actions, "select");
        break;
      case apc::FORM_CONTROL_TYPE_INPUT_CHECKBOX:
      case apc::FORM_CONTROL_TYPE_INPUT_RADIO:
      case apc::FORM_CONTROL_TYPE_BUTTON_BUTTON:
      case apc::FORM_CONTROL_TYPE_BUTTON_SUBMIT:
      case apc::FORM_CONTROL_TYPE_BUTTON_RESET:
      case apc::FORM_CONTROL_TYPE_BUTTON_POPOVER:
      case apc::FORM_CONTROL_TYPE_INPUT_BUTTON:
      case apc::FORM_CONTROL_TYPE_INPUT_IMAGE:
      case apc::FORM_CONTROL_TYPE_INPUT_RESET:
      case apc::FORM_CONTROL_TYPE_INPUT_SUBMIT:
        MaybeAddAction(actions, "click");
        break;
      default:
        MaybeAddAction(actions, "type");
        break;
    }
  }

  if (HasVisibleBounds(attributes)) {
    MaybeAddAction(actions, "hover");
  }
  return actions;
}

std::optional<base::DictValue> ScrollInfoForAttributes(
    const apc::ContentAttributes& attributes) {
  if (!attributes.has_interaction_info() ||
      !attributes.interaction_info().has_scroller_info()) {
    return std::nullopt;
  }

  const apc::ScrollerInfo& scroller =
      attributes.interaction_info().scroller_info();
  base::DictValue scroll_info;
  scroll_info.Set("scrollableX", scroller.user_scrollable_horizontal());
  scroll_info.Set("scrollableY", scroller.user_scrollable_vertical());
  if (scroller.has_scrolling_bounds()) {
    scroll_info.Set("scrollingBounds",
                    BuildSize(scroller.scrolling_bounds()));
  }
  if (scroller.has_visible_area()) {
    scroll_info.Set("visibleArea", BuildRect(scroller.visible_area()));
  }
  return scroll_info;
}

void CollectDescendantNamesForNode(const apc::ContentNode& node,
                                   std::vector<std::string>* names) {
  constexpr size_t kMaxNameParts = 8;
  if (names->size() >= kMaxNameParts) {
    return;
  }

  for (const apc::ContentNode& child : node.children_nodes()) {
    if (names->size() >= kMaxNameParts) {
      return;
    }

    const apc::ContentAttributes& attributes = child.content_attributes();
    if (attributes.has_label() && !attributes.label().empty()) {
      names->push_back(attributes.label());
      continue;
    }
    if (attributes.has_text_data() &&
        !attributes.text_data().text_content().empty()) {
      names->push_back(attributes.text_data().text_content());
      continue;
    }
    if (attributes.has_image_data() &&
        !attributes.image_data().image_caption().empty()) {
      names->push_back(attributes.image_data().image_caption());
      continue;
    }

    CollectDescendantNamesForNode(child, names);
  }
}

std::string DescendantNameForNode(const apc::ContentNode& node) {
  std::vector<std::string> names;
  CollectDescendantNamesForNode(node, &names);
  return base::JoinString(names, " ");
}

bool ShouldDeriveNameFromDescendants(
    const apc::ContentAttributes& attributes) {
  return attributes.attribute_type() == apc::CONTENT_ATTRIBUTE_FORM_CONTROL ||
         attributes.attribute_type() == apc::CONTENT_ATTRIBUTE_ANCHOR;
}

base::DictValue BuildFrameInfoFromFrameData(const apc::FrameData& frame_data,
                                            bool main) {
  base::DictValue frame;
  frame.Set("main", main);
  if (frame_data.has_document_identifier()) {
    frame.Set("documentId",
              frame_data.document_identifier().serialized_token());
  }
  if (frame_data.has_url()) {
    frame.Set("url", frame_data.url());
  }
  if (frame_data.has_title()) {
    frame.Set("title", frame_data.title());
  }
  if (frame_data.has_security_origin()) {
    frame.Set("origin", OriginFromSecurityOrigin(frame_data.security_origin()));
  }
  return frame;
}

base::DictValue BuildBuaPageSnapshotNode(
    const apc::ContentNode& node,
    const std::string& snapshot_id,
    std::string document_identifier,
    const apc::FrameData* main_frame_data,
    int max_nodes,
    int* visited_nodes,
    int* generated_node_id,
    bool* truncated,
    DocumentIdentifierToShortIdMap* document_identifier_to_short_id,
    ShortDocumentIdToIdentifierMap* short_document_id_to_identifier,
    int* next_short_document_id) {
  ++(*visited_nodes);
  if (*visited_nodes > max_nodes) {
    *truncated = true;
    base::DictValue omitted;
    omitted.Set("id", base::StringPrintf("omitted:%d", *generated_node_id));
    omitted.Set("snapshotId", snapshot_id);
    omitted.Set("kind", "unknown");
    omitted.Set("role", "omitted");
    omitted.Set("name", "Additional nodes omitted by snapshot budget.");
    ++(*generated_node_id);
    return omitted;
  }

  const apc::ContentAttributes& attributes = node.content_attributes();
  base::DictValue out;
  if (attributes.has_common_ancestor_dom_node_id() &&
      !document_identifier.empty()) {
    out.Set("id", EncodeBuaPageNodeId(
                      document_identifier,
                      attributes.common_ancestor_dom_node_id(),
                      document_identifier_to_short_id,
                      short_document_id_to_identifier,
                      next_short_document_id));
  } else {
    out.Set("id", base::StringPrintf("anon.%d", *generated_node_id));
    ++(*generated_node_id);
  }
  out.Set("snapshotId", snapshot_id);
  out.Set("kind", NodeKindForAttributes(attributes));
  out.Set("role", RoleForAttributes(attributes));

  if (attributes.has_label() && !attributes.label().empty()) {
    out.Set("name", attributes.label());
  }

  if (attributes.has_text_data()) {
    out.Set("text", attributes.text_data().text_content());
    if (!attributes.has_label() || attributes.label().empty()) {
      out.Set("name", attributes.text_data().text_content());
    }
  } else if (attributes.has_image_data() &&
             !attributes.image_data().image_caption().empty()) {
    out.Set("name", attributes.image_data().image_caption());
  } else if (attributes.has_anchor_data() &&
             !attributes.anchor_data().url().empty()) {
    out.Set("value", attributes.anchor_data().url());
  }

  if (!out.FindString("name") && ShouldDeriveNameFromDescendants(attributes)) {
    std::string descendant_name = DescendantNameForNode(node);
    if (!descendant_name.empty()) {
      out.Set("name", std::move(descendant_name));
    }
  }

  if (attributes.has_geometry() &&
      attributes.geometry().has_visible_bounding_box()) {
    out.Set("bounds", BuildRect(attributes.geometry().visible_bounding_box()));
  }

  base::DictValue state;
  state.Set("visible", HasVisibleBounds(attributes));
  state.Set("offscreen", !HasVisibleBounds(attributes));
  if (attributes.has_interaction_info()) {
    state.Set("disabled", attributes.interaction_info().is_disabled());
  }
  if (attributes.has_form_control_data()) {
    const apc::FormControlData& control = attributes.form_control_data();
    state.Set("checked", control.is_checked());
    state.Set("editable", !control.is_readonly());

    base::DictValue field;
    if (!control.field_name().empty()) {
      field.Set("name", control.field_name());
    }
    if (!control.field_value().empty()) {
      out.Set("value", control.field_value());
    }
    if (!control.placeholder().empty()) {
      field.Set("placeholder", control.placeholder());
    }
    field.Set("required", control.is_required());
    if (control.select_options_size() > 0) {
      base::ListValue options;
      for (const apc::SelectOption& option : control.select_options()) {
        base::DictValue option_value;
        option_value.Set("value", option.value());
        option_value.Set("label", option.text());
        option_value.Set("selected", option.is_selected());
        options.Append(std::move(option_value));
      }
      field.Set("options", std::move(options));
    }
    out.Set("field", std::move(field));
  }
  out.Set("state", std::move(state));
  out.Set("actions", ActionsForAttributes(attributes));
  if (std::optional<base::DictValue> scroll_info =
          ScrollInfoForAttributes(attributes)) {
    out.Set("scrollInfo", std::move(*scroll_info));
  }

  if (attributes.attribute_type() == apc::CONTENT_ATTRIBUTE_ROOT &&
      main_frame_data) {
    out.Set("frame", BuildFrameInfoFromFrameData(*main_frame_data, true));
  } else if (attributes.has_iframe_data() &&
             attributes.iframe_data().has_frame_data()) {
    const apc::FrameData& frame_data = attributes.iframe_data().frame_data();
    out.Set("frame", BuildFrameInfoFromFrameData(frame_data, false));
    if (frame_data.has_document_identifier()) {
      document_identifier = frame_data.document_identifier().serialized_token();
    }
  } else if (attributes.has_form_data()) {
    base::DictValue form;
    if (!attributes.form_data().form_name().empty()) {
      form.Set("name", attributes.form_data().form_name());
    }
    if (!attributes.form_data().action_url().empty()) {
      form.Set("actionUrl", attributes.form_data().action_url());
    }
    out.Set("form", std::move(form));
  }

  if (node.children_nodes_size() > 0 && *visited_nodes <= max_nodes) {
    base::ListValue children;
    for (const apc::ContentNode& child : node.children_nodes()) {
      if (*visited_nodes >= max_nodes) {
        *truncated = true;
        break;
      }
      children.Append(BuildBuaPageSnapshotNode(
          child, snapshot_id, document_identifier, main_frame_data, max_nodes,
          visited_nodes, generated_node_id, truncated,
          document_identifier_to_short_id, short_document_id_to_identifier,
          next_short_document_id));
    }
    if (!children.empty()) {
      out.Set("children", std::move(children));
    }
  }

  return out;
}

base::DictValue BuildFallbackContent(std::string snapshot_id,
                                     const glic::mojom::TabContext& context) {
  base::DictValue content;
  content.Set("id", "fallback-root");
  content.Set("snapshotId", snapshot_id);
  content.Set("kind", "frame");
  content.Set("role", "document");
  if (context.tab_data) {
    if (context.tab_data->title) {
      content.Set("name", *context.tab_data->title);
    }
    base::DictValue frame;
    frame.Set("main", true);
    frame.Set("url", context.tab_data->url.spec());
    frame.Set("origin", url::Origin::Create(context.tab_data->url).Serialize());
    if (context.tab_data->title) {
      frame.Set("title", *context.tab_data->title);
    }
    content.Set("frame", std::move(frame));
  }
  if (context.web_page_data) {
    content.Set("text", context.web_page_data->main_document->inner_text);
  }
  return content;
}

base::DictValue BuildSnapshotFromTabContext(
    std::string snapshot_id,
    std::string snapshot_mode,
    int generation,
    const glic::mojom::TabContext& context,
    content::WebContents* web_contents,
    int max_nodes,
    DocumentIdentifierToShortIdMap* document_identifier_to_short_id,
    ShortDocumentIdToIdentifierMap* short_document_id_to_identifier,
    int* next_short_document_id) {
  base::DictValue page;
  if (context.tab_data) {
    page.Set("url", context.tab_data->url.spec());
    page.Set("origin", url::Origin::Create(context.tab_data->url).Serialize());
    if (context.tab_data->title) {
      page.Set("title", *context.tab_data->title);
    }
    page.Set("mimeType", context.tab_data->document_mime_type);
  }
  if (web_contents) {
    page.Set("loading", web_contents->IsLoading());
    page.Set("crashed", web_contents->IsCrashed());
  }
  page.Set("pdf", !!context.pdf_document_data);

  base::DictValue quality;
  quality.Set("ok", true);

  bool content_available = false;
  bool content_truncated = false;
  base::DictValue content;
  std::optional<apc::AnnotatedPageContent> annotated_page_content;
  if (context.annotated_page_data &&
      context.annotated_page_data->annotated_page_content) {
    annotated_page_content = context.annotated_page_data->annotated_page_content
                                 ->As<apc::AnnotatedPageContent>();
  }

  if (annotated_page_content && annotated_page_content->has_root_node()) {
    std::string document_identifier;
    const apc::FrameData* main_frame_data = nullptr;
    if (annotated_page_content->has_main_frame_data()) {
      main_frame_data = &annotated_page_content->main_frame_data();
      if (main_frame_data->has_document_identifier()) {
        document_identifier =
            main_frame_data->document_identifier().serialized_token();
      }
    }
    int visited_nodes = 0;
    int generated_node_id = 0;
    content = BuildBuaPageSnapshotNode(
        annotated_page_content->root_node(), snapshot_id, document_identifier,
        main_frame_data, max_nodes, &visited_nodes, &generated_node_id,
        &content_truncated, document_identifier_to_short_id,
        short_document_id_to_identifier, next_short_document_id);
    content_available = true;

    if (annotated_page_content->has_viewport_geometry()) {
      const apc::BoundingRect& viewport =
          annotated_page_content->viewport_geometry();
      base::DictValue viewport_value;
      viewport_value.Set("width", viewport.width());
      viewport_value.Set("height", viewport.height());
      page.Set("viewportWidth", viewport.width());
      page.Set("viewportHeight", viewport.height());
    }
  } else {
    content = BuildFallbackContent(snapshot_id, context);
  }

  base::DictValue content_quality;
  content_quality.Set("requested", true);
  content_quality.Set("available", content_available);
  content_quality.Set("truncated", content_truncated);
  if (!content_available) {
    content_quality.Set(
        "error",
        "BUA snapshot backend did not return BuaPageSnapshot content.");
    quality.Set("partial", true);
  }
  quality.Set("content", std::move(content_quality));

  base::DictValue screenshot_quality;
  screenshot_quality.Set("requested", true);
  screenshot_quality.Set("available", !!context.viewport_screenshot);
  if (!context.viewport_screenshot) {
    quality.Set("partial", true);
  }
  quality.Set("screenshot", std::move(screenshot_quality));

  base::DictValue metadata_quality;
  metadata_quality.Set("requested", true);
  metadata_quality.Set("available", true);
  quality.Set("metadata", std::move(metadata_quality));

  base::DictValue pdf_quality;
  pdf_quality.Set("requested", true);
  pdf_quality.Set("available", !!context.pdf_document_data);
  if (context.pdf_document_data) {
    pdf_quality.Set("truncated",
                    context.pdf_document_data->size_limit_exceeded);
  }
  quality.Set("pdf", std::move(pdf_quality));

  base::DictValue snapshot;
  snapshot.Set("id", snapshot_id);
  snapshot.Set("mode", std::move(snapshot_mode));
  snapshot.Set("source", "explicit");
  snapshot.Set("createdAtMs", static_cast<double>(NowMs()));
  snapshot.Set("generation", generation);
  if (context.tab_data) {
    snapshot.Set("target", BuildTargetSnapshotFromTabData(*context.tab_data));
  }
  snapshot.Set("page", std::move(page));

  if (context.web_page_data && context.web_page_data->main_document) {
    base::DictValue text;
    text.Set("innerText",
             context.web_page_data->main_document->inner_text);
    snapshot.Set("text", std::move(text));
  }

  snapshot.Set("content", std::move(content));

  if (annotated_page_content &&
      annotated_page_content->has_viewport_geometry()) {
    const apc::BoundingRect& viewport =
        annotated_page_content->viewport_geometry();
    base::DictValue viewport_value;
    viewport_value.Set("width", viewport.width());
    viewport_value.Set("height", viewport.height());
    snapshot.Set("viewport", std::move(viewport_value));
  } else if (context.viewport_screenshot) {
    base::DictValue viewport_value;
    viewport_value.Set(
        "width", static_cast<int>(context.viewport_screenshot->width_pixels));
    viewport_value.Set(
        "height", static_cast<int>(context.viewport_screenshot->height_pixels));
    snapshot.Set("viewport", std::move(viewport_value));
  }

  if (context.viewport_screenshot) {
    const glic::mojom::Screenshot& screenshot = *context.viewport_screenshot;
    base::DictValue screenshot_value;
    screenshot_value.Set("id", snapshot_id + ":screenshot");
    screenshot_value.Set("width", static_cast<int>(screenshot.width_pixels));
    screenshot_value.Set("height", static_cast<int>(screenshot.height_pixels));
    screenshot_value.Set("mimeType", screenshot.mime_type);
    screenshot_value.Set("uri", "data:" + screenshot.mime_type + ";base64," +
                                    base::Base64Encode(screenshot.data));
    snapshot.Set("screenshot", std::move(screenshot_value));
  }

  snapshot.Set("quality", std::move(quality));
  return snapshot;
}

base::DictValue UnsupportedActionResult(std::string action_id,
                                        int action_index,
                                        std::string action_kind,
                                        std::string message) {
  base::DictValue result;
  result.Set("ok", false);
  result.Set("status", "unsupported");
  result.Set("actionId", std::move(action_id));
  result.Set("failedActionIndex", action_index);
  result.Set("code", "unsupported");
  result.Set("category", "unsupported");
  result.Set("message", std::move(message));
  result.Set("kind", std::move(action_kind));
  return result;
}

std::string BuaActionCodeString(actor::mojom::ActionResultCode code) {
  return "bua_action:" + base::NumberToString(static_cast<int>(code));
}

std::string BuaActionStatus(actor::mojom::ActionResultCode code) {
  switch (code) {
    case actor::mojom::ActionResultCode::kOk:
      return "succeeded";
    case actor::mojom::ActionResultCode::kActionsCancelled:
      return "cancelled";
    case actor::mojom::ActionResultCode::kTaskPaused:
      return "paused";
    default:
      return "failed";
  }
}

std::string BuaActionCategory(actor::mojom::ActionResultCode code) {
  switch (code) {
    case actor::mojom::ActionResultCode::kArgumentsInvalid:
    case actor::mojom::ActionResultCode::kInvalidDomNodeId:
    case actor::mojom::ActionResultCode::kCoordinatesOutOfBounds:
      return "input";
    case actor::mojom::ActionResultCode::kUrlBlocked:
    case actor::mojom::ActionResultCode::kActionsBlockedForSiteRisk:
    case actor::mojom::ActionResultCode::kActionsBlockedSafeBrowsingDisabled:
    case actor::mojom::ActionResultCode::kActionsBlockedByEnterprisePolicy:
    case actor::mojom::ActionResultCode::kActionsBlockedForScheme:
    case actor::mojom::ActionResultCode::kActionBlockedByEnterpriseContentScan:
      return "permission_policy";
    case actor::mojom::ActionResultCode::kTabWentAway:
    case actor::mojom::ActionResultCode::kWindowWentAway:
      return "target_not_found";
    case actor::mojom::ActionResultCode::kTaskWentAway:
    case actor::mojom::ActionResultCode::kInvalidTaskStateForAct:
      return "task_state";
    case actor::mojom::ActionResultCode::kToolTimeout:
      return "timeout";
    case actor::mojom::ActionResultCode::kRendererCrashed:
      return "browser_state";
    case actor::mojom::ActionResultCode::kTriggeredNavigationBlocked:
    case actor::mojom::ActionResultCode::kExternalProtocolNavigationBlocked:
    case actor::mojom::ActionResultCode::kCrossOriginNavigation:
    case actor::mojom::ActionResultCode::kUserNavigatedAway:
      return "navigation";
    default:
      return "backend_error";
  }
}

base::DictValue BuildBuaActionResult(
    const std::vector<std::string>& action_ids,
    base::TimeTicks start_time,
    std::vector<actor::ActionResultWithLatencyInfo> action_results) {
  actor::mojom::ActionResultCode result_code =
      actor::mojom::ActionResultCode::kOk;
  std::optional<size_t> failed_index;
  actor::ExtractErrorResult(action_results, &result_code, failed_index);

  size_t action_index = failed_index.value_or(0);
  if (action_index >= action_ids.size()) {
    action_index = 0;
  }

  base::DictValue result;
  result.Set("ok", actor::IsOk(result_code));
  result.Set("status", BuaActionStatus(result_code));
  result.Set("actionId", action_ids.empty() ? std::string("bua-action")
                                            : action_ids[action_index]);
  result.Set("code", BuaActionCodeString(result_code));
  result.Set("category", BuaActionCategory(result_code));
  if (failed_index) {
    result.Set("failedActionIndex", static_cast<int>(*failed_index));
  }
  if (failed_index && *failed_index < action_results.size() &&
      action_results[*failed_index].result &&
      !action_results[*failed_index].result->message.empty()) {
    result.Set("message", action_results[*failed_index].result->message);
  }

  base::DictValue timing;
  timing.Set("endedAtMs", static_cast<double>(NowMs()));
  timing.Set(
      "elapsedMs",
      static_cast<int>((base::TimeTicks::Now() - start_time).InMilliseconds()));
  base::ListValue phases;
  for (size_t i = 0; i < action_results.size(); ++i) {
    base::DictValue phase;
    phase.Set("name", action_ids.size() > i
                          ? action_ids[i]
                          : base::StringPrintf("action:%zu", i));
    phase.Set("elapsedMs", static_cast<int>((action_results[i].end_time -
                                             action_results[i].start_time)
                                                .InMilliseconds()));
    phases.Append(std::move(phase));
  }
  timing.Set("phases", std::move(phases));
  result.Set("timing", std::move(timing));

  base::DictValue diagnostic;
  diagnostic.Set("severity", actor::IsOk(result_code) ? "info" : "warning");
  diagnostic.Set("category", "actuation");
  diagnostic.Set("message",
                 "BUA actuation backend executed the action sequence.");
  diagnostic.Set("actionResultCode", static_cast<int>(result_code));
  base::ListValue diagnostics;
  diagnostics.Append(std::move(diagnostic));
  result.Set("diagnostics", std::move(diagnostics));

  return result;
}

int ClampWaitMs(int ms) {
  return std::clamp(ms, 0, kMaxWaitTimeoutMs);
}

base::TimeDelta WaitTimeoutFromAction(const base::DictValue& action) {
  return base::Milliseconds(
      ClampWaitMs(action.FindInt("timeoutMs").value_or(kDefaultWaitTimeoutMs)));
}

bool HasSupportedElementTargetShape(const base::DictValue& target) {
  const bool has_node_id = target.FindString("nodeId") != nullptr;
  const bool has_point = target.FindDict("point") != nullptr;
  return has_node_id != has_point;
}

std::optional<BuaWaitSpec> ParseWaitSpec(const base::DictValue& action,
                                         int action_index,
                                         std::string* error_json) {
  const base::DictValue* condition = action.FindDict("condition");
  if (!condition) {
    *error_json =
        Error("invalid_request", "wait action requires condition.", "input");
    return std::nullopt;
  }
  const std::string* type = condition->FindString("type");
  if (!type || type->empty()) {
    *error_json =
        Error("invalid_request", "wait condition requires type.", "input");
    return std::nullopt;
  }

  BuaWaitSpec spec;
  spec.action_id =
      FindStringMember(action, "id")
          .value_or(base::StringPrintf("bua-action-%d", action_index));
  spec.type = *type;
  spec.timeout = WaitTimeoutFromAction(action);
  if (const base::DictValue* target_ref = action.FindDict("targetRef")) {
    spec.target_ref = target_ref->Clone();
  }

  if (spec.type == "time") {
    spec.delay =
        base::Milliseconds(ClampWaitMs(condition->FindInt("ms").value_or(0)));
    return spec;
  }
  if (spec.type == "page_stable") {
    spec.stable_for = base::Milliseconds(ClampWaitMs(
        condition->FindInt("stableForMs").value_or(kDefaultStableForMs)));
    return spec;
  }
  if (spec.type == "url_matches") {
    const std::string* pattern = condition->FindString("pattern");
    if (!pattern || pattern->empty()) {
      *error_json = Error("invalid_request",
                          "url_matches wait requires pattern.", "input");
      return std::nullopt;
    }
    spec.pattern = *pattern;
    return spec;
  }
  if (spec.type == "text_present") {
    const std::string* text = condition->FindString("text");
    if (!text || text->empty()) {
      *error_json =
          Error("invalid_request", "text_present wait requires text.", "input");
      return std::nullopt;
    }
    spec.text = *text;
    return spec;
  }
  if (spec.type == "element_present" || spec.type == "element_absent") {
    const base::DictValue* target = condition->FindDict("target");
    if (!target) {
      *error_json = Error("invalid_request",
                          spec.type + " wait requires target.", "input");
      return std::nullopt;
    }
    if (!HasSupportedElementTargetShape(*target)) {
      *error_json =
          Error("invalid_request",
                spec.type + " wait target requires nodeId or point.", "input");
      return std::nullopt;
    }
    spec.target = target->Clone();
    spec.expect_absent = spec.type == "element_absent";
    return spec;
  }

  *error_json = Error("unsupported", "Unsupported wait condition: " + spec.type,
                      "unsupported");
  return std::nullopt;
}

bool UrlMatchesPattern(std::string_view url, std::string_view pattern) {
  return base::MatchPattern(url, pattern) ||
         url.find(pattern) != std::string_view::npos;
}

bool ContainsSensitive(std::string_view text, std::string_view needle) {
  return text.find(needle) != std::string_view::npos;
}

std::string SnapshotInnerText(const base::DictValue& snapshot) {
  if (const base::DictValue* text = snapshot.FindDict("text")) {
    if (const std::string* inner_text = text->FindString("innerText")) {
      return *inner_text;
    }
  }
  if (const std::string* text = snapshot.FindString("text")) {
    return *text;
  }
  return std::string();
}

bool BoundsContainPoint(const base::DictValue& bounds,
                        const base::DictValue& point) {
  std::optional<double> x = point.FindDouble("x");
  std::optional<double> y = point.FindDouble("y");
  std::optional<double> left = bounds.FindDouble("x");
  std::optional<double> top = bounds.FindDouble("y");
  std::optional<double> width = bounds.FindDouble("width");
  std::optional<double> height = bounds.FindDouble("height");
  if (!x || !y || !left || !top || !width || !height) {
    return false;
  }
  return *x >= *left && *x <= *left + *width && *y >= *top &&
         *y <= *top + *height;
}

bool NodeMatchesTarget(const base::DictValue& node,
                       const base::DictValue& target) {
  if (const std::string* node_id = target.FindString("nodeId")) {
    const std::string* id = node.FindString("id");
    if (id && *id == *node_id) {
      return true;
    }
  }
  if (const base::DictValue* point = target.FindDict("point")) {
    const base::DictValue* bounds = node.FindDict("bounds");
    if (bounds && BoundsContainPoint(*bounds, *point)) {
      return true;
    }
  }

  const base::ListValue* children = node.FindList("children");
  if (!children) {
    return false;
  }
  for (const base::Value& child_value : *children) {
    const base::DictValue* child = child_value.GetIfDict();
    if (child && NodeMatchesTarget(*child, target)) {
      return true;
    }
  }
  return false;
}

bool SnapshotHasTarget(const base::DictValue& snapshot,
                       const base::DictValue& target) {
  const base::DictValue* content = snapshot.FindDict("content");
  return content && NodeMatchesTarget(*content, target);
}

base::DictValue BuildWaitActionResult(const BuaWaitSpec& spec,
                                      base::TimeTicks start_time,
                                      bool ok,
                                      std::string message,
                                      std::optional<base::DictValue> snapshot) {
  base::DictValue result;
  result.Set("ok", ok);
  result.Set("status", ok ? "succeeded" : "timeout");
  result.Set("actionId", spec.action_id);
  result.Set("code", ok ? "bua:wait_satisfied" : "wait_timeout");
  result.Set("category", ok ? "wait" : "timeout");
  result.Set("message", std::move(message));

  base::DictValue timing;
  timing.Set("endedAtMs", static_cast<double>(NowMs()));
  timing.Set(
      "elapsedMs",
      static_cast<int>((base::TimeTicks::Now() - start_time).InMilliseconds()));
  result.Set("timing", std::move(timing));
  if (snapshot) {
    result.Set("snapshot", std::move(*snapshot));
  }
  return result;
}

bool FillPointTarget(const base::DictValue& point,
                     apc::ActionTarget* target,
                     std::string* error_json) {
  std::optional<double> x = point.FindDouble("x");
  std::optional<double> y = point.FindDouble("y");
  if (!x || !y || !std::isfinite(*x) || !std::isfinite(*y)) {
    *error_json =
        Error("invalid_request", "Action point target requires finite x and y.",
              "input");
    return false;
  }
  target->mutable_coordinate()->set_x(static_cast<int>(*x));
  target->mutable_coordinate()->set_y(static_cast<int>(*y));
  return true;
}

std::optional<gfx::PointF> ReadPointTarget(const base::DictValue& point,
                                           std::string* error_json) {
  std::optional<double> x = point.FindDouble("x");
  std::optional<double> y = point.FindDouble("y");
  if (!x || !y || !std::isfinite(*x) || !std::isfinite(*y)) {
    *error_json =
        Error("invalid_request", "Action point target requires finite x and y.",
              "input");
    return std::nullopt;
  }
  return gfx::PointF(static_cast<float>(*x), static_cast<float>(*y));
}

bool FillActionTarget(const base::DictValue& target_dict,
                      apc::ActionTarget* target,
                      tabs::TabInterface* tab,
                      const ShortDocumentIdToIdentifierMap&
                          short_document_id_to_identifier,
                      std::string* error_json) {
  if (const std::string* node_id = target_dict.FindString("nodeId")) {
    if (*node_id == kViewportNodeId) {
      content::RenderFrameHost* render_frame_host =
          tab && tab->GetContents() ? tab->GetContents()->GetPrimaryMainFrame()
                                    : nullptr;
      if (!render_frame_host) {
        *error_json =
            Error("target_not_found",
                  "viewport target requires a live target page main frame.",
                  "target_not_found");
        return false;
      }
      target->set_content_node_id(actor::kRootElementDomNodeId);
      target->mutable_document_identifier()->set_serialized_token(
          optimization_guide::DocumentIdentifierUserData::
              GetOrCreateForCurrentDocument(render_frame_host)
                  ->serialized_token());
      return true;
    }
    std::optional<BuaPageNodeRef> node_ref =
        DecodeBuaPageNodeId(*node_id, short_document_id_to_identifier);
    if (!node_ref) {
      *error_json =
          Error("invalid_request",
                "nodeId is not a BuaPageSnapshot node id from snapshot().",
                "input");
      return false;
    }
    target->set_content_node_id(node_ref->dom_node_id);
    target->mutable_document_identifier()->set_serialized_token(
        node_ref->document_identifier);
    return true;
  }

  if (const base::DictValue* point = target_dict.FindDict("point")) {
    return FillPointTarget(*point, target, error_json);
  }

  if (target_dict.FindDict("query")) {
    *error_json =
        Error("unsupported",
              "target.query is not wired yet. Call snapshot() and prefer "
              "nodeId; provide a point target only as a coordinate fallback.",
              "unsupported");
    return false;
  }

  *error_json = Error("invalid_request",
                      "Action target requires nodeId or point.", "input");
  return false;
}

const base::DictValue* RequiredTarget(const base::DictValue& action,
                                      std::string* error_json) {
  const base::DictValue* target = action.FindDict("target");
  if (!target) {
    *error_json =
        Error("invalid_request", "Action requires a target object.", "input");
    return nullptr;
  }
  return target;
}

std::optional<gfx::PointF> ReadMiddleClickPoint(
    const base::DictValue& action,
    std::string* error_json) {
  const base::DictValue* target = RequiredTarget(action, error_json);
  if (!target) {
    return std::nullopt;
  }

  const base::DictValue* point = target->FindDict("point");
  if (!point) {
    *error_json = Error(
        "invalid_request",
        "middle click requires a point target. Resolve element targets through "
        "snapshot bounds before calling native page.act().",
        "input");
    return std::nullopt;
  }
  return ReadPointTarget(*point, error_json);
}

void ForwardMiddleMouseEvent(content::RenderWidgetHost* render_widget_host,
                             blink::WebInputEvent::Type type,
                             const gfx::PointF& point,
                             int click_count) {
  const int modifiers = blink::WebInputEvent::kFromDebugger |
                        (type == blink::WebInputEvent::Type::kMouseDown
                             ? blink::WebInputEvent::kMiddleButtonDown
                             : blink::WebInputEvent::kNoModifiers);
  blink::WebMouseEvent event(
      type, point, point, blink::WebMouseEvent::Button::kMiddle, click_count,
      modifiers, base::TimeTicks::Now());
  event.UpdateEventModifiersToMatchButton();
  render_widget_host->ForwardMouseEvent(event);
}

void ForwardMiddleClick(content::RenderWidgetHost* render_widget_host,
                        const gfx::PointF& point,
                        int click_count) {
  for (int sequence = 1; sequence <= click_count; ++sequence) {
    ForwardMiddleMouseEvent(render_widget_host,
                            blink::WebInputEvent::Type::kMouseDown, point,
                            sequence);
    ForwardMiddleMouseEvent(render_widget_host,
                            blink::WebInputEvent::Type::kMouseUp, point,
                            sequence);
  }
}

bool AppendBuaActionToProto(const base::DictValue& action,
                            int action_index,
                            tabs::TabInterface* default_tab,
                            content::BrowserContext* browser_context,
                            const ShortDocumentIdToIdentifierMap&
                                short_document_id_to_identifier,
                            content::RenderFrameHost& render_frame_host,
                            apc::Actions* actions_proto,
                            std::vector<std::string>* action_ids,
                            std::string* error_json) {
  const std::string action_id =
      FindStringMember(action, "id")
          .value_or(base::StringPrintf("bua-action-%d", action_index));
  const std::string kind = FindStringMember(action, "kind").value_or("");
  if (kind.empty()) {
    *error_json =
        Error("invalid_request", "Each action requires a kind.", "input");
    return false;
  }

  apc::Action* proto_action = actions_proto->add_actions();
  int32_t tab_id = tabs::TabHandle::Null().raw_value();
  if (kind != "tab") {
    std::optional<int32_t> resolved_tab_id =
        ResolveActionTabId(action, default_tab, browser_context, error_json);
    if (!resolved_tab_id) {
      return false;
    }
    tab_id = *resolved_tab_id;
  }

  if (kind == "click") {
    const base::DictValue* target = RequiredTarget(action, error_json);
    if (!target) {
      return false;
    }
    apc::ClickAction* click = proto_action->mutable_click();
    if (!FillActionTarget(*target, click->mutable_target(), default_tab,
                          short_document_id_to_identifier, error_json)) {
      return false;
    }
    const std::string button =
        FindStringMember(action, "button").value_or("left");
    if (button == "left") {
      click->set_click_type(apc::ClickAction_ClickType_LEFT);
    } else if (button == "right") {
      click->set_click_type(apc::ClickAction_ClickType_RIGHT);
    } else if (button == "middle") {
      *error_json = SuccessDict(UnsupportedActionResult(
          action_id, action_index, kind,
          "middle click must run as a standalone click action."));
      return false;
    } else {
      *error_json = Error("invalid_request",
                          "click.button must be left, middle, or right.",
                          "input");
      return false;
    }
    const int click_count = action.FindInt("clickCount")
                                .value_or(action.FindInt("count").value_or(1));
    click->set_click_count(click_count == 2
                               ? apc::ClickAction_ClickCount_DOUBLE
                               : apc::ClickAction_ClickCount_SINGLE);
    click->set_tab_id(tab_id);
  } else if (kind == "type") {
    const base::DictValue* target = RequiredTarget(action, error_json);
    if (!target) {
      return false;
    }
    const std::string* text = action.FindString("text");
    if (!text) {
      *error_json =
          Error("invalid_request", "type action requires text.", "input");
      return false;
    }
    apc::TypeAction* type = proto_action->mutable_type();
    if (!FillActionTarget(*target, type->mutable_target(), default_tab,
                          short_document_id_to_identifier, error_json)) {
      return false;
    }
    type->set_text(*text);
    std::string mode = FindStringMember(action, "mode").value_or("replace");
    if (std::optional<bool> replace = action.FindBool("replace")) {
      mode = *replace ? "replace" : "append";
    }
    if (mode == "append") {
      type->set_mode(apc::TypeAction_TypeMode_APPEND);
    } else if (mode == "prepend") {
      type->set_mode(apc::TypeAction_TypeMode_PREPEND);
    } else {
      type->set_mode(apc::TypeAction_TypeMode_DELETE_EXISTING);
    }
    type->set_follow_by_enter(action.FindBool("submit").value_or(false));
    type->set_tab_id(tab_id);
  } else if (kind == "select") {
    const base::DictValue* target = RequiredTarget(action, error_json);
    if (!target) {
      return false;
    }
    const std::string* value = action.FindString("value");
    const base::ListValue* values = action.FindList("values");
    const std::string* first_value =
        values && !values->empty() ? (*values)[0].GetIfString() : nullptr;
    if (!value && !first_value) {
      *error_json =
          Error("invalid_request", "select action requires values.", "input");
      return false;
    }
    apc::SelectAction* select = proto_action->mutable_select();
    if (!FillActionTarget(*target, select->mutable_target(), default_tab,
                          short_document_id_to_identifier, error_json)) {
      return false;
    }
    select->set_value(value ? *value : *first_value);
    select->set_tab_id(tab_id);
  } else if (kind == "scroll") {
    apc::ScrollAction* scroll = proto_action->mutable_scroll();
    if (const base::DictValue* target = action.FindDict("target")) {
      if (!FillActionTarget(*target, scroll->mutable_target(), default_tab,
                            short_document_id_to_identifier, error_json)) {
        return false;
      }
    }
    std::string direction = FindStringMember(action, "direction").value_or("");
    if (direction.empty()) {
      const double delta_x = action.FindDouble("deltaX").value_or(0.0);
      const double delta_y = action.FindDouble("deltaY").value_or(0.0);
      if (std::abs(delta_x) > std::abs(delta_y)) {
        direction = delta_x < 0 ? "left" : "right";
      } else {
        direction = delta_y < 0 ? "up" : "down";
      }
    }
    if (direction == "up") {
      scroll->set_direction(apc::ScrollAction_ScrollDirection_UP);
    } else if (direction == "left") {
      scroll->set_direction(apc::ScrollAction_ScrollDirection_LEFT);
    } else if (direction == "right") {
      scroll->set_direction(apc::ScrollAction_ScrollDirection_RIGHT);
    } else {
      scroll->set_direction(apc::ScrollAction_ScrollDirection_DOWN);
    }
    const double distance = action.FindDouble("amount").value_or(
        action.FindDouble("distance")
            .value_or(
                std::max(std::abs(action.FindDouble("deltaX").value_or(0.0)),
                         std::abs(action.FindDouble("deltaY").value_or(0.0)))));
    scroll->set_distance(static_cast<float>(distance > 0 ? distance : 600.0));
    scroll->set_tab_id(tab_id);
  } else if (kind == "scroll_to") {
    const base::DictValue* target = RequiredTarget(action, error_json);
    if (!target) {
      return false;
    }
    apc::ScrollToAction* scroll_to = proto_action->mutable_scroll_to();
    if (!FillActionTarget(*target, scroll_to->mutable_target(), default_tab,
                          short_document_id_to_identifier, error_json)) {
      return false;
    }
    scroll_to->set_tab_id(tab_id);
  } else if (kind == "move_mouse" || kind == "hover") {
    const base::DictValue* target = RequiredTarget(action, error_json);
    if (!target) {
      return false;
    }
    apc::MoveMouseAction* move_mouse = proto_action->mutable_move_mouse();
    if (!FillActionTarget(*target, move_mouse->mutable_target(), default_tab,
                          short_document_id_to_identifier, error_json)) {
      return false;
    }
    move_mouse->set_tab_id(tab_id);
  } else if (kind == "drag") {
    const base::DictValue* from = action.FindDict("from");
    const base::DictValue* to = action.FindDict("to");
    if (!from || !to) {
      *error_json =
          Error("invalid_request", "drag action requires from and to targets.",
                "input");
      return false;
    }
    apc::DragAndReleaseAction* drag =
        proto_action->mutable_drag_and_release();
    if (!FillActionTarget(*from, drag->mutable_from_target(), default_tab,
                          short_document_id_to_identifier, error_json) ||
        !FillActionTarget(*to, drag->mutable_to_target(), default_tab,
                          short_document_id_to_identifier, error_json)) {
      return false;
    }
    drag->set_tab_id(tab_id);
  } else if (kind == "navigate") {
    const std::string* url_string = action.FindString("url");
    if (!url_string) {
      *error_json =
          Error("invalid_request", "navigate action requires url.", "input");
      return false;
    }
    const GURL url(*url_string);
    if (!url.is_valid() || !url.SchemeIsHTTPOrHTTPS()) {
      *error_json = Error("invalid_request",
                          "navigate action only accepts valid HTTP(S) URLs.",
                          "navigation");
      return false;
    }
    apc::NavigateAction* navigate = proto_action->mutable_navigate();
    navigate->set_url(url.spec());
    navigate->set_tab_id(tab_id);
  } else if (kind == "history") {
    const std::string direction =
        FindStringMember(action, "direction").value_or("");
    if (direction == "back") {
      proto_action->mutable_back()->set_tab_id(tab_id);
    } else if (direction == "forward") {
      proto_action->mutable_forward()->set_tab_id(tab_id);
    } else {
      *error_json = SuccessDict(UnsupportedActionResult(
          action_id, action_index, kind,
          "history.reload must run as a standalone history action."));
      return false;
    }
  } else if (kind == "wait") {
    apc::WaitAction* wait = proto_action->mutable_wait();
    int wait_ms = action.FindInt("waitMs").value_or(0);
    if (const base::DictValue* condition = action.FindDict("condition")) {
      if (const std::string* type = condition->FindString("type")) {
        if (*type == "time") {
          wait_ms = condition->FindInt("ms").value_or(wait_ms);
        } else {
          *error_json = SuccessDict(UnsupportedActionResult(
              action_id, action_index, kind,
              "condition waits must run as a standalone wait action."));
          return false;
        }
      }
    }
    if (wait_ms > 0) {
      wait->set_wait_time_ms(wait_ms);
    }
    wait->set_observe_tab_id(tab_id);
  } else if (kind == "tab") {
    const std::string operation =
        FindStringMember(action, "operation").value_or("");
    if (operation == "activate") {
      const base::DictValue* target_ref = action.FindDict("targetRef");
      const std::string* target_tab_id =
          target_ref ? target_ref->FindString("tabId") : nullptr;
      std::optional<int32_t> parsed_tab_id =
          target_tab_id ? ParseTabId(*target_tab_id) : std::nullopt;
      if (!parsed_tab_id) {
        *error_json = Error("invalid_request",
                            "tab activate requires targetRef.tabId.", "input");
        return false;
      }
      proto_action->mutable_activate_tab()->set_tab_id(*parsed_tab_id);
    } else if (operation == "close") {
      const base::DictValue* target_ref = action.FindDict("targetRef");
      const std::string* target_tab_id =
          target_ref ? target_ref->FindString("tabId") : nullptr;
      std::optional<int32_t> parsed_tab_id =
          target_tab_id ? ParseTabId(*target_tab_id) : std::nullopt;
      if (!parsed_tab_id) {
        *error_json = Error("invalid_request",
                            "tab close requires targetRef.tabId.", "input");
        return false;
      }
      proto_action->mutable_close_tab()->set_tab_id(*parsed_tab_id);
    } else {
      *error_json = SuccessDict(UnsupportedActionResult(
          action_id, action_index, kind,
          "Create tabs through the BuaTabs.create() API."));
      return false;
    }
  } else {
    *error_json = SuccessDict(UnsupportedActionResult(
        action_id, action_index, kind,
        "BUA action backend is unavailable in this build."));
    return false;
  }

  action_ids->push_back(action_id);
  return true;
}

base::DictValue BuildTaskState(const std::string& session_id,
                               bool has_task,
                               std::string status,
                               std::string reason = std::string()) {
  base::DictValue state;
  state.Set("id", session_id);
  state.Set("status", std::move(status));
  state.Set("updatedAtMs", static_cast<double>(NowMs()));
  if (!reason.empty()) {
    state.Set("reason", std::move(reason));
  }
  if (!has_task) {
    state.Set("startedAtMs", static_cast<double>(NowMs()));
  }
  return state;
}

std::string BuaTaskStatus(actor::ActorTask::State state) {
  switch (state) {
    case actor::ActorTask::State::kCreated:
      return "idle";
    case actor::ActorTask::State::kReflecting:
    case actor::ActorTask::State::kActing:
      return "running";
    case actor::ActorTask::State::kWaitingOnUser:
    case actor::ActorTask::State::kPausedByActor:
    case actor::ActorTask::State::kPausedByUser:
      return "paused";
    case actor::ActorTask::State::kCancelled:
    case actor::ActorTask::State::kFinished:
    case actor::ActorTask::State::kFailed:
      return "stopped";
  }
  return "idle";
}

actor::ActorTask::StoppedReason BuaStoppedReason(std::string_view reason) {
  if (reason == "completed") {
    return actor::ActorTask::StoppedReason::kTaskComplete;
  }
  if (reason == "cancelled" || reason == "user_requested") {
    return actor::ActorTask::StoppedReason::kStoppedByUser;
  }
  if (reason == "session_closed") {
    return actor::ActorTask::StoppedReason::kShutdown;
  }
  return actor::ActorTask::StoppedReason::kModelError;
}

base::DictValue BuildPageSnapshotRequest(const base::DictValue& request,
                                         bool screenshot_only) {
  const base::DictValue* input_options = request.FindDict("options");

  base::DictValue channels;
  channels.Set("content", !screenshot_only);
  channels.Set(
      "screenshot",
      screenshot_only ||
          (input_options
               ? input_options->FindBool("includeScreenshot").value_or(false)
               : false));
  channels.Set("metadata", true);

  base::DictValue budget;
  if (input_options) {
    if (std::optional<int> max_nodes = input_options->FindInt("maxNodes")) {
      budget.Set("maxNodes", *max_nodes);
    }
  }

  base::DictValue options;
  if (input_options) {
    if (const std::string* purpose = input_options->FindString("purpose")) {
      options.Set("purpose", *purpose);
    }
  }
  options.Set("channels", std::move(channels));
  options.Set("budget", std::move(budget));

  base::DictValue snapshot_request;
  if (const std::string* session_id = request.FindString("sessionId")) {
    snapshot_request.Set("sessionId", *session_id);
  }
  snapshot_request.Set("options", std::move(options));
  return snapshot_request;
}

base::DictValue BuildNavigateActRequest(const base::DictValue& request) {
  base::DictValue action;
  action.Set("kind", "navigate");
  if (const std::string* url = request.FindString("url")) {
    action.Set("url", *url);
  }

  base::ListValue actions;
  actions.Append(std::move(action));

  base::DictValue options;
  options.Set("snapshotAfter", "auto");
  options.Set("stopOnFirstError", true);

  base::DictValue act_request;
  if (const std::string* session_id = request.FindString("sessionId")) {
    act_request.Set("sessionId", *session_id);
  }
  act_request.Set("actions", std::move(actions));
  act_request.Set("options", std::move(options));
  return act_request;
}

glic::mojom::GetTabContextOptions BuildSnapshotOptions(
    const base::DictValue& request) {
  const base::DictValue* options = request.FindDict("options");
  const base::DictValue* channels =
      options ? options->FindDict("channels") : nullptr;
  const base::DictValue* budget =
      options ? options->FindDict("budget") : nullptr;

  const bool include_content =
      channels ? channels->FindBool("content").value_or(true) : true;
  const bool include_screenshot =
      channels ? channels->FindBool("screenshot").value_or(true) : true;
  const bool include_pdf =
      channels ? channels->FindBool("pdf").value_or(false) : false;

  glic::mojom::GetTabContextOptions context_options;
  context_options.include_inner_text = true;
  context_options.inner_text_bytes_limit = static_cast<uint32_t>(
      std::max(1, budget ? budget->FindInt("maxTextBytes")
                               .value_or(kDefaultInnerTextBytesLimit)
                         : kDefaultInnerTextBytesLimit));
  context_options.include_viewport_screenshot = include_screenshot;
  context_options.include_annotated_page_content = include_content;
  context_options.max_meta_tags = 32;
  context_options.include_pdf = include_pdf;
  context_options.pdf_size_limit = static_cast<uint32_t>(
      budget ? budget->FindInt("maxBytes").value_or(kDefaultPdfBytesLimit)
             : kDefaultPdfBytesLimit);
  context_options.annotated_page_content_mode =
      ApcModeForSnapshotMode(SnapshotModeFromRequest(request));

  if (budget) {
    int max_width = budget->FindInt("maxScreenshotWidth").value_or(0);
    int max_height = budget->FindInt("maxScreenshotHeight").value_or(0);
    if (max_width > 0) {
      context_options.screenshot_collection_options.max_width = max_width;
    }
    if (max_height > 0) {
      context_options.screenshot_collection_options.max_height = max_height;
    }
  }
  context_options.screenshot_collection_options.screenshot_image_format =
      page_content_annotations::ScreenshotOptions::ScreenshotImageFormat::kJpeg;
  context_options.screenshot_collection_options.screenshot_compression_quality =
      page_content_annotations::ScreenshotOptions::
          ScreenshotCompressionQuality::kMedium;
  return context_options;
}

int SnapshotMaxNodes(const base::DictValue& request) {
  const base::DictValue* options = request.FindDict("options");
  const base::DictValue* budget =
      options ? options->FindDict("budget") : nullptr;
  return std::max(
      1, budget ? budget->FindInt("maxNodes").value_or(kDefaultMaxNodes)
                : kDefaultMaxNodes);
}

}  // namespace

// static
void BuaDocumentService::Create(
    content::RenderFrameHost* render_frame_host,
    mojo::PendingReceiver<mojom::BuaHost> receiver) {
  if (!render_frame_host || !IsAllowedFrame(*render_frame_host)) {
    return;
  }

  new BuaDocumentService(*render_frame_host, std::move(receiver));
}

BuaDocumentService::BuaDocumentService(
    content::RenderFrameHost& render_frame_host,
    mojo::PendingReceiver<mojom::BuaHost> receiver)
    : DocumentService(render_frame_host, std::move(receiver)),
      session_id_(std::string("bua-session-") + base::NumberToString(NowMs())) {
}

BuaDocumentService::~BuaDocumentService() {
  StopActorTask(actor::ActorTask::StoppedReason::kShutdown);
}

bool BuaDocumentService::IsRequestAllowed() const {
  return IsAllowedFrame(render_frame_host());
}

content::WebContents* BuaDocumentService::GetWebContents() const {
  return content::WebContents::FromRenderFrameHost(&render_frame_host());
}

tabs::TabInterface* BuaDocumentService::GetRequestingTab() const {
  content::WebContents* web_contents = GetWebContents();
  return web_contents ? tabs::TabInterface::MaybeGetFromContents(web_contents)
                      : nullptr;
}

tabs::TabInterface* BuaDocumentService::GetDefaultTargetTab() const {
  content::BrowserContext* browser_context =
      render_frame_host().GetBrowserContext();
  content::WebContents* web_contents = GetWebContents();
  tabs::TabInterface* requesting_tab = GetRequestingTab();
  if (IsUsableDefaultTargetTab(requesting_tab, browser_context, web_contents)) {
    return requesting_tab;
  }

  return FindDefaultTabForProfile(browser_context, web_contents);
}

glic::GlicKeyedService* BuaDocumentService::GetGlicService() const {
  return glic::GlicKeyedServiceFactory::GetGlicKeyedService(
      render_frame_host().GetBrowserContext());
}

actor::ActorKeyedService* BuaDocumentService::GetActorService() const {
  return actor::ActorKeyedService::Get(render_frame_host().GetBrowserContext());
}

std::optional<actor::TaskId> BuaDocumentService::EnsureActorTask(
    std::string* error_json) {
  actor::ActorKeyedService* actor_service = GetActorService();
  if (!actor_service) {
    *error_json =
        Error("backend_unavailable",
              "BUA actuation backend is unavailable before using act().",
              "backend_error");
    return std::nullopt;
  }

  if (actor_task_id_ && actor_service->GetTask(*actor_task_id_)) {
    return actor_task_id_;
  }

  glic::GlicKeyedService* glic_service = GetGlicService();
  if (!glic_service || !glic_service->HasActorPolicyChecker()) {
    *error_json =
        Error("backend_unavailable",
              "BUA actuation policy checker is unavailable, so BUA cannot "
              "create a real browser-use task.",
              "backend_error");
    return std::nullopt;
  }

  glic::GlicActorPolicyChecker& policy_checker =
      glic_service->actor_policy_checker();
  if (!policy_checker.CanActOnWeb()) {
    *error_json =
        Error("act_blocked_by_policy",
              "BUA actuation policy denied act-on-web.",
              "permission_policy");
    return std::nullopt;
  }

  actor_task_id_ = actor_service->CreateTask(
      actor::TaskSourceInfo(actor::TaskSourceInfo::Client::kExperimentalActor,
                            session_id_),
      &policy_checker);
  if (!actor_task_id_ || actor_task_id_->is_null()) {
    *error_json =
        Error("create_task_failed",
              "BUA actuation backend did not create a task.",
              "task_state");
    actor_task_id_.reset();
    return std::nullopt;
  }

  return actor_task_id_;
}

void BuaDocumentService::StopActorTask(actor::ActorTask::StoppedReason reason) {
  if (!actor_task_id_) {
    return;
  }
  if (actor::ActorKeyedService* actor_service = GetActorService()) {
    if (actor_service->GetTask(*actor_task_id_)) {
      actor_service->StopTask(*actor_task_id_, reason);
    }
  }
  actor_task_id_.reset();
}

void BuaDocumentService::HandleSnapshot(const base::DictValue& request,
                                        RequestCallback callback) {
  content::BrowserContext* browser_context =
      render_frame_host().GetBrowserContext();
  tabs::TabInterface* tab = GetDefaultTargetTab();

  std::string error_json;
  tab = ResolveTargetTabOrDefault(request, tab, browser_context, &error_json);
  if (!tab) {
    std::move(callback).Run(
        error_json.empty() ? NoDefaultTargetError(browser_context, "snapshot()")
                           : std::move(error_json));
    return;
  }

  const std::string snapshot_id =
      std::string("snapshot-") + base::NumberToString(NowMs());
  const std::string snapshot_mode = SnapshotModeFromRequest(request);
  const int generation = ++snapshot_generation_;
  glic::FetchPageContext(
      tab, BuildSnapshotOptions(request),
      base::BindOnce(&BuaDocumentService::OnSnapshot,
                     weak_ptr_factory_.GetWeakPtr(), std::move(callback),
                     snapshot_id, snapshot_mode, generation,
                     SnapshotMaxNodes(request)),
      /*progress_listener=*/nullptr,
      /*is_screenshot_annotated=*/false);
}

void BuaDocumentService::OnSnapshot(
    RequestCallback callback,
    std::string snapshot_id,
    std::string snapshot_mode,
    int generation,
    int max_nodes,
    base::expected<glic::mojom::GetContextResultPtr,
                   page_content_annotations::FetchPageContextErrorDetails>
        result) {
  if (!result.has_value()) {
    std::move(callback).Run(
        Error("snapshot_failed", result.error().message, "backend_error"));
    return;
  }

  if (!result.value()->is_tab_context()) {
    std::move(callback).Run(Error("snapshot_failed",
                                  result.value()->is_error_reason()
                                      ? result.value()->get_error_reason()
                                      : "BUA snapshot backend did not return "
                                        "page context.",
                                  "backend_error"));
    return;
  }

  std::move(callback).Run(SuccessDict(BuildSnapshotFromTabContext(
      std::move(snapshot_id), std::move(snapshot_mode), generation,
      *result.value()->get_tab_context(), /*web_contents=*/nullptr,
      max_nodes, &document_identifier_to_short_id_,
      &short_document_id_to_identifier_, &next_short_document_id_)));
}

void BuaDocumentService::HandleWaitAction(const base::DictValue& action,
                                          RequestCallback callback) {
  std::string error_json;
  std::optional<BuaWaitSpec> spec =
      ParseWaitSpec(action, /*action_index=*/0, &error_json);
  if (!spec) {
    std::move(callback).Run(std::move(error_json));
    return;
  }

  const base::TimeTicks start_time = base::TimeTicks::Now();
  if (spec->type == "time") {
    base::SingleThreadTaskRunner::GetCurrentDefault()->PostDelayedTask(
        FROM_HERE,
        base::BindOnce(
            [](base::WeakPtr<BuaDocumentService> service,
               RequestCallback callback, BuaWaitSpec spec,
               base::TimeTicks start_time) {
              if (!service) {
                return;
              }
              std::move(callback).Run(SuccessDict(
                  BuildWaitActionResult(spec, start_time, /*ok=*/true,
                                        "Wait time elapsed.", std::nullopt)));
            },
            weak_ptr_factory_.GetWeakPtr(), std::move(callback),
            std::move(*spec), start_time),
        spec->delay);
    return;
  }

  const base::TimeTicks deadline = start_time + spec->timeout;
  PollWaitCondition(std::move(*spec), std::move(callback), start_time, deadline,
                    base::TimeTicks());
}

void BuaDocumentService::HandleReloadAction(const base::DictValue& action,
                                            tabs::TabInterface* default_tab,
                                            RequestCallback callback) {
  content::BrowserContext* browser_context =
      render_frame_host().GetBrowserContext();
  tabs::TabInterface* tab = default_tab;
  std::string error_json;
  if (const base::DictValue* target_ref = action.FindDict("targetRef")) {
    tab =
        ResolveTargetRef(*target_ref, default_tab, browser_context, &error_json);
  }
  if (!tab) {
    std::move(callback).Run(
        error_json.empty() ? NoDefaultTargetError(browser_context, "reload")
                           : std::move(error_json));
    return;
  }

  content::WebContents* contents = tab->GetContents();
  if (!contents) {
    std::move(callback).Run(
        Error("target_not_found",
              "reload requires a live WebContents for the selected tab.",
              "target_not_found"));
    return;
  }

  const bool ignore_cache = action.FindBool("ignoreCache").value_or(false);
  const base::TimeTicks start_time = base::TimeTicks::Now();
  contents->GetController().Reload(
      ignore_cache ? content::ReloadType::BYPASSING_CACHE
                   : content::ReloadType::NORMAL,
      /*check_for_repost=*/true);
  std::move(callback).Run(SuccessDict(
      BuildReloadActionResult(action, tab, start_time, ignore_cache)));
}

void BuaDocumentService::HandleMiddleClickAction(
    const base::DictValue& action,
    tabs::TabInterface* default_tab,
    RequestCallback callback) {
  std::string error_json;
  std::optional<gfx::PointF> point =
      ReadMiddleClickPoint(action, &error_json);
  if (!point) {
    std::move(callback).Run(std::move(error_json));
    return;
  }

  const std::string button =
      FindStringMember(action, "button").value_or("left");
  if (button != "middle") {
    std::move(callback).Run(
        Error("invalid_request",
              "HandleMiddleClickAction requires button=middle.", "input"));
    return;
  }

  content::BrowserContext* browser_context =
      render_frame_host().GetBrowserContext();
  tabs::TabInterface* tab = default_tab;
  if (const base::DictValue* target_ref = action.FindDict("targetRef")) {
    tab =
        ResolveTargetRef(*target_ref, default_tab, browser_context, &error_json);
  }
  if (!tab) {
    std::move(callback).Run(
        error_json.empty() ? NoDefaultTargetError(browser_context,
                                                  "middle click")
                           : std::move(error_json));
    return;
  }

  content::WebContents* contents = tab->GetContents();
  if (!contents || !contents->GetPrimaryMainFrame()) {
    std::move(callback).Run(Error(
        "target_not_found",
        "middle click requires a live WebContents for the selected tab.",
        "target_not_found"));
    return;
  }

  content::RenderWidgetHost* render_widget_host =
      contents->GetPrimaryMainFrame()->GetRenderWidgetHost();
  if (!render_widget_host) {
    std::move(callback).Run(Error(
        "target_not_found",
        "middle click requires a live render widget for the selected tab.",
        "target_not_found"));
    return;
  }

  const int requested_click_count = action.FindInt("clickCount").value_or(
      action.FindInt("count").value_or(1));
  if (requested_click_count < 1) {
    std::move(callback).Run(Error("invalid_request",
                                  "clickCount must be greater than zero.",
                                  "input"));
    return;
  }
  const int click_count = requested_click_count == 2 ? 2 : 1;

  const base::TimeTicks start_time = base::TimeTicks::Now();
  render_widget_host->Focus();
  ForwardMiddleClick(render_widget_host, *point, click_count);
  std::move(callback).Run(SuccessDict(
      BuildMiddleClickActionResult(action, tab, start_time, *point,
                                   click_count)));
}

void BuaDocumentService::PollWaitCondition(BuaWaitSpec spec,
                                           RequestCallback callback,
                                           base::TimeTicks start_time,
                                           base::TimeTicks deadline,
                                           base::TimeTicks stable_since) {
  const base::TimeTicks now = base::TimeTicks::Now();
  if (now >= deadline) {
    std::move(callback).Run(SuccessDict(
        BuildWaitActionResult(spec, start_time, /*ok=*/false,
                              "Wait condition timed out.", std::nullopt)));
    return;
  }

  content::BrowserContext* browser_context =
      render_frame_host().GetBrowserContext();
  tabs::TabInterface* tab = GetDefaultTargetTab();
  std::string error_json;
  if (spec.target_ref) {
    tab = ResolveTargetRef(*spec.target_ref, tab, browser_context, &error_json);
  }
  if (!tab) {
    std::move(callback).Run(error_json.empty()
                                ? NoDefaultTargetError(browser_context, "wait")
                                : std::move(error_json));
    return;
  }

  if (spec.type == "url_matches") {
    if (UrlMatchesPattern(tab->GetURL().spec(), spec.pattern)) {
      std::move(callback).Run(SuccessDict(
          BuildWaitActionResult(spec, start_time, /*ok=*/true,
                                "URL matched wait pattern.", std::nullopt)));
      return;
    }
    base::SingleThreadTaskRunner::GetCurrentDefault()->PostDelayedTask(
        FROM_HERE,
        base::BindOnce(&BuaDocumentService::PollWaitCondition,
                       weak_ptr_factory_.GetWeakPtr(), std::move(spec),
                       std::move(callback), start_time, deadline,
                       base::TimeTicks()),
        base::Milliseconds(kWaitPollIntervalMs));
    return;
  }

  if (spec.type == "page_stable") {
    content::WebContents* contents = tab->GetContents();
    const bool is_stable = contents && !contents->IsLoading();
    base::TimeTicks next_stable_since = stable_since;
    if (is_stable && stable_since.is_null()) {
      next_stable_since = now;
    } else if (!is_stable) {
      next_stable_since = base::TimeTicks();
    }
    if (is_stable && !next_stable_since.is_null() &&
        now - next_stable_since >= spec.stable_for) {
      std::move(callback).Run(SuccessDict(
          BuildWaitActionResult(spec, start_time, /*ok=*/true,
                                "Page remained stable.", std::nullopt)));
      return;
    }
    base::SingleThreadTaskRunner::GetCurrentDefault()->PostDelayedTask(
        FROM_HERE,
        base::BindOnce(&BuaDocumentService::PollWaitCondition,
                       weak_ptr_factory_.GetWeakPtr(), std::move(spec),
                       std::move(callback), start_time, deadline,
                       next_stable_since),
        base::Milliseconds(kWaitPollIntervalMs));
    return;
  }

  base::DictValue snapshot_request;
  base::DictValue options;
  options.Set("mode", "interact");
  base::DictValue channels;
  channels.Set("content", true);
  channels.Set("screenshot", false);
  channels.Set("pdf", false);
  options.Set("channels", std::move(channels));
  base::DictValue budget;
  budget.Set("maxNodes", kDefaultMaxNodes);
  budget.Set("maxTextBytes", kDefaultInnerTextBytesLimit);
  options.Set("budget", std::move(budget));
  snapshot_request.Set("options", std::move(options));

  glic::FetchPageContext(
      tab, BuildSnapshotOptions(snapshot_request),
      base::BindOnce(&BuaDocumentService::OnWaitSnapshot,
                     weak_ptr_factory_.GetWeakPtr(), std::move(spec),
                     std::move(callback), start_time, deadline, stable_since),
      /*progress_listener=*/nullptr,
      /*is_screenshot_annotated=*/false);
}

void BuaDocumentService::OnWaitSnapshot(
    BuaWaitSpec spec,
    RequestCallback callback,
    base::TimeTicks start_time,
    base::TimeTicks deadline,
    base::TimeTicks stable_since,
    base::expected<glic::mojom::GetContextResultPtr,
                   page_content_annotations::FetchPageContextErrorDetails>
        result) {
  if (!result.has_value()) {
    std::move(callback).Run(
        Error("snapshot_failed", result.error().message, "backend_error"));
    return;
  }

  if (!result.value()->is_tab_context()) {
    std::move(callback).Run(Error("snapshot_failed",
                                  result.value()->is_error_reason()
                                      ? result.value()->get_error_reason()
                                      : "BUA wait snapshot backend did not "
                                        "return page context.",
                                  "backend_error"));
    return;
  }

  base::DictValue snapshot = BuildSnapshotFromTabContext(
      std::string("wait-snapshot-") + base::NumberToString(NowMs()), "interact",
      ++snapshot_generation_, *result.value()->get_tab_context(),
      /*web_contents=*/nullptr, kDefaultMaxNodes,
      &document_identifier_to_short_id_, &short_document_id_to_identifier_,
      &next_short_document_id_);

  bool satisfied = false;
  if (spec.type == "text_present") {
    satisfied = ContainsSensitive(SnapshotInnerText(snapshot), spec.text);
  } else if ((spec.type == "element_present" ||
              spec.type == "element_absent") &&
             spec.target) {
    const bool present = SnapshotHasTarget(snapshot, *spec.target);
    satisfied = spec.expect_absent ? !present : present;
  }

  if (satisfied) {
    std::move(callback).Run(SuccessDict(BuildWaitActionResult(
        spec, start_time, /*ok=*/true, "Wait condition was satisfied.",
        std::move(snapshot))));
    return;
  }

  if (base::TimeTicks::Now() >= deadline) {
    std::move(callback).Run(SuccessDict(BuildWaitActionResult(
        spec, start_time, /*ok=*/false, "Wait condition timed out.",
        std::move(snapshot))));
    return;
  }

  base::SingleThreadTaskRunner::GetCurrentDefault()->PostDelayedTask(
      FROM_HERE,
      base::BindOnce(&BuaDocumentService::PollWaitCondition,
                     weak_ptr_factory_.GetWeakPtr(), std::move(spec),
                     std::move(callback), start_time, deadline, stable_since),
      base::Milliseconds(kWaitPollIntervalMs));
}

void BuaDocumentService::HandleAct(const base::DictValue& request,
                                   RequestCallback callback) {
  content::BrowserContext* browser_context =
      render_frame_host().GetBrowserContext();
  tabs::TabInterface* tab = GetDefaultTargetTab();

  std::string error_json;
  tab = ResolveTargetTabOrDefault(request, tab, browser_context, &error_json);
  if (!tab && !error_json.empty()) {
    std::move(callback).Run(std::move(error_json));
    return;
  }

  const base::ListValue* actions = request.FindList("actions");
  if (!actions || actions->empty()) {
    std::move(callback).Run(Error(
        "invalid_request", "act requires a non-empty actions list.", "input"));
    return;
  }

  if (actions->size() == 1) {
    const base::DictValue* action = (*actions)[0].GetIfDict();
    if (!action) {
      std::move(callback).Run(
          Error("invalid_request", "Each action must be an object.", "input"));
      return;
    }
    if (FindStringMember(*action, "kind").value_or("") == "wait") {
      HandleWaitAction(*action, std::move(callback));
      return;
    }
    if (FindStringMember(*action, "kind").value_or("") == "history" &&
        FindStringMember(*action, "direction").value_or("") == "reload") {
      HandleReloadAction(*action, tab, std::move(callback));
      return;
    }
    if (FindStringMember(*action, "kind").value_or("") == "click" &&
        FindStringMember(*action, "button").value_or("left") == "middle") {
      HandleMiddleClickAction(*action, tab, std::move(callback));
      return;
    }
  }

  if (!tab && actions->size() == 1) {
    const base::DictValue* action = (*actions)[0].GetIfDict();
    if (!action) {
      std::move(callback).Run(
          Error("invalid_request", "Each action must be an object.", "input"));
      return;
    }
    if (NavigateActionCanCreateTab(*action)) {
      std::optional<GURL> url = NavigateUrlFromAction(*action, &error_json);
      if (!url) {
        std::move(callback).Run(std::move(error_json));
        return;
      }
      const base::TimeTicks start_time = base::TimeTicks::Now();
      tabs::TabInterface* created_tab =
          CreateTabForProfile(browser_context, *url, /*background=*/false,
                              std::nullopt, &error_json);
      if (!created_tab) {
        std::move(callback).Run(std::move(error_json));
        return;
      }
      std::move(callback).Run(SuccessDict(
          BuildCreatedTabNavigateResult(*action, created_tab, start_time)));
      return;
    }
  }
  if (!tab) {
    std::move(callback).Run(NoDefaultTargetError(browser_context, "Action"));
    return;
  }

  std::optional<actor::TaskId> task_id = EnsureActorTask(&error_json);
  if (!task_id) {
    std::move(callback).Run(std::move(error_json));
    return;
  }

  apc::Actions actions_proto;
  actions_proto.set_task_id(task_id->value());
  actions_proto.set_skip_async_observation_collection(true);

  std::vector<std::string> action_ids;
  int action_index = 0;
  for (const base::Value& action_value : *actions) {
    const base::DictValue* action = action_value.GetIfDict();
    if (!action) {
      std::move(callback).Run(
          Error("invalid_request", "Each action must be an object.", "input"));
      return;
    }
    if (!AppendBuaActionToProto(*action, action_index, tab, browser_context,
                                short_document_id_to_identifier_,
                                render_frame_host(), &actions_proto,
                                &action_ids, &error_json)) {
      std::move(callback).Run(std::move(error_json));
      return;
    }
    ++action_index;
  }

  actor::BuildToolRequestResult tool_requests =
      actor::BuildToolRequest(actions_proto);
  if (!tool_requests.has_value()) {
    const auto& error = tool_requests.error();
    base::DictValue result;
    result.Set("ok", false);
    result.Set("status", "failed");
    result.Set("actionId", action_ids.size() > error.first
                               ? action_ids[error.first]
                               : "bua-action");
    result.Set("failedActionIndex", static_cast<int>(error.first));
    result.Set("code", BuaActionCodeString(error.second));
    result.Set("category", BuaActionCategory(error.second));
    result.Set("message", "BUA actuation backend rejected the action proto.");
    std::move(callback).Run(SuccessDict(std::move(result)));
    return;
  }

  actor::ActorKeyedService* actor_service = GetActorService();
  if (!actor_service) {
    std::move(callback).Run(Error("backend_unavailable",
                                  "BUA actuation backend went away.",
                                  "backend_error"));
    return;
  }

  const base::TimeTicks start_time = base::TimeTicks::Now();
  actor_service->PerformActions(
      *task_id, std::move(tool_requests.value()),
      actor::ActorTaskMetadata(actions_proto),
      base::BindOnce(&BuaDocumentService::OnActionsFinished,
                     weak_ptr_factory_.GetWeakPtr(), std::move(callback),
                     std::move(action_ids), start_time));
}

void BuaDocumentService::Request(const std::string& method,
                                 const std::string& request_json,
                                 RequestCallback callback) {
  if (!IsRequestAllowed()) {
    std::move(callback).Run(
        Error("frame_not_allowed",
              "BUA API is only available to primary main frame documents.",
              "permission_policy"));
    return;
  }

  std::optional<base::DictValue> request = ParseRequestDict(request_json);
  if (!request) {
    std::move(callback).Run(Error("invalid_request",
                                  "BUA request payload must be a JSON object.",
                                  "input"));
    return;
  }

  content::BrowserContext* browser_context =
      render_frame_host().GetBrowserContext();
  tabs::TabInterface* tab = GetDefaultTargetTab();
  actor::ActorKeyedService* actor_service = GetActorService();

  if (method == "capabilities") {
    std::move(callback).Run(SuccessDict(BuildCapabilities()));
    return;
  }

  if (method == "createSession") {
    const base::DictValue* options = request->FindDict("options");
    if (options) {
      if (const std::string* requested_id = options->FindString("id");
          requested_id && !requested_id->empty()) {
        session_id_ = *requested_id;
      }
    }

    base::DictValue session;
    session.Set("id", session_id_);
    session.Set("capabilities", BuildCapabilities());
    std::move(callback).Run(SuccessDict(std::move(session)));
    return;
  }

  if (method == "page.snapshot") {
    base::DictValue snapshot_request =
        BuildPageSnapshotRequest(*request, /*screenshot_only=*/false);
    HandleSnapshot(snapshot_request, std::move(callback));
    return;
  }

  if (method == "page.screenshot") {
    base::DictValue screenshot_request =
        BuildPageSnapshotRequest(*request, /*screenshot_only=*/true);
    HandleSnapshot(screenshot_request, std::move(callback));
    return;
  }

  if (method == "page.act") {
    HandleAct(*request, std::move(callback));
    return;
  }

  if (method == "page.navigate") {
    if (!request->FindString("url")) {
      std::move(callback).Run(
          Error("invalid_request", "page.navigate requires url.", "input"));
      return;
    }
    base::DictValue act_request = BuildNavigateActRequest(*request);
    HandleAct(act_request, std::move(callback));
    return;
  }

  if (method == "tabs.current") {
    if (tab) {
      std::move(callback).Run(SuccessDict(BuildTargetSnapshotFromTab(tab)));
    } else if (HasBrowserForProfile(browser_context)) {
      std::move(callback).Run(SuccessDict(BuildNoTargetState(
          "no_focusable_target",
          "Chrome has a browser window for this profile, but no tab is "
          "available as the default BUA target.")));
    } else {
      std::move(callback).Run(SuccessDict(BuildNoTargetState(
          "no_browser",
          "Chrome has no normal browser window for this profile.")));
    }
    return;
  }

  if (method == "tabs.list") {
    const base::DictValue* options = request->FindDict("options");
    std::optional<int32_t> window_id;
    if (const std::string* window_id_string =
            options ? options->FindString("windowId") : nullptr) {
      window_id = ParseWindowId(*window_id_string);
      if (!window_id) {
        std::move(callback).Run(
            Error("invalid_request", "options.windowId is invalid.", "input"));
        return;
      }
    }
    const bool include_background =
        options ? options->FindBool("includeBackground").value_or(true) : true;
    base::ListValue targets = BuildTargetListForProfile(
        browser_context, window_id, include_background);
    std::move(callback).Run(Success(base::Value(std::move(targets))));
    return;
  }

  if (method == "tabs.create") {
    const base::DictValue* options = request->FindDict("options");
    const std::string* url_string =
        options ? options->FindString("url") : nullptr;
    const GURL url(url_string ? *url_string : std::string("about:blank"));
    if (!IsCreateTabUrlAllowed(url)) {
      std::move(callback).Run(
          Error("invalid_request",
                "tabs.create only accepts valid HTTP(S) URLs or "
                "about:blank.",
                "input"));
      return;
    }

    const bool background =
        options ? !options->FindBool("activate").value_or(true) : false;
    std::optional<int32_t> window_id;
    if (const std::string* window_id_string =
            options ? options->FindString("windowId") : nullptr) {
      window_id = ParseWindowId(*window_id_string);
      if (!window_id) {
        std::move(callback).Run(
            Error("invalid_request", "options.windowId is invalid.", "input"));
        return;
      }
    }
    std::string error_json;
    tabs::TabInterface* created_tab = CreateTabForProfile(
        browser_context, url, background, window_id, &error_json);
    if (!created_tab) {
      std::move(callback).Run(std::move(error_json));
      return;
    }
    std::move(callback).Run(
        SuccessDict(BuildTargetSnapshotFromTab(created_tab)));
    return;
  }

  if (method == "tabs.activate" || method == "tabs.close") {
    const std::string* tab_id = request->FindString("tabId");
    if (!tab_id) {
      std::move(callback).Run(
          Error("invalid_request", "tabId is required.", "input"));
      return;
    }

    base::DictValue target;
    target.Set("tabId", *tab_id);
    std::string error_json;
    tabs::TabInterface* target_tab =
        ResolveTargetRef(target, tab, browser_context, &error_json);
    if (!target_tab) {
      std::move(callback).Run(std::move(error_json));
      return;
    }
    if (method == "tabs.activate") {
      BrowserWindowInterface* browser = target_tab->GetBrowserWindowInterface();
      TabListInterface* tab_list =
          browser ? TabListInterface::From(browser) : nullptr;
      if (!tab_list) {
        std::move(callback).Run(Error(
            "backend_unavailable",
            "target tab does not expose an owning tab list.", "backend_error"));
        return;
      }
      tab_list->ActivateTab(target_tab->GetHandle());
      std::move(callback).Run(
          SuccessDict(BuildTargetSnapshotFromTab(target_tab)));
    } else {
      target_tab->Close();
      std::move(callback).Run(Success(base::Value(true)));
    }
    return;
  }

  if (method == "task.start") {
    task_status_ = "running";
    task_reason_.clear();
    std::move(callback).Run(SuccessDict(
        BuildTaskState(session_id_, actor_task_id_.has_value(), task_status_)));
    return;
  }

  if (method == "task.state") {
    actor::ActorTask* task = actor_task_id_ && actor_service
                                 ? actor_service->GetTask(*actor_task_id_)
                                 : nullptr;
    std::move(callback).Run(SuccessDict(BuildTaskState(
        session_id_, task != nullptr,
        task ? BuaTaskStatus(task->GetState()) : task_status_, task_reason_)));
    return;
  }

  if (method == "task.pause") {
    const std::string* requested_reason = request->FindString("reason");
    task_reason_ = requested_reason ? *requested_reason : "user_takeover";
    task_status_ = "paused";
    actor::ActorTask* task = actor_task_id_ && actor_service
                                 ? actor_service->GetTask(*actor_task_id_)
                                 : nullptr;
    if (task) {
      task->Pause(/*from_actor=*/false);
      task_status_ = BuaTaskStatus(task->GetState());
    }
    std::move(callback).Run(SuccessDict(BuildTaskState(
        session_id_, task != nullptr, task_status_, task_reason_)));
    return;
  }

  if (method == "task.resume") {
    task_reason_.clear();
    task_status_ = "running";
    actor::ActorTask* task = actor_task_id_ && actor_service
                                 ? actor_service->GetTask(*actor_task_id_)
                                 : nullptr;
    if (task) {
      task->Resume();
      task_status_ = BuaTaskStatus(task->GetState());
    }
    base::DictValue result;
    result.Set("state", BuildTaskState(session_id_, task != nullptr,
                                       task_status_, task_reason_));
    std::move(callback).Run(SuccessDict(std::move(result)));
    return;
  }

  if (method == "task.cancelActions") {
    const std::string* requested_reason = request->FindString("reason");
    actor::ActorTask* task = actor_task_id_ && actor_service
                                 ? actor_service->GetTask(*actor_task_id_)
                                 : nullptr;
    bool cancelled = false;
    if (task) {
      cancelled = task->CancelOngoingActions(
          actor::mojom::ActionResultCode::kActionsCancelled);
      task_status_ = BuaTaskStatus(task->GetState());
    }
    base::DictValue result;
    result.Set("status", cancelled ? "cancelled" : "nothing_to_cancel");
    if (requested_reason && !requested_reason->empty()) {
      result.Set("reason", *requested_reason);
    }
    std::move(callback).Run(SuccessDict(std::move(result)));
    return;
  }

  if (method == "task.stop" || method == "session.close") {
    const std::string* requested_reason = request->FindString("reason");
    const std::string reason = method == "session.close"
                                   ? "session_closed"
                                   : (requested_reason ? *requested_reason
                                                       : "completed");
    task_status_ = "stopped";
    task_reason_ = reason;
    StopActorTask(BuaStoppedReason(reason));
    if (method == "task.stop") {
      base::DictValue result;
      result.Set("ok", true);
      result.Set("state",
                 BuildTaskState(session_id_, false, "stopped", reason));
      std::move(callback).Run(SuccessDict(std::move(result)));
    } else {
      std::move(callback).Run(SuccessDict(base::DictValue()));
    }
    return;
  }

  std::move(callback).Run(Error(
      "unsupported", "BUA method is not supported: " + method, "unsupported"));
}

void BuaDocumentService::OnActionsFinished(
    RequestCallback callback,
    std::vector<std::string> action_ids,
    base::TimeTicks start_time,
    std::vector<actor::ActionResultWithLatencyInfo> action_results,
    actor::TabObservationStrategy observation_strategy) {
  std::move(callback).Run(SuccessDict(BuildBuaActionResult(
      action_ids, start_time, std::move(action_results))));
}

}  // namespace bua
