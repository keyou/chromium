// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_BROWSER_BUA_BUA_DOCUMENT_SERVICE_H_
#define CHROME_BROWSER_BUA_BUA_DOCUMENT_SERVICE_H_

#include <map>
#include <optional>
#include <string>
#include <vector>

#include "base/memory/weak_ptr.h"
#include "base/time/time.h"
#include "base/types/expected.h"
#include "base/values.h"
#include "chrome/browser/actor/actor_task.h"
#include "chrome/browser/glic/host/glic.mojom.h"
#include "components/actor/core/task_id.h"
#include "components/bua/public/mojom/bua_host.mojom.h"
#include "components/page_content_annotations/content/page_context_fetcher.h"
#include "content/public/browser/document_service.h"

namespace actor {
class ActorKeyedService;
struct ActionResultWithLatencyInfo;
class TabObservationStrategy;
}  // namespace actor

namespace content {
class RenderFrameHost;
class WebContents;
}  // namespace content

namespace glic {
class GlicKeyedService;
}

namespace tabs {
class TabInterface;
}

namespace bua {

struct BuaWaitSpec {
  std::string action_id;
  std::string type;
  std::string pattern;
  std::string text;
  std::optional<base::DictValue> target_ref;
  std::optional<base::DictValue> target;
  bool expect_absent = false;
  base::TimeDelta timeout;
  base::TimeDelta delay;
  base::TimeDelta stable_for;
};

class BuaDocumentService final
    : public content::DocumentService<mojom::BuaHost> {
 public:
  static void Create(content::RenderFrameHost* render_frame_host,
                     mojo::PendingReceiver<mojom::BuaHost> receiver);

  BuaDocumentService(const BuaDocumentService&) = delete;
  BuaDocumentService& operator=(const BuaDocumentService&) = delete;
  ~BuaDocumentService() override;

  void Request(const std::string& method,
               const std::string& request_json,
               RequestCallback callback) override;

 private:
  BuaDocumentService(content::RenderFrameHost& render_frame_host,
                     mojo::PendingReceiver<mojom::BuaHost> receiver);

  bool IsRequestAllowed() const;
  content::WebContents* GetWebContents() const;
  tabs::TabInterface* GetRequestingTab() const;
  tabs::TabInterface* GetDefaultTargetTab() const;
  glic::GlicKeyedService* GetGlicService() const;
  actor::ActorKeyedService* GetActorService() const;

  void HandleSnapshot(const base::DictValue& request, RequestCallback callback);
  void OnSnapshot(
      RequestCallback callback,
      std::string snapshot_id,
      std::string snapshot_mode,
      int generation,
      int max_nodes,
      base::expected<glic::mojom::GetContextResultPtr,
                     page_content_annotations::FetchPageContextErrorDetails>
          result);

  void HandleAct(const base::DictValue& request, RequestCallback callback);
  void HandleWaitAction(const base::DictValue& action,
                        RequestCallback callback);
  void HandleReloadAction(const base::DictValue& action,
                          tabs::TabInterface* default_tab,
                          RequestCallback callback);
  void HandleMiddleClickAction(const base::DictValue& action,
                               tabs::TabInterface* default_tab,
                               RequestCallback callback);
  void PollWaitCondition(BuaWaitSpec spec,
                         RequestCallback callback,
                         base::TimeTicks start_time,
                         base::TimeTicks deadline,
                         base::TimeTicks stable_since);
  void OnWaitSnapshot(
      BuaWaitSpec spec,
      RequestCallback callback,
      base::TimeTicks start_time,
      base::TimeTicks deadline,
      base::TimeTicks stable_since,
      base::expected<glic::mojom::GetContextResultPtr,
                     page_content_annotations::FetchPageContextErrorDetails>
          result);
  void OnActionsFinished(
      RequestCallback callback,
      std::vector<std::string> action_ids,
      base::TimeTicks start_time,
      std::vector<actor::ActionResultWithLatencyInfo> action_results,
      actor::TabObservationStrategy observation_strategy);

  std::optional<actor::TaskId> EnsureActorTask(std::string* error_json);
  void StopActorTask(actor::ActorTask::StoppedReason reason);

  std::string session_id_;
  std::string task_status_ = "idle";
  std::string task_reason_;
  std::optional<actor::TaskId> actor_task_id_;
  int snapshot_generation_ = 0;
  std::map<std::string, int> document_identifier_to_short_id_;
  std::map<int, std::string> short_document_id_to_identifier_;
  int next_short_document_id_ = 1;

  base::WeakPtrFactory<BuaDocumentService> weak_ptr_factory_{this};
};

}  // namespace bua

#endif  // CHROME_BROWSER_BUA_BUA_DOCUMENT_SERVICE_H_
