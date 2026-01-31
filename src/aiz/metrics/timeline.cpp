#include <aiz/metrics/timeline.h>

#include <limits>

namespace aiz {

Timeline::Timeline(std::size_t capacity) : buf_(capacity, 0.0) {}

void Timeline::push(double value) {
  if (buf_.empty()) return;
  buf_[head_] = value;
  head_ = (head_ + 1) % buf_.size();
  if (size_ < buf_.size()) {
    ++size_;
  }
}

std::size_t Timeline::size() const { return size_; }
std::size_t Timeline::capacity() const { return buf_.size(); }

std::vector<double> Timeline::values() const {
  std::vector<double> out;
  out.reserve(size_);
  if (size_ == 0) return out;

  const std::size_t start = (size_ < buf_.size()) ? 0 : head_;
  const std::size_t count = size_;
  for (std::size_t i = 0; i < count; ++i) {
    const std::size_t idx = (start + i) % buf_.size();
    out.push_back(buf_[idx]);
  }
  return out;
}

double Timeline::maxLast(std::size_t n) const {
  if (size_ == 0) return 0.0;
  const std::size_t count = (n < size_) ? n : size_;
  double maxVal = -std::numeric_limits<double>::infinity();
  
  // Iterate backwards from most recent sample
  for (std::size_t i = 0; i < count; ++i) {
    // head_ points to next write position, so most recent is at (head_ - 1)
    const std::size_t idx = (head_ + buf_.size() - 1 - i) % buf_.size();
    if (buf_[idx] > maxVal) maxVal = buf_[idx];
  }
  return maxVal;
}

double Timeline::max() const {
  return maxLast(size_);
}

}  // namespace aiz
