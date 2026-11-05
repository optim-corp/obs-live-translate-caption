#include "live-caption-buffer.hpp"
#include "caption-wrap.hpp"

namespace lt {

namespace {

bool at_line_start(const std::string &text)
{
    return text.empty() || text.back() == '\n';
}

// Skips ASCII whitespace and U+3000 (ideographic space).
size_t skip_leading_space(std::string_view s)
{
    size_t i = 0;
    while (i < s.size()) {
        const char c = s[i];
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
            ++i;
        } else if (s.compare(i, 3, "\xE3\x80\x80") == 0) {
            i += 3;
        } else {
            break;
        }
    }
    return i;
}

} // namespace

void LiveCaptionBuffer::append(std::string_view delta, uint64_t now_ms)
{
    if (new_line_pending_ || at_line_start(text_)) {
        delta.remove_prefix(skip_leading_space(delta));
        if (delta.empty()) return;
        if (new_line_pending_ && !text_.empty()) text_ += '\n';
        new_line_pending_ = false;
    } else if (delta.empty()) {
        return;
    }
    text_.append(delta);
    last_append_ms_ = now_ms;

    if (text_.size() > kMaxBytes) {
        size_t cut = text_.size() - kMaxBytes;
        while (cut < text_.size() && (static_cast<unsigned char>(text_[cut]) & 0xc0) == 0x80)
            ++cut;
        text_.erase(0, cut);
    }
}

void LiveCaptionBuffer::end_turn()
{
    if (!text_.empty()) new_line_pending_ = true;
}

bool LiveCaptionBuffer::expire(uint64_t now_ms, uint64_t hold_ms)
{
    if (text_.empty()) return false;
    if (now_ms < last_append_ms_ || now_ms - last_append_ms_ < hold_ms) return false;
    clear();
    return true;
}

void LiveCaptionBuffer::clear()
{
    text_.clear();
    new_line_pending_ = false;
}

std::string LiveCaptionBuffer::render(int max_width, int max_lines) const
{
    if (text_.empty()) return "";
    return join_lines(wrap_tail(text_, max_width, max_lines));
}

}
