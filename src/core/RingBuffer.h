#pragma once
// Fixed-capacity ring buffer for metric histories. No allocation after construction.
#include <algorithm>
#include <cstddef>
#include <vector>

namespace nx {

template <typename T>
class RingBuffer {
public:
    explicit RingBuffer(size_t capacity = 60) : data_(capacity ? capacity : 1) {}

    void Push(const T& v) {
        data_[head_] = v;
        head_ = (head_ + 1) % data_.size();
        if (size_ < data_.size()) ++size_;
    }
    // i = 0 is the oldest sample, Size()-1 the newest.
    const T& At(size_t i) const { return data_[(head_ + data_.size() - size_ + i) % data_.size()]; }
    const T& Last() const { return At(size_ - 1); }
    size_t Size() const { return size_; }
    size_t Capacity() const { return data_.size(); }
    bool Empty() const { return size_ == 0; }
    void Clear() { size_ = 0; head_ = 0; }

    void Resize(size_t capacity) {
        if (capacity == 0) capacity = 1;
        if (capacity == data_.size()) return;
        std::vector<T> n(capacity);
        size_t keep = std::min(size_, capacity);
        for (size_t i = 0; i < keep; ++i) n[i] = At(size_ - keep + i);
        data_.swap(n);
        size_ = keep;
        head_ = keep % capacity;
    }

    T Max() const {
        T m{};
        for (size_t i = 0; i < size_; ++i) m = std::max(m, At(i));
        return m;
    }

    // Copies the newest n samples (oldest first) into out.
    void Tail(size_t n, std::vector<T>& out) const {
        n = std::min(n, size_);
        out.resize(n);
        for (size_t i = 0; i < n; ++i) out[i] = At(size_ - n + i);
    }

private:
    std::vector<T> data_;
    size_t head_ = 0, size_ = 0;
};

} // namespace nx
