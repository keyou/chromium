// Copyright 2015 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "third_party/blink/renderer/core/html/scroller/html_scroller_element.h"

#include <memory>

#include "gtest/gtest.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "third_party/blink/renderer/core/css/css_property_value_set.h"
#include "third_party/blink/renderer/core/css/parser/css_parser.h"
#include "third_party/blink/renderer/core/css/resolver/style_resolver.h"
#include "third_party/blink/renderer/core/dom/document.h"
#include "third_party/blink/renderer/core/execution_context/security_context.h"
#include "third_party/blink/renderer/core/frame/local_frame_view.h"
#include "third_party/blink/renderer/core/html/html_div_element.h"
#include "third_party/blink/renderer/core/html/html_object_element.h"
#include "third_party/blink/renderer/core/style/computed_style.h"
#include "third_party/blink/renderer/core/testing/page_test_base.h"
#include "ui/gfx/geometry/size.h"

namespace blink {

class HTMLScrollerElementTest : public PageTestBase {
 protected:
  static constexpr int kViewportWidth = 500;
  static constexpr int kViewportHeight = 600;

  void SetUp() override {
    PageTestBase::SetUp(gfx::Size(kViewportWidth, kViewportHeight));
  }
};

// Instantiate class constants. Not needed after C++17.
constexpr int HTMLScrollerElementTest::kViewportWidth;
constexpr int HTMLScrollerElementTest::kViewportHeight;

TEST_F(HTMLScrollerElementTest, scroller) {
  // Load <object> element with a <embed> child.
  // This can be seen on sites with Flash cookies,
  // for example on www.yandex.ru
  SetHtmlInnerHTML(R"HTML(
    <div id="divId" style="height: 100px;"></div>
    <scroller id="scrollerId" style="height: 100px;"></scroller>
  )HTML");

  auto* div_element = GetElementById("divId");
  auto* scroller_element = GetElementById("scrollerId");
  ASSERT_TRUE(div_element);
  ASSERT_TRUE(scroller_element);
  auto* div = To<HTMLDivElement>(div_element);
  auto* scroller = To<HTMLScrollerElement>(scroller_element);

  EXPECT_EQ(div->GetComputedStyle()->Display(),
            scroller->GetComputedStyle()->Display());

  auto div_bounding_rect = div->GetBoundingClientRectNoLifecycleUpdate();
  auto scroller_bounding_rect =
      scroller->GetBoundingClientRectNoLifecycleUpdate();
  EXPECT_EQ(div_bounding_rect.size(), scroller_bounding_rect.size());

  // scoped_refptr<ComputedStyle> initial_style =
  //     GetDocument().GetStyleResolver().InitialStyleForElement();

  // // We should get |true| as a result and don't trigger a DCHECK.
  // EXPECT_TRUE(
  //     static_cast<Element*>(scroller)->LayoutObjectIsNeeded(*initial_style));

  // At this moment updatePlugin() function is not called, so
  // useFallbackContent() will return false.
  // But the element will likely to use fallback content after updatePlugin().
  // EXPECT_TRUE(object->HasFallbackContent());
  // EXPECT_FALSE(object->UseFallbackContent());
  // EXPECT_TRUE(object->WillUseFallbackContentAtLayout());

  // auto* embed_element = GetElementById("fce");
  // ASSERT_TRUE(embed_element);
  // auto* embed = To<HTMLEmbedElement>(embed_element);

  // UpdateAllLifecyclePhasesForTest();

  // scoped_refptr<ComputedStyle> initial_style =
  //     GetDocument().GetStyleResolver().InitialStyleForElement();

  // // We should get |true| as a result and don't trigger a DCHECK.
  // EXPECT_TRUE(
  //     static_cast<Element*>(embed)->LayoutObjectIsNeeded(*initial_style));

  // // This call will update fallback state of the object.
  // object->UpdatePlugin();

  // EXPECT_TRUE(object->HasFallbackContent());
  // EXPECT_TRUE(object->UseFallbackContent());
  // EXPECT_TRUE(object->WillUseFallbackContentAtLayout());

  // UpdateAllLifecyclePhasesForTest();
  // EXPECT_TRUE(
  //     static_cast<Element*>(embed)->LayoutObjectIsNeeded(*initial_style));

  // auto* image = MakeGarbageCollected<HTMLScrollerElement>(GetDocument());
  // image->setAttribute(html_names::kWidthAttr, "400");
  // TODO(yoav): `width` does not impact resourceWidth until we resolve
  // https://github.com/ResponsiveImagesCG/picture-element/issues/268
  // EXPECT_EQ(500, image->GetResourceWidth().width);
  // image->setAttribute(html_names::kSizesAttr, "100vw");
  // EXPECT_EQ(500, image->GetResourceWidth().width);
}

TEST_F(HTMLScrollerElementTest, scroller_style) {
  LOG(ERROR) << "keyou: SetHtmlInnerHTML";
  // Load <object> element with a <embed> child.
  // This can be seen on sites with Flash cookies,
  // for example on www.yandex.ru
  SetHtmlInnerHTML(R"HTML(
    <scroller id="scrollerId" style="-keyou-layout: -keyou-layout2; app-region: drag;"></scroller>
  )HTML");

  auto* scroller_element = GetElementById("scrollerId");
  ASSERT_TRUE(scroller_element);
  auto* scroller = To<HTMLScrollerElement>(scroller_element);
  auto* computed_style = scroller->GetComputedStyle();
  EXPECT_EQ(computed_style->KeyouLayout(), EKeyouLayout::kKeyouLayout2);
  EXPECT_EQ(computed_style->DraggableRegionMode(), EDraggableRegionMode::kDrag);
}

}  // namespace blink
