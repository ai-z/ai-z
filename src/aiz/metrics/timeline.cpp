#include <aiz/metrics/timeline.h>

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

}  // namespace aiz
