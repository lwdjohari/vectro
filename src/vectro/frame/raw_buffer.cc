#include "vectro/frame/raw_buffer.h"
namespace vectro {
namespace frame {

/* STATIC */
RawBuffer RawBuffer::Make(std::size_t size, void* (*alloc)(std::size_t),
                          DeleterFn deleter) {
  if (!alloc || !deleter) {
    throw std::invalid_argument(
        "RawBuffer: allocator and deleter must be provided.");
  }
  void* ptr = alloc(size);
  if (!ptr) {
    throw std::bad_alloc();
  }
  return RawBuffer(ptr, size, deleter);
}

/* PUBLIC */

RawBuffer::RawBuffer(void* data, std::size_t size, DeleterFn deleter)
                : data_(data), size_(size), deleter_(deleter) {
  if (!data_ || size_ == 0 || !deleter_) {
    throw std::invalid_argument("RawBuffer: invalid construction arguments.");
  }
}

RawBuffer::~RawBuffer() {
  Reset();
}

RawBuffer::RawBuffer(RawBuffer&& other) noexcept {
  MoveFrom(std::move(other));
}

RawBuffer& RawBuffer::operator=(RawBuffer&& other) noexcept {
  if (this != &other) {
    Reset();
    MoveFrom(std::move(other));
  }
  return *this;
}

void* RawBuffer::data() const {
  return data_;
}
std::size_t RawBuffer::size() const {
  return size_;
}

bool RawBuffer::valid() const {
  return data_ != nullptr && size_ > 0;
}

void RawBuffer::Reset() {
  if (data_ && deleter_) {
    deleter_(data_, size_);
  }
  data_ = nullptr;
  size_ = 0;
  deleter_ = nullptr;
}

/*PRIVATE*/

void RawBuffer::MoveFrom(RawBuffer&& other) noexcept {
  data_ = other.data_;
  size_ = other.size_;
  deleter_ = other.deleter_;
  other.data_ = nullptr;
  other.size_ = 0;
  other.deleter_ = nullptr;
}

}  // namespace frame
}  // namespace vectro