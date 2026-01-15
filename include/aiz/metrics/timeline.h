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

private:
  std::vector<double> buf_;
  std::size_t head_ = 0;
  std::size_t size_ = 0;
};

}  // namespace aiz
