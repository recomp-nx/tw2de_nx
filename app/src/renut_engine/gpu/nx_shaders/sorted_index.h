// tw2de (2026-10-23): a u64 key's value by binary search over an array sorted by key, in place of a hash table
// (std::unordered_map: a heap node a key, ~26,000 of them built at every start). The array is made in one go from
// what the start read (a file already sorted is taken as it is, else sorted once); keys added later go to a small
// hash table beside it ("recent"), folded into the array when it reaches kFoldAt and before the array is written out.
// Not thread-safe: the owner's lock guards it. A pointer Find returns is valid until the next Set / Erase / Fold.
#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <unordered_map>
#include <utility>
#include <vector>

namespace renut::nxs {

template <typename Value>
class SortedIndex {
 public:
  struct Item {
    uint64_t key;
    Value value;
  };
  static constexpr size_t kFoldAt = 2048;

  // The index from items in any order. Of items with the same key the first stays (keep_last: the last).
  void Assign(std::vector<Item> items, bool keep_last = false) {
    recent_.clear();
    const auto less = [](const Item &a, const Item &b) { return a.key < b.key; };
    if (!IsStrictlySorted(items)) {
      std::stable_sort(items.begin(), items.end(), less);
      size_t out = 0;
      for (size_t i = 0; i < items.size();) {
        size_t j = i + 1;
        while (j < items.size() && items[j].key == items[i].key)
          ++j;
        items[out++] = items[keep_last ? j - 1 : i];
        i = j;
      }
      items.resize(out);
    }
    sorted_ = std::move(items);
  }

  const Value *Find(uint64_t key) const {
    auto it = Lower(key);
    if (it != sorted_.end() && it->key == key)
      return &it->value;
    if (recent_.empty())
      return nullptr;
    auto r = recent_.find(key);
    return r != recent_.end() ? &r->second : nullptr;
  }
  Value *Find(uint64_t key) { return const_cast<Value *>(static_cast<const SortedIndex *>(this)->Find(key)); }
  bool Contains(uint64_t key) const { return Find(key) != nullptr; }

  // Adds the key or replaces its value.
  void Set(uint64_t key, const Value &value) {
    if (Value *v = Find(key)) {
      *v = value;
      return;
    }
    recent_.emplace(key, value);
    if (recent_.size() >= kFoldAt)
      Fold();
  }
  // Adds the key when it is not there: true if it was added.
  bool Insert(uint64_t key, const Value &value) {
    if (Contains(key))
      return false;
    recent_.emplace(key, value);
    if (recent_.size() >= kFoldAt)
      Fold();
    return true;
  }
  bool Erase(uint64_t key) {
    if (recent_.erase(key))
      return true;
    auto it = Lower(key);
    if (it == sorted_.end() || it->key != key)
      return false;
    sorted_.erase(it);
    return true;
  }

  size_t Size() const { return sorted_.size() + recent_.size(); }
  void Clear() {
    sorted_.clear();
    recent_.clear();
  }
  void Reserve(size_t n) { sorted_.reserve(n); }

  // The recent keys into the array (a sort of them and one merge).
  void Fold() {
    if (recent_.empty())
      return;
    const size_t mid = sorted_.size();
    sorted_.reserve(mid + recent_.size());
    for (const auto &[k, v] : recent_)
      sorted_.push_back(Item{k, v});
    recent_.clear();
    const auto less = [](const Item &a, const Item &b) { return a.key < b.key; };
    std::sort(sorted_.begin() + std::ptrdiff_t(mid), sorted_.end(), less);
    std::inplace_merge(sorted_.begin(), sorted_.begin() + std::ptrdiff_t(mid), sorted_.end(), less);
  }
  // Every item in key order (after Fold: the recent ones too).
  const std::vector<Item> &Sorted() {
    Fold();
    return sorted_;
  }
  // Every item, the array's in key order, then the recent ones (any order).
  template <typename Fn>
  void ForEach(Fn &&fn) const {
    for (const Item &i : sorted_)
      fn(i.key, i.value);
    for (const auto &[k, v] : recent_)
      fn(k, v);
  }

 private:
  static bool IsStrictlySorted(const std::vector<Item> &items) {
    for (size_t i = 1; i < items.size(); ++i)
      if (!(items[i - 1].key < items[i].key))
        return false;
    return true;
  }
  typename std::vector<Item>::const_iterator Lower(uint64_t key) const {
    return std::lower_bound(sorted_.begin(), sorted_.end(), key,
                            [](const Item &a, uint64_t k) { return a.key < k; });
  }

  std::vector<Item> sorted_;
  std::unordered_map<uint64_t, Value> recent_;
};

}  // namespace renut::nxs
