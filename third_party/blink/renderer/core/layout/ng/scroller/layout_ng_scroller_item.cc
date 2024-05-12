// Copyright 2017 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "third_party/blink/renderer/core/layout/ng/scroller/layout_ng_scroller_item.h"
#include "third_party/blink/renderer/core/layout/layout_object.h"

#include "third_party/blink/renderer/core/layout/layout_view.h"
#include "third_party/blink/renderer/core/layout/list_marker.h"
#include "third_party/blink/renderer/core/layout/ng/legacy_layout_tree_walking.h"

namespace blink {

LayoutNGScrollerItem::LayoutNGScrollerItem(Element* element)
    : LayoutNGBlockFlow(element) {
  SetInline(false);
  SetFloating(false);
  SetPositionState(EPosition::kStatic);
  SetCanContainAbsolutePositionObjects(false);
  SetCanContainFixedPositionObjects(false);

  // SetConsumesSubtreeChangeNotification();
  // RegisterSubtreeChangeListenerOnDescendants(true);
}

bool LayoutNGScrollerItem::IsOfType(LayoutObjectType type) const {
  return type == kLayoutObjectNGScrollerItem ||
         LayoutNGBlockFlow::IsOfType(type);
}

void LayoutNGScrollerItem::StyleDidChange(StyleDifference diff,
                                          const ComputedStyle* old_style) {
  LayoutBlockFlow::StyleDidChange(diff, old_style);
  // TODO(keyou): Force position to be kStatic.
  SetPositionState(EPosition::kStatic);

  // TODO(keyou): 需要使 ScrollerItem 的子节点的绝对/相对布局方式无效
  SetInline(false);
  SetFloating(false);
  SetCanContainAbsolutePositionObjects(false);
  SetCanContainFixedPositionObjects(false);
}

}  // namespace blink
