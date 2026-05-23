#include "textport.h"

#include <cstring>
#include <algorithm>

extern "C" {
// 100 Hz tick from the DUART CTR_READY ISR (see rom.cpp / crt0.s).
extern volatile uint32_t tick_counter;
}

namespace griffin::textport
{

Textport g_textport;

// Cursor toggles every 50 ticks (= 0.5s at 100 Hz).
static constexpr uint32_t BLINK_PERIOD_TICKS = 50;

void Textport::configure(uint8_t* fb, unsigned pitch_bytes,
                         const FontRenderer* fr,
                         unsigned cols, unsigned rows)
{
    fb_    = fb;
    pitch_ = pitch_bytes;
    fr_    = fr;
    cols_  = (cols > MAX_COLS) ? MAX_COLS : static_cast<uint8_t>(cols);
    rows_  = (rows > MAX_ROWS) ? MAX_ROWS : static_cast<uint8_t>(rows);

    scroll_top_ = 0;
    scroll_bot_ = static_cast<uint8_t>(rows_ - 1);
    cursor_x_ = 0;
    cursor_y_ = 0;
    saved_x_ = 0;
    saved_y_ = 0;
    cursor_hanging_ = false;
    saved_hanging_ = false;
    cursor_shown_ = false;
    inverse_attr_ = false;
    last_blink_tick_ = tick_counter;
    blink_phase_ = true;

    clear();
}

void Textport::clear()
{
    if (!fr_)
    {
        return;
    }

    // Clear char buffer for the active region of the static arrays.
    for (unsigned y = 0; y < rows_; ++y)
    {
        std::memset(&chars_[y * MAX_COLS], ' ', cols_);
        std::memset(&attrs_[y * MAX_COLS], 0,    cols_);
    }

    // Black the entire framebuffer (faster than per-cell clear and covers
    // any margins outside the textport area).
    std::memset(fb_, 0, pitch_ * fr_->font->height * rows_);

    cursor_x_ = 0;
    cursor_y_ = 0;
    cursor_hanging_ = false;
    cursor_shown_ = false;
    show_cursor_();
}

void Textport::bell()
{
    // TODO: route to audio when hardware exists.
}

void Textport::paint_cell_(unsigned cx, unsigned cy)
{
    const uint8_t c    = chars_[idx_(cx, cy)];
    const bool    inv  = (attrs_[idx_(cx, cy)] & ATTR_INVERSE) != 0;
    fr_->draw_cell(fb_, pitch_, *fr_, cx, cy, c, inv);
}

void Textport::paint_blank_cell_(unsigned cx, unsigned cy)
{
    chars_[idx_(cx, cy)] = ' ';
    attrs_[idx_(cx, cy)] = 0;
    fr_->draw_cell(fb_, pitch_, *fr_, cx, cy, ' ', false);
}

void Textport::show_cursor_()
{
    if (cursor_shown_)
    {
        return;
    }
    fr_->invert_cell(fb_, pitch_, *fr_, cursor_x_, cursor_y_);
    cursor_shown_ = true;
}

void Textport::hide_cursor_()
{
    if (!cursor_shown_)
    {
        return;
    }
    fr_->invert_cell(fb_, pitch_, *fr_, cursor_x_, cursor_y_);
    cursor_shown_ = false;
}

void Textport::cursor_blink_tick()
{
    const uint32_t now = tick_counter;
    if (now - last_blink_tick_ < BLINK_PERIOD_TICKS)
    {
        return;
    }
    last_blink_tick_ = now;
    blink_phase_ = !blink_phase_;
    if (blink_phase_)
    {
        show_cursor_();
    }
    else
    {
        hide_cursor_();
    }
}

void Textport::process_hanging()
{
    cursor_hanging_ = false;
    cursor_x_ = 0;
    if (cursor_y_ == scroll_bot_)
    {
        scroll_region_up_(1);
    }
    else if (cursor_y_ + 1 < rows_)
    {
        ++cursor_y_;
    }
}

void Textport::put_glyph(uint8_t c)
{
    if (!fr_)
    {
        return;
    }
    if (cursor_hanging_)
    {
        hide_cursor_();
        process_hanging();
    }
    else
    {
        hide_cursor_();
    }

    const unsigned i = idx_(cursor_x_, cursor_y_);
    chars_[i] = c;
    attrs_[i] = inverse_attr_ ? ATTR_INVERSE : uint8_t{0};
    paint_cell_(cursor_x_, cursor_y_);

    if (cursor_x_ + 1 >= cols_)
    {
        cursor_hanging_ = true;
    }
    else
    {
        ++cursor_x_;
    }
    // Force cursor visible on next blink check so it doesn't appear to
    // lag behind fast output.
    blink_phase_ = true;
    last_blink_tick_ = tick_counter;
    show_cursor_();
}

void Textport::move_to(int x, int y)
{
    if (!fr_)
    {
        return;
    }
    if (x < 0) { x = 0; }
    if (y < 0) { y = 0; }
    if (x >= cols_) { x = cols_ - 1; }
    if (y >= rows_) { y = rows_ - 1; }
    hide_cursor_();
    cursor_x_ = static_cast<uint8_t>(x);
    cursor_y_ = static_cast<uint8_t>(y);
    cursor_hanging_ = false;
    blink_phase_ = true;
    last_blink_tick_ = tick_counter;
    show_cursor_();
}

void Textport::cursor_fwd()
{
    if (cursor_hanging_)
    {
        hide_cursor_();
        process_hanging();
        show_cursor_();
        return;
    }
    if (cursor_x_ + 1 >= cols_)
    {
        cursor_hanging_ = true;
        return;
    }
    hide_cursor_();
    ++cursor_x_;
    show_cursor_();
}

void Textport::cursor_back()
{
    if (cursor_hanging_)
    {
        cursor_hanging_ = false;
        return;
    }
    if (cursor_x_ == 0)
    {
        return;
    }
    hide_cursor_();
    --cursor_x_;
    show_cursor_();
}

void Textport::cursor_up(int n)
{
    if (n < 1) { n = 1; }
    hide_cursor_();
    int y = cursor_y_ - n;
    if (y < scroll_top_) { y = scroll_top_; }
    cursor_y_ = static_cast<uint8_t>(y);
    cursor_hanging_ = false;
    show_cursor_();
}

void Textport::cursor_down(int n)
{
    if (n < 1) { n = 1; }
    hide_cursor_();
    int y = cursor_y_ + n;
    if (y > scroll_bot_) { y = scroll_bot_; }
    cursor_y_ = static_cast<uint8_t>(y);
    cursor_hanging_ = false;
    show_cursor_();
}

void Textport::carriage_return()
{
    hide_cursor_();
    cursor_x_ = 0;
    cursor_hanging_ = false;
    show_cursor_();
}

void Textport::line_feed()
{
    hide_cursor_();
    cursor_hanging_ = false;
    if (cursor_y_ == scroll_bot_)
    {
        scroll_region_up_(1);
    }
    else if (cursor_y_ + 1 < rows_)
    {
        ++cursor_y_;
    }
    show_cursor_();
}

void Textport::reverse_line_feed()
{
    hide_cursor_();
    cursor_hanging_ = false;
    if (cursor_y_ == scroll_top_)
    {
        scroll_region_down_(1);
    }
    else if (cursor_y_ > 0)
    {
        --cursor_y_;
    }
    show_cursor_();
}

// -- Region scrolling -------------------------------------------------------
//
// scroll_region_up(n): rows [top..bot] shift up by n; bottom n rows cleared.
// scroll_region_down(n): rows [top..bot] shift down by n; top n rows cleared.
//
// Operates on both the char/attr buffer AND the pixel framebuffer so the
// repaint stays synchronous.  Cursor is hidden by callers before invoking.

void Textport::scroll_region_up_(int n)
{
    if (n < 1) { return; }
    const int region_rows = scroll_bot_ - scroll_top_ + 1;
    if (n >= region_rows)
    {
        // Clear entire region.
        for (int y = scroll_top_; y <= scroll_bot_; ++y)
        {
            std::memset(&chars_[y * MAX_COLS], ' ', cols_);
            std::memset(&attrs_[y * MAX_COLS], 0,   cols_);
        }
        const unsigned H = fr_->font->height;
        std::memset(fb_ + scroll_top_ * H * pitch_, 0,
                    static_cast<size_t>(region_rows) * H * pitch_);
        return;
    }

    // Move char/attr cells up by n rows.  Use memmove since the analyzer
    // can't prove cols_ <= MAX_COLS keeps rows disjoint (they always are).
    for (int y = scroll_top_; y + n <= scroll_bot_; ++y)
    {
        std::memmove(&chars_[y * MAX_COLS], &chars_[(y + n) * MAX_COLS], cols_);
        std::memmove(&attrs_[y * MAX_COLS], &attrs_[(y + n) * MAX_COLS], cols_);
    }
    for (int y = scroll_bot_ - n + 1; y <= scroll_bot_; ++y)
    {
        std::memset(&chars_[y * MAX_COLS], ' ', cols_);
        std::memset(&attrs_[y * MAX_COLS], 0,   cols_);
    }

    // Move pixels up by n*H rows; clear the bottom n*H rows.
    const unsigned H = fr_->font->height;
    const unsigned shift_bytes =
        static_cast<unsigned>(n) * H * pitch_;
    const unsigned region_bytes =
        static_cast<unsigned>(region_rows) * H * pitch_;
    uint8_t* base = fb_ + scroll_top_ * H * pitch_;
    std::memmove(base, base + shift_bytes, region_bytes - shift_bytes);
    std::memset(base + region_bytes - shift_bytes, 0, shift_bytes);
}

void Textport::scroll_region_down_(int n)
{
    if (n < 1) { return; }
    const int region_rows = scroll_bot_ - scroll_top_ + 1;
    if (n >= region_rows)
    {
        for (int y = scroll_top_; y <= scroll_bot_; ++y)
        {
            std::memset(&chars_[y * MAX_COLS], ' ', cols_);
            std::memset(&attrs_[y * MAX_COLS], 0,   cols_);
        }
        const unsigned H = fr_->font->height;
        std::memset(fb_ + scroll_top_ * H * pitch_, 0,
                    static_cast<size_t>(region_rows) * H * pitch_);
        return;
    }

    // Move char/attr cells down by n rows (top-down to avoid overlap).
    for (int y = scroll_bot_; y - n >= scroll_top_; --y)
    {
        std::memmove(&chars_[y * MAX_COLS], &chars_[(y - n) * MAX_COLS], cols_);
        std::memmove(&attrs_[y * MAX_COLS], &attrs_[(y - n) * MAX_COLS], cols_);
    }
    for (int y = scroll_top_; y < scroll_top_ + n; ++y)
    {
        std::memset(&chars_[y * MAX_COLS], ' ', cols_);
        std::memset(&attrs_[y * MAX_COLS], 0,   cols_);
    }

    const unsigned H = fr_->font->height;
    const unsigned shift_bytes =
        static_cast<unsigned>(n) * H * pitch_;
    const unsigned region_bytes =
        static_cast<unsigned>(region_rows) * H * pitch_;
    uint8_t* base = fb_ + scroll_top_ * H * pitch_;
    std::memmove(base + shift_bytes, base, region_bytes - shift_bytes);
    std::memset(base, 0, shift_bytes);
}

void Textport::scroll_up_region(int n)
{
    hide_cursor_();
    scroll_region_up_(n);
    show_cursor_();
}

void Textport::scroll_down_region(int n)
{
    hide_cursor_();
    scroll_region_down_(n);
    show_cursor_();
}

void Textport::set_scroll_region(int top, int bottom)
{
    // VT102: if bottom == 0 it means default to last row.  Caller maps
    // 1-based ESC[r params into 0-based row indices and passes 0 here to
    // mean "use rows-1".
    if (bottom <= 0) { bottom = rows_ - 1; }
    if (top < 0) { top = 0; }
    if (top >= rows_) { top = rows_ - 1; }
    if (bottom >= rows_) { bottom = rows_ - 1; }
    if (top >= bottom)
    {
        // Invalid; ignore per DEC behavior.
        return;
    }
    scroll_top_ = static_cast<uint8_t>(top);
    scroll_bot_ = static_cast<uint8_t>(bottom);
    // DEC: setting the region homes the cursor.
    move_to(0, scroll_top_);
}

void Textport::erase_in_line(int mode)
{
    hide_cursor_();
    int start;
    int end;
    switch (mode)
    {
        case 0:
            start = cursor_x_;
            end   = cols_;
            break;
        case 1:
            start = 0;
            end   = cursor_x_ + 1;
            break;
        case 2:
        default:
            start = 0;
            end   = cols_;
            break;
    }
    for (int x = start; x < end; ++x)
    {
        paint_blank_cell_(static_cast<unsigned>(x), cursor_y_);
    }
    show_cursor_();
}

void Textport::erase_in_display(int mode)
{
    hide_cursor_();
    int first_full_row;
    int last_full_row;
    bool partial_current = false;
    int partial_start = 0;
    int partial_end = 0;

    switch (mode)
    {
        case 0:  // cursor to end of screen
            partial_current = true;
            partial_start = cursor_x_;
            partial_end   = cols_;
            first_full_row = cursor_y_ + 1;
            last_full_row  = rows_ - 1;
            break;
        case 1:  // start of screen to cursor
            partial_current = true;
            partial_start = 0;
            partial_end   = cursor_x_ + 1;
            first_full_row = 0;
            last_full_row  = cursor_y_ - 1;
            break;
        case 2:
        default:
            first_full_row = 0;
            last_full_row  = rows_ - 1;
            break;
    }
    if (partial_current)
    {
        for (int x = partial_start; x < partial_end; ++x)
        {
            paint_blank_cell_(static_cast<unsigned>(x), cursor_y_);
        }
    }
    for (int y = first_full_row; y <= last_full_row; ++y)
    {
        std::memset(&chars_[y * MAX_COLS], ' ', cols_);
        std::memset(&attrs_[y * MAX_COLS], 0,   cols_);
        // Clear pixel row(s) for this character row.
        const unsigned H = fr_->font->height;
        std::memset(fb_ + static_cast<unsigned>(y) * H * pitch_, 0, H * pitch_);
    }
    show_cursor_();
}

void Textport::insert_lines(int n)
{
    if (n < 1) { n = 1; }
    // Only effective when cursor is inside the scroll region.
    if (cursor_y_ < scroll_top_ || cursor_y_ > scroll_bot_)
    {
        return;
    }
    // Temporarily set scroll region to [cursor_y_..scroll_bot_] and scroll
    // it down by n; that pushes existing content down and opens n blank
    // lines at cursor_y_.
    const uint8_t saved_top = scroll_top_;
    scroll_top_ = cursor_y_;
    hide_cursor_();
    scroll_region_down_(n);
    scroll_top_ = saved_top;
    cursor_x_ = 0;
    cursor_hanging_ = false;
    show_cursor_();
}

void Textport::delete_lines(int n)
{
    if (n < 1) { n = 1; }
    if (cursor_y_ < scroll_top_ || cursor_y_ > scroll_bot_)
    {
        return;
    }
    const uint8_t saved_top = scroll_top_;
    scroll_top_ = cursor_y_;
    hide_cursor_();
    scroll_region_up_(n);
    scroll_top_ = saved_top;
    cursor_x_ = 0;
    cursor_hanging_ = false;
    show_cursor_();
}

void Textport::insert_chars(int n)
{
    if (n < 1) { n = 1; }
    if (n >= cols_ - cursor_x_)
    {
        // All cells from cursor to EOL go blank.
        hide_cursor_();
        for (int x = cursor_x_; x < cols_; ++x)
        {
            paint_blank_cell_(static_cast<unsigned>(x), cursor_y_);
        }
        show_cursor_();
        return;
    }
    hide_cursor_();
    const unsigned row_base = cursor_y_ * MAX_COLS;
    for (int x = cols_ - 1; x >= cursor_x_ + n; --x)
    {
        chars_[row_base + x] = chars_[row_base + x - n];
        attrs_[row_base + x] = attrs_[row_base + x - n];
    }
    for (int x = cursor_x_; x < cursor_x_ + n; ++x)
    {
        chars_[row_base + x] = ' ';
        attrs_[row_base + x] = 0;
    }
    for (int x = cursor_x_; x < cols_; ++x)
    {
        paint_cell_(static_cast<unsigned>(x), cursor_y_);
    }
    show_cursor_();
}

void Textport::delete_chars(int n)
{
    if (n < 1) { n = 1; }
    hide_cursor_();
    const unsigned row_base = cursor_y_ * MAX_COLS;
    if (n >= cols_ - cursor_x_)
    {
        for (int x = cursor_x_; x < cols_; ++x)
        {
            chars_[row_base + x] = ' ';
            attrs_[row_base + x] = 0;
            paint_blank_cell_(static_cast<unsigned>(x), cursor_y_);
        }
        show_cursor_();
        return;
    }
    for (int x = cursor_x_; x + n < cols_; ++x)
    {
        chars_[row_base + x] = chars_[row_base + x + n];
        attrs_[row_base + x] = attrs_[row_base + x + n];
    }
    for (int x = cols_ - n; x < cols_; ++x)
    {
        chars_[row_base + x] = ' ';
        attrs_[row_base + x] = 0;
    }
    for (int x = cursor_x_; x < cols_; ++x)
    {
        paint_cell_(static_cast<unsigned>(x), cursor_y_);
    }
    show_cursor_();
}

void Textport::save_cursor()
{
    saved_x_ = cursor_x_;
    saved_y_ = cursor_y_;
    saved_hanging_ = cursor_hanging_;
}

void Textport::restore_cursor()
{
    hide_cursor_();
    cursor_x_ = saved_x_;
    cursor_y_ = saved_y_;
    cursor_hanging_ = saved_hanging_;
    if (cursor_x_ >= cols_) { cursor_x_ = cols_ - 1; }
    if (cursor_y_ >= rows_) { cursor_y_ = rows_ - 1; }
    blink_phase_ = true;
    last_blink_tick_ = tick_counter;
    show_cursor_();
}

// --- C shims --------------------------------------------------------------

extern "C" void textport_configure(uint8_t* fb, unsigned pitch_bytes,
                                   const FontRenderer* fr,
                                   unsigned cols, unsigned rows)
{
    g_textport.configure(fb, pitch_bytes, fr, cols, rows);
}

extern "C" void textport_putchar(int c)
{
    g_textport.put_glyph(static_cast<uint8_t>(c));
}

extern "C" void textport_clear()
{
    g_textport.clear();
}

extern "C" void textport_blink_tick()
{
    g_textport.cursor_blink_tick();
}

} // namespace griffin::textport
