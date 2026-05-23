// Synchronous text-mode overlay on a 1bpp linear framebuffer.
//
// Holds a backing char/attr buffer sized for the largest supported textport
// (106x60).  Renders directly into the framebuffer on every state change so
// no repaint happens out of the vsync ISR — VBLANK stays free for any
// real-time payload.
//
// Cursor blink is driven off the existing 100 Hz `tick_counter`; callers
// invoke cursor_blink_tick() periodically (e.g. from the main loop).
//
// VT102 sequences are parsed by `Vt102Parser` in vt102.h, which calls these
// semantic methods.

#pragma once

#include <cstdint>
#include <cstddef>

#include "font.h"

namespace griffin::textport
{

inline constexpr unsigned MAX_COLS = 106;
inline constexpr unsigned MAX_ROWS = 60;

inline constexpr uint8_t ATTR_INVERSE = 0x01;

class Textport
{
public:
    // Wire the textport to a framebuffer and font.  Clears char buffer and
    // pixel buffer, parks cursor at (0,0), resets scroll region to full
    // screen.  Caller owns the framebuffer (must be aligned per ENGINE).
    void configure(uint8_t* fb, unsigned pitch_bytes,
                   const FontRenderer* fr,
                   unsigned cols, unsigned rows);

    // --- VT102 sink interface ----------------------------------------------
    // Each of these is intended to be called by Vt102Parser.  Cursor is
    // hidden before mutation and re-shown afterwards so blink state stays
    // consistent.

    void put_glyph(uint8_t c);
    void set_inverse(bool on) { inverse_attr_ = on; }
    bool inverse() const { return inverse_attr_; }

    void move_to(int x, int y);
    void cursor_fwd();
    void cursor_back();
    void cursor_up(int n);
    void cursor_down(int n);
    void carriage_return();
    void line_feed();              // index; scrolls within region
    void reverse_line_feed();      // reverse index; scrolls within region

    void scroll_up_region(int n);   // scroll region content up n rows
    void scroll_down_region(int n); // scroll region content down n rows

    void erase_in_line(int mode);     // 0=to-eol, 1=to-bol, 2=line
    void erase_in_display(int mode);  // 0=to-eos, 1=to-bos, 2=all

    void insert_lines(int n);     // at cursor, scroll lower region down
    void delete_lines(int n);     // at cursor, scroll lower region up
    void insert_chars(int n);     // shift cells right within current line
    void delete_chars(int n);     // shift cells left  within current line

    void save_cursor();
    void restore_cursor();

    // Top/bottom inclusive, 0-based row indices.  Setting [0, rows-1] is the
    // default full-screen region.  An invalid request is ignored.
    void set_scroll_region(int top, int bottom);

    void clear();
    void bell();

    // Call periodically (e.g. from main loop).  Compares against the
    // running tick_counter and toggles cursor invert at ~2 Hz.
    void cursor_blink_tick();

    // --- introspection -----------------------------------------------------
    uint8_t cursor_col() const { return cursor_x_; }
    uint8_t cursor_row() const { return cursor_y_; }
    uint8_t cols() const { return cols_; }
    uint8_t rows() const { return rows_; }

private:
    void process_hanging();
    void scroll_region_up_(int n);     // helper: scroll region up n rows
    void scroll_region_down_(int n);   // helper: scroll region down n rows
    void paint_cell_(unsigned cx, unsigned cy);
    void paint_blank_cell_(unsigned cx, unsigned cy);
    void show_cursor_();
    void hide_cursor_();
    unsigned idx_(unsigned cx, unsigned cy) const { return cy * MAX_COLS + cx; }

    uint8_t* fb_ = nullptr;
    unsigned pitch_ = 0;
    const FontRenderer* fr_ = nullptr;

    uint8_t cols_ = 0;
    uint8_t rows_ = 0;
    uint8_t scroll_top_ = 0;
    uint8_t scroll_bot_ = 0;          // inclusive, last row of scroll region

    uint8_t cursor_x_ = 0;
    uint8_t cursor_y_ = 0;
    uint8_t saved_x_ = 0;
    uint8_t saved_y_ = 0;
    bool    cursor_hanging_ = false;
    bool    saved_hanging_ = false;
    bool    cursor_shown_ = false;     // last drawn state of the cursor cell
    bool    inverse_attr_ = false;

    // Cursor blink: track which half-second window we last painted.
    // tick_counter increments at 100 Hz; one cursor phase = 50 ticks.
    uint32_t last_blink_tick_ = 0;
    bool     blink_phase_ = true;      // true = cursor visible

    uint8_t chars_[MAX_COLS * MAX_ROWS];
    uint8_t attrs_[MAX_COLS * MAX_ROWS];
};

// --- C-callable shims (linkage for crt0.s / C code) ------------------------
extern "C" {

void textport_configure(uint8_t* fb, unsigned pitch_bytes,
                        const FontRenderer* fr,
                        unsigned cols, unsigned rows);
void textport_putchar(int c);           // raw glyph, bypasses VT102
void textport_clear();
void textport_blink_tick();             // periodic; call from main loop

// Vt102-aware putchar (defined in vt102.cpp).  Drives the parser then
// the textport.  Use this for general printf output.
int  textport_vt102_putchar(int c);

} // extern "C"

// Process-wide singleton wired by rom.cpp on startup.
extern Textport g_textport;

} // namespace griffin::textport
