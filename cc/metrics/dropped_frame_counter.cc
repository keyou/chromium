// Copyright 2020 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "cc/metrics/dropped_frame_counter.h"
#include <_types/_uint32_t.h>
#include <sys/_types/_u_int32_t.h>

#include <algorithm>
#include <cmath>
#include <iterator>
#include <sstream>

#include "base/bind.h"
#include "base/metrics/field_trial_params.h"
#include "base/metrics/histogram.h"
#include "base/metrics/histogram_macros.h"
#include "base/ranges/algorithm.h"
#include "base/time/time.h"
#include "base/trace_event/common/trace_event_common.h"
#include "base/trace_event/trace_event.h"
#include "build/chromeos_buildflags.h"
#include "cc/base/features.h"
#include "cc/metrics/custom_metrics_recorder.h"
#include "cc/metrics/frame_sorter.h"
#include "cc/metrics/total_frame_counter.h"
#include "cc/metrics/ukm_smoothness_data.h"

#include "base/trace_event/traced_value.h"

namespace cc {
namespace {

const char kSlidingWindowForDroppedFrameCounterSeconds[] = "seconds";
const base::TimeDelta kDefaultSlidingWindowInterval = base::Seconds(1);

// The start ranges of each bucket, up to but not including the start of the
// next bucket. The last bucket contains the remaining values.
constexpr double kBucketBounds[7] = {0, 3, 6, 12, 25, 50, 75};

// Search backwards using the bucket bounds defined above.
size_t DecideSmoothnessBucket(double pdf) {
  size_t i = std::size(kBucketBounds) - 1;
  while (pdf < kBucketBounds[i])
    i--;
  return i;
}

}  // namespace

using SlidingWindowHistogram = DroppedFrameCounter::SlidingWindowHistogram;

void SlidingWindowHistogram::AddPercentDroppedFrame(
    double percent_dropped_frame,
    size_t count) {
  DCHECK_GE(percent_dropped_frame, 0.0);
  DCHECK_GE(100.0, percent_dropped_frame);
  histogram_bins_[static_cast<int>(std::round(percent_dropped_frame))] += count;
  smoothness_buckets_[DecideSmoothnessBucket(percent_dropped_frame)] += count;
  total_count_ += count;
}

uint32_t SlidingWindowHistogram::GetPercentDroppedFramePercentile(
    double percentile) const {
  if (total_count_ == 0)
    return 0;
  DCHECK_GE(percentile, 0.0);
  DCHECK_GE(1.0, percentile);
  int current_index = 100;  // Last bin in historgam
  uint32_t skipped_counter = histogram_bins_[current_index];  // Last bin values
  double samples_to_skip = ((1 - percentile) * total_count_);
  // We expect this method to calculate higher end percentiles such 95 and as a
  // result we count from the last bin to find the correct bin.
  while (skipped_counter < samples_to_skip && current_index > 0) {
    current_index--;
    skipped_counter += histogram_bins_[current_index];
  }
  return current_index;
}

double SlidingWindowHistogram::GetPercentDroppedFrameVariance() const {
  double sum = 0;
  size_t bin_count = sizeof(histogram_bins_) / sizeof(uint32_t);
  for (size_t i = 0; i < bin_count; ++i) {
    sum += histogram_bins_[i] * i;
  }

  // Don't calculate if count is 1 or less. Avoid divide by zero.
  if (total_count_ <= 1)
    return 0;

  double average = sum / total_count_;
  sum = 0;  // Sum is reset to be used for variance calculation

  for (size_t i = 0; i < bin_count; ++i) {
    sum += histogram_bins_[i] * (i - average) * (i - average);
    // histogram_bins_[i] is the number of PDFs which were in the range of
    // [i,i+1) so i is used as the actual value which is repeated for
    // histogram_bins_[i] times.
  }

  return sum / (total_count_ - 1);
}

std::vector<double> SlidingWindowHistogram::GetPercentDroppedFrameBuckets()
    const {
  if (total_count_ == 0)
    return std::vector<double>(std::size(kBucketBounds), 0);
  std::vector<double> buckets(std::size(kBucketBounds));
  for (size_t i = 0; i < std::size(kBucketBounds); ++i) {
    buckets[i] =
        static_cast<double>(smoothness_buckets_[i]) * 100 / total_count_;
  }
  return buckets;
}

void SlidingWindowHistogram::Clear() {
  std::fill(std::begin(histogram_bins_), std::end(histogram_bins_), 0);
  std::fill(std::begin(smoothness_buckets_), std::end(smoothness_buckets_), 0);
  total_count_ = 0;
}

std::ostream& SlidingWindowHistogram::Dump(std::ostream& stream) const {
  for (size_t i = 0; i < std::size(histogram_bins_); ++i) {
    stream << i << ": " << histogram_bins_[i] << std::endl;
  }
  return stream << "Total: " << total_count_;
}

std::ostream& operator<<(
    std::ostream& stream,
    const DroppedFrameCounter::SlidingWindowHistogram& histogram) {
  return histogram.Dump(stream);
}

DroppedFrameCounter::DroppedFrameCounter()
    : frame_sorter_(base::BindRepeating(&DroppedFrameCounter::NotifyFrameResult,
                                        base::Unretained(this))) {
  sliding_window_interval_ = kDefaultSlidingWindowInterval;
  if (base::FeatureList::IsEnabled(
          features::kSlidingWindowForDroppedFrameCounter)) {
    int sliding_window_seconds = base::GetFieldTrialParamByFeatureAsInt(
        features::kSlidingWindowForDroppedFrameCounter,
        kSlidingWindowForDroppedFrameCounterSeconds, 0);
    TRACE_EVENT1("cc", "kSlidingWindowForDroppedFrameCounterKY",
                 "sliding_window_seconds", sliding_window_seconds);
    if (sliding_window_seconds)
      sliding_window_interval_ = base::Seconds(sliding_window_seconds);
  }
  TRACE_EVENT2("cc", "ctor::DroppedFrameCounterKY", "sliding_window_interval_",
               sliding_window_interval_, "this", (void*)this);
}
DroppedFrameCounter::~DroppedFrameCounter() = default;

uint32_t DroppedFrameCounter::GetAverageThroughput() const {
  size_t good_frames = 0;
  for (auto it = --end(); it; --it) {
    if (**it == kFrameStateComplete || **it == kFrameStatePartial)
      ++good_frames;
  }
  double throughput = 100. * good_frames / ring_buffer_.BufferSize();
  return static_cast<uint32_t>(throughput);
}

void DroppedFrameCounter::AddGoodFrame() {
  ring_buffer_.SaveToBuffer(kFrameStateComplete);
  ++total_frames_;
  auto value = std::make_unique<base::trace_event::TracedValue>();
  value->SetPointer("this", (void*)this);
  value->SetInteger("total_frames_", total_frames_);
  value->SetInteger("total_dropped_", total_dropped_);
  value->SetInteger("total_partial_", total_partial_);
  TRACE_EVENT1("cc", "DroppedFrameCounter::AddGoodFrameKY", "value",
               std::move(value));
}

void DroppedFrameCounter::AddPartialFrame() {
  ring_buffer_.SaveToBuffer(kFrameStatePartial);
  ++total_frames_;
  ++total_partial_;
  auto value = std::make_unique<base::trace_event::TracedValue>();
  value->SetPointer("this", (void*)this);
  value->SetInteger("total_frames_", total_frames_);
  value->SetInteger("total_dropped_", total_dropped_);
  value->SetInteger("total_partial_", total_partial_);
  TRACE_EVENT1("cc", "DroppedFrameCounter::AddPartialFrameKY", "value",
               std::move(value));
}

void DroppedFrameCounter::AddDroppedFrame() {
  ring_buffer_.SaveToBuffer(kFrameStateDropped);
  ++total_frames_;
  ++total_dropped_;
  auto value = std::make_unique<base::trace_event::TracedValue>();
  value->SetPointer("this", (void*)this);
  value->SetInteger("total_frames_", total_frames_);
  value->SetInteger("total_dropped_", total_dropped_);
  value->SetInteger("total_partial_", total_partial_);
  TRACE_EVENT1("cc", "DroppedFrameCounter::AddDroppedFrameKY", "value",
               std::move(value));
}

void DroppedFrameCounter::ResetPendingFrames(base::TimeTicks timestamp) {
  // Start with flushing the frames in frame_sorter ignoring the currently
  // pending frames (In other words calling NotifyFrameResult and update
  // smoothness metrics tracked for all frames that have received their ack).
  frame_sorter_.Reset();
  TRACE_EVENT1("cc", "DroppedFrameCounter::ResetPendingFramesKY", "this", this);

  // Before resetting the pending frames, update the measurements for the
  // sliding windows.
  if (!latest_sliding_window_start_.is_null()) {
    const auto report_until = timestamp - sliding_window_interval_;
    // Report the sliding window metrics for frames that have already been
    // completed (and some of which may have been dropped).
    while (!sliding_window_.empty()) {
      const auto& args = sliding_window_.front().first;
      if (args.frame_time > report_until)
        break;
      PopSlidingWindow();
    }
    if (sliding_window_.empty()) {
      DCHECK_EQ(
          dropped_frame_count_in_window_[SmoothnessStrategy::kDefaultStrategy],
          0u);
      DCHECK_EQ(dropped_frame_count_in_window_
                    [SmoothnessStrategy::kCompositorFocusedStrategy],
                0u);
      DCHECK_EQ(dropped_frame_count_in_window_
                    [SmoothnessStrategy::kMainFocusedStrategy],
                0u);
      DCHECK_EQ(dropped_frame_count_in_window_
                    [SmoothnessStrategy::kScrollFocusedStrategy],
                0u);
    }

    // Report no dropped frames for the sliding windows spanning the rest of the
    // time.
    if (latest_sliding_window_start_ < report_until) {
      const auto difference = report_until - latest_sliding_window_start_;
      const size_t count =
          std::ceil(difference / latest_sliding_window_interval_);
      if (count > 0) {
        sliding_window_histogram_[SmoothnessStrategy::kDefaultStrategy]
            .AddPercentDroppedFrame(0., count);
        sliding_window_histogram_[SmoothnessStrategy::kMainFocusedStrategy]
            .AddPercentDroppedFrame(0., count);
        sliding_window_histogram_
            [SmoothnessStrategy::kCompositorFocusedStrategy]
                .AddPercentDroppedFrame(0., count);
        sliding_window_histogram_[SmoothnessStrategy::kScrollFocusedStrategy]
            .AddPercentDroppedFrame(0., count);
      }
    }
  }

  std::fill_n(dropped_frame_count_in_window_,
              SmoothnessStrategy::kStrategyCount, 0);
  sliding_window_ = {};
  latest_sliding_window_start_ = {};
  latest_sliding_window_interval_ = {};
}

void DroppedFrameCounter::EnableReporForUI() {
  report_for_ui_ = true;
  // We do not allow parameterized sliding windows for UI reports.
  sliding_window_interval_ = base::Seconds(1);
}

void DroppedFrameCounter::OnBeginFrame(const viz::BeginFrameArgs& args,
                                       bool is_scroll_active) {
  // Remember when scrolling starts/ends. Do this even if fcp has not happened
  // yet.
  TRACE_EVENT2("cc", "DroppedFrameCounter::OnBeginFrameKY", "is_scroll_active",
               is_scroll_active, "this", this);
  if (!is_scroll_active) {
    scroll_start_.reset();
  } else if (!scroll_start_.has_value()) {
    ScrollStartInfo info = {args.frame_time, args.frame_id};
    scroll_start_ = info;
  }

  if (fcp_received_) {
    frame_sorter_.AddNewFrame(args);
    if (is_scroll_active) {
      DCHECK(scroll_start_.has_value());
      scroll_start_per_frame_[args.frame_id] = *scroll_start_;
    }
  }
}

void DroppedFrameCounter::OnEndFrame(const viz::BeginFrameArgs& args,
                                     const FrameInfo& frame_info) {
  const bool is_dropped = frame_info.IsDroppedAffectingSmoothness();
  TRACE_EVENT2("cc", "DroppedFrameCounter::OnEndFrameKY", "is_dropped",
               is_dropped, "this", this);
  if (!args.interval.is_zero())
    total_frames_in_window_ = sliding_window_interval_ / args.interval;

  // Don't measure smoothness for frames that start before FCP is received, or
  // that have already been reported as dropped.
  if (is_dropped && fcp_received_ && args.frame_time >= time_fcp_received_ &&
      !frame_sorter_.IsAlreadyReportedDropped(args.frame_id)) {
    ++total_smoothness_dropped_;

    if (report_for_ui_)
      ReportFramesForUI();
    else
      ReportFrames();

    auto iter = scroll_start_per_frame_.find(args.frame_id);
    if (iter != scroll_start_per_frame_.end()) {
      ScrollStartInfo& scroll_start = iter->second;
      if (args.frame_id.source_id == scroll_start.frame_id.source_id) {
        UMA_HISTOGRAM_CUSTOM_TIMES(
            "Graphics.Smoothness.Diagnostic.DroppedFrameAfterScrollStart2.Time",
            (args.frame_time - scroll_start.timestamp), base::Milliseconds(1),
            base::Seconds(4), 50);
        UMA_HISTOGRAM_CUSTOM_COUNTS(
            "Graphics.Smoothness.Diagnostic.DroppedFrameAfterScrollStart2."
            "Frames",
            (args.frame_id.sequence_number -
             scroll_start.frame_id.sequence_number),
            1, 250, 50);
      }
      scroll_start_per_frame_.erase(iter);
    }
  }

  if (fcp_received_)
    frame_sorter_.AddFrameResult(args, frame_info);
}

void DroppedFrameCounter::ReportFrames() {
  DCHECK(!report_for_ui_);
  TRACE_EVENT2("cc", "DroppedFrameCounter::ReportFramesKY", "total_frames_",
               total_frames_, "this", this);
  // 表示所有可见帧数量 = 可见时间/16.6
  const auto total_frames =
      total_counter_->ComputeTotalVisibleFrames(base::TimeTicks::Now());
  auto value1 = std::make_unique<base::trace_event::TracedValue>();
  value1->SetInteger("total_frames", total_frames);
  value1->SetInteger("total_frames_", total_frames_);
  value1->SetInteger("total_smoothness_dropped_", total_smoothness_dropped_);
  value1->SetInteger("total_dropped_", total_dropped_);
  value1->SetInteger("total_partial_", total_partial_);
  TRACE_EVENT1("cc,benchmark", "SmoothnessDroppedFrameKY", "value",
               std::move(value1));
  if (sliding_window_max_percent_dropped_ !=
      last_reported_metrics_.max_window) {
    UMA_HISTOGRAM_PERCENTAGE(
        "Graphics.Smoothness.MaxPercentDroppedFrames_1sWindow",
        sliding_window_max_percent_dropped_);
    last_reported_metrics_.max_window = sliding_window_max_percent_dropped_;
  }

  uint32_t sliding_window_95pct_percent_dropped =
      SlidingWindow95PercentilePercentDropped(
          SmoothnessStrategy::kDefaultStrategy);
  if (sliding_window_95pct_percent_dropped !=
      last_reported_metrics_.p95_window) {
    UMA_HISTOGRAM_PERCENTAGE(
        "Graphics.Smoothness.95pctPercentDroppedFrames_1sWindow",
        sliding_window_95pct_percent_dropped);
    last_reported_metrics_.p95_window = sliding_window_95pct_percent_dropped;
  }

  DCHECK_LE(
      sliding_window_95pct_percent_dropped,
      static_cast<uint32_t>(std::round(sliding_window_max_percent_dropped_)));

  // Emit trace event with most recent smoothness calculation. This matches
  // the smoothness metrics displayed on HeadsUpDisplay.
  TRACE_EVENT2("cc,benchmark",
               "SmoothnessDroppedFrame::MostRecentCalculationKY",
               "worst_smoothness-sliding_window_max_percent_dropped_",
               sliding_window_max_percent_dropped_,
               "95_percentile_smoothness-sliding_window_95pct_percent_dropped",
               sliding_window_95pct_percent_dropped);

  if (ukm_smoothness_data_ && total_frames > 0) {
    UkmSmoothnessData smoothness_data;
    // 如果画面一直没有变化则这些帧也会被记入 total_frames，导致得到的
    // avg_smoothness 不准确，这种情况下统计的 avg_smoothness 是最优情况
    smoothness_data.avg_smoothness =
        static_cast<double>(total_smoothness_dropped_) * 100 / total_frames;
    smoothness_data.worst_smoothness = sliding_window_max_percent_dropped_;
    smoothness_data.percentile_95 = sliding_window_95pct_percent_dropped;
    smoothness_data.median_smoothness =
        SlidingWindowMedianPercentDropped(SmoothnessStrategy::kDefaultStrategy);
    std::stringstream ss;
    ss << sliding_window_histogram_[SmoothnessStrategy::kDefaultStrategy];

    uint32_t default_variance =
        static_cast<uint32_t>(SlidingWindowPercentDroppedVariance(
            SmoothnessStrategy::kDefaultStrategy));
    DCHECK_LE(default_variance, 5000u);
    DCHECK_LE(0u, default_variance);
    smoothness_data.variance = default_variance;

    std::vector<double> sliding_window_buckets =
        sliding_window_histogram_[SmoothnessStrategy::kDefaultStrategy]
            .GetPercentDroppedFrameBuckets();
    DCHECK_EQ(sliding_window_buckets.size(),
              std::size(smoothness_data.buckets));
    base::ranges::copy(sliding_window_buckets, smoothness_data.buckets);

    smoothness_data.main_focused_median = SlidingWindowMedianPercentDropped(
        SmoothnessStrategy::kMainFocusedStrategy);
    smoothness_data.main_focused_percentile_95 =
        SlidingWindow95PercentilePercentDropped(
            SmoothnessStrategy::kMainFocusedStrategy);
    smoothness_data.main_focused_variance =
        static_cast<uint32_t>(SlidingWindowPercentDroppedVariance(
            SmoothnessStrategy::kMainFocusedStrategy));

    smoothness_data.compositor_focused_median =
        SlidingWindowMedianPercentDropped(
            SmoothnessStrategy::kCompositorFocusedStrategy);
    smoothness_data.compositor_focused_percentile_95 =
        SlidingWindow95PercentilePercentDropped(
            SmoothnessStrategy::kCompositorFocusedStrategy);
    smoothness_data.compositor_focused_variance =
        static_cast<uint32_t>(SlidingWindowPercentDroppedVariance(
            SmoothnessStrategy::kCompositorFocusedStrategy));

    smoothness_data.scroll_focused_median = SlidingWindowMedianPercentDropped(
        SmoothnessStrategy::kScrollFocusedStrategy);
    smoothness_data.scroll_focused_percentile_95 =
        SlidingWindow95PercentilePercentDropped(
            SmoothnessStrategy::kScrollFocusedStrategy);
    smoothness_data.scroll_focused_variance =
        static_cast<uint32_t>(SlidingWindowPercentDroppedVariance(
            SmoothnessStrategy::kScrollFocusedStrategy));

    if (sliding_window_max_percent_dropped_After_1_sec_.has_value())
      smoothness_data.worst_smoothness_after1sec =
          sliding_window_max_percent_dropped_After_1_sec_.value();
    if (sliding_window_max_percent_dropped_After_2_sec_.has_value())
      smoothness_data.worst_smoothness_after2sec =
          sliding_window_max_percent_dropped_After_2_sec_.value();
    if (sliding_window_max_percent_dropped_After_5_sec_.has_value())
      smoothness_data.worst_smoothness_after5sec =
          sliding_window_max_percent_dropped_After_5_sec_.value();
    auto value = std::make_unique<base::trace_event::TracedValue>();
    value->SetString(
        "sliding_window_histogram_[SmoothnessStrategy::kDefaultStrategy]",
        ss.str());
    value->SetDouble("avg_smoothness", smoothness_data.avg_smoothness);
    value->SetDouble("worst_smoothness-sliding_window_max_percent_dropped_",
                     smoothness_data.worst_smoothness);
    value->SetDouble("median_smoothness", smoothness_data.median_smoothness);

    value->SetDouble("worst_smoothness_after1sec",
                     smoothness_data.worst_smoothness_after1sec);
    value->SetDouble("worst_smoothness_after2sec",
                     smoothness_data.worst_smoothness_after2sec);
    value->SetDouble("worst_smoothness_after5sec",
                     smoothness_data.worst_smoothness_after5sec);

    value->SetDouble("above_threshold", smoothness_data.above_threshold);
    value->SetDouble("percentile_95-sliding_window_95pct_percent_dropped",
                     smoothness_data.percentile_95);
    value->SetDouble("variance-SlidingWindowPercentDroppedVariance",
                     smoothness_data.variance);

    value->SetDouble("scroll_focused_median",
                     smoothness_data.scroll_focused_median);
    value->SetDouble("scroll_focused_percentile_95",
                     smoothness_data.scroll_focused_percentile_95);
    value->SetDouble("scroll_focused_variance",
                     smoothness_data.scroll_focused_variance);

    value->SetDouble("main_focused_median",
                     smoothness_data.main_focused_median);
    value->SetDouble("main_focused_percentile_95",
                     smoothness_data.main_focused_percentile_95);
    value->SetDouble("main_focused_variance",
                     smoothness_data.main_focused_variance);

    value->SetDouble("compositor_focused_median",
                     smoothness_data.compositor_focused_median);
    value->SetDouble("compositor_focused_percentile_95",
                     smoothness_data.compositor_focused_percentile_95);
    value->SetDouble("compositor_focused_variance",
                     smoothness_data.compositor_focused_variance);

    TRACE_EVENT1("cc", "smoothness_dataKY", "value", std::move(value));
    ukm_smoothness_data_->Write(smoothness_data);
  }
}

void DroppedFrameCounter::ReportFramesForUI() {
  DCHECK(report_for_ui_);
  TRACE_EVENT1("cc", "DroppedFrameCounter::ReportFramesForUIKY", "this", this);

  auto* recorder = CustomMetricRecorder::Get();
  if (!recorder)
    return;

  recorder->ReportPercentDroppedFramesInOneSecondWindow(
      sliding_window_current_percent_dropped_);
}

double DroppedFrameCounter::GetMostRecentAverageSmoothness() const {
  if (ukm_smoothness_data_)
    return ukm_smoothness_data_->data.avg_smoothness;

  return -1.f;
}

double DroppedFrameCounter::GetMostRecent95PercentileSmoothness() const {
  if (ukm_smoothness_data_)
    return ukm_smoothness_data_->data.percentile_95;

  return -1.f;
}

void DroppedFrameCounter::SetUkmSmoothnessDestination(
    UkmSmoothnessDataShared* smoothness_data) {
  ukm_smoothness_data_ = smoothness_data;
}

void DroppedFrameCounter::Reset() {
  TRACE_EVENT1("cc", "DroppedFrameCounter::ResetKY", "this", this);
  frame_sorter_.Reset();
  total_frames_ = 0;
  total_partial_ = 0;
  total_dropped_ = 0;
  total_smoothness_dropped_ = 0;
  sliding_window_max_percent_dropped_ = 0;
  sliding_window_max_percent_dropped_After_1_sec_.reset();
  sliding_window_max_percent_dropped_After_2_sec_.reset();
  sliding_window_max_percent_dropped_After_5_sec_.reset();
  std::fill_n(dropped_frame_count_in_window_,
              SmoothnessStrategy::kStrategyCount, 0);
  fcp_received_ = false;
  sliding_window_ = {};
  latest_sliding_window_start_ = {};
  sliding_window_histogram_[SmoothnessStrategy::kDefaultStrategy].Clear();
  sliding_window_histogram_[SmoothnessStrategy::kScrollFocusedStrategy].Clear();
  sliding_window_histogram_[SmoothnessStrategy::kMainFocusedStrategy].Clear();
  sliding_window_histogram_[SmoothnessStrategy::kCompositorFocusedStrategy]
      .Clear();
  ring_buffer_.Clear();
  last_reported_metrics_ = {};
}

base::TimeDelta DroppedFrameCounter::ComputeCurrentWindowSize() const {
  if (sliding_window_.empty()) {
    TRACE_EVENT1("cc", "DroppedFrameCounter::ComputeCurrentWindowSizeKY-zero",
                 "zero", true);
    return {};
  }
  auto result = sliding_window_.back().first.frame_time +
                sliding_window_.back().first.interval -
                sliding_window_.front().first.frame_time;

  TRACE_EVENT1("cc", "DroppedFrameCounter::ComputeCurrentWindowSizeKY",
               "result", result.InMicroseconds());
  return result;
}

void DroppedFrameCounter::NotifyFrameResult(const viz::BeginFrameArgs& args,
                                            const FrameInfo& frame_info) {
  TRACE_EVENT2("cc", "DroppedFrameCounter::NotifyFrameResultKY",
               "sliding_window_interval_", sliding_window_interval_, "this",
               this);
  // Entirely disregard the frames with interval larger than the window --
  // these are violating the assumptions in the below code and should
  // only occur with external frame control, where dropped frame stats
  // are not relevant.
  if (args.interval >= sliding_window_interval_)
    return;

  if (sorted_frame_callback_)
    sorted_frame_callback_->Run(args, frame_info);

  {
    auto value = std::make_unique<base::trace_event::TracedValue>();
    value->SetString("args.frame_time",
                     std::to_string(args.frame_time.ToInternalValue()));
    value->SetString(
        "now-args.frame_time",
        std::to_string(
            (base::TimeTicks::Now() - args.frame_time).InMicroseconds()));
    value->SetString("args.frame_id", args.frame_id.ToString());
    TRACE_EVENT1("cc", "sliding_window_.pushKY", "value", std::move(value));
  }
  sliding_window_.push({args, frame_info});
  UpdateDroppedFrameCountInWindow(frame_info, 1);

  const bool is_dropped = frame_info.IsDroppedAffectingSmoothness();
  {
    TRACE_EVENT2("cc", "DroppedFrameDuration1KY", "is_dropped", is_dropped,
                 "in_dropping_", in_dropping_);
  }
  if (!in_dropping_ && is_dropped) {
    TRACE_EVENT_NESTABLE_ASYNC_BEGIN_WITH_TIMESTAMP0(
        "cc,benchmark", "DroppedFrameDurationKY", TRACE_ID_LOCAL(this),
        args.frame_time);
    in_dropping_ = true;
  } else if (in_dropping_ && !is_dropped) {
    TRACE_EVENT_NESTABLE_ASYNC_END_WITH_TIMESTAMP0(
        "cc,benchmark", "DroppedFrameDurationKY", TRACE_ID_LOCAL(this),
        args.frame_time);
    in_dropping_ = false;
  }

  if (ComputeCurrentWindowSize() < sliding_window_interval_)
    return;
  DCHECK_GE(
      dropped_frame_count_in_window_[SmoothnessStrategy::kDefaultStrategy], 0u);
  DCHECK_GE(
      sliding_window_.size(),
      dropped_frame_count_in_window_[SmoothnessStrategy::kDefaultStrategy]);

  auto current_window_interval = ComputeCurrentWindowSize();
  TRACE_EVENT2("cc",
               "ComputeCurrentWindowSize() < sliding_window_interval_ = false",
               "current_window_interval", current_window_interval, "size",
               sliding_window_.size());
  while (current_window_interval > sliding_window_interval_) {
    TRACE_EVENT2(
        "cc", "iterator:current_window_interval>sliding_window_interval_",
        "current_window_interval", current_window_interval.InMicroseconds(),
        "size", sliding_window_.size());
    PopSlidingWindow();
    current_window_interval = ComputeCurrentWindowSize();
  }
  DCHECK(!sliding_window_.empty());
}

void DroppedFrameCounter::PopSlidingWindow() {
  TRACE_EVENT1("cc", "DroppedFrameCounter::PopSlidingWindowKY", "this", this);
  const auto removed_args = sliding_window_.front().first;
  const auto removed_frame_info = sliding_window_.front().second;

  // uint32_t dropped_frame_count_in_window_backup[sizeof(
  //     dropped_frame_count_in_window_)];
  // std::copy(std::begin(dropped_frame_count_in_window_),
  //           std::end(dropped_frame_count_in_window_),
  //           std::begin(dropped_frame_count_in_window_backup));

  UpdateDroppedFrameCountInWindow(removed_frame_info, -1);
  sliding_window_.pop();
  if (sliding_window_.empty())
    return;
  auto value = std::make_unique<base::trace_event::TracedValue>();

  // Don't count the newest element if it is outside the current window.
  const auto& newest_args = sliding_window_.back().first;
  const auto newest_was_dropped =
      sliding_window_.back().second.IsDroppedAffectingSmoothness();

  uint32_t invalidated_frames = 0;
  if (ComputeCurrentWindowSize() > sliding_window_interval_ &&
      newest_was_dropped) {
    invalidated_frames++;
  }
  value->SetInteger("invalidated_frames", invalidated_frames);

  // If two consecutive 'completed' frames are far apart from each other (in
  // time), then report the 'dropped frame count' for the sliding window(s) in
  // between. Note that the window-size still needs to be at least
  // sliding_window_interval_.
  const auto max_sliding_window_start =
      newest_args.frame_time - sliding_window_interval_;
  const auto max_difference = newest_args.interval * 1.5;
  const auto& remaining_oldest_args = sliding_window_.front().first;
  const auto last_timestamp =
      std::min(remaining_oldest_args.frame_time, max_sliding_window_start);
  const auto difference = last_timestamp - removed_args.frame_time;
  // 如果画面在一段时间内没有变化，这些帧会被认为没有丢帧，也会统计在内。
  const size_t count = difference > max_difference
                           ? std::ceil(difference / newest_args.interval)
                           : 1;
  // size_t count2 = 0;
  // size_t idle_count = 0;
  // auto removed_duration =
  //     remaining_oldest_args.frame_time - removed_args.frame_time;
  // if (removed_duration > sliding_window_interval_) {
  //   count2 = total_frames_in_window_;
  //   const auto idle_duration = removed_duration - sliding_window_interval_;
  //   idle_count = idle_duration / remaining_oldest_args.interval;
  // } else {
  //   count2 = removed_duration / remaining_oldest_args.interval;
  // }
  value->BeginDictionary("count_compute");
  value->SetInteger("sliding_window_.size", sliding_window_.size());
  value->SetString("newest_args.frame_time",
                   std::to_string(newest_args.frame_time.ToInternalValue()));
  value->SetString(
      "remaining_oldest_args.frame_time",
      std::to_string(remaining_oldest_args.frame_time.ToInternalValue()));
  value->SetString("removed_args.frame_time",
                   std::to_string(removed_args.frame_time.ToInternalValue()));
  value->SetString("max_sliding_window_start",
                   std::to_string(max_sliding_window_start.ToInternalValue()));
  value->SetString("last_timestamp",
                   std::to_string(last_timestamp.ToInternalValue()));
  value->SetString("difference", std::to_string(difference.ToInternalValue()));
  value->SetString("max_difference",
                   std::to_string(max_difference.ToInternalValue()));
  value->SetString("newest_args.interval",
                   std::to_string(newest_args.interval.ToInternalValue()));
  value->EndDictionary();
  value->SetInteger("count", count);
  value->SetInteger("total_frames_", total_frames_);
  value->SetInteger("total_dropped_", total_dropped_);
  value->SetInteger("total_frames_in_window_", total_frames_in_window_);

  // uint32_t dropped = dropped_frame_count_in_window_backup
  //                        [SmoothnessStrategy::kDefaultStrategy] -
  //                    invalidated_frames;
  uint32_t dropped =
      dropped_frame_count_in_window_[SmoothnessStrategy::kDefaultStrategy] -
      invalidated_frames;
  const double percent_dropped_frame =
      std::min((dropped * 100.0) / total_frames_in_window_, 100.0);
  sliding_window_histogram_[SmoothnessStrategy::kDefaultStrategy]
      .AddPercentDroppedFrame(percent_dropped_frame, count);
  // sliding_window_histogram_[SmoothnessStrategy::kDefaultStrategy]
  //     .AddPercentDroppedFrame(percent_dropped_frame, count2);
  // if (idle_count > 0) {
  //   sliding_window_histogram_[SmoothnessStrategy::kDefaultStrategy]
  //       .AddPercentDroppedFrame(0, idle_count);
  // }

  std::stringstream default_strategy_ss;
  default_strategy_ss
      << sliding_window_histogram_[SmoothnessStrategy::kDefaultStrategy];
  value->SetString("update-default_strategy_ss", default_strategy_ss.str());

  uint32_t dropped_compositor =
      dropped_frame_count_in_window_
          [SmoothnessStrategy::kCompositorFocusedStrategy] -
      invalidated_frames;
  double percent_dropped_frame_compositor =
      std::min((dropped_compositor * 100.0) / total_frames_in_window_, 100.0);
  sliding_window_histogram_[SmoothnessStrategy::kCompositorFocusedStrategy]
      .AddPercentDroppedFrame(percent_dropped_frame_compositor, count);

  uint32_t dropped_main =
      dropped_frame_count_in_window_[SmoothnessStrategy::kMainFocusedStrategy] -
      invalidated_frames;
  double percent_dropped_frame_main =
      std::min((dropped_main * 100.0) / total_frames_in_window_, 100.0);
  sliding_window_histogram_[SmoothnessStrategy::kMainFocusedStrategy]
      .AddPercentDroppedFrame(percent_dropped_frame_main, count);

  uint32_t dropped_scroll = dropped_frame_count_in_window_
                                [SmoothnessStrategy::kScrollFocusedStrategy] -
                            invalidated_frames;
  double percent_dropped_frame_scroll =
      std::min((dropped_scroll * 100.0) / total_frames_in_window_, 100.0);
  sliding_window_histogram_[SmoothnessStrategy::kScrollFocusedStrategy]
      .AddPercentDroppedFrame(percent_dropped_frame_scroll, count);

  value->SetInteger("sliding_window_max_percent_dropped_",
                    sliding_window_max_percent_dropped_);
  if (percent_dropped_frame > sliding_window_max_percent_dropped_)
    sliding_window_max_percent_dropped_ = percent_dropped_frame;

  sliding_window_current_percent_dropped_ = percent_dropped_frame;

  latest_sliding_window_start_ = last_timestamp;
  latest_sliding_window_interval_ = remaining_oldest_args.interval;

  value->SetInteger("latest_sliding_window_start_",
                    latest_sliding_window_start_.ToInternalValue());
  value->SetInteger("latest_sliding_window_interval_",
                    latest_sliding_window_interval_.InMicroseconds());
  value->SetInteger("total_frames_in_window_", total_frames_in_window_);
  value->SetInteger("dropped", dropped);
  value->SetInteger("dropped_compositor", dropped_compositor);
  value->SetInteger("dropped_main", dropped_main);
  value->SetInteger("dropped_scroll", dropped_scroll);
  value->SetDouble("percent_dropped_frame", percent_dropped_frame);
  value->SetDouble("percent_dropped_frame_compositor",
                   percent_dropped_frame_compositor);
  value->SetDouble("percent_dropped_frame_main", percent_dropped_frame_main);
  value->SetDouble("percent_dropped_frame_scroll",
                   percent_dropped_frame_scroll);
  TRACE_EVENT1("cc", "slidewindow:sliding_window_histogram_ZK", "value",
               std::move(value));

  UpdateMaxPercentDroppedFrame(percent_dropped_frame);
}

void DroppedFrameCounter::UpdateDroppedFrameCountInWindow(
    const FrameInfo& frame_info,
    int count) {
  TRACE_EVENT1("cc", "DroppedFrameCounter::UpdateDroppedFrameCountInWindowKY",
               "count", count);
  if (frame_info.IsDroppedAffectingSmoothness()) {
    DCHECK_GE(
        dropped_frame_count_in_window_[SmoothnessStrategy::kDefaultStrategy] +
            count,
        0u);
    TRACE_EVENT0("cc", "Drop:kDefaultStrategyKY-IsDroppedAffectingSmoothness");
    dropped_frame_count_in_window_[SmoothnessStrategy::kDefaultStrategy] +=
        count;
  }
  if (frame_info.WasSmoothCompositorUpdateDropped()) {
    DCHECK_GE(dropped_frame_count_in_window_
                      [SmoothnessStrategy::kCompositorFocusedStrategy] +
                  count,
              0u);
    TRACE_EVENT0(
        "cc",
        "Drop:kCompositorFocusedStrategyKY-WasSmoothCompositorUpdateDropped");
    dropped_frame_count_in_window_
        [SmoothnessStrategy::kCompositorFocusedStrategy] += count;
  }
  if (frame_info.WasSmoothMainUpdateDropped()) {
    DCHECK_GE(dropped_frame_count_in_window_
                      [SmoothnessStrategy::kMainFocusedStrategy] +
                  count,
              0u);
    TRACE_EVENT0("cc",
                 "Drop:kMainFocusedStrategyKY-WasSmoothMainUpdateDropped");
    dropped_frame_count_in_window_[SmoothnessStrategy::kMainFocusedStrategy] +=
        count;
  }
  if (frame_info.IsScrollPrioritizeFrameDropped()) {
    DCHECK_GE(dropped_frame_count_in_window_
                      [SmoothnessStrategy::kScrollFocusedStrategy] +
                  count,
              0u);
    TRACE_EVENT0(
        "cc", "Drop:kScrollFocusedStrategyKY-IsScrollPrioritizeFrameDropped");
    dropped_frame_count_in_window_
        [SmoothnessStrategy::kScrollFocusedStrategy] += count;
  }
  auto value = std::make_unique<base::trace_event::TracedValue>();
  value->SetInteger("total_frames_in_window_", total_frames_in_window_);
  value->SetInteger("total_frames_", total_frames_);
  value->SetInteger("total_dropped_", total_dropped_);
  value->SetInteger(
      "dropped_frame_count_in_window_[kDefaultStrategy]",
      dropped_frame_count_in_window_[SmoothnessStrategy::kDefaultStrategy]);
  value->SetInteger(
      "dropped_frame_count_in_window_[kCompositorFocusedStrategy]",
      dropped_frame_count_in_window_
          [SmoothnessStrategy::kCompositorFocusedStrategy]);
  value->SetInteger(
      "dropped_frame_count_in_window_[kMainFocusedStrategy]",
      dropped_frame_count_in_window_[SmoothnessStrategy::kMainFocusedStrategy]);
  value->SetInteger("dropped_frame_count_in_window_[kScrollFocusedStrategy]",
                    dropped_frame_count_in_window_
                        [SmoothnessStrategy::kScrollFocusedStrategy]);
  value->SetInteger("count", count);
  TRACE_EVENT1("cc", "slidewindow:dropped_frame_count_in_window_KY", "value",
               std::move(value));
}

void DroppedFrameCounter::UpdateMaxPercentDroppedFrame(
    double percent_dropped_frame) {
  TRACE_EVENT2("cc", "DroppedFrameCounter::UpdateMaxPercentDroppedFrameKY",
               "this", (void*)this, "fcp_received_", fcp_received_);
  if (!fcp_received_)
    return;
  const auto fcp_time_delta = latest_sliding_window_start_ - time_fcp_received_;

  if (fcp_time_delta > base::Seconds(1))
    sliding_window_max_percent_dropped_After_1_sec_ =
        std::max(sliding_window_max_percent_dropped_After_1_sec_.value_or(0.0),
                 percent_dropped_frame);
  if (fcp_time_delta > base::Seconds(2))
    sliding_window_max_percent_dropped_After_2_sec_ =
        std::max(sliding_window_max_percent_dropped_After_2_sec_.value_or(0.0),
                 percent_dropped_frame);
  if (fcp_time_delta > base::Seconds(5))
    sliding_window_max_percent_dropped_After_5_sec_ =
        std::max(sliding_window_max_percent_dropped_After_5_sec_.value_or(0.0),
                 percent_dropped_frame);
  auto value = std::make_unique<base::trace_event::TracedValue>();
  value->SetDouble("percent_dropped_frame", percent_dropped_frame);
  value->SetString("time_fcp_received_",
                   std::to_string(time_fcp_received_.ToInternalValue()));
  value->SetString(
      "latest_sliding_window_start_",
      std::to_string(latest_sliding_window_start_.ToInternalValue()));
  value->SetDouble("fcp_time_delta(s)", fcp_time_delta.InSecondsF());
  value->SetDouble("sliding_window_max_percent_dropped_After_1_sec_",
                   sliding_window_max_percent_dropped_After_1_sec_.value_or(0));
  value->SetDouble("sliding_window_max_percent_dropped_After_3_sec_",
                   sliding_window_max_percent_dropped_After_1_sec_.value_or(0));
  value->SetDouble("sliding_window_max_percent_dropped_After_5_sec_",
                   sliding_window_max_percent_dropped_After_1_sec_.value_or(0));
  TRACE_EVENT1("cc", "slidewindow:max_percent_dropped_xx", "value",
               std::move(value));
}

void DroppedFrameCounter::OnFcpReceived() {
  DCHECK(!fcp_received_);
  fcp_received_ = true;
  time_fcp_received_ = base::TimeTicks::Now();
  TRACE_EVENT2("cc", "DroppedFrameCounter::OnFcpReceivedKY", "this",
               (void*)this, "time_fcp_received_", time_fcp_received_);
}

void DroppedFrameCounter::SetSortedFrameCallback(SortedFrameCallback callback) {
  sorted_frame_callback_ = callback;
}

}  // namespace cc
