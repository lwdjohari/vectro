#include <catch2/catch_test_macros.hpp>
#include "vectro/frame/raw_buffer.h"  // adjust include path if needed
#include <cstdlib>

using namespace vectro::frame;

// Tracking deleter calls
static bool dele_called = false;

// Simple allocator/deleter
static void* test_alloc(std::size_t n) { return std::malloc(n); }
static void  test_deleter(void* p, std::size_t /*sz*/) {
    dele_called = true;
    std::free(p);
}

TEST_CASE("Default-constructed RawBuffer is empty", "[RawBuffer]") {
    RawBuffer buf;
    REQUIRE_FALSE(buf.valid());
    REQUIRE(buf.data() == nullptr);
    REQUIRE(buf.size() == 0);
}

TEST_CASE("Reset on default RawBuffer is no-op", "[RawBuffer]") {
    RawBuffer buf;
    dele_called = false;
    buf.Reset();
    REQUIRE_FALSE(dele_called);
}

TEST_CASE("Make() allocates memory and Reset() releases it", "[RawBuffer]") {
    dele_called = false;
    RawBuffer buf = RawBuffer::Make(16, test_alloc, test_deleter);
    REQUIRE(buf.valid());
    REQUIRE(buf.size() == 16);
    REQUIRE(buf.data() != nullptr);

    // Write/read to buffer
    auto data = static_cast<unsigned char*>(buf.data());
    data[0] = 0xAB;
    REQUIRE(data[0] == 0xAB);

    buf.Reset();
    REQUIRE_FALSE(buf.valid());
    REQUIRE(dele_called);
    REQUIRE(buf.data() == nullptr);
    REQUIRE(buf.size() == 0);
}

TEST_CASE("Destructor calls deleter on scope exit", "[RawBuffer]") {
    dele_called = false;
    {
        RawBuffer buf = RawBuffer::Make(8, test_alloc, test_deleter);
        REQUIRE_FALSE(dele_called);
    }
    REQUIRE(dele_called);
}

TEST_CASE("Raw-pointer constructor works and Reset() frees", "[RawBuffer]") {
    dele_called = false;
    void* ptr = std::malloc(4);
    RawBuffer buf(ptr, 4, test_deleter);
    REQUIRE(buf.valid());
    REQUIRE(buf.size() == 4);

    buf.Reset();
    REQUIRE(dele_called);
}

TEST_CASE("Constructor rejects invalid inputs", "[RawBuffer]") {
    char dummy;
    REQUIRE_THROWS_AS(RawBuffer(nullptr,    1, test_deleter), std::invalid_argument);
    REQUIRE_THROWS_AS(RawBuffer(&dummy,     0, test_deleter), std::invalid_argument);
    REQUIRE_THROWS_AS(RawBuffer(&dummy,     1, nullptr),      std::invalid_argument);
}

TEST_CASE("Make() rejects null funcs or alloc failure", "[RawBuffer]") {
    // null allocator
    REQUIRE_THROWS_AS(RawBuffer::Make(8, nullptr,        test_deleter),
                      std::invalid_argument);
    // null deleter
    REQUIRE_THROWS_AS(RawBuffer::Make(8, test_alloc,     nullptr),
                      std::invalid_argument);
    // alloc returns nullptr
    auto bad_alloc = [](std::size_t)->void* { return nullptr; };
    REQUIRE_THROWS_AS(RawBuffer::Make(8, bad_alloc,       test_deleter),
                      std::bad_alloc);
}

TEST_CASE("Move constructor transfers ownership", "[RawBuffer]") {
    RawBuffer a = RawBuffer::Make(12, test_alloc, test_deleter);
    RawBuffer b(std::move(a));
    REQUIRE(b.valid());
    REQUIRE_FALSE(a.valid());
}

TEST_CASE("Move assignment transfers and resets previous", "[RawBuffer]") {
    // First prepare two buffers
    RawBuffer a = RawBuffer::Make(3, test_alloc, test_deleter);
    RawBuffer b = RawBuffer::Make(1, test_alloc, test_deleter);

    dele_called = false;
    b = std::move(a);
    REQUIRE(b.valid());
    REQUIRE_FALSE(a.valid());
}

TEST_CASE("Self move-assignment is safe", "[RawBuffer]") {
    RawBuffer buf = RawBuffer::Make(5, test_alloc, test_deleter);
    REQUIRE_NOTHROW(buf = std::move(buf));
    REQUIRE(buf.valid());
    REQUIRE(buf.size() == 5);
}
