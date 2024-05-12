// Copyright 2017 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef THIRD_PARTY_BLINK_RENDERER_CORE_LAYOUT_NG_SCROLLER_LAYOUT_NG_SCROLLER_ITEM_H_
#define THIRD_PARTY_BLINK_RENDERER_CORE_LAYOUT_NG_SCROLLER_LAYOUT_NG_SCROLLER_ITEM_H_

#include "third_party/blink/renderer/core/core_export.h"
#include "third_party/blink/renderer/core/html/list_item_ordinal.h"
#include "third_party/blink/renderer/core/layout/ng/layout_ng_block_flow.h"

namespace blink {

// A LayoutObject subclass for 'display: list-item' in LayoutNG.
class CORE_EXPORT LayoutNGScrollerItem final : public LayoutNGBlockFlow {
 public:
  explicit LayoutNGScrollerItem(Element*);

  const char* GetName() const override {
    NOT_DESTROYED();
    return "LayoutNGScrollerItem";
  }

  void StyleDidChange(StyleDifference diff,
                      const ComputedStyle* old_style) override;

 private:
  bool IsOfType(LayoutObjectType) const override;
};

template <>
struct DowncastTraits<LayoutNGScrollerItem> {
  static bool AllowFrom(const LayoutObject& object) {
    return object.IsLayoutNGScrollerItem();
  }
};

}  // namespace blink

#endif  // THIRD_PARTY_BLINK_RENDERER_CORE_LAYOUT_NG_SCROLLER_LAYOUT_NG_SCROLLER_ITEM_H_
