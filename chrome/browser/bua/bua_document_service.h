// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_BROWSER_BUA_BUA_DOCUMENT_SERVICE_H_
#define CHROME_BROWSER_BUA_BUA_DOCUMENT_SERVICE_H_

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
}

namespace glic {
class GlicKeyedService;
}

namespace tabs {
class TabInterface;
}

namespace bua {

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
  tabs::TabInterface* GetCurrentTab() const;
  glic::GlicKeyedService* GetGlicService() const;
  actor::ActorKeyedService* GetActorService() const;

  void HandleSnapshot(const base::DictValue& request, RequestCallback callback);
  void OnSnapshot(
      RequestCallback callback,
      std::string snapshot_id,
      int generation,
      int max_nodes,
      base::expected<
          glic::mojom::GetContextResultPtr,
          page_content_annotations::FetchPageContextErrorDetails> result);

  void HandleAct(const base::DictValue& request, RequestCallback callback);
  void OnActionsFinished(
      RequestCallback callback,
      std::vector<std::string> action_ids,
      base::TimeTicks start_time,
      std::vector<actor::ActionResultWithLatencyInfo> action_results,
      actor::TabObservationStrategy observation_strategy);

  std::optional<actor::TaskId> EnsureActorTask(std::string* error_json);
  void StopActorTask(actor::ActorTask::StoppedReason reason);

  std::string session_id_;
  std::optional<actor::TaskId> actor_task_id_;
  int snapshot_generation_ = 0;

  base::WeakPtrFactory<BuaDocumentService> weak_ptr_factory_{this};
};

}  // namespace bua

#endif  // CHROME_BROWSER_BUA_BUA_DOCUMENT_SERVICE_H_
