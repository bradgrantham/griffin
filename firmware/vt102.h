// VT102 terminal parser.  Pure state machine: ingests bytes one at a time,
// drives a Textport for visual state, and may emit response strings (e.g.
// device-attributes / cursor reports) through a caller-supplied Responder.

#pragma once

#include <cstdint>
#include <cstddef>

#include "textport.h"

namespace griffin::textport
{

class Vt102Parser
{
public:
    // Called when the parser needs to send a reply to the host (e.g. ESC[c).
    // Caller-supplied; typically wires into a UART TX helper.
    using Responder = void (*)(const char* s, size_t n);

    explicit Vt102Parser(Textport& tp, Responder respond = nullptr);
    void put(uint8_t c);
    void reset();
    void set_responder(Responder r) { respond_ = r; }

    Textport& textport() { return tp_; }

private:
    void execute_csi(uint8_t final_byte);
    void execute_esc(uint8_t c);
    void send_str(const char* s);
    void send_num(int n);
    int  par_or(unsigned i, int def) const;
    void tab_to_next();
    void set_tab_at(unsigned col);
    void clear_tab_at(unsigned col);
    void clear_all_tabs();
    void seed_default_tabs();

    enum class S : uint8_t
    {
        Normal,
        Esc,
        CsiEntry,
        CsiParam,
        Hash,
        SetG0,
        SetG1,
    };

    Textport& tp_;
    Responder respond_;

    S        state_ = S::Normal;

    static constexpr unsigned MAX_PARS = 16;
    int      pars_[MAX_PARS]{};
    uint8_t  npars_ = 0;
    bool     ques_ = false;

    // Tab stop bitmap covers MAX_COLS columns (106 → 7 words).
    uint16_t tab_stop_[(MAX_COLS + 15) / 16]{};
};

// Process-wide singleton wired by rom.cpp.
extern Vt102Parser g_vt102;

} // namespace griffin::textport
