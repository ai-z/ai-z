#pragma once

#include <cstddef>
#include <vector>

namespace aiz {

class Timeline {
public:
  explicit Timeline(std::size_t capacity);

  void push(double value);
  std::size_t size() const;
  std::size_t capacity() const;

  // Oldest -> newest.
  std::vector<double> values() const;

  // Maximum value over the last n samples (or all samples if n >= size()).
  double maxLast(std::size_t n) const;

  // Maximum value over all stored samples.
  double max() const;

private:
  std::vector<double> buf_;
  std::size_t head_ = 0;
  std::size_t size_ = 0;
};

}  // namespace aiz
