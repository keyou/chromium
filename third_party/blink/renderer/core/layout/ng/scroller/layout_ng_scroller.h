// Copyright 2017 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef THIRD_PARTY_BLINK_RENDERER_CORE_LAYOUT_NG_SCROLLER_LAYOUT_NG_SCROLLER_H_
#define THIRD_PARTY_BLINK_RENDERER_CORE_LAYOUT_NG_SCROLLER_LAYOUT_NG_SCROLLER_H_

#include "third_party/blink/renderer/core/core_export.h"
#include "third_party/blink/renderer/core/html/list_item_ordinal.h"
#include "third_party/blink/renderer/core/layout/ng/layout_ng_block_flow.h"

namespace blink {

// A LayoutObject subclass for 'display: list-item' in LayoutNG.
class CORE_EXPORT LayoutNGScroller final : public LayoutNGBlockFlow {
 public:
  explicit LayoutNGScroller(Element*);

  const char* GetName() const override {
    NOT_DESTROYED();
    return "LayoutNGScroller";
  }

 private:
  bool IsOfType(LayoutObjectType) const override;
};

template <>
struct DowncastTraits<LayoutNGScroller> {
  static bool AllowFrom(const LayoutObject& object) {
    return object.IsLayoutNGScroller();
  }
};

}  // namespace blink

#endif  // THIRD_PARTY_BLINK_RENDERER_CORE_LAYOUT_NG_SCROLLER_LAYOUT_NG_SCROLLER_H_
