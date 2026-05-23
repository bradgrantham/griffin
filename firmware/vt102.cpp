#include "vt102.h"

#include <cstring>

namespace griffin::textport
{

// Process-wide singleton; Responder gets wired by rom.cpp.
Vt102Parser g_vt102(g_textport, nullptr);

Vt102Parser::Vt102Parser(Textport& tp, Responder respond)
    : tp_(tp), respond_(respond)
{
    reset();
}

void Vt102Parser::reset()
{
    state_ = S::Normal;
    npars_ = 0;
    ques_ = false;
    for (unsigned i = 0; i < MAX_PARS; ++i)
    {
        pars_[i] = 0;
    }
    seed_default_tabs();
}

int Vt102Parser::par_or(unsigned i, int def) const
{
    if (i >= MAX_PARS)
    {
        return def;
    }
    return pars_[i] == 0 ? def : pars_[i];
}

void Vt102Parser::seed_default_tabs()
{
    // VT102 default: tab every 8 columns.
    clear_all_tabs();
    for (unsigned c = 0; c < MAX_COLS; c += 8)
    {
        set_tab_at(c);
    }
}

void Vt102Parser::set_tab_at(unsigned col)
{
    if (col >= MAX_COLS) { return; }
    tab_stop_[col / 16] |= static_cast<uint16_t>(1u << (col % 16));
}

void Vt102Parser::clear_tab_at(unsigned col)
{
    if (col >= MAX_COLS) { return; }
    tab_stop_[col / 16] &= static_cast<uint16_t>(~(1u << (col % 16)));
}

void Vt102Parser::clear_all_tabs()
{
    for (unsigned i = 0; i < sizeof(tab_stop_)/sizeof(tab_stop_[0]); ++i)
    {
        tab_stop_[i] = 0;
    }
}

void Vt102Parser::tab_to_next()
{
    // Advance the cursor (writing spaces is optional; standard VT102 just
    // moves) until hitting a column with a tab stop set, or EOL.
    for (;;)
    {
        if (tp_.cursor_col() + 1 >= tp_.cols())
        {
            tp_.cursor_fwd();   // sets hanging
            return;
        }
        tp_.cursor_fwd();
        const unsigned col = tp_.cursor_col();
        if (tab_stop_[col / 16] & (1u << (col % 16)))
        {
            return;
        }
    }
}

void Vt102Parser::send_str(const char* s)
{
    if (!respond_ || !s) { return; }
    respond_(s, std::strlen(s));
}

void Vt102Parser::send_num(int n)
{
    char buf[12];
    int  i = sizeof(buf);
    buf[--i] = '\0';
    if (n == 0)
    {
        buf[--i] = '0';
    }
    else
    {
        bool neg = (n < 0);
        unsigned v = static_cast<unsigned>(neg ? -n : n);
        while (v != 0 && i > 0)
        {
            buf[--i] = static_cast<char>('0' + (v % 10u));
            v /= 10u;
        }
        if (neg && i > 0)
        {
            buf[--i] = '-';
        }
    }
    send_str(buf + i);
}

void Vt102Parser::put(uint8_t c)
{
    // Fast path: printable in Normal state.
    if (state_ == S::Normal && c >= ' ' && c != 0x7F)
    {
        tp_.put_glyph(c);
        return;
    }

    // C0 controls that act in any state (per VT102 spec).
    switch (c)
    {
        case 0x00: return;                 // NUL
        case 0x07: tp_.bell(); return;     // BEL
        case 0x08:                          // BS
            tp_.cursor_back();
            return;
        case 0x09:                          // HT
            tab_to_next();
            return;
        case 0x0A:                          // LF
        case 0x0B:                          // VT
        case 0x0C:                          // FF
            tp_.line_feed();
            return;
        case 0x0D:                          // CR
            tp_.carriage_return();
            return;
        case 0x0E:                          // SO (alt charset; ignored)
        case 0x0F:                          // SI (normal charset; ignored)
            return;
        case 0x18:                          // CAN
        case 0x1A:                          // SUB
            state_ = S::Normal;
            return;
        case 0x1B:                          // ESC
            state_ = S::Esc;
            npars_ = 0;
            ques_ = false;
            return;
        case 0x7F:                          // DEL — ignored
            return;
        default: break;
    }

    switch (state_)
    {
        case S::Normal:
            // Non-printable above 0x7F not handled; ignore.
            return;

        case S::Esc:
            execute_esc(c);
            return;

        case S::CsiEntry:
            for (unsigned i = 0; i < MAX_PARS; ++i) { pars_[i] = 0; }
            npars_ = 0;
            ques_  = (c == '?');
            state_ = S::CsiParam;
            if (ques_) { return; }
            [[fallthrough]];

        case S::CsiParam:
            if (c >= '0' && c <= '9')
            {
                if (npars_ < MAX_PARS)
                {
                    pars_[npars_] = pars_[npars_] * 10 + (c - '0');
                }
                return;
            }
            if (c == ';')
            {
                if (npars_ < MAX_PARS - 1) { ++npars_; }
                return;
            }
            // Final byte (0x40..0x7E) — dispatch.
            if (npars_ < MAX_PARS) { ++npars_; }
            execute_csi(c);
            state_ = S::Normal;
            return;

        case S::Hash:
            // DEC private (e.g. screen alignment test).  Ignored.
            state_ = S::Normal;
            return;

        case S::SetG0:
        case S::SetG1:
            // Character-set designators.  Ignored.
            state_ = S::Normal;
            return;
    }
}

void Vt102Parser::execute_esc(uint8_t c)
{
    state_ = S::Normal;
    switch (c)
    {
        case '[':
            state_ = S::CsiEntry;
            return;
        case 'D':  // IND — line feed
            tp_.line_feed();
            return;
        case 'E':  // NEL — newline (CR + LF)
            tp_.carriage_return();
            tp_.line_feed();
            return;
        case 'M':  // RI — reverse line feed
            tp_.reverse_line_feed();
            return;
        case 'H':  // HTS — set tab stop at cursor column
            set_tab_at(tp_.cursor_col());
            return;
        case '7':  // DECSC — save cursor
            tp_.save_cursor();
            return;
        case '8':  // DECRC — restore cursor
            tp_.restore_cursor();
            return;
        case 'c':  // RIS — reset to initial state
            tp_.clear();
            tp_.move_to(0, 0);
            tp_.set_inverse(false);
            seed_default_tabs();
            return;
        case 'Z':  // DECID — respond with terminal ID
            send_str("\x1B[?6c");
            return;
        case '(':
            state_ = S::SetG0;
            return;
        case ')':
            state_ = S::SetG1;
            return;
        case '#':
            state_ = S::Hash;
            return;
        case '>':  // numeric keypad
        case '=':  // application keypad
            return;
        default:
            return;
    }
}

void Vt102Parser::execute_csi(uint8_t final_byte)
{
    // Most VT102 control sequences are >= 1 parameter; treat missing as 0
    // and let par_or() substitute the relevant default.
    const int p0 = pars_[0];
    const int p1 = (npars_ > 1) ? pars_[1] : 0;
    (void)p0;

    if (ques_)
    {
        // DEC private sequences (mode set/reset, reports).  We only handle
        // the report path; mode set/reset are accepted-and-ignored.
        switch (final_byte)
        {
            case 'h':   // DECSET — ignored
            case 'l':   // DECRST — ignored
                return;
            case 'n':   // DEC private report — ignored
                return;
            default:
                return;
        }
    }

    switch (final_byte)
    {
        case '@':  // ICH — insert chars
            tp_.insert_chars(par_or(0, 1));
            return;
        case 'A':  // CUU — cursor up
            tp_.cursor_up(par_or(0, 1));
            return;
        case 'B':  // CUD — cursor down
        case 'e':  // VPR
            tp_.cursor_down(par_or(0, 1));
            return;
        case 'C':  // CUF — cursor forward
        case 'a':  // HPR
            for (int i = 0; i < par_or(0, 1); ++i) { tp_.cursor_fwd(); }
            return;
        case 'D':  // CUB — cursor back
            for (int i = 0; i < par_or(0, 1); ++i) { tp_.cursor_back(); }
            return;
        case 'E':  // CNL
            tp_.cursor_down(par_or(0, 1));
            tp_.carriage_return();
            return;
        case 'F':  // CPL
            tp_.cursor_up(par_or(0, 1));
            tp_.carriage_return();
            return;
        case 'G':  // CHA — column absolute (1-based)
        case '`':  // HPA
            tp_.move_to(par_or(0, 1) - 1, tp_.cursor_row());
            return;
        case 'H':  // CUP
        case 'f':  // HVP
            tp_.move_to(par_or(1, 1) - 1, par_or(0, 1) - 1);
            return;
        case 'J':  // ED
            tp_.erase_in_display(pars_[0]);
            return;
        case 'K':  // EL
            tp_.erase_in_line(pars_[0]);
            return;
        case 'L':  // IL
            tp_.insert_lines(par_or(0, 1));
            return;
        case 'M':  // DL
            tp_.delete_lines(par_or(0, 1));
            return;
        case 'P':  // DCH
            tp_.delete_chars(par_or(0, 1));
            return;
        case 'd':  // VPA — row absolute
            tp_.move_to(tp_.cursor_col(), par_or(0, 1) - 1);
            return;
        case 'c':  // DA — device attributes
            if (pars_[0] == 0)
            {
                send_str("\x1B[?6c");
            }
            return;
        case 'g':  // TBC — tab clear
            if (pars_[0] == 0)
            {
                clear_tab_at(tp_.cursor_col());
            }
            else if (pars_[0] == 3)
            {
                clear_all_tabs();
            }
            return;
        case 'h':  // SM — set mode (ignored; no insert mode here)
        case 'l':  // RM — reset mode
            return;
        case 'm':  // SGR — limited: 0=reset, 7=reverse, 27=no-reverse
            if (npars_ == 0 || (npars_ == 1 && pars_[0] == 0))
            {
                tp_.set_inverse(false);
                return;
            }
            for (unsigned i = 0; i < npars_; ++i)
            {
                if (pars_[i] == 0)       { tp_.set_inverse(false); }
                else if (pars_[i] == 7)  { tp_.set_inverse(true);  }
                else if (pars_[i] == 27) { tp_.set_inverse(false); }
            }
            return;
        case 'n':  // DSR — device status report
            if (pars_[0] == 5)
            {
                send_str("\x1B[0n");   // ready, no malfunction
            }
            else if (pars_[0] == 6)
            {
                // CPR — cursor position report (1-based row;col)
                send_str("\x1B[");
                send_num(tp_.cursor_row() + 1);
                send_str(";");
                send_num(tp_.cursor_col() + 1);
                send_str("R");
            }
            return;
        case 'r':  // DECSTBM — set top and bottom margins (1-based)
        {
            int top = par_or(0, 1) - 1;
            int bot = (npars_ > 1)
                ? (pars_[1] == 0 ? tp_.rows() - 1 : pars_[1] - 1)
                : (tp_.rows() - 1);
            (void)p1;
            tp_.set_scroll_region(top, bot);
            return;
        }
        case 's':  // ANSI save cursor
            tp_.save_cursor();
            return;
        case 'u':  // ANSI restore cursor
            tp_.restore_cursor();
            return;
        default:
            return;
    }
}

extern "C" int textport_vt102_putchar(int c)
{
    g_vt102.put(static_cast<uint8_t>(c));
    return c;
}

} // namespace griffin::textport
