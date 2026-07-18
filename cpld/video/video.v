`include "../../griffin.generated.vh"

// VIDEO — VGA 640x480 progressive 1bpp video generator
//
// VESA 640x480@60: 25.175 MHz pixel clock, 800x525 raster, negative
// HSync/VSync.  Pixel data is read byte-at-a-time from two IDT7200L15
// 256x9-bit FIFOs (filled simultaneously by ENGINE with 16-bit words).
// FIFO_EVEN holds MSB (D[15:8]), FIFO_ODD holds LSB (D[7:0]).  VIDEO
// reads alternately: EVEN first, then ODD, yielding big-endian byte
// order.  Q[8:0] outputs are shared (active one at a time); separate
// nRE pins select which FIFO to read.  Each byte is 8 pixels, MSB
// first.  The current pixel bit selects between two R3G3B2 palette
// entries (fg/bg).
//
// Palette-and-pixels: each scanline in memory begins with a 4-byte
// header — word 0 = {fg, bg} (R3G3B2 each), word 1 bit 0 = per-line
// mode select (0 = direct 1bpp, 1 = micro-HAM; see microham.txt) — that
// ENGINE streams through the FIFO ahead of the 80 pixel bytes.  VIDEO
// pops the header during hblank, latching fg/bg before the line's
// pixels, so the per-line palette is carried in-band with the pixel
// data and stays perfectly synchronized to scanout with no CPU
// involvement (no PALETTE register, no LINE_TOGGLE).
//
module Video
(
    // ----------------------------------------------------------------
    // Clocks
    // ----------------------------------------------------------------
    input  wire        SYSCLK,          // pin 83 (GCLK1) — 12 MHz CPU clock
    input  wire        PIXEL_CLK,       // pin 2  (GCLK2) — 25.175 MHz VGA

    // ----------------------------------------------------------------
    // Reset
    // ----------------------------------------------------------------
    input  wire        nRESET,          // pin 1  (GCLR)

    // ----------------------------------------------------------------
    // Bus interface
    // ----------------------------------------------------------------
    input  wire        nVIDEO_SELECT,   // pin 84 (OE1)
    input  wire        nAS,             // pin 21
    input  wire        nUDS,            // pin 22
    input  wire        nLDS,            // pin 64
    input  wire        R_nW,            // pin 25
    input  wire [5:1]  A,               // pins 48,49,51,54,55
    inout  wire [15:0] D,               // pins (see pin list)

    // ----------------------------------------------------------------
    // Pixel oscillator enables
    // ----------------------------------------------------------------
    output wire        CPST_CLK_ENB,    // pin 40 — Y1 14.318 MHz enable (held off)
    output wire        VGA_CLK_ENB,     // pin 41 — Y3 25.175 MHz enable (held on)

    // ----------------------------------------------------------------
    // VGA outputs
    // ----------------------------------------------------------------
    output wire        VGA_HSYNC,       // pin 46
    output wire        VGA_VSYNC,       // pin 29
    output reg         VGA_R0,          // pin 4
    output reg         VGA_R1,          // pin 8
    output reg         VGA_R2,          // pin 6
    output reg         VGA_G0,          // pin 76
    output reg         VGA_G1,          // pin 77
    output reg         VGA_G2,          // pin 81 (GCLK3)
    output reg         VGA_B0,          // pin 74
    output reg         VGA_B1,          // pin 75

    // ----------------------------------------------------------------
    // Control outputs
    // ----------------------------------------------------------------
    output wire        VIDEO_STALL,     // pin 79 — held low, fitter anchor
    output wire        nVIDEO_IRQ,      // pin 5

    // ----------------------------------------------------------------
    // 7200 FIFO read interface (bodge wires to breadboard)
    //   Q[8:0] shared between EVEN and ODD FIFOs; only one nRE
    //   is asserted at a time so only one FIFO drives.
    // ----------------------------------------------------------------
    input  wire [7:0]  FIFO_Q,          // pins 36,31,30,28,37,39,44,9
    input  wire        FIFO_Q8,         // pin 45 — 9th bit toggle
    output reg         nFIFO_RE_EVEN,   // pin 50 — EVEN FIFO read enable
    output reg         nFIFO_RE_ODD     // pin 52 — ODD FIFO read enable
);

    wire RESET = ~nRESET;

    // ----------------------------------------------------------------
    // Static assignments
    // ----------------------------------------------------------------
    assign CPST_CLK_ENB = 1'b0;
    assign VGA_CLK_ENB  = 1'b1;
    assign VIDEO_STALL  = 1'b0;

    // ----------------------------------------------------------------
    // VGA 640x480@60 timing parameters
    // ----------------------------------------------------------------

    localparam H_ACTIVE      = 10'd640;
    localparam H_FRONT_PORCH = 10'd16;
    localparam H_SYNC        = 10'd96;
    localparam H_BACK_PORCH  = 10'd48;
    localparam H_TOTAL       = 10'd800;

    localparam H_SYNC_START  = H_ACTIVE + H_FRONT_PORCH;        // 656
    localparam H_SYNC_END    = H_SYNC_START + H_SYNC;            // 752

    localparam V_ACTIVE      = 10'd480;
    localparam V_FRONT_PORCH = 10'd10;
    localparam V_SYNC        = 10'd2;
    localparam V_BACK_PORCH  = 10'd33;
    localparam V_TOTAL       = 10'd525;

    localparam V_SYNC_START  = V_ACTIVE + V_FRONT_PORCH;        // 490
    localparam V_SYNC_END    = V_SYNC_START + V_SYNC;            // 492

    // Per-line FIFO read layout: a 4-byte header (word 0 = {fg, bg},
    // word 1 = reserved) followed by 80 pixel bytes.  The header is read
    // during hblank and latched into the palette; pixels are shifted out
    // during active video.  Total reads/line = 84 (= 42 16-bit words),
    // matching ENGINE's 42-word-per-line framebuffer stride.
    localparam HEADER_BYTES   = 7'd4;
    localparam PIXEL_BYTES    = 7'd80;
    localparam BYTES_PER_LINE = HEADER_BYTES + PIXEL_BYTES;   // 84

    // ----------------------------------------------------------------
    // Horizontal and vertical counters (PIXEL_CLK domain)
    // ----------------------------------------------------------------

    reg [9:0] h_cnt;
    reg [9:0] v_cnt;

    // h_last is registered (compare against 798, one cycle early) so
    // consumers see one signal instead of a 10-bit h_cnt compare —
    // this keeps the 10 h_cnt bits out of the v_cnt LAB's UIM fan-in.
    reg h_last;

    always @(posedge PIXEL_CLK or posedge RESET)
    begin
        if (RESET)
        begin
            h_last <= 1'b0;
        end
        else
        begin
            h_last <= (h_cnt == H_TOTAL - 10'd2);
        end
    end

    always @(posedge PIXEL_CLK or posedge RESET)
    begin
        if (RESET)
        begin
            h_cnt <= 10'd0;
        end
        else if (h_last)
        begin
            h_cnt <= 10'd0;
        end
        else
        begin
            h_cnt <= h_cnt + 10'd1;
        end
    end

    always @(posedge PIXEL_CLK or posedge RESET)
    begin
        if (RESET)
        begin
            v_cnt <= 10'd0;
        end
        else if (h_last)
        begin
            if (v_cnt == V_TOTAL - 10'd1)
            begin
                v_cnt <= 10'd0;
            end
            else
            begin
                v_cnt <= v_cnt + 10'd1;
            end
        end
    end

    wire h_active = (h_cnt < H_ACTIVE);
    wire v_active = (v_cnt < V_ACTIVE);
    wire active_video = h_active & v_active;

    wire in_hsync = (h_cnt >= H_SYNC_START) & (h_cnt < H_SYNC_END);
    wire in_vsync = (v_cnt >= V_SYNC_START) & (v_cnt < V_SYNC_END);

    // ----------------------------------------------------------------
    // VGA sync (negative polarity)
    // ----------------------------------------------------------------
    assign VGA_HSYNC = ~in_hsync;
    assign VGA_VSYNC = ~in_vsync;

    // ----------------------------------------------------------------
    // VSYNC interrupt — toggle + 2FF synchronizer across clock domains
    // ----------------------------------------------------------------

    wire vsync_event = (v_cnt == V_SYNC_START) & h_last;
    reg  vsync_tog;
    always @(posedge PIXEL_CLK or posedge RESET)
    begin
        if (RESET)
        begin
            vsync_tog <= 1'b0;
        end
        else if (vsync_event)
        begin
            vsync_tog <= ~vsync_tog;
        end
    end

    reg [2:0] vsync_sync;
    reg       video_irq_latched;
    wire      clrint_write;   // assigned below, after the CPU bus decode
    wire      vsync_edge   = vsync_sync[2] ^ vsync_sync[1];

    always @(posedge SYSCLK or posedge RESET)
    begin
        if (RESET)
        begin
            vsync_sync        <= 3'b000;
            video_irq_latched <= 1'b0;
        end
        else
        begin
            vsync_sync <= {vsync_sync[1:0], vsync_tog};
            if (vsync_edge)
            begin
                video_irq_latched <= 1'b1;
            end
            else if (clrint_write)
            begin
                video_irq_latched <= 1'b0;
            end
        end
    end

    // The vsync timing generator free-runs regardless of ENABLE (it must
    // already be counting before ENABLE is set, since output starts at the
    // next vsync boundary), so video_irq_latched sets every frame whether or
    // not video is in use.  Gate only the pin with IRQENB -- the latch (and
    // CLRINT's ability to clear it) stays unconditional, matching DUART's
    // ISR/IMR split, so a late unmask still sees any pending vsync rather
    // than silently losing it.
    assign nVIDEO_IRQ = ~(video_irq_latched & video_irqenb);

    // ----------------------------------------------------------------
    // CPU register interface (SYSCLK domain)
    // ----------------------------------------------------------------
    wire cpu_selected = ~nVIDEO_SELECT & ~nAS;
    wire cpu_reading  = cpu_selected & R_nW;
    wire cpu_writing  = cpu_selected & ~R_nW;

    // CLRINT (offset 0x03, A[5:1] = 5'h01) — declared with the VSYNC
    // interrupt logic above
    assign clrint_write = cpu_writing & (A == 5'h01) & ~nLDS;

    // CTRL register (offset 0x05, A[5:1] = 5'h02)
    reg video_enable;
    reg video_irqenb;

    wire ctrl_write = cpu_writing & (A == 5'h02) & ~nLDS;

    always @(posedge SYSCLK or posedge RESET)
    begin
        if (RESET)
        begin
            video_enable <= 1'b0;
            video_irqenb <= 1'b0;
        end
        else if (ctrl_write)
        begin
            video_enable <= D[0];
            video_irqenb <= D[1];
        end
    end

    // ----------------------------------------------------------------
    // Synchronize video_enable to PIXEL_CLK and latch frame_active at
    // the start of each frame (vsync_event).  This ensures pixel
    // shifting and FIFO draining always begin aligned to row 0 even
    // if the CPU asserts ENABLE mid-frame.  Free-running h_cnt/v_cnt
    // keep monitor sync locked the whole time.
    // ----------------------------------------------------------------
    reg [1:0] enable_sync;
    reg       frame_active;

    always @(posedge PIXEL_CLK or posedge RESET)
    begin
        if (RESET)
        begin
            enable_sync  <= 2'b00;
            frame_active <= 1'b0;
        end
        else
        begin
            enable_sync <= {enable_sync[0], video_enable};
            if (vsync_event)
            begin
                frame_active <= enable_sync[1];
            end
        end
    end

    // Palette (fg/bg) is no longer CPU-written — it arrives in-band via the
    // FIFO header word and is latched in the PIXEL_CLK domain (see the FIFO
    // read block below, where palette_fg/palette_bg are declared).

    // CTRL read (offset 0x05, A[5:1] = 5'h02)
    wire ctrl_read = cpu_reading & (A == 5'h02) & ~nLDS;

    // Set by the 9th-bit error detector below
    reg fifo_error;

    wire any_read = ctrl_read;
    wire [15:0] read_data = {13'd0, video_irqenb, fifo_error, video_enable};

    assign D = any_read ? read_data : 16'bz;

    // CLRERR (offset 0x09, A[5:1] = 5'h04)
    wire clrerr_write = cpu_writing & (A == 5'h04) & ~nLDS;

    // ----------------------------------------------------------------
    // 9th bit error detection
    //
    // ENGINE toggles bit 8 on each word written (both FIFOs get the
    // same Q8 per write).  VIDEO checks Q8 only on EVEN FIFO reads
    // (one check per word pair) — successive EVEN reads must toggle.
    // saved_9th_bit is 1 on reset so the first ENGINE byte (Q8=0)
    // is valid.
    //
    // PIXEL_CLK domain: toggle on error, sync to SYSCLK via 3FF.
    // ----------------------------------------------------------------

    reg saved_9th_bit;
    reg fifo_err_tog;

    reg [2:0] err_sync;
    wire      err_edge = err_sync[2] ^ err_sync[1];

    always @(posedge SYSCLK or posedge RESET)
    begin
        if (RESET)
        begin
            err_sync   <= 3'b000;
            fifo_error <= 1'b0;
        end
        else
        begin
            err_sync <= {err_sync[1:0], fifo_err_tog};
            if (err_edge)
            begin
                fifo_error <= 1'b1;
            end
            else if (clrerr_write)
            begin
                fifo_error <= 1'b0;
            end
        end
    end

    // ----------------------------------------------------------------
    // FIFO read logic and pixel shift register (PIXEL_CLK domain)
    //
    // Read 84 bytes per line (4 header + 80 pixel) from two FIFOs
    // alternately (EVEN first, then ODD, then EVEN, ...).  Each pixel
    // byte is 8 pixels (MSB first).  Only the selected FIFO's nRE is
    // asserted; the other stays high.  fifo_select toggles after each
    // byte load.
    //
    // Preload: assert nRE at h_cnt == 766 (via the registered preload
    // signal) when the next line is active; the 4 header bytes are
    // read during h 767..799, the first pixel byte is loaded at the
    // end of h 799, and pixel 0 is in shift_reg[7] during h_cnt == 0.
    // fifo_select resets to 0 (EVEN) at the start of each line.
    //
    // Mid-line: nRE is asserted at bit position 6 within the byte
    // (h_cnt[2:0] == 6, overlapping the second-to-last pixel of the
    // current byte); data is captured at bit position 7, and the new
    // byte's MSB drives the pixel output starting the next cycle (via
    // current_pixel_reg pipeline).
    // ----------------------------------------------------------------

    reg [7:0] shift_reg;
    reg [6:0] byte_cnt;
    reg       fifo_loading;
    reg       fifo_select;    // 0 = EVEN (MSB), 1 = ODD (LSB)

    // Palette latched in-band from the FIFO header (PIXEL_CLK domain).
    // byte_cnt 0 = fg (EVEN of header word 0), byte_cnt 1 = bg (ODD of
    // header word 0); byte_cnt 2 = mode high byte (ignored), byte_cnt 3
    // = mode low byte (bit 0 = mode select, see microham.txt).
    reg [7:0] palette_fg;
    reg [7:0] palette_bg;
    reg       mode_ham;       // header word 1 bit 0: 0 = direct 1bpp, 1 = micro-HAM

    // Next line will be active: v_cnt 0..478 -> lines 1..479; v_cnt 524 -> line 0
    wire next_line_active = (v_cnt < V_ACTIVE - 10'd1) | (v_cnt == V_TOTAL - 10'd1);

    // Preload the line's first FIFO byte 34 px before line end (was 2 px for
    // the pixel-only design).  The extra 32 px = the 4 header bytes at 8 px
    // each, read during hblank, so the first PIXEL byte still loads at h 799
    // and drives h 0 exactly as before.
    //
    // preload is registered (computed one cycle early, at h 765) so the
    // FIFO block's ~13 registers see a single-literal enable instead of
    // each replicating the h_cnt compare and next_line_active product
    // terms — that duplication alone overflowed the CPLD.
    reg preload;

    always @(posedge PIXEL_CLK or posedge RESET)
    begin
        if (RESET)
        begin
            preload <= 1'b0;
        end
        else
        begin
            preload <= (h_cnt == H_TOTAL - 10'd35) & next_line_active & frame_active;
        end
    end

    // Sticky flag: a preload has occurred since reset, so the pixel
    // shifter is (or has been) running.  See the shift condition below.
    reg line_run;

    always @(posedge PIXEL_CLK or posedge RESET)
    begin
        if (RESET)
        begin
            line_run <= 1'b0;
        end
        else
        begin
            line_run <= line_run | preload;
        end
    end

    always @(posedge PIXEL_CLK or posedge RESET)
    begin
        if (RESET)
        begin
            shift_reg      <= 8'd0;
            byte_cnt       <= 7'd0;
            nFIFO_RE_EVEN  <= 1'b1;
            nFIFO_RE_ODD   <= 1'b1;
            fifo_loading   <= 1'b0;
            fifo_select    <= 1'b0;
            saved_9th_bit  <= 1'b1;
            fifo_err_tog   <= 1'b0;
            palette_fg     <= 8'hFF;
            palette_bg     <= 8'h00;
            mode_ham       <= 1'b0;
        end
        else if (preload)
        begin
            nFIFO_RE_EVEN  <= 1'b0;
            nFIFO_RE_ODD   <= 1'b1;
            fifo_loading   <= 1'b1;
            fifo_select    <= 1'b0;
            byte_cnt       <= 7'd0;
        end
        else if (fifo_loading)
        begin
            shift_reg      <= FIFO_Q;
            nFIFO_RE_EVEN  <= 1'b1;
            nFIFO_RE_ODD   <= 1'b1;
            fifo_loading   <= 1'b0;
            byte_cnt       <= byte_cnt + 7'd1;
            fifo_select    <= ~fifo_select;
            if (~fifo_select & (FIFO_Q8 == saved_9th_bit))
            begin
                fifo_err_tog <= ~fifo_err_tog;
            end
            if (~fifo_select)
            begin
                saved_9th_bit <= FIFO_Q8;
            end
            // Latch the in-band palette from the header word (bytes 0,1).
            if (byte_cnt == 7'd0)
            begin
                palette_fg <= FIFO_Q;
            end
            if (byte_cnt == 7'd1)
            begin
                palette_bg <= FIFO_Q;
            end
            if (byte_cnt == 7'd3)
            begin
                mode_ham <= FIFO_Q[0];
            end
        end
        // The original shift condition 0 < byte_cnt <= 84 is true from
        // the first preload after reset onward (byte_cnt only returns
        // to 0 at a preload, with fifo_loading set), so the single
        // sticky line_run literal replaces a 7-product byte_cnt != 0
        // guard that was replicated into every shift-register bit.
        // Likewise byte_cnt < 84 reduces to
        // ~&{byte_cnt[6],byte_cnt[4],byte_cnt[2]} (84 = 7'b1010100; no
        // smaller value has all three bits set).  ABC can't derive
        // these range invariants, so the cheap forms are spelled out —
        // the CPLD can't spare the product terms.
        else if (line_run)
        begin
            shift_reg <= {shift_reg[6:0], 1'b0};
            // Bit position within the byte is h_cnt[2:0]: the preload
            // at h 765/766 puts byte boundaries at h == 0 (mod 8), and
            // H_TOTAL (800) is a multiple of 8, so a separate bit_cnt
            // register is redundant.
            if (h_cnt[2:0] == 3'd6 & ~(byte_cnt[6] & byte_cnt[4] & byte_cnt[2]))
            begin
                nFIFO_RE_EVEN <= fifo_select;
                nFIFO_RE_ODD  <= ~fifo_select;
                fifo_loading  <= 1'b1;
            end
        end
        else
        begin
            nFIFO_RE_EVEN <= 1'b1;
            nFIFO_RE_ODD  <= 1'b1;
            shift_reg      <= 8'd0;
        end
    end

    // ----------------------------------------------------------------
    // VGA color output (PIXEL_CLK domain)
    //
    // Pipeline current_pixel and active_video through one FF stage;
    // the micro-HAM decoder below turns those into the ham_held color
    // register (both modes), and each VGA output FF reads a single
    // ham_held bit — fan-in 1, far under the ATF1508's 40-signal
    // per-LAB limit.
    // ----------------------------------------------------------------

    wire current_pixel = shift_reg[7];

    reg current_pixel_reg;
    reg active_video_reg;

    always @(posedge PIXEL_CLK or posedge RESET)
    begin
        if (RESET)
        begin
            current_pixel_reg <= 1'b0;
            active_video_reg  <= 1'b0;
        end
        else
        begin
            current_pixel_reg <= current_pixel;
            active_video_reg  <= active_video & frame_active;
        end
    end

    // ----------------------------------------------------------------
    // Micro-HAM decoder (mode 1, see microham.txt) — PIXEL_CLK domain
    //
    // Consumes current_pixel_reg one bit per pixel clock during active
    // video (same pipeline stage as the mode-0 display path, so mode 0
    // is untouched).  Bits parse as 2- or 4-bit codes that modify an
    // 8-bit held R3G3B2 color:
    //   0_p    : held <- palette entry p (1 = fg, 0 = bg)
    //   10_g_r : held green <- ggg, held red <- rrr
    //   11_g_b : held green <- ggg, held blue <- bb
    // Serial decode with no lookahead: the updated held color appears
    // on the pixel after its code's last bit; pixels within a code
    // show the previous held color.  Between lines the state resets
    // and held tracks palette_fg, so each line starts with held = fg
    // (no state carries across lines).
    //
    // ham_held doubles as the display color register for BOTH modes:
    // in mode 0 the ham_lp enable is forced on every cycle, so held
    // loads (px ? fg : bg) each pixel — direct 1bpp is just a HAM
    // "0_p" load every pixel.  The VGA output FFs then read only
    // ham_held (fan-in 1 per bit), and palette fg/bg fan into a
    // single LAB instead of three.  This adds one pipeline stage in
    // both modes (whole image shifts 1 px within the porches — sync
    // is unaffected) and the blanking-region color becomes fg rather
    // than bg (held tracks fg between lines, per the spec's
    // held <- fg line init).
    //
    // The parse state is one-hot registered write-enables (ham_lp,
    // ham_t, ham_g, ham_rb_r, ham_rb_b) rather than a binary state
    // register: each ham_held bit then sees only single-literal
    // enables, so its hold term is one product and the whole bit fits
    // in a macrocell's 5 product terms with no foldback expanders and
    // no mode_ham literal in the datapath.  (A binary-coded state
    // register overflowed the held bits' product terms into ~32 extra
    // foldbacks; a separate held register + output mux blew LAB
    // fan-in; both failed grouping.)
    // ----------------------------------------------------------------

    reg [7:0] ham_held;
    reg       ham_lp;     // load palette color: every cycle in mode 0, or p of 0_p
    reg       ham_t;      // this cycle's bit is the second bit of a 1x code
    reg       ham_g;      // this cycle's bit is g of 10_g_r / 11_g_b
    reg       ham_rb_r;   // this cycle's bit is r of 10_g_r
    reg       ham_rb_b;   // this cycle's bit is b of 11_g_b
    reg       ham_type;   // latched second code bit: 0 = 10_g_r, 1 = 11_g_b

    wire act = active_video_reg;
    wire px  = current_pixel_reg;

    // In mode 0, ham_lp is constantly 1, so ham_first is constantly 0
    // and the ham_t/g/rb enables can never fire — the parser is inert.
    wire ham_first = ~ham_lp & ~ham_t & ~ham_g & ~ham_rb_r & ~ham_rb_b;

    always @(posedge PIXEL_CLK or posedge RESET)
    begin
        if (RESET)
        begin
            ham_lp   <= 1'b0;
            ham_t    <= 1'b0;
            ham_g    <= 1'b0;
            ham_rb_r <= 1'b0;
            ham_rb_b <= 1'b0;
            ham_type <= 1'b0;
        end
        else
        begin
            ham_lp   <= ~mode_ham | (act & ham_first & ~px);
            ham_t    <= act & ham_first & px;
            ham_g    <= act & ham_t;
            ham_rb_r <= act & ham_g & ~ham_type;   // ham_type latched entering ham_g
            ham_rb_b <= act & ham_g & ham_type;
            ham_type <= ham_t ? px : ham_type;
        end
    end

    always @(posedge PIXEL_CLK or posedge RESET)
    begin
        if (RESET)
        begin
            ham_held <= 8'd0;
        end
        else
        begin
            ham_held[7:5] <= ~act     ? palette_fg[7:5] :
                             ham_lp   ? (px ? palette_fg[7:5] : palette_bg[7:5]) :
                             ham_rb_r ? {3{px}} :
                                        ham_held[7:5];
            ham_held[4:2] <= ~act     ? palette_fg[4:2] :
                             ham_lp   ? (px ? palette_fg[4:2] : palette_bg[4:2]) :
                             ham_g    ? {3{px}} :
                                        ham_held[4:2];
            ham_held[1:0] <= ~act     ? palette_fg[1:0] :
                             ham_lp   ? (px ? palette_fg[1:0] : palette_bg[1:0]) :
                             ham_rb_b ? {2{px}} :
                                        ham_held[1:0];
        end
    end

    always @(posedge PIXEL_CLK or posedge RESET)
    begin
        if (RESET)
        begin
            VGA_R0 <= 1'b0;
            VGA_R1 <= 1'b0;
            VGA_R2 <= 1'b0;
            VGA_G0 <= 1'b0;
            VGA_G1 <= 1'b0;
            VGA_G2 <= 1'b0;
            VGA_B0 <= 1'b0;
            VGA_B1 <= 1'b0;
        end
        else
        begin
            VGA_R2 <= ham_held[7];
            VGA_R1 <= ham_held[6];
            VGA_R0 <= ham_held[5];
            VGA_G2 <= ham_held[4];
            VGA_G1 <= ham_held[3];
            VGA_G0 <= ham_held[2];
            VGA_B1 <= ham_held[1];
            VGA_B0 <= ham_held[0];
        end
    end

endmodule

// VIDEO ATF1508 — Griffin board Rev 1
// Pin assignments for atf15xx_yosys / fit1508.exe, PLCC-84 package
//
// Format: grep '//PIN:' video.v | cut -d' ' -f2-  -> video.pin
//   Bus elements use underscore notation: D_0, A_18, FC_0
//   JTAG pins (TDI:14, TMS:23, TCK:62, TDO:71) are dedicated; no PIN entry needed
//
//PIN: CHIP "video" ASSIGNED TO AN PLCC84
//
// Clocks and reset
//PIN: SYSCLK         : 83
//PIN: PIXEL_CLK      : 2
//PIN: nRESET         : 1
//
// Bus interface
//PIN: nVIDEO_SELECT  : 84
//PIN: nAS            : 21
//PIN: nUDS           : 22
//PIN: nLDS           : 64
//PIN: R_nW           : 25
//PIN: A_4            : 55
//PIN: A_3            : 54
//PIN: A_2            : 51
//PIN: A_1            : 49
//PIN: A_0            : 48
//PIN: D_15           : 27
//PIN: D_14           : 63
//PIN: D_13           : 24
//PIN: D_12           : 65
//PIN: D_11           : 20
//PIN: D_10           : 67
//PIN: D_9            : 69
//PIN: D_8            : 16
//PIN: D_7            : 70
//PIN: D_6            : 73
//PIN: D_5            : 11
//PIN: D_4            : 12
//PIN: D_3            : 15
//PIN: D_2            : 17
//PIN: D_1            : 68
//PIN: D_0            : 18
//PIN: FC_2           : 35
//PIN: FC_1           : 34
//PIN: FC_0           : 33
//
// Pixel oscillator enables (Y1 14.318 MHz NTSC, Y3 25.175 MHz VGA)
//PIN: CPST_CLK_ENB   : 40
//PIN: VGA_CLK_ENB    : 41
//
// VGA outputs
//PIN: VGA_HSYNC      : 46
//PIN: VGA_VSYNC      : 29
//PIN: VGA_R0         : 4
//PIN: VGA_R1         : 8
//PIN: VGA_R2         : 6
//PIN: VGA_G0         : 76
//PIN: VGA_G1         : 77
//PIN: VGA_G2         : 81
//PIN: VGA_B0         : 74
//PIN: VGA_B1         : 75
//
// Control outputs
//PIN: VIDEO_STALL    : 79
//PIN: nVIDEO_IRQ     : 5
//
// 7200 FIFO read interface (bodge wires to breadboard)
//   Q[8:0] shared between EVEN and ODD FIFOs
//PIN: FIFO_Q_0       : 36
//PIN: FIFO_Q_1       : 31
//PIN: FIFO_Q_2       : 30
//PIN: FIFO_Q_3       : 28
//PIN: FIFO_Q_4       : 37
//PIN: FIFO_Q_5       : 39
//PIN: FIFO_Q_6       : 44
//PIN: FIFO_Q_7       : 9
//PIN: FIFO_Q8        : 45
//PIN: nFIFO_RE_EVEN  : 50
//PIN: nFIFO_RE_ODD   : 52
