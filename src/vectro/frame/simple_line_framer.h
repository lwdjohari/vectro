#pragma once

#include <chrono>
#include <cstring>  // for std::memcpy

#include "vectro/frame/framer_base.h"

namespace vectro {
namespace frame {

/// A trivial line-oriented framer: splits on '\n', emitting each line
/// as one InternalMessage whose payload is a RawBuffer.
class SimpleLineFramer : public FramerBase {
 public:
  SimpleLineFramer()
                  : scratch_(RawBuffer::Make(
                        4096,
                        /*alloc*/
                        [](std::size_t s) { return ::operator new(s); },
                        /*deleter*/
                        [](void* p, std::size_t s) {
                          ::operator delete(p, s);
                        })),
                    write_pos_(0) {}

  RawBuffer& PrepareRead() override {
    // ensure at least 1024 free bytes at end
    if (scratch_.size() - write_pos_ < 1024) {
      scratch_.Reset();
      scratch_ = RawBuffer::Make(
          write_pos_ + 4096,
          /*alloc*/ [](std::size_t s) { return ::operator new(s); },
          /*deleter*/ [](void* p, std::size_t s) { ::operator delete(p, s); });
      write_pos_ = 0;
    }
    return scratch_;
  }

  std::vector<InternalMessage> OnData(std::size_t n) override {
    write_pos_ += n;
    std::vector<InternalMessage> out;

    std::size_t start = 0;
    auto data = static_cast<char*>(scratch_.data());
    for (std::size_t i = 0; i < write_pos_; ++i) {
      if (data[i] == '\n') {
        std::size_t len = i - start;
        // allocate a new RawBuffer for this message
        RawBuffer msgbuf = RawBuffer::Make(
            len,
            /*alloc*/ [](std::size_t s) { return ::operator new(s); },
            /*deleter*/
            [](void* p, std::size_t s) { ::operator delete(p, s); });
        std::memcpy(msgbuf.data(), data + start, len);

        InternalMessageMeta meta;
        meta.timestamp = std::chrono::steady_clock::now();
        // TODO: fill meta.connection_id, origin, etc.
        InternalMessage msg(std::move(meta), std::move(msgbuf));
        out.push_back(std::move(msg));

        start = i + 1;
      }
    }

    // move leftover bytes to front of scratch_
    if (start < write_pos_) {
      std::memmove(data, data + start, write_pos_ - start);
      write_pos_ -= start;
    } else {
      write_pos_ = 0;
    }

    return out;
  }

 private:
  RawBuffer scratch_;
  std::size_t write_pos_;
};

}  // namespace frame
}  // namespace vectro
