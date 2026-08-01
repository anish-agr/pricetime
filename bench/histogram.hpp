#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace pricetime::bench {

struct LatencyStats {
  double p50 = 0;
  double p90 = 0;
  double p99 = 0;
  double p999 = 0;
  double mean = 0;
  std::uint64_t min = 0;
  std::uint64_t max = 0;
  std::size_t count = 0;
};

// Records every sample and sorts once at the end: exact percentiles, no
// binning error. Memory cost is 8 bytes/sample, which is fine at bench scale.
class SampleSet {
 public:
  explicit SampleSet(std::size_t expected = 0) { samples_.reserve(expected); }

  void add(std::uint64_t ticks) { samples_.push_back(ticks); }

  [[nodiscard]] std::size_t size() const noexcept { return samples_.size(); }

  // Sorts in place; call once when sampling is done.
  [[nodiscard]] LatencyStats stats() {
    LatencyStats s;
    if (samples_.empty()) return s;
    std::sort(samples_.begin(), samples_.end());
    s.count = samples_.size();
    s.min = samples_.front();
    s.max = samples_.back();
    long double sum = 0;
    for (const std::uint64_t v : samples_) sum += v;
    s.mean = static_cast<double>(sum / static_cast<long double>(samples_.size()));
    s.p50 = percentile(0.50);
    s.p90 = percentile(0.90);
    s.p99 = percentile(0.99);
    s.p999 = percentile(0.999);
    return s;
  }

 private:
  [[nodiscard]] double percentile(double q) const {
    const auto idx =
        static_cast<std::size_t>(std::llround(q * static_cast<double>(samples_.size() - 1)));
    return static_cast<double>(samples_[idx]);
  }

  std::vector<std::uint64_t> samples_;
};

}  // namespace pricetime::bench
