// Copyright 2017 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "third_party/blink/renderer/core/layout/layout_object.h"
#include "third_party/blink/renderer/core/layout/ng/scroller/layout_ng_scroller.h"

#include "third_party/blink/renderer/core/layout/layout_view.h"
#include "third_party/blink/renderer/core/layout/list_marker.h"
#include "third_party/blink/renderer/core/layout/ng/legacy_layout_tree_walking.h"

namespace blink {

LayoutNGScroller::LayoutNGScroller(Element* element)
    : LayoutNGBlockFlow(element) {
  SetInline(false);
  SetFloating(false);
  SetPositionState(EPosition::kStatic);
  SetCanContainAbsolutePositionObjects(false);
  SetCanContainFixedPositionObjects(false);

  // SetConsumesSubtreeChangeNotification();
  // RegisterSubtreeChangeListenerOnDescendants(true);
}

bool LayoutNGScroller::IsOfType(LayoutObjectType type) const {
  return type == kLayoutObjectNGScroller || LayoutNGBlockFlow::IsOfType(type);
}

}  // namespace blink
