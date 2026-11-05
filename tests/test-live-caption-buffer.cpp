#include <catch2/catch_test_macros.hpp>
#include "live-caption-buffer.hpp"

using namespace lt;

TEST_CASE("live caption buffer concatenates English deltas and trims the line start")
{
    LiveCaptionBuffer b;
    b.append(" Hello", 0);
    b.append(",", 10);
    b.append(" world", 20);
    REQUIRE(b.text() == "Hello, world");
}

TEST_CASE("live caption buffer concatenates Japanese deltas without separators")
{
    LiveCaptionBuffer b;
    b.append("こんにち", 0);
    b.append("は世界", 10);
    REQUIRE(b.text() == "こんにちは世界");
}

TEST_CASE("live caption buffer starts a new line after a turn")
{
    LiveCaptionBuffer b;
    b.append("First.", 0);
    b.end_turn();
    b.append(" ", 5);   // blank delta at a line start is dropped
    b.append(" Second.", 10);
    REQUIRE(b.text() == "First.\nSecond.");
}

TEST_CASE("live caption buffer ignores end_turn while empty")
{
    LiveCaptionBuffer b;
    b.end_turn();
    b.append("Only", 0);
    REQUIRE(b.text() == "Only");
}

TEST_CASE("live caption buffer strips an ideographic space at the line start")
{
    LiveCaptionBuffer b;
    b.append("\xE3\x80\x80はい", 0);
    REQUIRE(b.text() == "はい");
}

TEST_CASE("live caption buffer expires after the hold measured from the last append")
{
    LiveCaptionBuffer b;
    b.append("Hi", 1000);
    REQUIRE_FALSE(b.expire(4999, 4000));
    b.append(" there", 4000);
    b.append("   ", 4500); // mid-line whitespace is text, so it counts
    REQUIRE_FALSE(b.expire(8000, 4000));
    REQUIRE(b.expire(8500, 4000));
    REQUIRE(b.empty());
    REQUIRE_FALSE(b.expire(9000, 4000)); // nothing left to clear
}

TEST_CASE("live caption buffer blank deltas at a line start do not extend the hold")
{
    LiveCaptionBuffer b;
    b.append("Hi", 0);
    b.end_turn();
    b.append("  ", 3000);
    REQUIRE(b.expire(4000, 4000));
}

TEST_CASE("live caption buffer clear drops a pending new line")
{
    LiveCaptionBuffer b;
    b.append("One", 0);
    b.end_turn();
    b.clear();
    b.append("Two", 10);
    REQUIRE(b.text() == "Two");
}

TEST_CASE("live caption buffer renders the newest wrapped lines")
{
    LiveCaptionBuffer b;
    b.append("one two three four five six", 0);
    // wrap_text: "one two" / "three four" / "five six"
    REQUIRE(b.render(10, 2) == "three four\nfive six");
    REQUIRE(LiveCaptionBuffer().render(10, 2).empty());
}

TEST_CASE("live caption buffer renders turns as separate lines")
{
    LiveCaptionBuffer b;
    b.append("A.", 0);
    b.end_turn();
    b.append("B.", 1);
    b.end_turn();
    b.append("C.", 2);
    REQUIRE(b.render(60, 2) == "B.\nC.");
}

TEST_CASE("live caption buffer stays bounded at a UTF-8 boundary")
{
    LiveCaptionBuffer b;
    for (int i = 0; i < 5000; ++i) b.append("あ", static_cast<uint64_t>(i));
    REQUIRE(b.text().size() <= LiveCaptionBuffer::kMaxBytes);
    REQUIRE(b.text().size() % 3 == 0);
    REQUIRE(b.text().compare(0, 3, "あ") == 0);
}
