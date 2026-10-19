#pragma once
#include <cstdint>
#include <string>
#include <string_view>

// Display state for one two-way caption source: accumulates the streamed
// outputTranscription deltas of one live-translate session and renders the
// newest lines into the configured text box. Pure logic: no libobs, no
// threads, no clock — callers pass timestamps in.
// Spec: docs/specs/007-two-way-live-translate.md §4.2.
namespace lt {

class LiveCaptionBuffer {
public:
    // Appends one transcription delta. Deltas are concatenated verbatim (English
    // deltas carry their own leading space, Japanese ones none), except that
    // whitespace at the start of a line is dropped. A whitespace-only delta at
    // the start of a line is ignored and does not restart the hold timer.
    void append(std::string_view delta, uint64_t now_ms);

    // Ends the current turn: the next non-blank delta starts on a new line.
    void end_turn();

    // Clears the text once `hold_ms` passed since the last non-blank append.
    // Returns true iff this call cleared visible text.
    bool expire(uint64_t now_ms, uint64_t hold_ms);

    void clear();
    bool empty() const { return text_.empty(); }
    const std::string &text() const { return text_; }

    // The last `max_lines` lines of the text wrapped to `max_width` display
    // units (CJK = 2), joined with '\n'. "" when empty.
    std::string render(int max_width, int max_lines) const;

    // Bytes retained; older text is dropped at a UTF-8 boundary beyond this.
    static constexpr size_t kMaxBytes = 8192;

private:
    std::string text_;
    uint64_t last_append_ms_ = 0;
    bool new_line_pending_ = false;
};

}
