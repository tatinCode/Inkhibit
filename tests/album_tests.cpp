#include "inkhibit/album/image.hpp"
#include "inkhibit/album/store.hpp"
#include "album_png_fixture.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <limits>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {
namespace fs = std::filesystem;
namespace album = inkhibit::album;
using Bytes = std::vector<std::uint8_t>;
using Ids = std::vector<album::ImageId>;

#define REQUIRE(condition) do { if (!(condition)) { \
    throw std::runtime_error(std::string(__FILE__) + ":" \
        + std::to_string(__LINE__) + ": " + #condition); \
} } while (false)

template<class Exception, class Function>
void expect_throw(Function function)
{
    try {
        function();
    } catch (const Exception&) {
        return;
    }
    throw std::runtime_error("Expected exception was not thrown");
}

struct TemporaryDirectory {
    fs::path path;
    TemporaryDirectory()
    {
        auto pattern = (fs::current_path() / "album-tests-XXXXXX").string();
        const char* result = ::mkdtemp(pattern.data());
        REQUIRE(result);
        path = result;
    }
    ~TemporaryDirectory()
    {
        std::error_code ignored;
        fs::remove_all(path, ignored);
    }
    TemporaryDirectory(const TemporaryDirectory&) = delete;
    TemporaryDirectory& operator=(const TemporaryDirectory&) = delete;
};

void write_file(const fs::path& path, const std::string& text)
{
    std::ofstream out(path, std::ios::binary);
    REQUIRE(out.is_open());
    out.write(text.data(), static_cast<std::streamsize>(text.size()));
    out.close();
    REQUIRE(!out.fail());
}

Bytes read_file(const fs::path& path)
{
    std::ifstream in(path, std::ios::binary);
    REQUIRE(in.is_open());
    Bytes bytes{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
    REQUIRE(!in.bad());
    return bytes;
}

using album_fixture::png;

void validation()
{
    album::validate_png(png());
    album::validate_png(png(600, 400, 0));
    // The contract allows full RGB, not just six palette colors.
    album::validate_png(png(600, 400, 123));
    expect_throw<std::invalid_argument>([] { album::validate_png({}); });
    expect_throw<std::invalid_argument>([] { album::validate_png(Bytes(32)); });
    expect_throw<std::invalid_argument>([] {
        album::validate_png(Bytes{0xff, 0xd8, 0xff, 0xd9});
    });
    auto invalid = png();
    invalid[0] = 0;
    expect_throw<std::invalid_argument>([&] { album::validate_png(invalid); });
}

void dimensions()
{
    for (auto size : {std::pair{599u, 400u}, {600u, 399u}, {400u, 600u}}) {
        const auto bytes = png(size.first, size.second);
        expect_throw<std::invalid_argument>([&] { album::validate_png(bytes); });
    }
    auto huge = png();
    std::fill(huge.begin() + 16, huge.begin() + 24, 0xff);
    expect_throw<std::invalid_argument>([&] { album::validate_png(huge); });
}

void corrupt_pixels()
{
    auto bytes = png();
    bytes.resize(33); // Complete, correct IHDR but no image data.
    expect_throw<std::invalid_argument>([&] { album::validate_png(bytes); });
}

void size_limit()
{
    auto bytes = png();
    // Decoder permits trailing bytes; exercise the inclusive size boundary.
    bytes.resize(album::max_png_bytes, 0);
    album::validate_png(bytes);
    bytes.push_back(0);
    expect_throw<std::invalid_argument>([&] { album::validate_png(bytes); });
}

void persistence()
{
    TemporaryDirectory temp;
    const auto root = temp.path / "nested" / "album";
    const auto white = png(), black = png(600, 400, 0);
    album::ImageId first, second;
    {
        album::AlbumStore store(root);
        REQUIRE(store.list().empty());
        first = store.add(white);
        second = store.add(black);
        REQUIRE(first > 0 && second > first);
        REQUIRE(store.list() == (Ids{first, second}));
    }
    {
        album::AlbumStore store(root);
        REQUIRE(store.list() == (Ids{first, second}));
        REQUIRE(read_file(root / (std::to_string(first) + ".png")) == white);
        REQUIRE(read_file(root / (std::to_string(second) + ".png")) == black);
        REQUIRE(store.remove(second));
    }
    {
        album::AlbumStore store(root);
        REQUIRE(store.list() == (Ids{first}));
        REQUIRE(!store.remove(second));
        const auto third = store.add(black);
        REQUIRE(third > second);
        REQUIRE(store.remove(first));
        REQUIRE(store.remove(third));
    }
    album::AlbumStore reopened(root);
    REQUIRE(reopened.list().empty());
    REQUIRE(reopened.add(white) > second);
}

void numeric_order()
{
    TemporaryDirectory temp;
    album::AlbumStore store(temp.path);
    const auto bytes = png();
    Ids ids;
    for (int i = 0; i < 12; ++i) {
        ids.push_back(store.add(bytes));
    }
    REQUIRE(std::is_sorted(ids.begin(), ids.end()));
    REQUIRE(store.list() == ids); // 10 must not sort before 2.
}

void invalid_add()
{
    TemporaryDirectory temp;
    album::AlbumStore store(temp.path);
    expect_throw<std::invalid_argument>([&] { store.add({}); });
    REQUIRE(fs::is_empty(temp.path));
    const auto first = store.add(png());
    const auto counter = read_file(temp.path / ".next-id");
    auto bytes = png();
    bytes.resize(33);
    expect_throw<std::exception>([&] { store.add(bytes); });
    REQUIRE(store.list() == (Ids{first}));
    REQUIRE(read_file(temp.path / ".next-id") == counter);
}

void ignored_entries()
{
    TemporaryDirectory temp;
    album::AlbumStore store(temp.path);
    const auto id = store.add(png());
    for (const auto* name : {"0.png", "01.png", "-1.png", "abc.png",
            "18446744073709551616.png", "2.PNG", "3.png.tmp"}) {
        write_file(temp.path / name, "not an album entry");
    }
    fs::create_directory(temp.path / "4.png");
    fs::create_symlink(temp.path / "1.png", temp.path / "5.png");
    fs::create_symlink(temp.path / "absent", temp.path / "6.png");
    REQUIRE(store.list() == (Ids{id}));
    REQUIRE(!store.remove(0));
    REQUIRE(!store.remove(99));
    REQUIRE(!store.remove(4));
    REQUIRE(!store.remove(5));
    REQUIRE(fs::is_symlink(temp.path / "5.png"));
    REQUIRE(read_file(temp.path / "1.png") == png());
}

void invalid_root()
{
    TemporaryDirectory temp;
    const auto path = temp.path / "file";
    write_file(path, "sentinel");
    expect_throw<fs::filesystem_error>([&] { album::AlbumStore store(path); });
}

void bad_counter()
{
    TemporaryDirectory temp;
    album::AlbumStore store(temp.path);
    const auto bytes = png();
    for (const auto* text : {"", "0\n", "01\n", "-1\n", "abc\n",
            "18446744073709551616\n", "18446744073709551615\n"}) {
        write_file(temp.path / ".next-id", text);
        const auto before = read_file(temp.path / ".next-id");
        expect_throw<std::runtime_error>([&] { store.add(bytes); });
        REQUIRE(store.list().empty());
        REQUIRE(read_file(temp.path / ".next-id") == before);
    }
}

void collision()
{
    TemporaryDirectory temp;
    album::AlbumStore store(temp.path);
    const auto bytes = png();
    const auto id = store.add(bytes);
    write_file(temp.path / ".next-id", std::to_string(id) + "\n");
    expect_throw<std::runtime_error>([&] { store.add(png(600, 400, 0)); });
    REQUIRE(read_file(temp.path / (std::to_string(id) + ".png")) == bytes);
    REQUIRE(store.list() == (Ids{id}));
}

void write_failure()
{
    TemporaryDirectory temp;
    album::AlbumStore store(temp.path);
    // Deterministic failure even when tests run as root: cannot write a directory.
    fs::create_directory(temp.path / "1.png.tmp");
    const auto bytes = png();
    expect_throw<std::runtime_error>([&] { store.add(bytes); });
    REQUIRE(store.list().empty());
    REQUIRE(!fs::exists(temp.path / "1.png"));
    fs::remove(temp.path / "1.png.tmp");
    REQUIRE(store.add(bytes) == 2); // Failed publication reserved ID 1.
}

void concurrent_adds()
{
    TemporaryDirectory temp;
    album::AlbumStore store(temp.path);
    const auto bytes = png();
    constexpr std::size_t count = 8;
    Ids ids(count);
    std::vector<std::exception_ptr> errors(count);
    std::vector<std::thread> threads;
    // Join already-created threads even if creating another thread fails.
    try {
        for (std::size_t i = 0; i < count; ++i) {
            threads.emplace_back([&, i] {
                try { ids[i] = store.add(bytes); }
                catch (...) { errors[i] = std::current_exception(); }
            });
        }
    } catch (...) {
        for (auto& thread : threads) { thread.join(); }
        throw;
    }
    for (auto& thread : threads) { thread.join(); }
    for (const auto& error : errors) {
        if (error) { std::rethrow_exception(error); }
    }
    std::sort(ids.begin(), ids.end());
    REQUIRE(ids.front() > 0);
    REQUIRE(std::adjacent_find(ids.begin(), ids.end()) == ids.end());
    REQUIRE(store.list() == ids);
    for (const auto id : ids) {
        REQUIRE(read_file(temp.path / (std::to_string(id) + ".png")) == bytes);
    }
}
} // namespace

int main(int argc, char** argv)
{
    try {
        REQUIRE(argc == 2);
        const std::pair<const char*, void (*)()> cases[]{
            {"validation", validation}, {"dimensions", dimensions},
            {"corrupt_pixels", corrupt_pixels}, {"size_limit", size_limit},
            {"persistence", persistence}, {"numeric_order", numeric_order},
            {"invalid_add", invalid_add}, {"ignored_entries", ignored_entries},
            {"invalid_root", invalid_root}, {"bad_counter", bad_counter},
            {"collision", collision}, {"write_failure", write_failure},
            {"concurrent_adds", concurrent_adds},
        };
        for (const auto& test : cases) {
            if (test.first == std::string(argv[1])) {
                test.second();
                std::cout << "PASS: " << test.first << '\n';
                return EXIT_SUCCESS;
            }
        }
        throw std::runtime_error("Unknown album test");
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return EXIT_FAILURE;
    }
}
