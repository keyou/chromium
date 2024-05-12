// Copyright 2016 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "third_party/blink/renderer/core/layout/ng/scroller/scroller_layout_algorithm.h"
#include "gtest/gtest.h"
#include "third_party/blink/renderer/core/layout/ng/ng_block_layout_algorithm.h"

#include "testing/gmock/include/gmock/gmock.h"
#include "third_party/blink/renderer/core/dom/dom_token_list.h"
#include "third_party/blink/renderer/core/dom/node_computed_style.h"
#include "third_party/blink/renderer/core/dom/tag_collection.h"
#include "third_party/blink/renderer/core/layout/ng/layout_ng_block_flow.h"
#include "third_party/blink/renderer/core/layout/ng/ng_base_layout_algorithm_test.h"
#include "third_party/blink/renderer/core/layout/ng/ng_block_break_token.h"
#include "third_party/blink/renderer/core/layout/ng/ng_block_node.h"
#include "third_party/blink/renderer/core/layout/ng/ng_constraint_space.h"
#include "third_party/blink/renderer/core/layout/ng/ng_constraint_space_builder.h"
#include "third_party/blink/renderer/core/layout/ng/ng_layout_result.h"
#include "third_party/blink/renderer/core/layout/ng/ng_length_utils.h"
#include "third_party/blink/renderer/core/layout/ng/ng_physical_box_fragment.h"
#include "third_party/blink/renderer/core/layout/ng/ng_physical_fragment.h"
#include "third_party/blink/renderer/core/style/computed_style.h"
#include "third_party/blink/renderer/core/testing/core_unit_test_helper.h"
#include "third_party/blink/renderer/platform/geometry/layout_rect.h"
#include "third_party/blink/renderer/platform/testing/runtime_enabled_features_test_helpers.h"

namespace blink {
namespace {

using testing::ElementsAre;
using testing::Pointee;

class ScrollerLayoutAlgorithmTest : public NGBaseLayoutAlgorithmTest {
 protected:
  void SetUp() override { NGBaseLayoutAlgorithmTest::SetUp(); }

  MinMaxSizes RunComputeMinMaxSizes(NGBlockNode node) {
    // The constraint space is not used for min/max computation, but we need
    // it to create the algorithm.
    NGConstraintSpace space = ConstructBlockLayoutTestConstraintSpace(
        {WritingMode::kHorizontalTb, TextDirection::kLtr},
        LogicalSize(LayoutUnit(), LayoutUnit()));
    NGFragmentGeometry fragment_geometry = CalculateInitialFragmentGeometry(
        space, node, /* break_token */ nullptr, /* is_intrinsic */ true);

    NGBlockLayoutAlgorithm algorithm({node, fragment_geometry, space});
    return algorithm.ComputeMinMaxSizes(MinMaxSizesFloatInput()).sizes;
  }

  const NGLayoutResult* RunCachedLayoutResult(const NGConstraintSpace& space,
                                              const NGBlockNode& node) {
    NGLayoutCacheStatus cache_status;
    absl::optional<NGFragmentGeometry> initial_fragment_geometry;
    return To<LayoutBlockFlow>(node.GetLayoutBox())
        ->CachedLayoutResult(space, nullptr, nullptr, nullptr,
                             &initial_fragment_geometry, &cache_status);
  }

  String DumpFragmentTree(const NGPhysicalBoxFragment* fragment) {
    NGPhysicalFragment::DumpFlags flags =
        NGPhysicalFragment::DumpHeaderText | NGPhysicalFragment::DumpSubtree |
        NGPhysicalFragment::DumpIndentation | NGPhysicalFragment::DumpOffset |
        NGPhysicalFragment::DumpSize;

    return fragment->DumpFragmentTree(flags);
  }

  template <typename UpdateFunc>
  void UpdateStyleForElement(Element* element, const UpdateFunc& update) {
    auto* layout_object = element->GetLayoutObject();
    ComputedStyleBuilder builder(layout_object->StyleRef());
    update(builder);
    layout_object->SetStyle(builder.TakeStyle(),
                            LayoutObject::ApplyStyleChanges::kNo);
    layout_object->SetNeedsLayout("");
  }
};

TEST_F(ScrollerLayoutAlgorithmTest, FixedSize) {
  SetBodyInnerHTML(R"HTML(
    <div id="box" style="width:30px; height:40px"></div>
  )HTML");

  NGConstraintSpace space = ConstructBlockLayoutTestConstraintSpace(
      {WritingMode::kHorizontalTb, TextDirection::kLtr},
      LogicalSize(LayoutUnit(100), kIndefiniteSize));

  NGBlockNode box(GetLayoutBoxByElementId("box"));

  const NGPhysicalBoxFragment* fragment = RunBlockLayoutAlgorithm(box, space);

  EXPECT_EQ(PhysicalSize(30, 40), fragment->Size());
}

// Verifies that two children are laid out with the correct size and position.
TEST_F(ScrollerLayoutAlgorithmTest, ScrollerLayout) {
  SetBodyInnerHTML(R"HTML(
    <scroller id="container" style="display: -keyou-dynamic-block;width: 30px;height:40px;">
      <div style="-keyou-layout: -keyou-layout1;height: 20px; position: absolute; left: 100px;">
      </div>
      <div style="-keyou-layout: -keyou-layout2;height: 30px; margin-top: 5px; margin-bottom: 20px">
      </div>
    </scroller>
  )HTML");
  const int kWidth = 30;
  const int kHeight1 = 20;
  const int kHeight2 = 30;
  const int kMarginTop = 5;

  NGConstraintSpace space = ConstructBlockLayoutTestConstraintSpace(
      {WritingMode::kHorizontalTb, TextDirection::kLtr},
      LogicalSize(LayoutUnit(100), kIndefiniteSize));
  NGBlockNode box(GetDocument().body()->GetLayoutBox());

  const NGPhysicalBoxFragment* fragment = RunBlockLayoutAlgorithm(box, space);
  // EXPECT_EQ(PhysicalSize(84, 55), fragment->Size());
  ASSERT_EQ(1u, fragment->Children().size());
  fragment = To<NGPhysicalBoxFragment>(fragment->Children()[0].get());
  // EXPECT_EQ(PhysicalSize(50, 22), fragment->Size());
  // ASSERT_EQ(1u, fragment->Children().size());
  // EXPECT_EQ(PhysicalSize(29, 22), fragment->Children()[0]->Size());

  // NGBlockNode container(GetLayoutBoxByElementId("container"));
  // NGConstraintSpace space = ConstructBlockLayoutTestConstraintSpace(
  //     {WritingMode::kHorizontalTb, TextDirection::kLtr},
  //     LogicalSize(LayoutUnit(100), kIndefiniteSize));

  // const NGPhysicalBoxFragment* fragment = RunBlockLayoutAlgorithm(box,
  // space);

  EXPECT_EQ(LayoutUnit(kWidth), fragment->Size().width);
  EXPECT_EQ(LayoutUnit(kHeight1 + kHeight2 + kMarginTop),
            fragment->Size().height);
  EXPECT_EQ(NGPhysicalFragment::kFragmentBox, fragment->Type());
  ASSERT_EQ(fragment->Children().size(), 2UL);

  const NGLink& first_child = fragment->Children()[0];
  EXPECT_EQ(kHeight1, first_child->Size().height);
  EXPECT_EQ(0, first_child.Offset().top);

  const NGLink& second_child = fragment->Children()[1];
  EXPECT_EQ(kHeight2, second_child->Size().height);
  EXPECT_EQ(kHeight1 + kMarginTop, second_child.Offset().top);
}

TEST_F(ScrollerLayoutAlgorithmTest, ScrollerLayoutWithScroll) {
  SetBodyInnerHTML(R"HTML(
    <scroller id="container" style="display: -keyou-dynamic-block;width: 30px;height:40px; overflow: auto;
    padding-top: 100px;
    ">
      <div style="-keyou-layout: -keyou-layout1;height: 20px; position: absolute; left: 100px;">
      </div>
      <div style="-keyou-layout: -keyou-layout2;height: 30px; margin-top: 5px; margin-bottom: 20px">
      </div>
    </scroller>
  )HTML");

  const int kHeight = 40;
  const int kWidth = 30;
  const int kHeight1 = 20;
  const int kHeight2 = 30;
  const int kMarginTop = 5;
  const int kMarginBottom = 20;

  NGConstraintSpace space = ConstructBlockLayoutTestConstraintSpace(
      {WritingMode::kHorizontalTb, TextDirection::kLtr},
      LogicalSize(LayoutUnit(100), kIndefiniteSize));
  NGBlockNode box(GetDocument().body()->GetLayoutBox());

  const NGPhysicalBoxFragment* fragment = RunBlockLayoutAlgorithm(box, space);
  // EXPECT_EQ(PhysicalSize(84, 55), fragment->Size());
  ASSERT_EQ(1u, fragment->Children().size());
  fragment = To<NGPhysicalBoxFragment>(fragment->Children()[0].get());

  auto* element = GetDocument().getElementById("container");
  auto* layout_box = element->GetLayoutBox();
  EXPECT_TRUE(layout_box->PhysicalFragmentCount() == 1);
  EXPECT_EQ(true, layout_box->HasNonVisibleOverflow());
  EXPECT_EQ(true, layout_box->IsScrollContainer());
  EXPECT_EQ(true, layout_box->ScrollsOverflowY());
  EXPECT_EQ(true, layout_box->ScrollsOverflowX());
  EXPECT_EQ(true, layout_box->HasScrollableOverflowY());
  EXPECT_EQ(false, layout_box->HasScrollableOverflowX());

  auto kScrollHeight = kHeight1 + kHeight2 + kMarginTop + kMarginBottom;
  EXPECT_EQ(LayoutUnit(kScrollHeight), layout_box->ScrollHeight());
  EXPECT_EQ(LayoutRect(0, 0, kWidth, kScrollHeight),
            layout_box->LayoutOverflowRect());
  EXPECT_EQ(LayoutRect(0, 0, kWidth, kHeight),
            layout_box->VisualOverflowRect());

  EXPECT_EQ(NGPhysicalFragment::kFragmentBox, fragment->Type());
  ASSERT_EQ(fragment->Children().size(), 2UL);

  const NGLink& first_child = fragment->Children()[0];
  EXPECT_EQ(kHeight1, first_child->Size().height);
  EXPECT_EQ(0, first_child.Offset().top);

  const NGLink& second_child = fragment->Children()[1];
  EXPECT_EQ(kHeight2, second_child->Size().height);
  EXPECT_EQ(kHeight1 + kMarginTop, second_child.Offset().top);
}

TEST_F(ScrollerLayoutAlgorithmTest, BlockLayoutWithScroll) {
  SetBodyInnerHTML(R"HTML(
    <div id="container" style="width: 30px; height: 40px; overflow: auto;">
      <div style="height: 20px">
      </div>
      <div style="height: 30px; margin-top: 5px; margin-bottom: 20px">
      </div>
    </div>
  )HTML");
  const int kHeight = 40;
  const int kWidth = 30;
  const int kHeight1 = 20;
  const int kHeight2 = 30;
  const int kMarginTop = 5;
  const int kMarginBottom = 20;

  NGConstraintSpace space = ConstructBlockLayoutTestConstraintSpace(
      {WritingMode::kHorizontalTb, TextDirection::kLtr},
      LogicalSize(LayoutUnit(100), kIndefiniteSize));
  NGBlockNode box(GetDocument().body()->GetLayoutBox());

  const NGPhysicalBoxFragment* fragment = RunBlockLayoutAlgorithm(box, space);
  // EXPECT_EQ(PhysicalSize(84, 55), fragment->Size());
  ASSERT_EQ(1u, fragment->Children().size());
  fragment = To<NGPhysicalBoxFragment>(fragment->Children()[0].get());

  auto* element = GetDocument().getElementById("container");
  auto* layout_box = element->GetLayoutBox();
  EXPECT_TRUE(layout_box->PhysicalFragmentCount() == 1);
  EXPECT_EQ(true, layout_box->HasNonVisibleOverflow());
  EXPECT_EQ(true, layout_box->IsScrollContainer());
  EXPECT_EQ(true, layout_box->ScrollsOverflowY());
  EXPECT_EQ(true, layout_box->ScrollsOverflowX());
  EXPECT_EQ(true, layout_box->HasScrollableOverflowY());
  EXPECT_EQ(false, layout_box->HasScrollableOverflowX());

  auto kScrollHeight = kHeight1 + kHeight2 + kMarginTop + kMarginBottom;
  EXPECT_EQ(LayoutUnit(kScrollHeight), layout_box->ScrollHeight());
  EXPECT_EQ(LayoutRect(0, 0, kWidth, kScrollHeight),
            layout_box->LayoutOverflowRect());
  EXPECT_EQ(LayoutRect(0, 0, kWidth, kHeight),
            layout_box->VisualOverflowRect());

  EXPECT_EQ(NGPhysicalFragment::kFragmentBox, fragment->Type());
  ASSERT_EQ(fragment->Children().size(), 2UL);

  const NGLink& first_child = fragment->Children()[0];
  EXPECT_EQ(kHeight1, first_child->Size().height);
  EXPECT_EQ(0, first_child.Offset().top);

  const NGLink& second_child = fragment->Children()[1];
  EXPECT_EQ(kHeight2, second_child->Size().height);
  EXPECT_EQ(kHeight1 + kMarginTop, second_child.Offset().top);
}

TEST_F(ScrollerLayoutAlgorithmTest, BlockLayout) {
  // SetBodyInnerHTML(R"HTML(
  //   <div style="display: flex; flex-direction: column; width: 50px">
  //     <svg width="29" height="22" style="width: auto; height: auto;
  //                                        margin: auto"></svg>
  //   </div>
  // )HTML");

  SetBodyInnerHTML(R"HTML(
    <div id="container" style="width: 30px">
      <div style="height: 20px">
      </div>
      <div style="height: 30px; margin-top: 5px; margin-bottom: 20px">
      </div>
    </div>
  )HTML");
  const int kWidth = 30;
  const int kHeight1 = 20;
  const int kHeight2 = 30;
  const int kMarginTop = 5;

  NGConstraintSpace space = ConstructBlockLayoutTestConstraintSpace(
      {WritingMode::kHorizontalTb, TextDirection::kLtr},
      LogicalSize(LayoutUnit(100), kIndefiniteSize));
  NGBlockNode box(GetDocument().body()->GetLayoutBox());

  const NGPhysicalBoxFragment* fragment = RunBlockLayoutAlgorithm(box, space);
  // EXPECT_EQ(PhysicalSize(84, 55), fragment->Size());
  ASSERT_EQ(1u, fragment->Children().size());
  fragment = To<NGPhysicalBoxFragment>(fragment->Children()[0].get());

  // NGBlockNode container(GetLayoutBoxByElementId("container"));
  // NGConstraintSpace space = ConstructBlockLayoutTestConstraintSpace(
  //     {WritingMode::kHorizontalTb, TextDirection::kLtr},
  //     LogicalSize(LayoutUnit(100), kIndefiniteSize));

  // const NGPhysicalBoxFragment* fragment = RunBlockLayoutAlgorithm(box,
  // space);

  EXPECT_EQ(LayoutUnit(kWidth), fragment->Size().width);
  EXPECT_EQ(LayoutUnit(kHeight1 + kHeight2 + kMarginTop),
            fragment->Size().height);
  EXPECT_EQ(NGPhysicalFragment::kFragmentBox, fragment->Type());
  ASSERT_EQ(fragment->Children().size(), 2UL);

  const NGLink& first_child = fragment->Children()[0];
  EXPECT_EQ(kHeight1, first_child->Size().height);
  EXPECT_EQ(0, first_child.Offset().top);

  const NGLink& second_child = fragment->Children()[1];
  EXPECT_EQ(kHeight2, second_child->Size().height);
  EXPECT_EQ(kHeight1 + kMarginTop, second_child.Offset().top);
}

}  // namespace
}  // namespace blink
