// Compact graph adjacency: one pointer occupies the same space as a heap pointer.
// A one-element list therefore needs no allocation and costs no extra bytes.
#ifndef SNINJA_POINTER_VECTOR_H_
#define SNINJA_POINTER_VECTOR_H_
#include <algorithm>
#include <cassert>
#include <concepts>
#include <functional>
#include <iterator>
#include <memory>
#include <type_traits>
#include <utility>
#include <vector>

namespace sninja {
template <class T> requires std::is_pointer_v<T>
class PointerVector {
  union Storage {
    T one;
    T* heap;
    Storage() : one(nullptr) {}
  } storage_;
  size_t size_ = 0;
  size_t capacity_ = 1;

  size_t GrowthCapacity() const {
    // Preserve cheap doubling for the usual tiny adjacency lists. Large
    // fan-out nodes otherwise waste nearly half their pointer storage.
    return capacity_ + (capacity_ < 64 ? capacity_ : capacity_ / 2);
  }

 public:
  using value_type = T;
  using iterator = T*;
  using const_iterator = const T*;
  PointerVector() = default;
  PointerVector(const PointerVector& other) {
    reserve(other.size());
    std::copy(other.begin(), other.end(), data());
    size_ = other.size();
  }
  PointerVector(PointerVector&& other) noexcept {
    if (other.capacity_ == 1)
      storage_.one = other.storage_.one;
    else
      storage_.heap = other.storage_.heap;
    size_ = std::exchange(other.size_, 0);
    capacity_ = std::exchange(other.capacity_, 1);
    other.storage_.one = nullptr;
  }
  PointerVector& operator=(const PointerVector& other) {
    if (this != &other) {
      reserve(other.size());
      std::copy(other.begin(), other.end(), data());
      size_ = other.size();
    }
    return *this;
  }
  PointerVector& operator=(PointerVector&& other) noexcept {
    if (this != &other) {
      this->~PointerVector();
      std::construct_at(this, std::move(other));
    }
    return *this;
  }
  ~PointerVector() { if (capacity_ > 1) delete[] storage_.heap; }
  operator std::vector<T>() const { return {begin(), end()}; }
  size_t size() const { return size_; }
  size_t capacity() const { return capacity_; }
  bool empty() const { return size_ == 0; }
  T* data() { return capacity_ == 1 ? &storage_.one : storage_.heap; }
  const T* data() const { return capacity_ == 1 ? &storage_.one : storage_.heap; }
  iterator begin() { return data(); }
  const_iterator begin() const { return data(); }
  const_iterator cbegin() const { return data(); }
  iterator end() { return data() + size_; }
  const_iterator end() const { return data() + size_; }
  const_iterator cend() const { return data() + size_; }
  T& operator[](size_t i) { assert(i < size_); return data()[i]; }
  const T& operator[](size_t i) const { assert(i < size_); return data()[i]; }
  T& front() { return (*this)[0]; }
  const T& front() const { return (*this)[0]; }
  T& back() { return (*this)[size_ - 1]; }
  const T& back() const { return (*this)[size_ - 1]; }
  void clear() { size_ = 0; }
  void pop_back() { assert(size_); --size_; }
  void reserve(size_t capacity) {
    if (capacity <= capacity_) return;
    T* replacement = new T[capacity];
    std::copy(begin(), end(), replacement);
    if (capacity_ > 1) delete[] storage_.heap;
    storage_.heap = replacement;
    capacity_ = capacity;
  }
  void push_back(T value) {
    if (size_ == capacity_) reserve(GrowthCapacity());
    data()[size_++] = value;
  }
  void resize(size_t size, T value = nullptr) {
    reserve(size);
    if (size > size_) std::fill(data() + size_, data() + size, value);
    size_ = size;
  }
  iterator erase(const_iterator first, const_iterator last) {
    size_t index = first - begin();
    size_t count = last - first;
    std::move(data() + index + count, end(), data() + index);
    size_ -= count;
    return begin() + index;
  }
  iterator erase(const_iterator position) { return erase(position, position + 1); }
  iterator insert(const_iterator position, size_t count, T value) {
    size_t index = position - begin();
    if (count == 0) return begin() + index;
    if (size_ + count > capacity_)
      reserve(std::max(size_ + count, GrowthCapacity()));
    std::move_backward(begin() + index, end(), end() + count);
    std::fill_n(begin() + index, count, value);
    size_ += count;
    return begin() + index;
  }
  template <std::input_iterator Iterator>
  iterator insert(const_iterator position, Iterator first, Iterator last) {
    size_t index = position - begin();
    if (first == last) return begin() + index;
    if constexpr (std::contiguous_iterator<Iterator>) {
      const T* source = std::to_address(first);
      const std::less<const T*> less;
      // Discovered dependencies normally live in a separate deps-log buffer.
      // Copy them straight into their final slots, with no temporary allocation.
      if (less(source, begin()) || !less(source, end())) {
        const size_t count = static_cast<size_t>(last - first);
        insert(begin() + index, count, nullptr);
        std::copy(first, last, begin() + index);
        return begin() + index;
      }
    }
    // Also handles self-insertion across a reallocation correctly.
    std::vector<T> values(first, last);
    const size_t count = values.size();
    insert(begin() + index, count, nullptr);
    std::copy(values.begin(), values.end(), begin() + index);
    return begin() + index;
  }
};

#ifdef SNINJA_STD_ADJACENCY
template <class T> using Adjacency = std::vector<T>;
#else
template <class T> using Adjacency = PointerVector<T>;
#endif
}
#endif
