// Copyright 2016 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "third_party/blink/renderer/core/layout/ng/scroller/scroller_layout_algorithm.h"

#include <algorithm>
#include <memory>
#include <utility>

#include "base/check_op.h"
#include "third_party/abseil-cpp/absl/types/optional.h"
#include "third_party/blink/renderer/core/dom/node.h"
#include "third_party/blink/renderer/core/frame/web_feature.h"
#include "third_party/blink/renderer/core/html/forms/html_input_element.h"
#include "third_party/blink/renderer/core/layout/deferred_shaping.h"
#include "third_party/blink/renderer/core/layout/layout_multi_column_flow_thread.h"
#include "third_party/blink/renderer/core/layout/layout_object.h"
#include "third_party/blink/renderer/core/layout/ng/inline/ng_inline_cursor.h"
#include "third_party/blink/renderer/core/layout/ng/inline/ng_inline_node.h"
#include "third_party/blink/renderer/core/layout/ng/inline/ng_physical_line_box_fragment.h"
#include "third_party/blink/renderer/core/layout/ng/legacy_layout_tree_walking.h"
#include "third_party/blink/renderer/core/layout/ng/list/ng_unpositioned_list_marker.h"
#include "third_party/blink/renderer/core/layout/ng/ng_block_child_iterator.h"
#include "third_party/blink/renderer/core/layout/ng/ng_block_layout_algorithm_utils.h"
#include "third_party/blink/renderer/core/layout/ng/ng_box_fragment.h"
#include "third_party/blink/renderer/core/layout/ng/ng_box_fragment_builder.h"
#include "third_party/blink/renderer/core/layout/ng/ng_column_spanner_path.h"
#include "third_party/blink/renderer/core/layout/ng/ng_constraint_space.h"
#include "third_party/blink/renderer/core/layout/ng/ng_constraint_space_builder.h"
#include "third_party/blink/renderer/core/layout/ng/ng_early_break.h"
#include "third_party/blink/renderer/core/layout/ng/ng_floats_utils.h"
#include "third_party/blink/renderer/core/layout/ng/ng_fragment.h"
#include "third_party/blink/renderer/core/layout/ng/ng_fragmentation_utils.h"
#include "third_party/blink/renderer/core/layout/ng/ng_layout_result.h"
#include "third_party/blink/renderer/core/layout/ng/ng_length_utils.h"
#include "third_party/blink/renderer/core/layout/ng/ng_out_of_flow_layout_part.h"
#include "third_party/blink/renderer/core/layout/ng/ng_physical_box_fragment.h"
#include "third_party/blink/renderer/core/layout/ng/ng_positioned_float.h"
#include "third_party/blink/renderer/core/layout/ng/ng_space_utils.h"
#include "third_party/blink/renderer/core/layout/ng/ng_unpositioned_float.h"
#include "third_party/blink/renderer/core/layout/ng/table/ng_table_layout_algorithm_utils.h"
#include "third_party/blink/renderer/core/mathml/mathml_element.h"
#include "third_party/blink/renderer/core/mathml_names.h"
#include "third_party/blink/renderer/core/style/computed_style.h"
#include "third_party/blink/renderer/platform/geometry/layout_unit.h"

namespace blink {
namespace {

bool HasLineEvenIfEmpty(LayoutBox* box) {
  // Note: We should reduce calling |LayoutBlock::HasLineIfEmpty()|, because
  // it calls slow function |IsRootEditableElement()|.
  LayoutBlockFlow* const block_flow = DynamicTo<LayoutBlockFlow>(box);
  if (!block_flow) {
    return false;
  }
  // Note: |block_flow->NeedsCollectInline()| is true after removing all
  // children from block[1].
  // [1] editing/inserting/insert_after_delete.html
  if (!GetLayoutObjectForFirstChildNode(block_flow)) {
    // Note: |block_flow->ChildrenInline()| can be both true or false:
    //  - true: just after construction, <div></div>
    //  - true: one of child is inline them remove all, <div>abc</div>
    //  - false: all children are block then remove all, <div><p></p></div>
    return block_flow->HasLineIfEmpty();
  }
  if (AreNGBlockFlowChildrenInline(block_flow)) {
    return block_flow->HasLineIfEmpty() &&
           NGInlineNode(block_flow).IsBlockLevel();
  }
  if (const auto* const flow_thread = block_flow->MultiColumnFlowThread()) {
    DCHECK(!flow_thread->ChildrenInline());
    for (const auto* child = flow_thread->FirstChild(); child;
         child = child->NextSibling()) {
      if (child->IsInline()) {
        // Note: |LayoutNGOutsideListMarker| is out-of-flow for the tree
        // building purpose in |LayoutBlockFlow::AddChild()|.
        // |MultiColumnRenderingTest.ListItem| reaches here.
        DCHECK(child->IsLayoutNGOutsideListMarker()) << child;
        return false;
      }
      if (!child->IsFloatingOrOutOfFlowPositioned()) {
        // We reach here when we have in-flow child.
        // <div style="columns: 3"><div style="float:left"><div></div></div>
        return false;
      }
    }
    // There are no children or all children are floating or out of flow
    // positioned.
    return block_flow->HasLineIfEmpty();
  }
  return false;
}

LogicalOffset CenterBlockChild(LogicalOffset offset,
                               LayoutUnit available_block_size,
                               LayoutUnit child_block_size) {
  if (available_block_size == child_block_size) {
    return offset;
  }
  // We don't clamp a negative difference to zero. We'd like to center the
  // child even if its taller than the container.
  LayoutUnit block_size_diff = available_block_size - child_block_size;
  offset.block_offset += block_size_diff / 2 + LayoutMod(block_size_diff, 2);
  return offset;
}

inline const NGLayoutResult* LayoutBlockChild(
    const NGConstraintSpace& space,
    const NGBreakToken* break_token,
    const NGEarlyBreak* early_break,
    const NGColumnSpannerPath* column_spanner_path,
    NGBlockNode* node) {
  const NGEarlyBreak* early_break_in_child = nullptr;
  if (UNLIKELY(early_break)) {
    early_break_in_child = EnterEarlyBreakInChild(*node, *early_break);
  }
  column_spanner_path = FollowColumnSpannerPath(column_spanner_path, *node);
  return node->Layout(space, To<NGBlockBreakToken>(break_token),
                      early_break_in_child, column_spanner_path);
}

inline const NGLayoutResult* LayoutInflow(
    const NGConstraintSpace& space,
    const NGBreakToken* break_token,
    const NGEarlyBreak* early_break,
    const NGColumnSpannerPath* column_spanner_path,
    NGLayoutInputNode* node,
    NGInlineChildLayoutContext* context) {
  if (auto* inline_node = DynamicTo<NGInlineNode>(node)) {
    return inline_node->Layout(space, break_token, column_spanner_path,
                               context);
  }
  return LayoutBlockChild(space, break_token, early_break, column_spanner_path,
                          To<NGBlockNode>(node));
}

NGAdjoiningObjectTypes ToAdjoiningObjectTypes(EClear clear) {
  switch (clear) {
    default:
      NOTREACHED();
      [[fallthrough]];
    case EClear::kNone:
      return kAdjoiningNone;
    case EClear::kLeft:
      return kAdjoiningFloatLeft;
    case EClear::kRight:
      return kAdjoiningFloatRight;
    case EClear::kBoth:
      return kAdjoiningFloatBoth;
  };
}

// Return true if a child is to be cleared past adjoining floats. These are
// floats that would otherwise (if 'clear' were 'none') be pulled down by the
// BFC block offset of the child. If the child is to clear floats, though, we
// obviously need separate the child from the floats and move it past them,
// since that's what clearance is all about. This means that if we have any such
// floats to clear, we know for sure that we get clearance, even before layout.
inline bool HasClearancePastAdjoiningFloats(
    NGAdjoiningObjectTypes adjoining_object_types,
    const ComputedStyle& child_style,
    const ComputedStyle& cb_style) {
  return ToAdjoiningObjectTypes(child_style.Clear(cb_style)) &
         adjoining_object_types;
}

// Adjust BFC block offset for clearance, if applicable. Return true of
// clearance was applied.
//
// Clearance applies either when the BFC block offset calculated simply isn't
// past all relevant floats, *or* when we have already determined that we're
// directly preceded by clearance.
//
// The latter is the case when we need to force ourselves past floats that would
// otherwise be adjoining, were it not for the predetermined clearance.
// Clearance inhibits margin collapsing and acts as spacing before the
// block-start margin of the child. It needs to be exactly what takes the
// block-start border edge of the cleared block adjacent to the block-end outer
// edge of the "bottommost" relevant float.
//
// We cannot reliably calculate the actual clearance amount at this point,
// because 1) this block right here may actually be a descendant of the block
// that is to be cleared, and 2) we may not yet have separated the margin before
// and after the clearance. None of this matters, though, because we know where
// to place this block if clearance applies: exactly at the ConstraintSpace's
// ClearanceOffset().
bool ApplyClearance(const NGConstraintSpace& constraint_space,
                    LayoutUnit* bfc_block_offset) {
  if (constraint_space.HasClearanceOffset() &&
      *bfc_block_offset < constraint_space.ClearanceOffset()) {
    *bfc_block_offset = constraint_space.ClearanceOffset();
    return true;
  }
  return false;
}

LayoutUnit LogicalFromBfcLineOffset(LayoutUnit child_bfc_line_offset,
                                    LayoutUnit parent_bfc_line_offset,
                                    LayoutUnit child_inline_size,
                                    LayoutUnit parent_inline_size,
                                    TextDirection direction) {
  // We need to respect the current text direction to calculate the logical
  // offset correctly.
  LayoutUnit relative_line_offset =
      child_bfc_line_offset - parent_bfc_line_offset;

  LayoutUnit inline_offset =
      direction == TextDirection::kLtr
          ? relative_line_offset
          : parent_inline_size - relative_line_offset - child_inline_size;

  return inline_offset;
}

LogicalOffset LogicalFromBfcOffsets(const NGBfcOffset& child_bfc_offset,
                                    const NGBfcOffset& parent_bfc_offset,
                                    LayoutUnit child_inline_size,
                                    LayoutUnit parent_inline_size,
                                    TextDirection direction) {
  LayoutUnit inline_offset = LogicalFromBfcLineOffset(
      child_bfc_offset.line_offset, parent_bfc_offset.line_offset,
      child_inline_size, parent_inline_size, direction);

  return {inline_offset,
          child_bfc_offset.block_offset - parent_bfc_offset.block_offset};
}

}  // namespace

ScrollerLayoutAlgorithm::ScrollerLayoutAlgorithm(
    const NGLayoutAlgorithmParams& params)
    : NGLayoutAlgorithm(params),
      previous_result_(params.previous_result),
      column_spanner_path_(params.column_spanner_path),
      fit_all_lines_(false),
      is_resuming_(IsBreakInside(params.break_token)),
      abort_when_bfc_block_offset_updated_(false),
      has_processed_first_child_(false),
      ignore_line_clamp_(false),
      is_line_clamp_context_(params.space.IsLineClampContext()),
      lines_until_clamp_(params.space.LinesUntilClamp()) {
  container_builder_.SetExclusionSpace(params.space.ExclusionSpace());

  // If this node has a column spanner inside, we'll force it to stay within the
  // current fragmentation flow, so that it doesn't establish a parallel flow,
  // even if it might have content that overflows into the next fragmentainer.
  // This way we'll prevent content that comes after the spanner from being laid
  // out *before* it.
  if (column_spanner_path_) {
    container_builder_.SetShouldForceSameFragmentationFlow();
  }

  child_percentage_size_ = CalculateChildPercentageSize(
      ConstraintSpace(), Node(), ChildAvailableSize());
  replaced_child_percentage_size_ = CalculateReplacedChildPercentageSize(
      ConstraintSpace(), Node(), ChildAvailableSize(), BorderScrollbarPadding(),
      BorderPadding());

  // If |this| is a list item, keep track of the unpositioned list marker in
  // |container_builder_|.
  // if (const NGBlockNode marker_node = Node().ListMarkerBlockNodeIfListItem())
  // {
  //   if (ShouldPlaceUnpositionedListMarker() &&
  //       !marker_node.ListMarkerOccupiesWholeLine() &&
  //       (!BreakToken() || BreakToken()->HasUnpositionedListMarker())) {
  //     container_builder_.SetUnpositionedListMarker(
  //         NGUnpositionedListMarker(marker_node));
  //   }
  // }

  LOG(ERROR) << "keyou: ctor: ChildAvailableSize: " << ChildAvailableSize()
             << ", ConstraintSpace: " << ConstraintSpace();
}

// Define the destructor here, so that we can forward-declare more in the
// header.
ScrollerLayoutAlgorithm::~ScrollerLayoutAlgorithm() = default;

MinMaxSizesResult ScrollerLayoutAlgorithm::ComputeMinMaxSizes(
    const MinMaxSizesFloatInput& float_input) {
  MinMaxSizes sizes;
  bool depends_on_block_constraints = false;

  return MinMaxSizesResult(sizes, depends_on_block_constraints);
}

const NGLayoutResult* ScrollerLayoutAlgorithm::Layout() {
  auto node = Node().ToString();
  LOG(ERROR) << "keyou: Layout: node: " << node.Utf8();
  auto* dom_node = Node().GetDOMNode();
  DCHECK(dom_node);
  auto node_str = Node().GetDOMNode()->ToTreeStringForThis().Utf8();
  auto* c_str = node_str.c_str();
  // LOG(ERROR) << "keyou: Layout: layout node: " << c_str;
  LOG(ERROR) << "keyou: Layout: ChildAvailableSize: " << ChildAvailableSize()
             << ", ConstraintSpace: " << ConstraintSpace();

  LayoutUnit content_edge = BorderScrollbarPadding().block_start;
  NGPreviousInflowPosition previous_inflow_position = {
      LayoutUnit(), ConstraintSpace().MarginStrut(),
      is_resuming_ ? LayoutUnit() : container_builder_.Padding().block_start,
      /* self_collapsing_child_had_clearance */ false};

  if (content_edge || is_resuming_ ||
      ConstraintSpace().IsNewFormattingContext()) {
    bool discard_subsequent_margins =
        previous_inflow_position.margin_strut.discard_margins && !content_edge;
    if (!ResolveBfcBlockOffset(&previous_inflow_position)) {
      // There should be no preceding content that depends on the BFC block
      // offset of a new formatting context block, and likewise when resuming
      // from a break token.
      DCHECK(!ConstraintSpace().IsNewFormattingContext());
      DCHECK(!is_resuming_);
      return container_builder_.Abort(NGLayoutResult::kBfcBlockOffsetResolved);
    }
    // Move to the content edge. This is where the first child should be placed.
    previous_inflow_position.logical_block_offset = content_edge;

    // If we resolved the BFC block offset now, the margin strut has been
    // reset. If margins are to be discarded, and this box would otherwise have
    // adjoining margins between its own margin and those subsequent content,
    // we need to make sure subsequent content discard theirs.
    if (discard_subsequent_margins) {
      previous_inflow_position.margin_strut.discard_margins = true;
    }
  }

#if DCHECK_IS_ON()
  // If this is a new formatting context, we should definitely be at the origin
  // here. If we're resuming from a break token (for a block that doesn't
  // establish a new formatting context), that may not be the case,
  // though. There may e.g. be clearance involved, or inline-start margins.
  if (ConstraintSpace().IsNewFormattingContext()) {
    DCHECK_EQ(*container_builder_.BfcBlockOffset(), LayoutUnit());
  }
  // If this is a new formatting context, or if we're resuming from a break
  // token, no margin strut must be lingering around at this point.
  if (ConstraintSpace().IsNewFormattingContext() || is_resuming_) {
    DCHECK(ConstraintSpace().MarginStrut().IsEmpty());
  }

  if (!container_builder_.BfcBlockOffset()) {
    // New formatting-contexts, and when we have a self-collapsing child
    // affected by clearance must already have their BFC block-offset resolved.
    DCHECK(!previous_inflow_position.self_collapsing_child_had_clearance);
    DCHECK(!ConstraintSpace().IsNewFormattingContext());
  }
#endif

  // If this node is a quirky container, (we are in quirks mode and either a
  // table cell or body), we set our margin strut to a mode where it only
  // considers non-quirky margins. E.g.
  // <body>
  //   <p></p>
  //   <div style="margin-top: 10px"></div>
  //   <h1>Hello</h1>
  // </body>
  // In the above example <p>'s & <h1>'s margins are ignored as they are
  // quirky, and we only consider <div>'s 10px margin.
  if (node_.IsQuirkyContainer()) {
    previous_inflow_position.margin_strut.is_quirky_container_start = true;
  }

  // Try to reuse line box fragments from cached fragments if possible.
  // When possible, this adds fragments to |container_builder_| and update
  // |previous_inflow_position| and |BreakToken()|.
  const NGInlineBreakToken* previous_inline_break_token = nullptr;
  NGInlineChildLayoutContext* inline_child_layout_context = nullptr;

  NGBlockChildIterator child_iterator(Node().FirstChild(), BreakToken());

  // If this layout is blocked by a display-lock, then we pretend this node has
  // no children and that there are no break tokens. Due to this, we skip layout
  // on these children.
  if (Node().ChildLayoutBlockedByDisplayLock()) {
    child_iterator = NGBlockChildIterator(NGBlockNode(nullptr), nullptr);
  }

  NGBlockNode ruby_text_child(nullptr);
  NGBlockNode placeholder_child(nullptr);
  for (auto entry = child_iterator.NextChild();
       NGLayoutInputNode child = entry.node;
       entry = child_iterator.NextChild(previous_inline_break_token)) {
    const NGBreakToken* child_break_token = entry.token;

    NGBlockNode block_child = To<NGBlockNode>(child);
    // if (child.IsOutOfFlowPositioned()) {
    //   container_builder_.AddOutOfFlowChildCandidate(
    //       block_child, BorderScrollbarPadding().StartOffset());
    //   continue;
    // }
    DCHECK(!child.IsOutOfFlowPositioned());

    NGLayoutResult::EStatus status;
    status =
        HandleInflow(child, child_break_token, &previous_inflow_position,
                     inline_child_layout_context, &previous_inline_break_token);
    DCHECK_EQ(status, NGLayoutResult::kSuccess);
    // const auto child_space = CreateConstraintSpaceForChild(
    //     Node(), ChildAvailableSize(), ConstraintSpace(), block_child);
    // const NGLayoutResult* child_layout_result =
    // block_child.Layout(child_space);

    // container_builder_.SetBaselines(fraction_ascent);

    // LogicalOffset child_offset;
    // if (child_layout_result->BfcBlockOffset() &&
    //     container_builder_.BfcBlockOffset()) {
    //   child_offset.inline_offset = child_layout_result->BfcLineOffset() -
    //                                ContainerBfcOffset().line_offset;
    //   child_offset.block_offset =
    //       child_layout_result->BfcBlockOffset().value() -
    //       ContainerBfcOffset().block_offset;
    // } else {
    //   child_offset.inline_offset = child_layout_result->BfcLineOffset() -
    //                                container_builder_.BfcLineOffset();
    // }

    // container_builder_.AddResult(*child_layout_result, child_offset);

    // const auto& physical_fragment = child_layout_result->PhysicalFragment();
    // LOG(ERROR) << "keyou: result: " << child.GetDOMNode()->ToString()
    //            << ", \nphysical_fragment: " << physical_fragment.ToString()
    //            << ", intrinsic_block_size_: "
    //            << child_layout_result->IntrinsicBlockSize().ToString()
    //            << ", BfcBlockOffset: "
    //            << child_layout_result->BfcBlockOffset()
    //                   .value_or(LayoutUnit(999))
    //                   .ToString();
  }

  // The intrinsic block size is not allowed to be less than the content edge
  // offset, as that could give us a negative content box size.
  intrinsic_block_size_ = content_edge;

  // To save space of the stack when we recurse into children, the rest of this
  // function is continued within |FinishLayout|. However it should be read as
  // one function.
  auto result =
      FinishLayout(&previous_inflow_position, inline_child_layout_context);

  // LayoutUnit intrinsic_block_size(kIndefiniteSize);
  // container_builder_.SetIntrinsicBlockSize(intrinsic_block_size);

  // LayoutUnit block_size(17);
  // container_builder_.SetBfcBlockOffset(LayoutUnit(8));
  // container_builder_.SetFragmentsTotalBlockSize(block_size);

  // auto* result = container_builder_.ToBoxFragment();
  const auto& physical_fragment = result->PhysicalFragment();
  LOG(ERROR) << "keyou: Layout: result: " << Node().GetDOMNode()->ToString()
             << ", \nphysical_fragment: " << physical_fragment.ToString()
             << ", intrinsic_block_size_: "
             << result->IntrinsicBlockSize().ToString() << ", BfcBlockOffset: "
             << result->BfcBlockOffset().value_or(LayoutUnit(999)).ToString();
  DCHECK_EQ(result->Status(), NGLayoutResult::kSuccess);
  return result;
}

const NGLayoutResult* ScrollerLayoutAlgorithm::FinishLayout(
    NGPreviousInflowPosition* previous_inflow_position,
    NGInlineChildLayoutContext* inline_child_layout_context) {
  LogicalSize border_box_size = container_builder_.InitialBorderBoxSize();
  NGMarginStrut end_margin_strut = previous_inflow_position->margin_strut;

  // Add line height for empty content editable or button with empty label, e.g.
  // <div contenteditable></div>, <input type="button" value="">
  if (container_builder_.HasSeenAllChildren() &&
      HasLineEvenIfEmpty(Node().GetLayoutBox())) {
    DCHECK(!111);
    intrinsic_block_size_ = std::max(
        intrinsic_block_size_, BorderScrollbarPadding().block_start +
                                   Node().EmptyLineBlockSize(BreakToken()));
    if (container_builder_.IsInitialColumnBalancingPass()) {
      container_builder_.PropagateTallestUnbreakableBlockSize(
          intrinsic_block_size_);
    }
    // Test [1][2] require baseline offset for empty editable.
    // [1] css3/flexbox/baseline-for-empty-line.html
    // [2] inline-block/contenteditable-baseline.html
    const LayoutBlock* const layout_block =
        To<LayoutBlock>(Node().GetLayoutBox());
    if (auto baseline_offset = layout_block->BaselineForEmptyLine(
            layout_block->IsHorizontalWritingMode() ? kHorizontalLine
                                                    : kVerticalLine)) {
      container_builder_.SetBaselines(*baseline_offset);
    }
  }

  // Collapse annotation overflow and padding.
  // logical_block_offset already contains block-end annotation overflow.
  // However, if the container has non-zero block-end padding, the annotation
  // can extend on the padding. So we decrease logical_block_offset by
  // shareable part of the annotation overflow and the padding.
  if (previous_inflow_position->block_end_annotation_space < LayoutUnit()) {
    const LayoutUnit annotation_overflow =
        -previous_inflow_position->block_end_annotation_space;
    previous_inflow_position->logical_block_offset -=
        std::min(container_builder_.Padding().block_end, annotation_overflow);
  }

  // If the current layout is a new formatting context, we need to encapsulate
  // all of our floats.
  if (ConstraintSpace().IsNewFormattingContext()) {
    intrinsic_block_size_ = std::max(
        intrinsic_block_size_,
        ExclusionSpace().ClearanceOffsetIncludingInitialLetter(EClear::kBoth));
  }

  // If line clamping occurred, the intrinsic block-size comes from the
  // intrinsic block-size at the time of the clamp.
  if (intrinsic_block_size_when_clamped_) {
    DCHECK(container_builder_.BfcBlockOffset());
    intrinsic_block_size_ = *intrinsic_block_size_when_clamped_ +
                            BorderScrollbarPadding().block_end;
    end_margin_strut = NGMarginStrut();
  } else if (BorderScrollbarPadding().block_end ||
             previous_inflow_position->self_collapsing_child_had_clearance ||
             ConstraintSpace().IsNewFormattingContext()) {
    // The end margin strut of an in-flow fragment contributes to the size of
    // the current fragment if:
    //  - There is block-end border/scrollbar/padding.
    //  - There was a self-collapsing child affected by clearance.
    //  - We are a new formatting context.
    // Additionally this fragment produces no end margin strut.

    if (!container_builder_.BfcBlockOffset()) {
      // If we have collapsed through the block start and all children (if any),
      // now is the time to determine the BFC block offset, because finally we
      // have found something solid to hang on to (like clearance or a bottom
      // border, for instance). If we're a new formatting context, though, we
      // shouldn't be here, because then the offset should already have been
      // determined.
      DCHECK(!ConstraintSpace().IsNewFormattingContext());
      if (!ResolveBfcBlockOffset(previous_inflow_position)) {
        return container_builder_.Abort(
            NGLayoutResult::kBfcBlockOffsetResolved);
      }
      DCHECK(container_builder_.BfcBlockOffset());
    } else {
      // If we are a quirky container, we ignore any quirky margins and just
      // consider normal margins to extend our size.  Other UAs perform this
      // calculation differently, e.g. by just ignoring the *last* quirky
      // margin.
      LayoutUnit margin_strut_sum = node_.IsQuirkyContainer()
                                        ? end_margin_strut.QuirkyContainerSum()
                                        : end_margin_strut.Sum();

      if (ConstraintSpace().HasKnownFragmentainerBlockSize()) {
        LayoutUnit new_margin_strut_sum = AdjustedMarginAfterFinalChildFragment(
            ConstraintSpace(), previous_inflow_position->logical_block_offset,
            margin_strut_sum);
        if (new_margin_strut_sum != margin_strut_sum) {
          container_builder_.SetIsTruncatedByFragmentationLine();
          margin_strut_sum = new_margin_strut_sum;
        }
      }

      // The trailing margin strut will be part of our intrinsic block size, but
      // only if there is something that separates the end margin strut from the
      // input margin strut (typically child content, block start
      // border/padding, or this being a new BFC). If the margin strut from a
      // previous sibling or ancestor managed to collapse through all our
      // children (if any at all, that is), it means that the resulting end
      // margin strut actually pushes us down, and it should obviously not be
      // doubly accounted for as our block size.
      intrinsic_block_size_ = std::max(
          intrinsic_block_size_,
          previous_inflow_position->logical_block_offset + margin_strut_sum);
    }

    intrinsic_block_size_ += BorderScrollbarPadding().block_end;
    end_margin_strut = NGMarginStrut();
  } else {
    // Update our intrinsic block size to be just past the block-end border edge
    // of the last in-flow child. The pending margin is to be propagated to our
    // container, so ignore it.
    intrinsic_block_size_ = std::max(
        intrinsic_block_size_, previous_inflow_position->logical_block_offset);
  }

  intrinsic_block_size_ = ClampIntrinsicBlockSize(
      ConstraintSpace(), Node(), BreakToken(), BorderScrollbarPadding(),
      intrinsic_block_size_,
      CalculateQuirkyBodyMarginBlockSum(end_margin_strut));

  // In order to calculate the block-size for the fragment, we need to compare
  // the combined intrinsic block-size of all fragments to e.g. specified
  // block-size. We'll skip this part if this is a fragmentainer.
  // Fragmentainers never have a specified block-size anyway, but, more
  // importantly, adding consumed block-size, and then subtracting it again
  // later (when setting the final fragment size) would produce incorrect
  // results if the sum becomes "infinity", i.e. LayoutUnit::Max(). Skipping
  // this will allow the total block-size of all the fragmentainers to become
  // greater than LayoutUnit::Max(). This is important for column balancing, or
  // we'd fail to finish very tall child content properly, ending up with too
  // many fragmentainers, since the fragmentainers produced would be too short
  // to fit as much as necessary. Basically: don't mess up (clamp) the measument
  // we've already done.
  LayoutUnit previously_consumed_block_size;
  if (UNLIKELY(BreakToken() && !container_builder_.IsFragmentainerBoxType())) {
    previously_consumed_block_size = BreakToken()->ConsumedBlockSize();
  }

  // Recompute the block-axis size now that we know our content size.
  border_box_size.block_size = ComputeBlockSizeForFragment(
      ConstraintSpace(), Style(), BorderPadding(),
      previously_consumed_block_size + intrinsic_block_size_,
      border_box_size.inline_size);
  container_builder_.SetFragmentsTotalBlockSize(border_box_size.block_size);

  // If our BFC block-offset is still unknown, we check:
  //  - If we have a non-zero block-size (margins don't collapse through us).
  //  - If we have a break token. (Even if we are self-collapsing we position
  //    ourselves at the very start of the fragmentainer).
  //  - We got interrupted by a column spanner.
  if (!container_builder_.BfcBlockOffset() &&
      (border_box_size.block_size || BreakToken() ||
       container_builder_.FoundColumnSpanner())) {
    if (!ResolveBfcBlockOffset(previous_inflow_position)) {
      return container_builder_.Abort(NGLayoutResult::kBfcBlockOffsetResolved);
    }
    DCHECK(container_builder_.BfcBlockOffset());
  }

  if (container_builder_.BfcBlockOffset()) {
    // Do not collapse margins between the last in-flow child and bottom margin
    // of its parent if:
    //  - The block-size differs from the intrinsic size.
    //  - The parent has computed block-size != auto.
    if (border_box_size.block_size != intrinsic_block_size_ ||
        !BlockLengthUnresolvable(ConstraintSpace(), Style().LogicalHeight())) {
      end_margin_strut = NGMarginStrut();
    }
  }

  DCHECK(!ShouldPlaceUnpositionedListMarker());
  // // List markers should have been positioned if we had line boxes, or boxes
  // // that have line boxes. If there were no line boxes, position without line
  // // boxes.
  // if (container_builder_.UnpositionedListMarker() &&
  //     ShouldPlaceUnpositionedListMarker() &&
  //     // If the list-item is block-fragmented, leave it unpositioned and
  //     expect
  //     // following fragments have a line box.
  //     !container_builder_.HasInflowChildBreakInside()) {
  //   if (!PositionListMarkerWithoutLineBoxes(previous_inflow_position)) {
  //     return
  //     container_builder_.Abort(NGLayoutResult::kBfcBlockOffsetResolved);
  //   }
  // }

  container_builder_.SetEndMarginStrut(end_margin_strut);
  container_builder_.SetIntrinsicBlockSize(intrinsic_block_size_);

  if (container_builder_.BfcBlockOffset()) {
    // If we know our BFC block-offset we should have correctly placed all
    // adjoining objects, and shouldn't propagate this information to siblings.
    container_builder_.ResetAdjoiningObjectTypes();
  } else {
    // If we don't know our BFC block-offset yet, we know that for
    // margin-collapsing purposes we are self-collapsing.
    container_builder_.SetIsSelfCollapsing();

    // If we've been forced at a particular BFC block-offset, (either from
    // clearance past adjoining floats, or a re-layout), we can safely set our
    // BFC block-offset now.
    if (ConstraintSpace().ForcedBfcBlockOffset()) {
      container_builder_.SetBfcBlockOffset(
          *ConstraintSpace().ForcedBfcBlockOffset());

      // Also make sure that this is treated as a valid class C breakpoint (if
      // it is one).
      if (ConstraintSpace().IsPushedByFloats()) {
        container_builder_.SetIsPushedByFloats();
      }
    }
  }

  DCHECK(!InvolvedInBlockFragmentation(container_builder_));
  if (UNLIKELY(InvolvedInBlockFragmentation(container_builder_))) {
    // NGBreakStatus status = FinalizeForFragmentation();
    // if (status != NGBreakStatus::kContinue) {
    //   if (status == NGBreakStatus::kNeedsEarlierBreak) {
    //     return container_builder_.Abort(NGLayoutResult::kNeedsEarlierBreak);
    //   }
    //   DCHECK_EQ(status, NGBreakStatus::kDisableFragmentation);
    //   return container_builder_.Abort(NGLayoutResult::kDisableFragmentation);
    // }

    // // Read the intrinsic block-size back, since it may have been reduced due
    // to
    // // fragmentation.
    // intrinsic_block_size_ = container_builder_.IntrinsicBlockSize();
  } else {
#if DCHECK_IS_ON()
    // If we're not participating in a fragmentation context, no block
    // fragmentation related fields should have been set.
    container_builder_.CheckNoBlockFragmentation();
#endif
  }

  // At this point, perform any final table-cell adjustments needed.
  if (ConstraintSpace().IsTableCell()) {
    NGTableAlgorithmUtils::FinalizeTableCellLayout(intrinsic_block_size_,
                                                   &container_builder_);
  }

  NGOutOfFlowLayoutPart(Node(), ConstraintSpace(), &container_builder_).Run();

  if (ConstraintSpace().BaselineAlgorithmType() ==
      NGBaselineAlgorithmType::kInlineBlock) {
    container_builder_.SetUseLastBaselineForInlineBaseline();
  }

  // An exclusion space is confined to nodes within the same formatting context.
  if (ConstraintSpace().IsNewFormattingContext()) {
    container_builder_.SetExclusionSpace(NGExclusionSpace());
  } else {
    container_builder_.SetLinesUntilClamp(lines_until_clamp_);
  }

  if (ConstraintSpace().UseFirstLineStyle()) {
    container_builder_.SetStyleVariant(NGStyleVariant::kFirstLine);
  }

  return container_builder_.ToBoxFragment();
}

absl::optional<LayoutUnit>
ScrollerLayoutAlgorithm::CalculateQuirkyBodyMarginBlockSum(
    const NGMarginStrut& end_margin_strut) {
  if (!Node().IsQuirkyAndFillsViewport()) {
    return absl::nullopt;
  }

  if (!Style().LogicalHeight().IsAuto()) {
    return absl::nullopt;
  }

  if (ConstraintSpace().IsNewFormattingContext()) {
    return absl::nullopt;
  }

  DCHECK(Node().IsBody());
  LayoutUnit block_end_margin =
      ComputeMarginsForSelf(ConstraintSpace(), Style()).block_end;

  // The |end_margin_strut| is the block-start margin if the body doesn't have
  // a resolved BFC block-offset.
  if (!container_builder_.BfcBlockOffset()) {
    return end_margin_strut.Sum() + block_end_margin;
  }

  NGMarginStrut body_strut = end_margin_strut;
  body_strut.Append(block_end_margin, Style().HasMarginAfterQuirk());
  return *container_builder_.BfcBlockOffset() -
         ConstraintSpace().BfcOffset().block_offset + body_strut.Sum();
}

NGLayoutResult::EStatus ScrollerLayoutAlgorithm::HandleInflow(
    NGLayoutInputNode child,
    const NGBreakToken* child_break_token,
    NGPreviousInflowPosition* previous_inflow_position,
    NGInlineChildLayoutContext* inline_child_layout_context,
    const NGInlineBreakToken** previous_inline_break_token) {
  DCHECK(child);
  DCHECK(!child.IsFloating());
  DCHECK(!child.IsOutOfFlowPositioned());
  DCHECK(!child.CreatesNewFormattingContext());
  DCHECK(child.IsBlock());

  bool has_clearance_past_adjoining_floats =
      !container_builder_.BfcBlockOffset() && child.IsBlock() &&
      HasClearancePastAdjoiningFloats(container_builder_.AdjoiningObjectTypes(),
                                      child.Style(), Style());
  DCHECK(!has_clearance_past_adjoining_floats);

  absl::optional<LayoutUnit> forced_bfc_block_offset;
  bool is_pushed_by_floats = false;

  // Perform layout on the child.
  NGInflowChildData child_data =
      ComputeChildData(*previous_inflow_position, child, child_break_token,
                       /* is_new_fc */ false);
  child_data.is_pushed_by_floats = is_pushed_by_floats;
  NGConstraintSpace child_space = CreateConstraintSpaceForChild(
      child, child_break_token, child_data, ChildAvailableSize(),
      /* is_new_fc */ false, forced_bfc_block_offset,
      has_clearance_past_adjoining_floats,
      previous_inflow_position->block_end_annotation_space);
  auto minimum_top = CreateMinimumTopScopeForChild(child, child_data);
  const NGLayoutResult* layout_result =
      LayoutInflow(child_space, child_break_token, early_break_,
                   column_spanner_path_, &child, inline_child_layout_context);

  // To save space of the stack when we recurse into |NGBlockNode::Layout|
  // above, the rest of this function is continued within |FinishInflow|.
  // However it should be read as one function.
  return FinishInflow(child, child_break_token, child_space,
                      has_clearance_past_adjoining_floats,
                      std::move(layout_result), &child_data,
                      previous_inflow_position, inline_child_layout_context,
                      previous_inline_break_token);
}

NGLayoutResult::EStatus ScrollerLayoutAlgorithm::FinishInflow(
    NGLayoutInputNode child,
    const NGBreakToken* child_break_token,
    const NGConstraintSpace& child_space,
    bool has_clearance_past_adjoining_floats,
    const NGLayoutResult* layout_result,
    NGInflowChildData* child_data,
    NGPreviousInflowPosition* previous_inflow_position,
    NGInlineChildLayoutContext* inline_child_layout_context,
    const NGInlineBreakToken** previous_inline_break_token) {
  absl::optional<LayoutUnit> child_bfc_block_offset =
      layout_result->BfcBlockOffset();

  bool is_self_collapsing = layout_result->IsSelfCollapsing();

  // "Normal child" here means non-self-collapsing. Even self-collapsing
  // children may be cleared by floats, if they have a forced BFC block-offset.
  bool normal_child_had_clearance =
      layout_result->IsPushedByFloats() && !is_self_collapsing;

  // A child may have aborted its layout if it resolved its BFC block-offset.
  // If we don't have a BFC block-offset yet, we need to propagate the abort
  // signal up to our parent.
  if (layout_result->Status() == NGLayoutResult::kBfcBlockOffsetResolved &&
      !container_builder_.BfcBlockOffset()) {
    // There's no need to do anything apart from resolving the BFC block-offset
    // here, so make sure that it aborts before trying to position floats or
    // anything like that, which would just be waste of time.
    //
    // This is simply propagating an abort up to a node which is able to
    // restart the layout (a node that has resolved its BFC block-offset).
    DCHECK(child_bfc_block_offset);
    abort_when_bfc_block_offset_updated_ = true;

    LayoutUnit bfc_block_offset = *child_bfc_block_offset;

    if (normal_child_had_clearance) {
      // If the child has the same clearance-offset as ourselves it means that
      // we should *also* resolve ourselves at that offset, (and we also have
      // been pushed by floats).
      if (ConstraintSpace().ClearanceOffset() ==
          child_space.ClearanceOffset()) {
        container_builder_.SetIsPushedByFloats();
      } else {
        bfc_block_offset = NextBorderEdge(*previous_inflow_position);
      }
    }

    // A new formatting-context may have previously tried to resolve the BFC
    // block-offset. In this case we'll have a "forced" BFC block-offset
    // present, but we shouldn't apply it (instead preferring the child's new
    // BFC block-offset).
    DCHECK(!ConstraintSpace().AncestorHasClearancePastAdjoiningFloats());

    if (!ResolveBfcBlockOffset(previous_inflow_position, bfc_block_offset,
                               /* forced_bfc_block_offset */ absl::nullopt)) {
      return NGLayoutResult::kBfcBlockOffsetResolved;
    }
  }

  // We have special behavior for a self-collapsing child which gets pushed
  // down due to clearance, see comment inside |ComputeInflowPosition|.
  bool self_collapsing_child_had_clearance =
      is_self_collapsing && has_clearance_past_adjoining_floats;

  // We try and position the child within the block formatting-context. This
  // may cause our BFC block-offset to be resolved, in which case we should
  // abort our layout if needed.
  if (!child_bfc_block_offset) {
    DCHECK(is_self_collapsing);
    if (child_space.HasClearanceOffset() && child.Style().HasClear()) {
      // This is a self-collapsing child that we collapsed through, so we have
      // to detect clearance manually. See if the child's hypothetical border
      // edge is past the relevant floats. If it's not, we need to apply
      // clearance before it.
      LayoutUnit child_block_offset_estimate =
          BfcBlockOffset() + layout_result->EndMarginStrut().Sum();
      if (child_block_offset_estimate < child_space.ClearanceOffset()) {
        self_collapsing_child_had_clearance = true;
      }
    }
  }

  bool child_had_clearance =
      self_collapsing_child_had_clearance || normal_child_had_clearance;
  if (child_had_clearance) {
    // The child has clearance. Clearance inhibits margin collapsing and acts as
    // spacing before the block-start margin of the child. Our BFC block offset
    // is therefore resolvable, and if it hasn't already been resolved, we'll
    // do it now to separate the child's collapsed margin from this container.
    if (!ResolveBfcBlockOffset(previous_inflow_position)) {
      return NGLayoutResult::kBfcBlockOffsetResolved;
    }
  } else if (layout_result->SubtreeModifiedMarginStrut()) {
    // The child doesn't have clearance, and modified its incoming
    // margin-strut. Propagate this information up to our parent if needed.
    SetSubtreeModifiedMarginStrutIfNeeded();
  }

  bool self_collapsing_child_needs_relayout = false;
  if (!child_bfc_block_offset) {
    // Layout wasn't able to determine the BFC block-offset of the child. This
    // has to mean that the child is self-collapsing.
    DCHECK(is_self_collapsing);

    if (container_builder_.BfcBlockOffset() &&
        layout_result->Status() == NGLayoutResult::kSuccess) {
      // Since we know our own BFC block-offset, though, we can calculate that
      // of the child as well.
      child_bfc_block_offset = PositionSelfCollapsingChildWithParentBfc(
          child, child_space, *child_data, *layout_result);

      // We may need to relayout this child if it had any (adjoining) objects
      // which were positioned in the incorrect place.
      if (layout_result->PhysicalFragment().HasAdjoiningObjectDescendants() &&
          *child_bfc_block_offset != child_space.ExpectedBfcBlockOffset()) {
        self_collapsing_child_needs_relayout = true;
      }
    }
  } else if (!child_had_clearance && !is_self_collapsing) {
    // Only non self-collapsing children are allowed resolve their parent's BFC
    // block-offset. We check the BFC block-offset at the end of layout
    // determine if this fragment is self-collapsing.
    //
    // The child's BFC block-offset is known, and since there's no clearance,
    // this container will get the same offset, unless it has already been
    // resolved.
    if (!ResolveBfcBlockOffset(previous_inflow_position,
                               *child_bfc_block_offset)) {
      return NGLayoutResult::kBfcBlockOffsetResolved;
    }
  }

  // We need to re-layout a self-collapsing child if it was affected by
  // clearance in order to produce a new margin strut. For example:
  // <div style="margin-bottom: 50px;"></div>
  // <div id="float" style="height: 50px;"></div>
  // <div id="zero" style="clear: left; margin-top: -20px;">
  //   <div id="zero-inner" style="margin-top: 40px; margin-bottom: -30px;">
  // </div>
  //
  // The end margin strut for #zero will be {50, -30}. #zero will be affected
  // by clearance (as 50 > {50, -30}).
  //
  // As #zero doesn't touch the incoming margin strut now we need to perform a
  // relayout with an empty incoming margin strut.
  //
  // The resulting margin strut in the above example will be {40, -30}. See
  // |ComputeInflowPosition| for how this end margin strut is used.
  if (self_collapsing_child_had_clearance) {
    NGMarginStrut margin_strut;
    margin_strut.Append(child_data->margins.block_start,
                        child.Style().HasMarginBeforeQuirk());

    // We only need to relayout if the new margin strut is different to the
    // previous one.
    if (child_data->margin_strut != margin_strut) {
      child_data->margin_strut = margin_strut;
      self_collapsing_child_needs_relayout = true;
    }
  }

  // We need to layout a child if we know its BFC block offset and:
  //  - It aborted its layout as it resolved its BFC block offset.
  //  - It has some unpositioned floats.
  //  - It was affected by clearance.
  if ((layout_result->Status() == NGLayoutResult::kBfcBlockOffsetResolved ||
       self_collapsing_child_needs_relayout) &&
      child_bfc_block_offset) {
    // Assert that any clearance previously detected isn't lost.
    DCHECK(!child_data->is_pushed_by_floats ||
           layout_result->IsPushedByFloats());
    // If the child got pushed down by floats (normally because of clearance),
    // we need to carry over this state to the next layout pass, as clearance
    // won't automatically be detected then, since the BFC block-offset will
    // already be past the relevant floats.
    child_data->is_pushed_by_floats = layout_result->IsPushedByFloats();

    NGConstraintSpace new_child_space = CreateConstraintSpaceForChild(
        child, child_break_token, *child_data, ChildAvailableSize(),
        /* is_new_fc */ false, child_bfc_block_offset);
    auto minimum_top = CreateMinimumTopScopeForChild(child, *child_data);
    layout_result =
        LayoutInflow(new_child_space, child_break_token, early_break_,
                     column_spanner_path_, &child, inline_child_layout_context);

    if (layout_result->Status() == NGLayoutResult::kBfcBlockOffsetResolved) {
      // Even a second layout pass may abort, if the BFC block offset initially
      // calculated turned out to be wrong. This happens when we discover that
      // an in-flow block-level descendant that establishes a new formatting
      // context doesn't fit beside the floats at its initial position. Allow
      // one more pass.
      child_bfc_block_offset = layout_result->BfcBlockOffset();
      DCHECK(child_bfc_block_offset);

      // We don't expect clearance to be detected at this point. Any clearance
      // should already have been detected above.
      DCHECK(child_data->is_pushed_by_floats ||
             !layout_result->IsPushedByFloats());

      new_child_space = CreateConstraintSpaceForChild(
          child, child_break_token, *child_data, ChildAvailableSize(),
          /* is_new_fc */ false, child_bfc_block_offset);
      auto minimum_top2 = CreateMinimumTopScopeForChild(child, *child_data);
      layout_result = LayoutInflow(new_child_space, child_break_token,
                                   early_break_, column_spanner_path_, &child,
                                   inline_child_layout_context);
    }

    DCHECK_EQ(layout_result->Status(), NGLayoutResult::kSuccess);

    // We stored this in a local variable, so it better not have changed.
    DCHECK_EQ(layout_result->IsSelfCollapsing(), is_self_collapsing);
  }

  const absl::optional<LayoutUnit> line_box_bfc_block_offset =
      layout_result->LineBoxBfcBlockOffset();

  DCHECK(!ConstraintSpace().HasBlockFragmentation());
  // if (ConstraintSpace().HasBlockFragmentation()) {
  //   if (container_builder_.BfcBlockOffset() && child_bfc_block_offset) {
  //     bool is_line_box_pushed_by_floats =
  //         line_box_bfc_block_offset &&
  //         *line_box_bfc_block_offset > *child_bfc_block_offset;

  //     // Floats only cause container separation for the outermost block child
  //     // that gets pushed down (the container and the child may have
  //     adjoining
  //     // block-start margins).
  //     bool has_container_separation =
  //         has_processed_first_child_ ||
  //         (!container_builder_.IsPushedByFloats() &&
  //          (layout_result->IsPushedByFloats() ||
  //          is_line_box_pushed_by_floats));
  //     NGBreakStatus break_status = BreakBeforeChildIfNeeded(
  //         child, *layout_result, previous_inflow_position,
  //         line_box_bfc_block_offset.value_or(*child_bfc_block_offset),
  //         has_container_separation);
  //     if (break_status == NGBreakStatus::kBrokeBefore) {
  //       return NGLayoutResult::kSuccess;
  //     }
  //     if (break_status == NGBreakStatus::kNeedsEarlierBreak) {
  //       return NGLayoutResult::kNeedsEarlierBreak;
  //     }
  //   }

  //   if (inline_child_layout_context) {
  //     for (auto token : inline_child_layout_context->PropagatedBreakTokens())
  //     {
  //       container_builder_.AddBreakToken(std::move(token),
  //                                        /* is_in_parallel_flow */ true);
  //     }
  //   }
  // }

  // It is now safe to update our version of the exclusion space, and any
  // propagated adjoining floats.
  container_builder_.SetExclusionSpace(layout_result->ExclusionSpace());

  // Only self-collapsing children should have adjoining objects.
  DCHECK(!layout_result->AdjoiningObjectTypes() || is_self_collapsing);
  container_builder_.SetAdjoiningObjectTypes(
      layout_result->AdjoiningObjectTypes());

  // If we don't know our BFC block-offset yet, and the child stumbled into
  // something that needs it (unable to position floats yet), we need abort
  // layout, and trigger a re-layout once we manage to resolve it.
  //
  // NOTE: This check is performed after the optional second layout pass above,
  // since we may have been able to resolve our BFC block-offset (e.g. due to
  // clearance) and position any descendant floats in the second pass.
  // In particular, when it comes to clearance of self-collapsing children, if
  // we just applied it and resolved the BFC block-offset to separate the
  // margins before and after clearance, we cannot abort and re-layout this
  // child, or clearance would be lost.
  //
  // If we are a new formatting context, the child will get re-laid out once it
  // has been positioned.
  if (!container_builder_.BfcBlockOffset()) {
    abort_when_bfc_block_offset_updated_ |=
        layout_result->AdjoiningObjectTypes();
    // If our BFC block offset is unknown, and the child got pushed down by
    // floats, so will we.
    if (layout_result->IsPushedByFloats()) {
      container_builder_.SetIsPushedByFloats();
    }
  }

  const auto& physical_fragment = layout_result->PhysicalFragment();
  NGFragment fragment(ConstraintSpace().GetWritingDirection(),
                      physical_fragment);

  if (line_box_bfc_block_offset) {
    child_bfc_block_offset = line_box_bfc_block_offset;
  }

  LogicalOffset logical_offset = CalculateLogicalOffset(
      fragment, layout_result->BfcLineOffset(), child_bfc_block_offset);
  DCHECK(!child.IsSliderThumb());
  // if (UNLIKELY(child.IsSliderThumb())) {
  //   logical_offset = AdjustSliderThumbInlineOffset(fragment, logical_offset);
  // }

  // if (!PositionOrPropagateListMarker(*layout_result, &logical_offset,
  //                                    previous_inflow_position)) {
  //   return NGLayoutResult::kBfcBlockOffsetResolved;
  // }

  // The box with -internal-align-self:center should create new
  // formatting context.
  DCHECK(child.IsInline() || !child.Style().AlignSelfBlockCenter());

  if (physical_fragment.IsLineBox()) {
    PropagateBaselineFromLineBox(physical_fragment,
                                 logical_offset.block_offset);
  } else {
    PropagateBaselineFromBlockChild(physical_fragment, child_data->margins,
                                    logical_offset.block_offset);
  }
  container_builder_.AddResult(*layout_result, logical_offset);

  LOG(ERROR)
      << "keyou: result: " << child.GetDOMNode()->ToString()
      << ", \nphysical_fragment: " << physical_fragment.ToString()
      << ", intrinsic_block_size_: "
      << layout_result->IntrinsicBlockSize().ToString() << ", BfcBlockOffset: "
      << layout_result->BfcBlockOffset().value_or(LayoutUnit(999)).ToString();

  if (auto* block_child = DynamicTo<NGBlockNode>(child)) {
    // We haven't yet resolved margins wrt. overconstrainedness, unless that was
    // also required to calculate line-left offset (due to block alignment)
    // before layout. Do so now, so that we store the correct values (which is
    // required by e.g. getComputedStyle()).
    if (!child_data->margins_fully_resolved) {
      ResolveInlineMargins(child.Style(), Style(),
                           ChildAvailableSize().inline_size,
                           fragment.InlineSize(), &child_data->margins);
      child_data->margins_fully_resolved = true;
    }

    block_child->StoreMargins(ConstraintSpace(), child_data->margins);
  }

  *previous_inflow_position = ComputeInflowPosition(
      *previous_inflow_position, child, *child_data, child_bfc_block_offset,
      logical_offset, *layout_result, fragment,
      self_collapsing_child_had_clearance);

  if (child.IsInline()) {
    const auto* inline_break_token =
        To<NGInlineBreakToken>(physical_fragment.BreakToken());
    if (UNLIKELY(inline_break_token &&
                 inline_break_token->BlockInInlineBreakToken())) {
      if (inline_break_token->BlockInInlineBreakToken()->IsAtBlockEnd()) {
        // We resumed a block in inline in a parallel flow, and broke
        // again. This will have to wait until we get to the next
        // fragmentainer. The break token has already been added to the fragment
        // builder.
        DCHECK(child_break_token);
        inline_break_token = nullptr;
      }
    }
    *previous_inline_break_token = inline_break_token;
  } else {
    *previous_inline_break_token = nullptr;
  }

  // Update |lines_until_clamp_| from the LayoutResult.
  if (lines_until_clamp_) {
    lines_until_clamp_ = layout_result->LinesUntilClamp();

    if (lines_until_clamp_ <= 0 &&
        !intrinsic_block_size_when_clamped_.has_value()) {
      // If line-clamping occurred save the intrinsic block-size, as this
      // becomes the final intrinsic block-size.
      intrinsic_block_size_when_clamped_ =
          previous_inflow_position->logical_block_offset;
    }
  }
  return NGLayoutResult::kSuccess;
}

LogicalOffset ScrollerLayoutAlgorithm::CalculateLogicalOffset(
    const NGFragment& fragment,
    LayoutUnit child_bfc_line_offset,
    const absl::optional<LayoutUnit>& child_bfc_block_offset) {
  LayoutUnit inline_size = container_builder_.InlineSize();
  TextDirection direction = ConstraintSpace().Direction();

  if (child_bfc_block_offset && container_builder_.BfcBlockOffset()) {
    return LogicalFromBfcOffsets(
        {child_bfc_line_offset, *child_bfc_block_offset}, ContainerBfcOffset(),
        fragment.InlineSize(), inline_size, direction);
  }

  LayoutUnit inline_offset = LogicalFromBfcLineOffset(
      child_bfc_line_offset, container_builder_.BfcLineOffset(),
      fragment.InlineSize(), inline_size, direction);

  // If we've reached here, either the parent, or the child don't have a BFC
  // block-offset yet. Children in this situation are always placed at a
  // logical block-offset of zero.
  return {inline_offset, LayoutUnit()};
}

NGInflowChildData ScrollerLayoutAlgorithm::ComputeChildData(
    const NGPreviousInflowPosition& previous_inflow_position,
    NGLayoutInputNode child,
    const NGBreakToken* child_break_token,
    bool is_new_fc) {
  DCHECK(child);
  DCHECK(!child.IsFloating());
  DCHECK_EQ(is_new_fc, child.CreatesNewFormattingContext());

  // Calculate margins in parent's writing mode.
  bool margins_fully_resolved;
  NGBoxStrut margins =
      CalculateMargins(child, is_new_fc, &margins_fully_resolved);

  // Append the current margin strut with child's block start margin.
  // Non empty border/padding, and new formatting-context use cases are handled
  // inside of the child's layout
  NGMarginStrut margin_strut = previous_inflow_position.margin_strut;

  const auto* child_block_break_token =
      DynamicTo<NGBlockBreakToken>(child_break_token);
  if (UNLIKELY(child_block_break_token)) {
    AdjustMarginsForFragmentation(child_block_break_token, &margins);
    if (child_block_break_token->IsForcedBreak()) {
      // After a forced fragmentainer break we need to reset the margin strut,
      // in case it was set to discard all margins (which is the default at
      // breaks). Margins after a forced break should be retained.
      margin_strut = NGMarginStrut();
    }
  }

  LayoutUnit logical_block_offset =
      previous_inflow_position.logical_block_offset;

  margin_strut.Append(margins.block_start,
                      child.Style().HasMarginBeforeQuirk());
  if (child.IsBlock()) {
    SetSubtreeModifiedMarginStrutIfNeeded(&child.Style().MarginBefore());
  }

  NGBfcOffset child_bfc_offset = {
      ConstraintSpace().BfcOffset().line_offset +
          BorderScrollbarPadding().LineLeft(ConstraintSpace().Direction()) +
          margins.LineLeft(ConstraintSpace().Direction()),
      BfcBlockOffset() + logical_block_offset};
  LOG(ERROR) << "keyou1: child_bfc_offset: " << child_bfc_offset
             << "BfcBlockOffset: " << BfcBlockOffset()
             << ", logical_block_offset: " << logical_block_offset;

  return {child_bfc_offset, margin_strut, margins, margins_fully_resolved,
          IsBreakInside(child_block_break_token)};
}

NGPreviousInflowPosition ScrollerLayoutAlgorithm::ComputeInflowPosition(
    const NGPreviousInflowPosition& previous_inflow_position,
    const NGLayoutInputNode child,
    const NGInflowChildData& child_data,
    const absl::optional<LayoutUnit>& child_bfc_block_offset,
    const LogicalOffset& logical_offset,
    const NGLayoutResult& layout_result,
    const NGFragment& fragment,
    bool self_collapsing_child_had_clearance) {
  // Determine the child's end logical offset, for the next child to use.
  LayoutUnit logical_block_offset;

  bool is_self_collapsing = layout_result.IsSelfCollapsing();
  if (is_self_collapsing) {
    // The default behavior for self-collapsing children is they just pass
    // through the previous inflow position.
    logical_block_offset = previous_inflow_position.logical_block_offset;

    if (self_collapsing_child_had_clearance) {
      // If there's clearance, we must have applied that by now and thus
      // resolved our BFC block-offset.
      DCHECK(container_builder_.BfcBlockOffset());
      DCHECK(child_bfc_block_offset.has_value());

      // If a self-collapsing child was affected by clearance (that is it got
      // pushed down past a float), we need to do something slightly bizarre.
      //
      // Instead of just passing through the previous inflow position, we make
      // the inflow position our new position (which was affected by the
      // float), minus what the margin strut which the self-collapsing child
      // produced.
      //
      // Another way of thinking about this is that when you *add* back the
      // margin strut, you end up with the same position as you started with.
      //
      // This is essentially what the spec refers to as clearance [1], and,
      // while we normally don't have to calculate it directly, in the case of
      // a self-collapsing cleared child like here, we actually have to.
      //
      // We have to calculate clearance for self-collapsing cleared children,
      // because we need the margin that's between the clearance and this block
      // to collapse correctly with subsequent content. This is something that
      // needs to take place after the margin strut preceding and following the
      // clearance have been separated. Clearance may be positive, negative or
      // zero, depending on what it takes to (hypothetically) place this child
      // just below the last relevant float. Since the margins before and after
      // the clearance have been separated, we may have to pull the child back,
      // and that's an example of negative clearance.
      //
      // (In the other case, when a cleared child is non self-collapsing (i.e.
      // when we don't end up here), we don't need to explicitly calculate
      // clearance, because then we just place its border edge where it should
      // be and we're done with it.)
      //
      // [1] https://www.w3.org/TR/CSS22/visuren.html#flow-control

      // First move past the margin that is to precede the clearance. It will
      // not participate in any subsequent margin collapsing.
      LayoutUnit margin_before_clearance =
          previous_inflow_position.margin_strut.Sum();
      logical_block_offset += margin_before_clearance;

      // Calculate and apply actual clearance.
      LayoutUnit clearance = *child_bfc_block_offset -
                             layout_result.EndMarginStrut().Sum() -
                             NextBorderEdge(previous_inflow_position);
      logical_block_offset += clearance;
    }
    if (!container_builder_.BfcBlockOffset()) {
      DCHECK_EQ(logical_block_offset, LayoutUnit());
    }
  } else {
    // We add the greater of AnnotationOverflow and ClearanceAfterLine here.
    // Then, we cancel the AnnotationOverflow part if
    //  - The next line box has block-start annotation space, or
    //  - There are no following child boxes and this container has block-end
    //    padding.
    //
    // See NGInlineLayoutAlgorithm::CreateLine() and
    // BlockLayoutAlgorithm::Layout().
    logical_block_offset = logical_offset.block_offset + fragment.BlockSize() +
                           std::max(layout_result.AnnotationOverflow(),
                                    layout_result.ClearanceAfterLine());
  }

  NGMarginStrut margin_strut = layout_result.EndMarginStrut();

  // Self collapsing child's end margin can "inherit" quirkiness from its start
  // margin. E.g.
  // <ol style="margin-bottom: 20px"></ol>
  bool is_quirky =
      (is_self_collapsing && child.Style().HasMarginBeforeQuirk()) ||
      child.Style().HasMarginAfterQuirk();
  margin_strut.Append(child_data.margins.block_end, is_quirky);
  if (child.IsBlock()) {
    SetSubtreeModifiedMarginStrutIfNeeded(&child.Style().MarginAfter());
  }

  if (UNLIKELY(ConstraintSpace().HasBlockFragmentation())) {
    // If the child broke inside, don't apply any trailing margin, since it's
    // only to be applied to the last fragment that's not in a parallel flow
    // (due to overflow). While trailing margins are normally truncated at
    // fragmentainer boundaries, so that whether or not we add such margins
    // doesn't really make much of a difference, this isn't the case in the
    // initial column balancing pass.
    if (const auto* physical_fragment = DynamicTo<NGPhysicalBoxFragment>(
            &layout_result.PhysicalFragment())) {
      if (const NGBlockBreakToken* token = physical_fragment->BreakToken()) {
        // TODO(mstensho): Don't apply the margin to all overflowing fragments
        // (if any). It should only be applied after the fragment where we
        // reached the block-end of the node.
        if (!token->IsAtBlockEnd()) {
          margin_strut = NGMarginStrut();
        }
      }
    }
  }

  // This flag is subtle, but in order to determine our size correctly we need
  // to check if our last child is self-collapsing, and it was affected by
  // clearance *or* an adjoining self-collapsing sibling was affected by
  // clearance. E.g.
  // <div id="container">
  //   <div id="float"></div>
  //   <div id="zero-with-clearance"></div>
  //   <div id="another-zero"></div>
  // </div>
  // In the above case #container's size will depend on the end margin strut of
  // #another-zero, even though usually it wouldn't.
  bool self_or_sibling_self_collapsing_child_had_clearance =
      self_collapsing_child_had_clearance ||
      (previous_inflow_position.self_collapsing_child_had_clearance &&
       is_self_collapsing);

  LayoutUnit annotation_space = layout_result.BlockEndAnnotationSpace();
  if (layout_result.AnnotationOverflow() > LayoutUnit()) {
    DCHECK(!annotation_space);
    // Allow the portion of the annotation overflow that isn't also part of
    // clearance to overlap with certain types of subsequent content.
    annotation_space =
        -std::max(LayoutUnit(), layout_result.AnnotationOverflow() -
                                    layout_result.ClearanceAfterLine());
  }

  return {logical_block_offset, margin_strut, annotation_space,
          self_or_sibling_self_collapsing_child_had_clearance};
}

LayoutUnit ScrollerLayoutAlgorithm::PositionSelfCollapsingChildWithParentBfc(
    const NGLayoutInputNode& child,
    const NGConstraintSpace& child_space,
    const NGInflowChildData& child_data,
    const NGLayoutResult& layout_result) const {
  DCHECK(layout_result.IsSelfCollapsing());

  // The child must be an in-flow zero-block-size fragment, use its end margin
  // strut for positioning.
  LayoutUnit child_bfc_block_offset =
      child_data.bfc_offset_estimate.block_offset +
      layout_result.EndMarginStrut().Sum();

  ApplyClearance(child_space, &child_bfc_block_offset);

  return child_bfc_block_offset;
}

DeferredShapingMinimumTopScope
ScrollerLayoutAlgorithm::CreateMinimumTopScopeForChild(
    const NGLayoutInputNode child,
    const NGInflowChildData& child_data) const {
  LayoutUnit minimum_top =
      DeferredShapingController::From(Node()).CurrentMinimumTop();
  if (Node().CreatesNewFormattingContext()) {
    minimum_top += child_data.bfc_offset_estimate.block_offset;
  } else {
    minimum_top = minimum_top - ConstraintSpace().BfcOffset().block_offset +
                  child_data.bfc_offset_estimate.block_offset;
  }
  return DeferredShapingMinimumTopScope(child, minimum_top);
}

void ScrollerLayoutAlgorithm::PropagateBaselineFromLineBox(
    const NGPhysicalFragment& child,
    LayoutUnit block_offset) {
  const auto& line_box = To<NGPhysicalLineBoxFragment>(child);

  // Skip over a line-box which is empty. These don't have any baselines
  // which should be added.
  if (line_box.IsEmptyLineBox()) {
    return;
  }

  // Skip over the line-box if we are past our clamp point.
  if (lines_until_clamp_ && *lines_until_clamp_ <= 0) {
    return;
  }

  if (UNLIKELY(line_box.IsBlockInInline())) {
    // Block-in-inline may have different first/last baselines.
    DCHECK(container_builder_.ItemsBuilder());
    const NGLogicalLineItems& items =
        container_builder_.ItemsBuilder()->LogicalLineItems(line_box);
    const NGLayoutResult* result = items.BlockInInlineLayoutResult();
    DCHECK(result);
    PropagateBaselineFromBlockChild(result->PhysicalFragment(),
                                    /* margins */ NGBoxStrut(), block_offset);
    return;
  }

  FontHeight metrics = line_box.BaselineMetrics();
  DCHECK(!metrics.IsEmpty());
  LayoutUnit baseline =
      block_offset +
      (Style().IsFlippedLinesWritingMode() ? metrics.descent : metrics.ascent);

  if (!container_builder_.FirstBaseline()) {
    container_builder_.SetFirstBaseline(baseline);
  }
  container_builder_.SetLastBaseline(baseline);
}

void ScrollerLayoutAlgorithm::PropagateBaselineFromBlockChild(
    const NGPhysicalFragment& child,
    const NGBoxStrut& margins,
    LayoutUnit block_offset) {
  DCHECK(child.IsBox());
  const auto baseline_algorithm = ConstraintSpace().BaselineAlgorithmType();

  // When computing baselines for an inline-block, table's don't contribute any
  // baselines.
  if (child.IsTableNG() &&
      baseline_algorithm == NGBaselineAlgorithmType::kInlineBlock) {
    return;
  }

  // Skip over the block if we are past our clamp point.
  if (lines_until_clamp_ && *lines_until_clamp_ <= 0) {
    return;
  }

  const auto& physical_fragment = To<NGPhysicalBoxFragment>(child);
  NGBoxFragment fragment(ConstraintSpace().GetWritingDirection(),
                         physical_fragment);

  if (!container_builder_.FirstBaseline()) {
    if (auto first_baseline = fragment.FirstBaseline()) {
      container_builder_.SetFirstBaseline(block_offset + *first_baseline);
    }
  }

  // Counter-intuitively, when computing baselines for an inline-block, some
  // fragments use their first-baseline for the container's last-baseline.
  bool use_last_baseline =
      baseline_algorithm == NGBaselineAlgorithmType::kDefault ||
      physical_fragment.UseLastBaselineForInlineBaseline();

  auto last_baseline =
      use_last_baseline ? fragment.LastBaseline() : fragment.FirstBaseline();

  // When computing baselines for an inline-block, some block-boxes (e.g. with
  // "overflow: hidden") will force the baseline to the block-end margin edge.
  if (baseline_algorithm == NGBaselineAlgorithmType::kInlineBlock &&
      physical_fragment.UseBlockEndMarginEdgeForInlineBaseline() &&
      !child.ShouldApplyLayoutContainment() && fragment.IsWritingModeEqual()) {
    last_baseline = fragment.BlockSize() + margins.block_end;
  }

  if (last_baseline) {
    container_builder_.SetLastBaseline(block_offset + *last_baseline);
  }
}

NGConstraintSpace ScrollerLayoutAlgorithm::CreateConstraintSpaceForChild(
    const NGLayoutInputNode child,
    const NGBreakToken* child_break_token,
    const NGInflowChildData& child_data,
    const LogicalSize child_available_size,
    bool is_new_fc,
    const absl::optional<LayoutUnit> child_bfc_block_offset,
    bool has_clearance_past_adjoining_floats,
    LayoutUnit block_start_annotation_space) {
  const ComputedStyle& child_style = child.Style();
  const auto child_writing_direction = child_style.GetWritingDirection();

  NGConstraintSpaceBuilder builder(ConstraintSpace(), child_writing_direction,
                                   is_new_fc);

  if (UNLIKELY(
          !IsParallelWritingMode(ConstraintSpace().GetWritingMode(),
                                 child_writing_direction.GetWritingMode()))) {
    SetOrthogonalFallbackInlineSize(Style(), child, &builder);
  } else if (ShouldBlockContainerChildStretchAutoInlineSize(child)) {
    builder.SetInlineAutoBehavior(NGAutoBehavior::kStretchImplicit);
  }

  builder.SetAvailableSize(child_available_size);
  builder.SetPercentageResolutionSize(child_percentage_size_);
  builder.SetReplacedPercentageResolutionSize(replaced_child_percentage_size_);

  if (ConstraintSpace().IsTableCell()) {
    builder.SetIsTableCellChild(true);

    // Always shrink-to-fit children within a <mtd> element.
    if (Node().GetDOMNode() &&
        Node().GetDOMNode()->HasTagName(mathml_names::kMtdTag)) {
      builder.SetInlineAutoBehavior(NGAutoBehavior::kFitContent);
    }

    // Some scrollable percentage-sized children of table-cells use their
    // min-size (instead of sizing normally).
    //
    // We only apply this rule if the block size of the containing table cell
    // is considered to be "restricted". Otherwise, especially if this is the
    // only child of the cell, and that is the only cell in the row, we'd end
    // up with zero block size.
    if (ConstraintSpace().IsRestrictedBlockSizeTableCell() &&
        child_percentage_size_.block_size == kIndefiniteSize &&
        !child.ShouldBeConsideredAsReplaced() &&
        child_style.LogicalHeight().IsPercentOrCalc() &&
        (child_style.OverflowBlockDirection() == EOverflow::kAuto ||
         child_style.OverflowBlockDirection() == EOverflow::kScroll)) {
      builder.SetIsRestrictedBlockSizeTableCellChild();
    }
  }

  bool has_bfc_block_offset = container_builder_.BfcBlockOffset().has_value();

  // Propagate the |NGConstraintSpace::ForcedBfcBlockOffset| down to our
  // children.
  if (!has_bfc_block_offset && ConstraintSpace().ForcedBfcBlockOffset()) {
    builder.SetForcedBfcBlockOffset(*ConstraintSpace().ForcedBfcBlockOffset());
  }
  if (child_bfc_block_offset && !is_new_fc) {
    builder.SetForcedBfcBlockOffset(*child_bfc_block_offset);
  }

  if (has_bfc_block_offset) {
    // Typically we aren't allowed to look at the previous layout result within
    // a layout algorithm. However this is fine (honest), as it is just a hint
    // to the child algorithm for where floats should be placed. If it doesn't
    // have this flag, or gets this estimate wrong, it'll relayout with the
    // appropriate "forced" BFC block-offset.
    if (child.IsBlock()) {
      if (const NGLayoutResult* cached_result =
              child.GetLayoutBox()->GetCachedLayoutResult(
                  To<NGBlockBreakToken>(child_break_token))) {
        const auto& prev_space = cached_result->GetConstraintSpaceForCaching();

        // To increase the hit-rate we adjust the previous "optimistic"/"forced"
        // BFC block-offset by how much the child has shifted from the previous
        // layout.
        LayoutUnit bfc_block_delta =
            child_data.bfc_offset_estimate.block_offset -
            prev_space.BfcOffset().block_offset;
        if (prev_space.ForcedBfcBlockOffset()) {
          builder.SetOptimisticBfcBlockOffset(
              *prev_space.ForcedBfcBlockOffset() + bfc_block_delta);
        } else if (prev_space.OptimisticBfcBlockOffset()) {
          builder.SetOptimisticBfcBlockOffset(
              *prev_space.OptimisticBfcBlockOffset() + bfc_block_delta);
        }
      }
    }
  } else if (ConstraintSpace().OptimisticBfcBlockOffset()) {
    // Propagate the |NGConstraintSpace::OptimisticBfcBlockOffset| down to our
    // children.
    builder.SetOptimisticBfcBlockOffset(
        *ConstraintSpace().OptimisticBfcBlockOffset());
  }

  // Propagate the |NGConstraintSpace::AncestorHasClearancePastAdjoiningFloats|
  // flag down to our children.
  if (!has_bfc_block_offset &&
      ConstraintSpace().AncestorHasClearancePastAdjoiningFloats()) {
    builder.SetAncestorHasClearancePastAdjoiningFloats();
  }
  if (has_clearance_past_adjoining_floats) {
    builder.SetAncestorHasClearancePastAdjoiningFloats();
  }

  LayoutUnit clearance_offset = LayoutUnit::Min();
  if (!IsBreakInside(DynamicTo<NGBlockBreakToken>(child_break_token))) {
    if (!ConstraintSpace().IsNewFormattingContext()) {
      clearance_offset = ConstraintSpace().ClearanceOffset();
    }
    if (child.IsBlock()) {
      LayoutUnit child_clearance_offset =
          ExclusionSpace().ClearanceOffset(child_style.Clear(Style()));
      clearance_offset = std::max(clearance_offset, child_clearance_offset);
    }
  }
  builder.SetClearanceOffset(clearance_offset);
  builder.SetBaselineAlgorithmType(ConstraintSpace().BaselineAlgorithmType());

  if (child_data.is_pushed_by_floats) {
    // Clearance has been applied, but it won't be automatically detected when
    // laying out the child, since the BFC block-offset has already been updated
    // to be past the relevant floats. We therefore need a flag.
    builder.SetIsPushedByFloats();
  }

  if (!is_new_fc) {
    builder.SetMarginStrut(child_data.margin_strut);
    builder.SetBfcOffset(child_data.bfc_offset_estimate);
    builder.SetExclusionSpace(ExclusionSpace());
    if (!has_bfc_block_offset) {
      builder.SetAdjoiningObjectTypes(
          container_builder_.AdjoiningObjectTypes());
    }
    builder.SetIsLineClampContext(is_line_clamp_context_);
    builder.SetLinesUntilClamp(lines_until_clamp_);
  } else if (child_data.allow_discard_start_margin) {
    // If the child is being resumed after a break, margins inside the child may
    // be adjoining with the fragmentainer boundary, regardless of whether the
    // child establishes a new formatting context or not.
    builder.SetDiscardingMarginStrut();
  }
  builder.SetBlockStartAnnotationSpace(block_start_annotation_space);

  if (ConstraintSpace().HasBlockFragmentation()) {
    LayoutUnit fragmentainer_offset_delta;
    // We need to keep track of our block-offset within the fragmentation
    // context, to be able to tell where the fragmentation line is (i.e. where
    // to break).
    if (is_new_fc) {
      fragmentainer_offset_delta =
          *child_bfc_block_offset - ConstraintSpace().ExpectedBfcBlockOffset();
    } else {
      fragmentainer_offset_delta = builder.ExpectedBfcBlockOffset() -
                                   ConstraintSpace().ExpectedBfcBlockOffset();
    }
    SetupSpaceBuilderForFragmentation(
        ConstraintSpace(), child, fragmentainer_offset_delta, &builder,
        is_new_fc, container_builder_.RequiresContentBeforeBreaking());

    // If there's a child break inside (typically in a parallel flow, or we
    // would have finished layout by now), we need to produce more
    // fragmentainers, before we can insert any column spanners, so that
    // everything that is supposed to come before the spanner actually ends up
    // there.
    if (ConstraintSpace().IsPastBreak() ||
        container_builder_.HasInsertedChildBreak()) {
      builder.SetIsPastBreak();
    }
  }

  return builder.ToConstraintSpace();
}

// NGConstraintSpace ScrollerLayoutAlgorithm::CreateConstraintSpaceForChild(
//     const NGBlockNode& parent_node,
//     const LogicalSize& child_available_size,
//     const NGConstraintSpace& parent_space,
//     const NGLayoutInputNode& child,
//     const NGCacheSlot cache_slot,
//     const absl::optional<NGConstraintSpace::MathTargetStretchBlockSizes>
//         target_stretch_block_sizes,
//     const absl::optional<LayoutUnit> target_stretch_inline_size) {
//   const ComputedStyle& parent_style = parent_node.Style();
//   const ComputedStyle& child_style = child.Style();
//   // DCHECK(child.CreatesNewFormattingContext());
//   NGConstraintSpaceBuilder builder(
//       parent_space, child_style.GetWritingDirection(), false /* is_new_fc
//       */);

//   const auto child_writing_direction = child_style.GetWritingDirection();

//   if (UNLIKELY(
//           !IsParallelWritingMode(ConstraintSpace().GetWritingMode(),
//                                  child_writing_direction.GetWritingMode())))
//                                  {
//     SetOrthogonalFallbackInlineSize(Style(), child, &builder);
//   } else if (ShouldBlockContainerChildStretchAutoInlineSize(child)) {
//     builder.SetInlineAutoBehavior(NGAutoBehavior::kStretchImplicit);
//   }

//   // SetOrthogonalFallbackInlineSizeIfNeeded(parent_style, child, &builder);
//   builder.SetAvailableSize(child_available_size);

//   // Calculate margins in parent's writing mode.
//   bool margins_fully_resolved;
//   NGBoxStrut margins = CalculateMargins(child, false,
//   &margins_fully_resolved);

//   // Append the current margin strut with child's block start margin.
//   // Non empty border/padding, and new formatting-context use cases are
//   handled
//   // inside of the child's layout
//   NGMarginStrut margin_strut = ConstraintSpace().MarginStrut();
//   if (node_.IsQuirkyContainer()) {
//     margin_strut.is_quirky_container_start = true;
//   }
//   margin_strut.Append(margins.block_start,
//                       child.Style().HasMarginBeforeQuirk());

//   LayoutUnit logical_block_offset =
//       container_builder_.BorderScrollbarPadding().block_start;
//   NGBfcOffset child_bfc_offset = {
//       ConstraintSpace().BfcOffset().line_offset +
//           BorderScrollbarPadding().LineLeft(ConstraintSpace().Direction()) +
//           margins.LineLeft(ConstraintSpace().Direction()),
//       BfcBlockOffset() + logical_block_offset};

//   LOG(ERROR) << "keyou: child_bfc_offset: " << child_bfc_offset
//              << ", BfcBlockOffset: " << BfcBlockOffset()
//              << ", logical_block_offset: " << logical_block_offset;

//   builder.SetBfcOffset(child_bfc_offset);
//   builder.SetMarginStrut(margin_strut);
//   builder.SetPercentageResolutionSize(child_available_size);
//   builder.SetCacheSlot(cache_slot);
//   if (target_stretch_block_sizes) {
//     builder.SetTargetStretchBlockSizes(*target_stretch_block_sizes);
//   }
//   if (target_stretch_inline_size) {
//     builder.SetTargetStretchInlineSize(*target_stretch_inline_size);
//   }

//   // TODO(crbug.com/1125137): add ink metrics.
//   return builder.ToConstraintSpace();
// }

NGBoxStrut ScrollerLayoutAlgorithm::CalculateMargins(
    NGLayoutInputNode child,
    bool is_new_fc,
    bool* margins_fully_resolved) {
  // We need to at least partially resolve margins before creating a constraint
  // space for layout. Layout needs to know the line-left offset before
  // starting. If the line-left offset cannot be calculated without fully
  // resolving the margins (because of block alignment), we have to create a
  // temporary constraint space now to figure out the inline size first. In all
  // other cases we'll postpone full resolution until after child layout, when
  // we actually have a child constraint space to use (and know the inline
  // size).
  *margins_fully_resolved = false;

  DCHECK(child);
  if (child.IsInline()) {
    return {};
  }
  const ComputedStyle& child_style = child.Style();
  bool needs_inline_size =
      NeedsInlineSizeToResolveLineLeft(child_style, Style());
  if (!needs_inline_size && !child_style.MayHaveMargin()) {
    return {};
  }

  NGBoxStrut margins =
      ComputeMarginsFor(child_style, child_percentage_size_.inline_size,
                        ConstraintSpace().GetWritingDirection());

  // As long as the child isn't establishing a new formatting context, we need
  // to know its line-left offset before layout, to be able to position child
  // floats correctly. If we need to resolve auto margins or other alignment
  // properties to calculate the line-left offset, we also need to calculate its
  // inline size first.
  if (!is_new_fc && needs_inline_size) {
    NGConstraintSpaceBuilder builder(ConstraintSpace(),
                                     child_style.GetWritingDirection(),
                                     /* is_new_fc */ false);
    builder.SetAvailableSize(ChildAvailableSize());
    builder.SetPercentageResolutionSize(child_percentage_size_);
    builder.SetInlineAutoBehavior(NGAutoBehavior::kStretchImplicit);
    NGConstraintSpace space = builder.ToConstraintSpace();

    const auto block_child = To<NGBlockNode>(child);
    NGBoxStrut child_border_padding =
        ComputeBorders(space, block_child) + ComputePadding(space, child_style);
    LayoutUnit child_inline_size =
        ComputeInlineSizeForFragment(space, block_child, child_border_padding);

    ResolveInlineMargins(child_style, Style(),
                         space.AvailableSize().inline_size, child_inline_size,
                         &margins);
    *margins_fully_resolved = true;
  }
  return margins;
}

bool ScrollerLayoutAlgorithm::ResolveBfcBlockOffset(
    NGPreviousInflowPosition* previous_inflow_position,
    LayoutUnit bfc_block_offset,
    absl::optional<LayoutUnit> forced_bfc_block_offset) {
  // Clearance may have been resolved (along with BFC block-offset) in a
  // previous layout pass, so check the constraint space for pre-applied
  // clearance. This is important in order to identify possible class C break
  // points.
  if (ConstraintSpace().IsPushedByFloats()) {
    container_builder_.SetIsPushedByFloats();
  }

  if (container_builder_.BfcBlockOffset()) {
    return true;
  }

  bfc_block_offset = forced_bfc_block_offset.value_or(bfc_block_offset);

  if (ApplyClearance(ConstraintSpace(), &bfc_block_offset)) {
    container_builder_.SetIsPushedByFloats();
  }

  container_builder_.SetBfcBlockOffset(bfc_block_offset);

  // if (NeedsAbortOnBfcBlockOffsetChange()) {
  //   // A formatting context root should always be able to resolve its
  //   // whereabouts before layout, so there should never be any incorrect
  //   // estimates that we need to go back and fix.
  //   DCHECK(!ConstraintSpace().IsNewFormattingContext());

  //   return false;
  // }

  // Set the offset to our block-start border edge. We'll now end up at the
  // block-start border edge. If the BFC block offset was resolved due to a
  // block-start border or padding, that must be added by the caller, for
  // subsequent layout to continue at the right position. Whether we need to add
  // border+padding or not isn't something we should determine here, so it must
  // be dealt with as part of initializing the layout algorithm.
  previous_inflow_position->logical_block_offset = LayoutUnit();

  // Resolving the BFC offset normally means that we have finished collapsing
  // adjoining margins, so that we can reset the margin strut. One exception
  // here is if we're resuming after a break, in which case we know that we can
  // resolve the BFC offset to the block-start of the fragmentainer
  // (block-offset 0). But keep the margin strut, since we're essentially still
  // collapsing with the fragmentainer boundary, which will eat / discard all
  // adjoining margins - unless this is at a forced break. DCHECK that the strut
  // is empty (note that a strut that's set up to eat all margins will also be
  // considered to be empty).
  if (!is_resuming_) {
    previous_inflow_position->margin_strut = NGMarginStrut();
  } else {
    DCHECK(previous_inflow_position->margin_strut.IsEmpty());
  }

  return true;
}

}  // namespace blink
