// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/bua/bua_document_service.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "base/base64.h"
#include "base/base64url.h"
#include "base/check.h"
#include "base/functional/bind.h"
#include "base/json/json_reader.h"
#include "base/json/json_writer.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/stringprintf.h"
#include "base/strings/utf_string_conversions.h"
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
#include "chrome/common/actor/action_result.h"
#include "components/actor/core/task_source_info.h"
#include "components/actor/public/mojom/actor_types.mojom.h"
#include "components/optimization_guide/proto/features/actions_data.pb.h"
#include "components/optimization_guide/proto/features/common_quality_data.pb.h"
#include "components/page_content_annotations/content/page_context_fetcher.h"
#include "components/page_content_annotations/content/page_context_fetcher_options.h"
#include "components/tabs/public/tab_interface.h"
#include "content/public/browser/render_frame_host.h"
#include "content/public/browser/web_contents.h"
#include "mojo/public/cpp/base/proto_wrapper.h"
#include "url/gurl.h"
#include "url/origin.h"

namespace bua {
namespace {

namespace apc = optimization_guide::proto;

constexpr char kNodeIdPrefix[] = "apc:";
constexpr int kDefaultInnerTextBytesLimit = 256 * 1024;
constexpr int kDefaultPdfBytesLimit = 2 * 1024 * 1024;
constexpr int kDefaultMaxNodes = 2000;

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

base::DictValue OkCheck() {
  base::DictValue check;
  check.Set("ok", true);
  return check;
}

base::DictValue FailedCheck(std::string reason) {
  base::DictValue check;
  check.Set("ok", false);
  check.Set("reason", std::move(reason));
  return check;
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

const base::DictValue* FindRequestTargetRef(const base::DictValue& request) {
  if (const base::DictValue* target = request.FindDict("target")) {
    return target;
  }
  if (const base::DictValue* options = request.FindDict("options")) {
    return options->FindDict("target");
  }
  return nullptr;
}

tabs::TabInterface* ResolveTargetTabOrCurrent(
    const base::DictValue& request,
    tabs::TabInterface* current_tab,
    std::string* error_json) {
  const base::DictValue* target = FindRequestTargetRef(request);
  const std::string* target_tab_id =
      target ? target->FindString("tabId") : nullptr;
  if (!target_tab_id) {
    return current_tab;
  }

  std::optional<int32_t> parsed_tab_id = ParseTabId(*target_tab_id);
  if (!parsed_tab_id) {
    *error_json = Error("invalid_request", "target.tabId is invalid.",
                        "input");
    return nullptr;
  }

  tabs::TabInterface* target_tab = tabs::TabHandle(*parsed_tab_id).Get();
  if (!target_tab) {
    *error_json =
        Error("target_not_found", "target.tabId does not refer to a live tab.",
              "target_not_found");
    return nullptr;
  }
  return target_tab;
}

std::optional<int32_t> ResolveActionTabId(
    const base::DictValue& action,
    tabs::TabInterface* default_tab,
    std::string* error_json) {
  const base::DictValue* target_ref = action.FindDict("targetRef");
  const std::string* target_tab_id =
      target_ref ? target_ref->FindString("tabId") : nullptr;
  if (!target_tab_id) {
    if (!default_tab) {
      *error_json =
          Error("no_target",
                "Action requires targetRef.tabId when BUA is not running in "
                "a browser tab.",
                "target_not_found");
      return std::nullopt;
    }
    return default_tab->GetHandle().raw_value();
  }

  std::optional<int32_t> parsed_tab_id = ParseTabId(*target_tab_id);
  if (!parsed_tab_id) {
    *error_json =
        Error("invalid_request", "action targetRef.tabId is invalid.",
              "input");
    return std::nullopt;
  }
  if (!tabs::TabHandle(*parsed_tab_id).Get()) {
    *error_json = Error(
        "target_not_found",
        "action targetRef.tabId does not refer to a live tab.",
        "target_not_found");
    return std::nullopt;
  }
  return *parsed_tab_id;
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

base::DictValue BuildTargetSnapshot(tabs::TabInterface* tab,
                                    content::WebContents* web_contents) {
  if (tab) {
    glic::mojom::TabDataPtr tab_data = glic::CreateTabData(tab);
    if (tab_data) {
      return BuildTargetSnapshotFromTabData(*tab_data);
    }
  }

  base::DictValue target;
  target.Set("id", "current");
  target.Set("kind", "page");
  if (web_contents) {
    target.Set("url", web_contents->GetVisibleURL().spec());
    target.Set("title",
               base::UTF16ToUTF8(web_contents->GetTitle()));
    target.Set("active", true);
    target.Set("focused", true);
    target.Set("readable", true);
    target.Set("updatedAtMs", static_cast<double>(NowMs()));
  } else {
    target.Set("readable", false);
  }
  return target;
}

base::DictValue BuildCapabilities() {
  base::DictValue backend;
  backend.Set("name", "glic-actor");
  backend.Set("version", "0.2");
  backend.Set("protocol", "bua-mojo");

  base::DictValue snapshot;
  snapshot.Set("content", true);
  snapshot.Set("screenshot", true);
  snapshot.Set("metadata", true);
  snapshot.Set("pdf", true);

  base::DictValue act;
  act.Set("actions", StringList({"click", "type", "select", "scroll",
                                  "scroll_to", "hover", "navigate", "history",
                                  "wait", "tab"}));
  act.Set("sequences", true);
  act.Set("snapshotAfterAction", false);
  act.Set("cancel", false);
  act.Set("pause", false);
  act.Set("resume", false);

  base::DictValue targets;
  targets.Set("current", true);
  targets.Set("list", true);
  targets.Set("createTab", true);
  targets.Set("activateTab", true);
  targets.Set("closeTab", true);
  targets.Set("createWindow", false);
  targets.Set("activateWindow", false);
  targets.Set("closeWindow", false);

  base::DictValue user_requests;
  user_requests.Set("credentialSelection", false);
  user_requests.Set("autofillSelection", false);
  user_requests.Set("userConfirmation", false);
  user_requests.Set("navigationConfirmation", false);
  user_requests.Set("filePicker", false);
  user_requests.Set("userTakeover", false);

  base::DictValue capabilities;
  capabilities.Set("backend", std::move(backend));
  capabilities.Set("snapshot", std::move(snapshot));
  capabilities.Set("act", std::move(act));
  capabilities.Set("targets", std::move(targets));
  capabilities.Set("userRequests", std::move(user_requests));
  capabilities.Set("events", base::ListValue());
  return capabilities;
}

base::DictValue BuildAvailability(bool has_tab,
                                  bool has_glic_service,
                                  bool has_actor_service,
                                  bool has_policy_checker,
                                  bool can_act_on_web) {
  base::ListValue permissions;

  base::DictValue page_context;
  page_context.Set("name", "page_context");
  page_context.Set("state", has_tab ? "granted" : "denied");
  page_context.Set("source", "browser");
  permissions.Append(std::move(page_context));

  base::DictValue screenshot;
  screenshot.Set("name", "screenshot");
  screenshot.Set("state", has_tab ? "granted" : "denied");
  screenshot.Set("source", "browser");
  permissions.Append(std::move(screenshot));

  base::DictValue browser_actuation;
  browser_actuation.Set("name", "browser_actuation");
  browser_actuation.Set("state", can_act_on_web ? "granted" : "denied");
  browser_actuation.Set("source", "backend");
  if (!can_act_on_web) {
    browser_actuation.Set(
        "reason",
        !has_actor_service
            ? "ActorKeyedService is unavailable. Enable the Glic Actor "
              "backend for real browser actuation."
            : !has_glic_service || !has_policy_checker
                  ? "Glic actor policy checker is unavailable."
                  : "Glic actor policy checker denied act-on-web.");
  }
  permissions.Append(std::move(browser_actuation));

  base::ListValue policies;
  base::DictValue scheme;
  scheme.Set("name", "scheme");
  scheme.Set("state", "allowed");
  policies.Append(std::move(scheme));

  base::ListValue diagnostics;
  if (!has_actor_service || !has_glic_service || !has_policy_checker ||
      !can_act_on_web) {
    base::DictValue diagnostic;
    diagnostic.Set("severity", can_act_on_web ? "info" : "warning");
    diagnostic.Set("category", "backend");
    diagnostic.Set(
        "message",
        "BUA is wired to Glic page context and Actor actions. Actuation "
        "requires ActorKeyedService plus GlicActorPolicyChecker permission.");
    diagnostic.Set("hasGlicService", has_glic_service);
    diagnostic.Set("hasActorService", has_actor_service);
    diagnostic.Set("hasPolicyChecker", has_policy_checker);
    diagnostic.Set("canActOnWeb", can_act_on_web);
    diagnostics.Append(std::move(diagnostic));
  }

  base::DictValue availability;
  availability.Set("status",
                   has_tab && can_act_on_web
                       ? "available"
                       : has_tab ? "degraded" : "unavailable");
  availability.Set("canReadPage",
                   has_tab ? OkCheck() : FailedCheck("No tab is available."));
  availability.Set(
      "canAct",
      can_act_on_web
          ? OkCheck()
          : FailedCheck("Actor/Glic act-on-web backend is unavailable or "
                        "blocked by policy."));
  availability.Set("focusedTarget",
                   has_tab ? OkCheck() : FailedCheck("No tab is available."));
  availability.Set("permissions", std::move(permissions));
  availability.Set("policies", std::move(policies));
  availability.Set("diagnostics", std::move(diagnostics));
  return availability;
}

std::string EncodeApcNodeId(std::string_view document_identifier,
                            int dom_node_id) {
  std::string encoded_document;
  base::Base64UrlEncode(document_identifier,
                        base::Base64UrlEncodePolicy::OMIT_PADDING,
                        &encoded_document);
  return base::StringPrintf("%s%s:%d", kNodeIdPrefix,
                            encoded_document.c_str(), dom_node_id);
}

struct ApcNodeRef {
  std::string document_identifier;
  int dom_node_id = 0;
};

std::optional<ApcNodeRef> DecodeApcNodeId(std::string_view node_id) {
  if (!node_id.starts_with(kNodeIdPrefix)) {
    return std::nullopt;
  }
  std::string_view body = node_id.substr(std::string_view(kNodeIdPrefix).size());
  size_t separator = body.rfind(':');
  if (separator == std::string_view::npos || separator == 0 ||
      separator == body.size() - 1) {
    return std::nullopt;
  }

  std::string document_identifier;
  if (!base::Base64UrlDecode(
          body.substr(0, separator),
          base::Base64UrlDecodePolicy::DISALLOW_PADDING,
          &document_identifier)) {
    return std::nullopt;
  }

  int dom_node_id = 0;
  if (!base::StringToInt(body.substr(separator + 1), &dom_node_id)) {
    return std::nullopt;
  }
  return ApcNodeRef{std::move(document_identifier), dom_node_id};
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

base::ListValue ActionsForAttributes(
    const apc::ContentAttributes& attributes) {
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
    frame.Set("origin", OriginFromSecurityOrigin(
                            frame_data.security_origin()));
  }
  return frame;
}

base::DictValue BuildApcNode(const apc::ContentNode& node,
                             const std::string& snapshot_id,
                             std::string document_identifier,
                             const apc::FrameData* main_frame_data,
                             int max_nodes,
                             int* visited_nodes,
                             int* generated_node_id,
                             bool* truncated) {
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
    out.Set("id", EncodeApcNodeId(document_identifier,
                                  attributes.common_ancestor_dom_node_id()));
  } else {
    out.Set("id", base::StringPrintf("apc-anon:%d", *generated_node_id));
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

  if (attributes.has_geometry() &&
      attributes.geometry().has_visible_bounding_box()) {
    out.Set("bounds",
            BuildRect(attributes.geometry().visible_bounding_box()));
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

  if (attributes.attribute_type() == apc::CONTENT_ATTRIBUTE_ROOT &&
      main_frame_data) {
    out.Set("frame", BuildFrameInfoFromFrameData(*main_frame_data, true));
  } else if (attributes.has_iframe_data() &&
             attributes.iframe_data().has_frame_data()) {
    const apc::FrameData& frame_data = attributes.iframe_data().frame_data();
    out.Set("frame", BuildFrameInfoFromFrameData(frame_data, false));
    if (frame_data.has_document_identifier()) {
      document_identifier =
          frame_data.document_identifier().serialized_token();
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
      children.Append(BuildApcNode(child, snapshot_id, document_identifier,
                                  main_frame_data, max_nodes, visited_nodes,
                                  generated_node_id, truncated));
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
    frame.Set("origin",
              url::Origin::Create(context.tab_data->url).Serialize());
    if (context.tab_data->title) {
      frame.Set("title", *context.tab_data->title);
    }
    content.Set("frame", std::move(frame));
  }
  if (context.web_page_data) {
    content.Set("text",
                context.web_page_data->main_document->inner_text);
  }
  return content;
}

base::DictValue BuildSnapshotFromTabContext(
    std::string snapshot_id,
    int generation,
    const glic::mojom::TabContext& context,
    content::WebContents* web_contents,
    int max_nodes) {
  base::DictValue page;
  if (context.tab_data) {
    page.Set("url", context.tab_data->url.spec());
    page.Set("origin",
             url::Origin::Create(context.tab_data->url).Serialize());
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
    annotated_page_content =
        context.annotated_page_data->annotated_page_content
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
    content = BuildApcNode(annotated_page_content->root_node(), snapshot_id,
                           document_identifier, main_frame_data, max_nodes,
                           &visited_nodes, &generated_node_id,
                           &content_truncated);
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
    content_quality.Set("error", "Glic did not return annotated page content.");
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
  snapshot.Set("source", "explicit");
  snapshot.Set("createdAtMs", static_cast<double>(NowMs()));
  snapshot.Set("generation", generation);
  if (context.tab_data) {
    snapshot.Set("target",
                 BuildTargetSnapshotFromTabData(*context.tab_data));
  }
  snapshot.Set("page", std::move(page));
  snapshot.Set("content", std::move(content));

  if (annotated_page_content && annotated_page_content->has_viewport_geometry()) {
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
    const glic::mojom::Screenshot& screenshot =
        *context.viewport_screenshot;
    base::DictValue screenshot_value;
    screenshot_value.Set("id", snapshot_id + ":screenshot");
    screenshot_value.Set("width", static_cast<int>(screenshot.width_pixels));
    screenshot_value.Set("height", static_cast<int>(screenshot.height_pixels));
    screenshot_value.Set("mimeType", screenshot.mime_type);
    screenshot_value.Set(
        "uri", "data:" + screenshot.mime_type + ";base64," +
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

std::string ActorCodeString(actor::mojom::ActionResultCode code) {
  return "actor:" + base::NumberToString(static_cast<int>(code));
}

std::string ActorStatus(actor::mojom::ActionResultCode code) {
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

std::string ActorCategory(actor::mojom::ActionResultCode code) {
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

base::DictValue BuildActorActionResult(
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
  result.Set("status", ActorStatus(result_code));
  result.Set("actionId",
             action_ids.empty() ? std::string("bua-action")
                                : action_ids[action_index]);
  result.Set("code", ActorCodeString(result_code));
  result.Set("category", ActorCategory(result_code));
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
  timing.Set("elapsedMs",
             static_cast<int>(
                 (base::TimeTicks::Now() - start_time).InMilliseconds()));
  base::ListValue phases;
  for (size_t i = 0; i < action_results.size(); ++i) {
    base::DictValue phase;
    phase.Set("name", action_ids.size() > i ? action_ids[i]
                                            : base::StringPrintf("action:%zu",
                                                                 i));
    phase.Set("elapsedMs",
              static_cast<int>((action_results[i].end_time -
                                action_results[i].start_time)
                                   .InMilliseconds()));
    phases.Append(std::move(phase));
  }
  timing.Set("phases", std::move(phases));
  result.Set("timing", std::move(timing));

  base::DictValue diagnostic;
  diagnostic.Set("severity", actor::IsOk(result_code) ? "info" : "warning");
  diagnostic.Set("category", "actor");
  diagnostic.Set("message", "ActorKeyedService executed the action sequence.");
  diagnostic.Set("actionResultCode", static_cast<int>(result_code));
  base::ListValue diagnostics;
  diagnostics.Append(std::move(diagnostic));
  result.Set("diagnostics", std::move(diagnostics));

  return result;
}

bool FillPointTarget(const base::DictValue& point,
                     apc::ActionTarget* target,
                     std::string* error_json) {
  std::optional<double> x = point.FindDouble("x");
  std::optional<double> y = point.FindDouble("y");
  if (!x || !y || !std::isfinite(*x) || !std::isfinite(*y)) {
    *error_json = Error("invalid_request",
                        "Action point target requires finite x and y.",
                        "input");
    return false;
  }
  target->mutable_coordinate()->set_x(static_cast<int>(*x));
  target->mutable_coordinate()->set_y(static_cast<int>(*y));
  return true;
}

bool FillActionTarget(const base::DictValue& target_dict,
                      apc::ActionTarget* target,
                      content::RenderFrameHost& render_frame_host,
                      std::string* error_json) {
  if (const std::string* node_id = target_dict.FindString("nodeId")) {
    std::optional<ApcNodeRef> node_ref = DecodeApcNodeId(*node_id);
    if (!node_ref) {
      *error_json =
          Error("invalid_request",
                "nodeId is not a BUA APC node id from snapshot().",
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
              "target.query is not wired yet. Call snapshot() and use nodeId "
              "or provide a point target.",
              "unsupported");
    return false;
  }

  *error_json =
      Error("invalid_request", "Action target requires nodeId or point.",
            "input");
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

bool AppendBuaActionToProto(const base::DictValue& action,
                            int action_index,
                            tabs::TabInterface* default_tab,
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
        ResolveActionTabId(action, default_tab, error_json);
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
    if (!FillActionTarget(*target, click->mutable_target(), render_frame_host,
                          error_json)) {
      return false;
    }
    const std::string button =
        FindStringMember(action, "button").value_or("left");
    click->set_click_type(button == "right"
                              ? apc::ClickAction_ClickType_RIGHT
                              : apc::ClickAction_ClickType_LEFT);
    click->set_click_count(action.FindInt("count").value_or(1) == 2
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
    if (!FillActionTarget(*target, type->mutable_target(), render_frame_host,
                          error_json)) {
      return false;
    }
    type->set_text(*text);
    const std::string mode =
        FindStringMember(action, "mode").value_or("replace");
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
    if (!value) {
      *error_json =
          Error("invalid_request", "select action requires value.", "input");
      return false;
    }
    apc::SelectAction* select = proto_action->mutable_select();
    if (!FillActionTarget(*target, select->mutable_target(), render_frame_host,
                          error_json)) {
      return false;
    }
    select->set_value(*value);
    select->set_tab_id(tab_id);
  } else if (kind == "scroll") {
    apc::ScrollAction* scroll = proto_action->mutable_scroll();
    if (const base::DictValue* target = action.FindDict("target")) {
      if (!FillActionTarget(*target, scroll->mutable_target(),
                            render_frame_host, error_json)) {
        return false;
      }
    }
    const std::string direction =
        FindStringMember(action, "direction").value_or("down");
    if (direction == "up") {
      scroll->set_direction(apc::ScrollAction_ScrollDirection_UP);
    } else if (direction == "left") {
      scroll->set_direction(apc::ScrollAction_ScrollDirection_LEFT);
    } else if (direction == "right") {
      scroll->set_direction(apc::ScrollAction_ScrollDirection_RIGHT);
    } else {
      scroll->set_direction(apc::ScrollAction_ScrollDirection_DOWN);
    }
    scroll->set_distance(
        static_cast<float>(action.FindDouble("distance").value_or(600.0)));
    scroll->set_tab_id(tab_id);
  } else if (kind == "scroll_to") {
    const base::DictValue* target = RequiredTarget(action, error_json);
    if (!target) {
      return false;
    }
    apc::ScrollToAction* scroll_to = proto_action->mutable_scroll_to();
    if (!FillActionTarget(*target, scroll_to->mutable_target(),
                          render_frame_host, error_json)) {
      return false;
    }
    scroll_to->set_tab_id(tab_id);
  } else if (kind == "hover") {
    const base::DictValue* target = RequiredTarget(action, error_json);
    if (!target) {
      return false;
    }
    apc::MoveMouseAction* move_mouse = proto_action->mutable_move_mouse();
    if (!FillActionTarget(*target, move_mouse->mutable_target(),
                          render_frame_host, error_json)) {
      return false;
    }
    move_mouse->set_tab_id(tab_id);
  } else if (kind == "navigate") {
    const std::string* url_string = action.FindString("url");
    if (!url_string) {
      *error_json =
          Error("invalid_request", "navigate action requires url.", "input");
      return false;
    }
    const GURL url(*url_string);
    if (!url.is_valid() || !url.SchemeIsHTTPOrHTTPS()) {
      *error_json = Error(
          "invalid_request",
          "navigate action only accepts valid HTTP(S) URLs.", "navigation");
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
          "history.reload is not supported by Actor actions yet."));
      return false;
    }
  } else if (kind == "wait") {
    apc::WaitAction* wait = proto_action->mutable_wait();
    int wait_ms = action.FindInt("waitMs").value_or(0);
    if (const base::DictValue* until = action.FindDict("until")) {
      if (const std::string* type = until->FindString("type")) {
        if (*type == "time") {
          wait_ms = until->FindInt("waitMs").value_or(wait_ms);
        } else {
          *error_json = SuccessDict(UnsupportedActionResult(
              action_id, action_index, kind,
              "wait.until conditions other than time are not wired yet."));
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
                            "tab activate requires targetRef.tabId.",
                            "input");
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
        *error_json =
            Error("invalid_request", "tab close requires targetRef.tabId.",
                  "input");
        return false;
      }
      proto_action->mutable_close_tab()->set_tab_id(*parsed_tab_id);
    } else {
      *error_json = SuccessDict(UnsupportedActionResult(
          action_id, action_index, kind,
          "tab create is exposed through targets.createTab in BUA v1."));
      return false;
    }
  } else {
    *error_json = SuccessDict(UnsupportedActionResult(
        action_id, action_index, kind,
        "BUA action is not wired to Actor in this build."));
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

glic::mojom::GetTabContextOptions BuildSnapshotOptions(
    const base::DictValue& request) {
  const base::DictValue* options = request.FindDict("options");
  const base::DictValue* channels =
      options ? options->FindDict("channels") : nullptr;
  const base::DictValue* budget = options ? options->FindDict("budget")
                                          : nullptr;

  const bool include_content =
      channels ? channels->FindBool("content").value_or(true) : true;
  const bool include_screenshot =
      channels ? channels->FindBool("screenshot").value_or(true) : true;
  const bool include_pdf =
      channels ? channels->FindBool("pdf").value_or(false) : false;

  glic::mojom::GetTabContextOptions context_options;
  context_options.include_inner_text = true;
  context_options.inner_text_bytes_limit =
      static_cast<uint32_t>(std::max(
          1, budget ? budget->FindInt("maxTextBytes").value_or(
                          kDefaultInnerTextBytesLimit)
                    : kDefaultInnerTextBytesLimit));
  context_options.include_viewport_screenshot = include_screenshot;
  context_options.include_annotated_page_content = include_content;
  context_options.max_meta_tags = 32;
  context_options.include_pdf = include_pdf;
  context_options.pdf_size_limit = static_cast<uint32_t>(
      budget ? budget->FindInt("maxBytes").value_or(kDefaultPdfBytesLimit)
             : kDefaultPdfBytesLimit);
  context_options.annotated_page_content_mode =
      apc::ANNOTATED_PAGE_CONTENT_MODE_ACTIONABLE_ELEMENTS;

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
      page_content_annotations::ScreenshotOptions::ScreenshotImageFormat::
          kJpeg;
  context_options.screenshot_collection_options.screenshot_compression_quality =
      page_content_annotations::ScreenshotOptions::ScreenshotCompressionQuality::
          kMedium;
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

tabs::TabInterface* BuaDocumentService::GetCurrentTab() const {
  content::WebContents* web_contents = GetWebContents();
  return web_contents ? tabs::TabInterface::MaybeGetFromContents(web_contents)
                      : nullptr;
}

glic::GlicKeyedService* BuaDocumentService::GetGlicService() const {
  return glic::GlicKeyedServiceFactory::GetGlicKeyedService(
      render_frame_host().GetBrowserContext());
}

actor::ActorKeyedService* BuaDocumentService::GetActorService() const {
  return actor::ActorKeyedService::Get(
      render_frame_host().GetBrowserContext());
}

std::optional<actor::TaskId> BuaDocumentService::EnsureActorTask(
    std::string* error_json) {
  actor::ActorKeyedService* actor_service = GetActorService();
  if (!actor_service) {
    *error_json =
        Error("backend_unavailable",
              "ActorKeyedService is unavailable. Enable the Glic Actor "
              "backend before using BUA act().",
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
              "GlicActorPolicyChecker is unavailable, so BUA cannot create "
              "a real Actor task.",
              "backend_error");
    return std::nullopt;
  }

  glic::GlicActorPolicyChecker& policy_checker =
      glic_service->actor_policy_checker();
  if (!policy_checker.CanActOnWeb()) {
    *error_json = Error("act_blocked_by_policy",
                        "Glic actor policy denied act-on-web.",
                        "permission_policy");
    return std::nullopt;
  }

  actor_task_id_ = actor_service->CreateTask(
      actor::TaskSourceInfo(actor::TaskSourceInfo::Client::kExperimentalActor,
                            session_id_),
      &policy_checker);
  if (!actor_task_id_ || actor_task_id_->is_null()) {
    *error_json =
        Error("create_task_failed", "ActorKeyedService did not create a task.",
              "task_state");
    actor_task_id_.reset();
    return std::nullopt;
  }

  return actor_task_id_;
}

void BuaDocumentService::StopActorTask(
    actor::ActorTask::StoppedReason reason) {
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
  tabs::TabInterface* tab = GetCurrentTab();

  std::string error_json;
  tab = ResolveTargetTabOrCurrent(request, tab, &error_json);
  if (!tab) {
    std::move(callback).Run(
        error_json.empty()
            ? Error("no_target",
                    "snapshot() requires target.tabId when BUA is not running "
                    "in a browser tab.",
                    "target_not_found")
            : std::move(error_json));
    return;
  }

  const std::string snapshot_id =
      std::string("snapshot-") + base::NumberToString(NowMs());
  const int generation = ++snapshot_generation_;
  glic::FetchPageContext(
      tab, BuildSnapshotOptions(request),
      base::BindOnce(&BuaDocumentService::OnSnapshot,
                     weak_ptr_factory_.GetWeakPtr(), std::move(callback),
                     snapshot_id, generation, SnapshotMaxNodes(request)),
      /*progress_listener=*/nullptr,
      /*is_screenshot_annotated=*/false);
}

void BuaDocumentService::OnSnapshot(
    RequestCallback callback,
    std::string snapshot_id,
    int generation,
    int max_nodes,
    base::expected<
        glic::mojom::GetContextResultPtr,
        page_content_annotations::FetchPageContextErrorDetails> result) {
  if (!result.has_value()) {
    std::move(callback).Run(Error("snapshot_failed", result.error().message,
                                  "backend_error"));
    return;
  }

  if (!result.value()->is_tab_context()) {
    std::move(callback).Run(
        Error("snapshot_failed",
              result.value()->is_error_reason()
                  ? result.value()->get_error_reason()
                  : "Glic did not return tab context.",
              "backend_error"));
    return;
  }

  std::move(callback).Run(SuccessDict(BuildSnapshotFromTabContext(
      std::move(snapshot_id), generation, *result.value()->get_tab_context(),
      GetWebContents(), max_nodes)));
}

void BuaDocumentService::HandleAct(const base::DictValue& request,
                                   RequestCallback callback) {
  tabs::TabInterface* tab = GetCurrentTab();

  const base::ListValue* actions = request.FindList("actions");
  if (!actions || actions->empty()) {
    std::move(callback).Run(Error(
        "invalid_request", "act requires a non-empty actions list.", "input"));
    return;
  }

  std::string error_json;
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
    if (!AppendBuaActionToProto(*action, action_index, tab,
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
    result.Set("actionId",
               action_ids.size() > error.first ? action_ids[error.first]
                                               : "bua-action");
    result.Set("failedActionIndex", static_cast<int>(error.first));
    result.Set("code", ActorCodeString(error.second));
    result.Set("category", ActorCategory(error.second));
    result.Set("message", "Actor rejected the action proto.");
    std::move(callback).Run(SuccessDict(std::move(result)));
    return;
  }

  actor::ActorKeyedService* actor_service = GetActorService();
  if (!actor_service) {
    std::move(callback).Run(
        Error("backend_unavailable", "ActorKeyedService went away.",
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

  tabs::TabInterface* tab = GetCurrentTab();
  content::WebContents* web_contents = GetWebContents();
  glic::GlicKeyedService* glic_service = GetGlicService();
  actor::ActorKeyedService* actor_service = GetActorService();
  const bool has_policy_checker =
      glic_service && glic_service->HasActorPolicyChecker();
  const bool can_act_on_web =
      has_policy_checker && glic_service->actor_policy_checker().CanActOnWeb();

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

  if (method == "availability.current") {
    std::move(callback).Run(SuccessDict(BuildAvailability(
        !!tab, !!glic_service, !!actor_service, has_policy_checker,
        can_act_on_web)));
    return;
  }

  if (method == "snapshot") {
    HandleSnapshot(*request, std::move(callback));
    return;
  }

  if (method == "act") {
    HandleAct(*request, std::move(callback));
    return;
  }

  if (method == "targets.current") {
    std::move(callback).Run(
        SuccessDict(BuildTargetSnapshot(tab, web_contents)));
    return;
  }

  if (method == "targets.list") {
    base::ListValue targets;
    targets.Append(BuildTargetSnapshot(tab, web_contents));
    std::move(callback).Run(Success(base::Value(std::move(targets))));
    return;
  }

  if (method == "targets.createTab") {
    const base::DictValue* options = request->FindDict("options");
    const std::string* url_string =
        options ? options->FindString("url") : nullptr;
    if (!url_string) {
      std::move(callback).Run(
          Error("invalid_request", "targets.createTab requires url.", "input"));
      return;
    }
    const GURL url(*url_string);
    if (!url.is_valid() || !url.SchemeIsHTTPOrHTTPS()) {
      std::move(callback).Run(
          Error("invalid_request",
                "targets.createTab only accepts valid HTTP(S) URLs.", "input"));
      return;
    }
    if (!glic_service) {
      std::move(callback).Run(
          Error("backend_unavailable", "GlicKeyedService is unavailable.",
                "backend_error"));
      return;
    }

    const bool background =
        options ? options->FindBool("background").value_or(false) : false;
    glic_service->CreateTab(
        url, background, std::nullopt,
        base::BindOnce(
            [](RequestCallback callback, glic::mojom::TabDataPtr tab_data) {
              if (!tab_data) {
                std::move(callback).Run(Error(
                    "create_tab_failed",
                    "GlicKeyedService did not create a tab.", "browser_state"));
                return;
              }
              std::move(callback).Run(
                  SuccessDict(BuildTargetSnapshotFromTabData(*tab_data)));
            },
            std::move(callback)));
    return;
  }

  if (method == "targets.activate" || method == "targets.close") {
    const base::DictValue* target = request->FindDict("target");
    const std::string* target_tab_id =
        target ? target->FindString("tabId") : nullptr;
    std::optional<int32_t> parsed_tab_id =
        target_tab_id ? ParseTabId(*target_tab_id) : std::nullopt;
    if (!parsed_tab_id) {
      std::move(callback).Run(
          Error("invalid_request", "target.tabId is required.", "input"));
      return;
    }

    base::DictValue action;
    action.Set("kind", "tab");
    action.Set("operation",
               method == "targets.activate" ? "activate" : "close");
    base::DictValue target_ref;
    target_ref.Set("tabId", *target_tab_id);
    action.Set("targetRef", std::move(target_ref));

    base::ListValue actions;
    actions.Append(std::move(action));
    base::DictValue act_request;
    act_request.Set("actions", std::move(actions));
    HandleAct(act_request, std::move(callback));
    return;
  }

  if (method == "task.start") {
    std::string error_json;
    std::optional<actor::TaskId> task_id = EnsureActorTask(&error_json);
    if (!task_id) {
      std::move(callback).Run(std::move(error_json));
      return;
    }
    std::move(callback).Run(
        SuccessDict(BuildTaskState(session_id_, true, "running")));
    return;
  }

  if (method == "task.state") {
    bool has_task = actor_task_id_ && actor_service &&
                    actor_service->GetTask(*actor_task_id_);
    std::move(callback).Run(
        SuccessDict(BuildTaskState(session_id_, has_task,
                                   has_task ? "running" : "idle")));
    return;
  }

  if (method == "task.stop" || method == "session.close") {
    StopActorTask(actor::ActorTask::StoppedReason::kTaskComplete);
    if (method == "task.stop") {
      base::DictValue result;
      result.Set("ok", true);
      result.Set("state",
                 BuildTaskState(session_id_, false, "stopped", "completed"));
      std::move(callback).Run(SuccessDict(std::move(result)));
    } else {
      std::move(callback).Run(SuccessDict(base::DictValue()));
    }
    return;
  }

  if (method == "requests.next") {
    std::move(callback).Run(Success(base::Value()));
    return;
  }

  if (method == "requests.respond") {
    std::move(callback).Run(SuccessDict(base::DictValue()));
    return;
  }

  if (method == "diagnostics.current") {
    base::ListValue diagnostics;
    base::DictValue diagnostic;
    diagnostic.Set("severity", "info");
    diagnostic.Set("category", "adapter");
    diagnostic.Set(
        "message",
        "BUA native bridge is active. snapshot() uses Glic page context; "
        "act() uses ActorKeyedService when available.");
    diagnostic.Set("hasGlicService", !!glic_service);
    diagnostic.Set("hasActorService", !!actor_service);
    diagnostic.Set("hasPolicyChecker", has_policy_checker);
    diagnostic.Set("canActOnWeb", can_act_on_web);
    diagnostics.Append(std::move(diagnostic));
    std::move(callback).Run(Success(base::Value(std::move(diagnostics))));
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
  std::move(callback).Run(SuccessDict(BuildActorActionResult(
      action_ids, start_time, std::move(action_results))));
}

}  // namespace bua
