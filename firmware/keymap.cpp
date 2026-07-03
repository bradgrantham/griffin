// keymap.cpp -- cooked PS/2 (scan-code set 2) -> ASCII translation.
//
// Sits directly above ps2.cpp (the GLUE frame/hardware layer): it pulls raw
// scancodes from ps2_getchar(), runs the make/break/extended decode, and
// pushes the resulting ASCII -- including VT-style escape sequences for the
// arrow / navigation keys -- into a ring that the console layer drains via
// keyboard_ready() / keyboard_getchar().
//
// This facility is deliberately PS/2-only: it does not touch the DUART.
// Merging the keyboard with the serial console is the consumer's job
// (read() in syscalls.c), per the project's "facilities don't
// cross-communicate" rule.
//
// The base keymap_table[] and kbd_lookup() are lifted verbatim from the
// Alice 3 firmware (from-alice3/ps2_keyboard.c).  New here: the E0/F0/E1
// decode state machine, the scancode bounds check, escape-sequence emission
// for extended keys, the ASCII ring, and the raw-mode switch.
//
// No line editing or echo yet -- bytes are returned as they arrive (that
// line-discipline layer is deferred).

#include <stdint.h>

#include "keymap.h"
#include "ps2.h"

namespace {

// --- scan-code set 2 prefix / modifier bytes -------------------------------
constexpr uint8_t SC_BREAK  = 0xF0;   // next byte is a key release
constexpr uint8_t SC_EXT    = 0xE0;   // next byte is an extended scancode
constexpr uint8_t SC_EXT2   = 0xE1;   // Pause/Break lead-in (multi-byte)
constexpr uint8_t SC_LSHIFT = 0x12;
constexpr uint8_t SC_RSHIFT = 0x59;
constexpr uint8_t SC_CTRL   = 0x14;   // left ctrl; right ctrl arrives as E0 14
constexpr uint8_t SC_ALT    = 0x11;   // left alt;  right alt  arrives as E0 11

// --- base keymap, lifted verbatim from from-alice3/ps2_keyboard.c ----------
// Four columns per scancode: normal, shift, ctrl, alt.  '?' doubles as the
// "unmapped key" filler, so unmapped base keys emit '?' (faithful to the
// original -- refine later if it proves annoying).
static const unsigned char keymap_table[] = {
   '?', '?', '?', '?',
   '9', '9', '9', '9',
   '?', '?', '?', '?',
   '5', '5', '5', '5',
   '3', '3', '3', '3',
   '1', '1', '1', '1',
   '2', '2', '2', '2',
   '1', '1', '1', '1',
   '?', '?', '?', '?',
   '1', '1', '1', '1',
   '8', '8', '8', '8',
   '6', '6', '6', '6',
   '4', '4', '4', '4',
   9,   9,   9,   9,
   '`', '~', '`', '`',
   '?', '?', '?', '?',
   '?', '?', '?', '?',
   '?', '?', '?', '?',
   '?', '?', '?', '?',
   '?', '?', '?', '?',
   '?', '?', '?', '?',
   'q', 'Q',  17,  17,
   '1', '!', '1', '1',
   '?', '?', '?', '?',
   '?', '?', '?', '?',
   '?', '?', '?', '?',
   'z', 'Z',  26,  26,
   's', 'S',  19,  19,
   'a', 'A',   1,   1,
   'w', 'W',  23,  23,
   '2', '@', '2', '2',
   '?', '?', '?', '?',
   '?', '?', '?', '?',
   'c', 'C',   3,   3,
   'x', 'X',  24,  24,
   'd', 'D',   4,   4,
   'e', 'E',   5,   5,
   '4', '$', '4', '4',
   '3', '#', '3', '3',
   '?', '?', '?', '?',
   '?', '?', '?', '?',
   ' ', ' ', ' ', ' ' ,
   'v', 'V',  22,  22,
   'f', 'F',   6,   6,
   't', 'T',  20,  20,
   'r', 'R',  18,  18,
   '5', '%', '5', '5',
   '?', '?', '?', '?',
   '?', '?', '?', '?',
   'n', 'N',  14,  14,
   'b', 'B',   2,   2,
   'h', 'H',   8,   8,
   'g', 'G',   7,   7,
   'y', 'Y',  25,  25,
   '6', '^', '6', '6',
   '?', '?', '?', '?',
   '?', '?', '?', '?',
   '?', '?', '?', '?',
   'm', 'M',  13,  13,
   'j', 'J',  10,  10,
   'u', 'U',  21,  21,
   '7', '&', '7', '7',
   '8', '*', '8', '8',
   '?', '?', '?', '?',
   '?', '?', '?', '?',
   ',', '<', ',', ',',
   'k', 'K',  11,  11,
   'i', 'I',   9,   9,
   'o', 'O',  15,  15,
   '0', ')', '0', '0',
   '9', '(', '9', '9',
   '?', '?', '?', '?',
   '?', '?', '?', '?',
   '.', '>', '.', '.',
   '/', '?', '/', '/',
   'l', 'L',  12,  12,
   ';', ':', ';', ';',
   'p', 'P',  16,  16,
   '-', '_', '-', '-',
   '?', '?', '?', '?',
   '?', '?', '?', '?',
   '?', '?', '?', '?',
   39, '"',  39,  39,
   '?', '?', '?', '?',
   '[', '{', '[', '[',
   '=', '+', '=', '=',
   '?', '?', '?', '?',
   '?', '?', '?', '?',
   '?', '?', '?', '?',
   '?', '?', '?', '?',
   13,  13,  13,  13,
   ']', '}', ']', ']',
   '?', '?', '?', '?',
   92, '|',  92,  92,
   '?', '?', '?', '?',
   '?', '?', '?', '?',
   '?', '?', '?', '?',
   '?', '?', '?', '?',
   '?', '?', '?', '?',
   '?', '?', '?', '?',
   '?', '?', '?', '?',
   '?', '?', '?', '?',
   8,   8,   8,   8,
   '?', '?', '?', '?',
   '?', '?', '?', '?',
   '1', '1', '1', '1',
   '?', '?', '?', '?',
   '4', '4', '4', '4',
   '7', '7', '7', '7',
   '?', '?', '?', '?',
   '?', '?', '?', '?',
   '?', '?', '?', '?',
   '0', '0', '0', '0',
   '.', '.', '.', '.',
   '2', '2', '2', '2',
   '5', '5', '5', '5',
   '6', '6', '6', '6',
   '8', '8', '8', '8',
   27,  27,  27,  27,
   '?', '?', '?', '?',
   '1', '1', '1', '1',
   '+', '+', '+', '+',
   '3', '3', '3', '3',
   '-', '-', '-', '-',
   '*', '*', '*', '*',
   '9', '9', '9', '9',
   '?', '?', '?', '?',
   '?', '?', '?', '?',
};

constexpr unsigned KEYMAP_ENTRIES = sizeof(keymap_table) / 4u;

// Column-priority lookup (shift > ctrl > alt), lifted verbatim from the
// original.  Caller guarantees byte < KEYMAP_ENTRIES.
unsigned char kbd_lookup(int shift, int alt, int ctrl, unsigned char byte)
{
    int which = 0;
    if (shift) which = 1;
    else if (ctrl) which = 2;
    else if (alt) which = 3;
    return keymap_table[byte * 4 + which];
}

// --- cooked ASCII ring -----------------------------------------------------
// Producer and consumer are both mainline (the PS/2 ISR only fills ps2.cpp's
// scancode queue), so no IRQ masking is needed here.
constexpr unsigned RING_SIZE = 64;        // power of two
uint8_t  ring[RING_SIZE];
unsigned ring_head = 0;
unsigned ring_tail = 0;

bool ring_empty() { return ring_head == ring_tail; }

void ring_push(uint8_t c)
{
    unsigned next = (ring_tail + 1u) & (RING_SIZE - 1u);
    if (next == ring_head) { return; }    // full: drop (cooked input overrun)
    ring[ring_tail] = c;
    ring_tail = next;
}

void ring_push_str(const char* s)
{
    while (*s != '\0') { ring_push(static_cast<uint8_t>(*s++)); }
}

// --- modifier + decode state ----------------------------------------------
bool mod_shift = false;
bool mod_ctrl  = false;
bool mod_alt   = false;

enum class St { Normal, Break, Ext, ExtBreak };
St       state   = St::Normal;
unsigned e1_skip = 0;        // bytes to swallow after an E1 (Pause) lead-in

bool raw_mode = false;

// VT/xterm-style sequences for extended (E0-prefixed) navigation keys, so
// input round-trips through the same VT102 vocabulary the output side speaks.
// Returns nullptr for extended keys we deliberately drop.
const char* ext_escape(uint8_t sc)
{
    switch (sc)
    {
        case 0x75: return "\x1b[A";    // up arrow
        case 0x72: return "\x1b[B";    // down arrow
        case 0x74: return "\x1b[C";    // right arrow
        case 0x6B: return "\x1b[D";    // left arrow
        case 0x6C: return "\x1b[H";    // home
        case 0x69: return "\x1b[F";    // end
        case 0x7D: return "\x1b[5~";   // page up
        case 0x7A: return "\x1b[6~";   // page down
        case 0x70: return "\x1b[2~";   // insert
        case 0x71: return "\x1b[3~";   // delete
        case 0x4A: return "/";         // keypad /
        case 0x5A: return "\r";        // keypad enter
        default:   return nullptr;     // unmapped extended key: drop
    }
}

bool is_modifier(uint8_t sc)
{
    return sc == SC_LSHIFT || sc == SC_RSHIFT || sc == SC_CTRL || sc == SC_ALT;
}

void set_modifier(uint8_t sc, bool down)
{
    switch (sc)
    {
        case SC_LSHIFT:
        case SC_RSHIFT: mod_shift = down; break;
        case SC_CTRL:   mod_ctrl  = down; break;
        case SC_ALT:    mod_alt   = down; break;
        default:        break;
    }
}

// Emit ASCII for a base-page (non-extended) make event.
void emit_base_make(uint8_t sc)
{
    if (sc >= KEYMAP_ENTRIES) { return; }   // bounds check (orig had none)
    unsigned char c = kbd_lookup(mod_shift, mod_alt, mod_ctrl, sc);
    ring_push(c);
}

// Advance the decode state machine by one raw scancode byte.
void feed_scancode(uint8_t b)
{
    if (e1_skip > 0) { --e1_skip; return; }

    switch (state)
    {
        case St::Normal:
            if (b == SC_BREAK)       { state = St::Break; }
            else if (b == SC_EXT)    { state = St::Ext; }
            else if (b == SC_EXT2)   { e1_skip = 7; }   // Pause: swallow the rest
            else if (is_modifier(b)) { set_modifier(b, true); }
            else                     { emit_base_make(b); }
            break;

        case St::Break:                          // F0 xx : base-page release
            if (is_modifier(b)) { set_modifier(b, false); }
            state = St::Normal;
            break;

        case St::Ext:
            if (b == SC_BREAK)     { state = St::ExtBreak; }
            else if (b == SC_CTRL) { mod_ctrl = true;  state = St::Normal; }  // right ctrl
            else if (b == SC_ALT)  { mod_alt  = true;  state = St::Normal; }  // right alt
            else
            {
                const char* esc = ext_escape(b);
                if (esc != nullptr) { ring_push_str(esc); }
                state = St::Normal;
            }
            break;

        case St::ExtBreak:                       // E0 F0 xx : extended release
            if (b == SC_CTRL)     { mod_ctrl = false; }
            else if (b == SC_ALT) { mod_alt  = false; }
            state = St::Normal;
            break;
    }
}

// Drain whatever scancodes ps2.cpp has buffered, translating into the ring.
void pump()
{
    while (ps2_received_ready())
    {
        feed_scancode(ps2_getchar());
    }
}

// Reset all decode state.  Used on raw-mode transitions so a half-decoded
// E0/F0 or a modifier held across the switch can't leak into the next mode.
void reset_decode()
{
    state     = St::Normal;
    e1_skip   = 0;
    mod_shift = false;
    mod_ctrl  = false;
    mod_alt   = false;
    ring_head = 0;
    ring_tail = 0;
    while (ps2_received_ready()) { static_cast<void>(ps2_getchar()); }
}

} // namespace

extern "C" {

bool keyboard_ready(void)
{
    if (raw_mode) { return false; }   // cooked path is idle while raw
    pump();
    return !ring_empty();
}

uint8_t keyboard_getchar(void)
{
    if (ring_empty()) { return 0; }   // result defined only when ready
    uint8_t c = ring[ring_head];
    ring_head = (ring_head + 1u) & (RING_SIZE - 1u);
    return c;
}

void griffin_enter_raw(void)
{
    raw_mode = true;
    reset_decode();
}

void griffin_leave_raw(void)
{
    raw_mode = false;
    reset_decode();
}

bool griffin_is_raw(void) { return raw_mode; }

bool keyboard_raw_ready(void) { return ps2_received_ready(); }

uint8_t keyboard_raw_getchar(void)
{
    if (!ps2_received_ready()) { return 0; }
    return ps2_getchar();
}

} // extern "C"
