#pragma once
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <stdexcept>

namespace vectro {
namespace frame {
class RawBuffer {
 public:
  using DeleterFn = void (*)(void*, std::size_t);

  static RawBuffer Make(std::size_t size, void* (*alloc)(std::size_t),
                        DeleterFn deleter);

  RawBuffer() = default;

  RawBuffer(const RawBuffer&)            = delete;
  RawBuffer& operator=(const RawBuffer&) = delete;
  RawBuffer(void* data, std::size_t size, DeleterFn deleter);
  RawBuffer(RawBuffer&& other) noexcept;
  ~RawBuffer();
  RawBuffer& operator=(RawBuffer&& other) noexcept;
  void* data() const;
  std::size_t size() const;
  bool valid() const;
  void Reset();

 private:
  void MoveFrom(RawBuffer&& other) noexcept;

  void* data_        = nullptr;
  std::size_t size_  = 0;
  DeleterFn deleter_ = nullptr;
};
}  // namespace frame
}  // namespace vectro
