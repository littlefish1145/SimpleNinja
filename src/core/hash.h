#ifndef SNINJA_HASH_H_
#define SNINJA_HASH_H_

#include <cstdint>
#include <cstring>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace sninja {

/// Hash raw bytes. Scalar implementation; M5 swaps in a SIMD version
/// at runtime dispatch (same signature, same results contract: any hash
/// is fine as long as it is consistent within a process).
size_t HashBytes(const void* data, size_t len);

inline size_t HashString(std::string_view s) {
  return HashBytes(s.data(), s.size());
}

/// An open-addressing hash map keyed by externally-owned strings
/// (string_views into stable storage such as Node::path_).
/// Insert-only; no erase (mirrors ninja's State::Paths usage).
template <class V>
class StrViewMap {
 public:
  using key_type = std::string_view;

  struct Slot {
    std::string_view key;
    V value;
  };

  StrViewMap() = default;
  StrViewMap(const StrViewMap&) = delete;
  StrViewMap& operator=(const StrViewMap&) = delete;

  size_t size() const { return size_; }
  bool empty() const { return size_ == 0; }

  /// Find a value by key.  Usable from const contexts; the map never hands
  /// out ownership, and V itself may be a pointer (State::Paths maps to
  /// Node*), so this returns V*.
  V* find(std::string_view key) const {
    if (slots_.empty()) return nullptr;
    size_t mask = slots_.size() - 1;
    size_t i = HashString(key) & mask;
    for (;;) {
      const Slot& s = slots_[i];
      if (s.key.data() == nullptr) return nullptr;
      if (s.key == key) return const_cast<V*>(&s.value);
      i = (i + 1) & mask;
    }
  }

  /// Insert or overwrite.  |value| may be move-only.
  void insert(std::string_view key, V value) {
    if (slots_.empty()) Grow(16);
    if (size_ * 4 >= slots_.size() * 3) Grow(slots_.size() * 2);
    size_t mask = slots_.size() - 1;
    size_t i = HashString(key) & mask;
    for (;;) {
      Slot& s = slots_[i];
      if (s.key.data() == nullptr) {
        s.key = key;
        s.value = std::move(value);
        ++size_;
        return;
      }
      if (s.key == key) {
        s.value = std::move(value);
        return;
      }
      i = (i + 1) & mask;
    }
  }

  V& operator[](const std::string& key) {
    if (slots_.empty()) Grow(16);
    if (size_ * 4 >= slots_.size() * 3) Grow(slots_.size() * 2);
    size_t mask = slots_.size() - 1;
    size_t i = HashString(key) & mask;
    for (;;) {
      Slot& s = slots_[i];
      if (s.key.data() == nullptr) {
        s.key = key;
        ++size_;
        return s.value;
      }
      if (s.key == key) return s.value;
      i = (i + 1) & mask;
    }
  }

  /// A view of one entry: like std::pair<std::string_view, V>, but V may be
  /// move-only, so the value is exposed by reference.
  struct Entry {
    std::string_view first;
    V& second;

    Entry(std::string_view key, V& value) : first(key), second(value) {}
  };

  struct const_iterator {
    const Slot* slot;
    const Slot* end;

    const_iterator(const Slot* slot, const Slot* end)
        : slot(slot), end(end) {}

    void SkipEmpty() {
      while (slot != end && slot->key.data() == nullptr) ++slot;
    }
    const_iterator& operator++() {
      ++slot;
      SkipEmpty();
      return *this;
    }
    bool operator!=(const const_iterator& o) const { return slot != o.slot; }
    bool operator==(const const_iterator& o) const { return slot == o.slot; }
    Entry& operator*() const {
      return entry_.emplace(slot->key, const_cast<V&>(slot->value));
    }
    Entry* operator->() const { return &**this; }

   private:
    // Entry has no default constructor, so operator->() materializes it here.
    mutable std::optional<Entry> entry_;
  };

  using iterator = const_iterator;

  const_iterator begin() const {
    const_iterator it{slots_.empty() ? nullptr : slots_.data(),
                      slots_.empty() ? nullptr : slots_.data() + slots_.size()};
    it.SkipEmpty();
    return it;
  }
  const_iterator end() const {
    return const_iterator{slots_.empty() ? nullptr
                                         : slots_.data() + slots_.size(),
                          slots_.empty() ? nullptr
                                         : slots_.data() + slots_.size()};
  }

 private:
  void Grow(size_t new_cap) {
    std::vector<Slot> old = std::move(slots_);
    slots_.clear();
    slots_.resize(new_cap);
    size_ = 0;
    for (Slot& s : old) {
      if (s.key.data() == nullptr) continue;
      size_t mask = slots_.size() - 1;
      size_t i = HashString(s.key) & mask;
      while (slots_[i].key.data() != nullptr) i = (i + 1) & mask;
      slots_[i] = std::move(s);
      ++size_;
    }
  }

  std::vector<Slot> slots_;
  size_t size_ = 0;
};

}  // namespace sninja

#endif  // SNINJA_HASH_H_
