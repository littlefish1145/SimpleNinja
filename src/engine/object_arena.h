// Stable, owning graph storage: amortize heap calls without extending lifetimes.
#ifndef SNINJA_OBJECT_ARENA_H_
#define SNINJA_OBJECT_ARENA_H_
#include <cassert>
#include <cstddef>
#include <memory>
#include <utility>
#include <vector>

namespace sninja {
template <class T, size_t BlockSize = 256>
class ObjectArena {
  struct alignas(T) Slot { std::byte bytes[sizeof(T)]; };
  std::vector<std::unique_ptr<Slot[]>> blocks_;
  size_t size_ = 0;

  T* At(size_t i) const {
    return std::launder(reinterpret_cast<T*>(blocks_[i / BlockSize][i % BlockSize].bytes));
  }
 public:
  ObjectArena() = default;
  ObjectArena(const ObjectArena&) = delete;
  ObjectArena& operator=(const ObjectArena&) = delete;
  ~ObjectArena() {
    while (size_)
      std::destroy_at(At(--size_));
  }
  template <class... Args>
  T* Create(Args&&... args) {
    if (size_ == blocks_.size() * BlockSize)
      blocks_.push_back(std::make_unique_for_overwrite<Slot[]>(BlockSize));
    // Before construction there is no T to launder yet.
    T* slot = reinterpret_cast<T*>(blocks_[size_ / BlockSize][size_ % BlockSize].bytes);
    T* result = std::construct_at(slot, std::forward<Args>(args)...);
    ++size_;
    return result;
  }
  void Pop(T* last) {
    assert(size_ && At(size_ - 1) == last);
    std::destroy_at(last);
    --size_;
  }
};
}
#endif
