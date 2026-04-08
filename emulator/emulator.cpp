#include <array>
#include <chrono>
#include <cstdint>
#include <cinttypes>
#include <thread>

#include "../griffin.generated.h"

// pty.h / util.h pull in termios.h which #defines EXTB, colliding with
// a Moira enum member.  Include Moira first, then the PTY header.
#include "Moira.h"
#include <SDL3/SDL.h>

#include <fcntl.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <unistd.h>
#ifdef __APPLE__
#include <util.h>
#else
#include <pty.h>
#endif

constexpr uint32_t DEBUG_BUS = 0x0001;
constexpr uint32_t DEBUG_IO = 0x0002;
constexpr uint32_t DEBUG_UART = 0x0004;
constexpr uint32_t DEBUG_DISASSEMBLE = 0x0008;
constexpr uint32_t DEBUG_DEBUG_BIT = 0x0020;
constexpr uint32_t DEBUG_CF = 0x0040;
constexpr uint32_t DEBUG_VIDEO = 0x0080;
constexpr uint32_t debug = 0; // DEBUG_BUS | DEBUG_IO | DEBUG_UART;

using namespace Griffin;

// PTY-based console for serial emulation.
// The master fd acts like the UART: write() sends to the terminal,
// read() receives keystrokes.  Works on macOS and Linux.
struct PTYConsole
{
    int master_fd = -1;

    bool open()
    {
        int slave_fd;
        if(openpty(&master_fd, &slave_fd, NULL, NULL, NULL) < 0)
        {
            perror("openpty");
            return false;
        }
        fprintf(stderr, "Console PTY: %s\n", ttyname(slave_fd));
        close(slave_fd);
        fcntl(master_fd, F_SETFL, O_NONBLOCK);
        return true;
    }

    ~PTYConsole()
    {
        if(master_fd >= 0)
        {
            close(master_fd);
        }
    }

    bool is_data_ready() const
    {
        if(master_fd < 0)
        {
            return false;
        }
        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(master_fd, &fds);
        struct timeval tv = {0, 0};
        int result = select(master_fd + 1, &fds, NULL, NULL, &tv);
        if(result < 0)
        {
            if(errno == EINTR)
            {
                return false;
            }
            perror("select");
            return false;
        }
        return result > 0 && FD_ISSET(master_fd, &fds);
    }

    // Read one byte.  Returns true if a byte was read.
    bool receive(uint8_t *out) const
    {
        if(master_fd < 0)
        {
            return false;
        }
        ssize_t n = read(master_fd, out, 1);
        return n == 1;
    }

    void send(uint8_t ch) const
    {
        if(master_fd >= 0)
        {
            write(master_fd, &ch, 1);
        }
    }
};

// ---------------------------------------------------------------------------
// Compact Flash emulation — True IDE 8-bit PIO register model
// ---------------------------------------------------------------------------

struct CFState
{
    int fd = -1;
    bool read_only = false;
    uint64_t file_size = 0;

    uint8_t error = 0;
    uint8_t features = 0;
    uint8_t sector_count = 0;
    uint8_t sector_num = 0;     // LBA 7:0
    uint8_t cyl_lo = 0;         // LBA 15:8
    uint8_t cyl_hi = 0;         // LBA 23:16
    uint8_t drive_head = 0;     // LBA 27:24 + flags
    uint8_t status = 0;
    uint8_t command = 0;

    uint8_t data_buf[512];
    int data_idx = 0;
    int data_len = 0;
    int sectors_remaining = 0;
    bool is_write = false;

    uint8_t identify_buf[512];

    bool is_present() const { return fd >= 0; }

    // Store an ATA string into the identify buffer at the given word offset.
    // ATA strings have the first char of each word in the high byte.
    // In 8-bit PIO mode the low byte is read first, so we store:
    //   buf[word*2]   = second char (low byte)
    //   buf[word*2+1] = first char  (high byte)
    static void set_ata_string(uint8_t *buf, int word_start, int word_count, const char *str)
    {
        int len = word_count * 2;
        int slen = strlen(str);
        for (int i = 0; i < len; i += 2)
        {
            char c0 = (i < slen) ? str[i] : ' ';
            char c1 = (i + 1 < slen) ? str[i + 1] : ' ';
            buf[word_start * 2 + i]     = c1;  // low byte = second char
            buf[word_start * 2 + i + 1] = c0;  // high byte = first char
        }
    }

    void build_identify()
    {
        memset(identify_buf, 0, 512);
        uint32_t sectors = file_size / 512;

        // Word 0: general config — CF flag + non-removable
        identify_buf[0] = 0x8A;
        identify_buf[1] = 0x84;

        // Word 1: default cylinders
        uint16_t cyls = (sectors > 16 * 63) ? (sectors / (16 * 63)) : 1;
        if (cyls > 16383)
        {
            cyls = 16383;
        }
        identify_buf[2] = cyls & 0xFF;
        identify_buf[3] = (cyls >> 8) & 0xFF;

        // Word 3: default heads
        identify_buf[6] = 16;
        identify_buf[7] = 0;

        // Word 6: sectors per track
        identify_buf[12] = 63;
        identify_buf[13] = 0;

        // Words 7-8: number of sectors in card (CHS compat)
        uint32_t chs_sectors = (uint32_t)cyls * 16 * 63;
        if (chs_sectors > sectors)
        {
            chs_sectors = sectors;
        }
        identify_buf[14] = chs_sectors & 0xFF;
        identify_buf[15] = (chs_sectors >> 8) & 0xFF;
        identify_buf[16] = (chs_sectors >> 16) & 0xFF;
        identify_buf[17] = (chs_sectors >> 24) & 0xFF;

        // Words 10-19: serial number
        set_ata_string(identify_buf, 10, 10, "GRIFFIN00001");

        // Words 23-26: firmware revision
        set_ata_string(identify_buf, 23, 4, "EMU 1.0");

        // Words 27-46: model string
        set_ata_string(identify_buf, 27, 20, "GRIFFIN CF EMULATOR");

        // Word 49: capabilities — LBA supported (bit 9)
        identify_buf[98] = 0x00;
        identify_buf[99] = 0x02;  // bit 9 = LBA

        // Words 60-61: total addressable LBA sectors
        identify_buf[120] = sectors & 0xFF;
        identify_buf[121] = (sectors >> 8) & 0xFF;
        identify_buf[122] = (sectors >> 16) & 0xFF;
        identify_buf[123] = (sectors >> 24) & 0xFF;
    }

    bool open(const char *path, bool ro)
    {
        read_only = ro;
        fd = ::open(path, ro ? O_RDONLY : O_RDWR);
        if (fd < 0)
        {
            perror(path);
            return false;
        }
        struct stat st;
        if (fstat(fd, &st) < 0)
        {
            perror("fstat");
            ::close(fd);
            fd = -1;
            return false;
        }
        file_size = st.st_size;
        if (file_size < 512)
        {
            fprintf(stderr, "CF image too small (%llu bytes, need at least 512)\n",
                    (unsigned long long)file_size);
            ::close(fd);
            fd = -1;
            return false;
        }
        status = CF_STATUS_DRDY;
        build_identify();
        uint32_t sectors = file_size / 512;
        fprintf(stderr, "CF: %s (%u sectors, %llu bytes%s)\n",
                path, sectors, (unsigned long long)file_size,
                ro ? ", read-only" : "");
        return true;
    }

    ~CFState()
    {
        if (fd >= 0)
        {
            ::close(fd);
        }
    }

    uint32_t lba() const
    {
        return (uint32_t)sector_num
             | ((uint32_t)cyl_lo << 8)
             | ((uint32_t)cyl_hi << 16)
             | ((uint32_t)(drive_head & 0x0F) << 24);
    }

    void load_sector()
    {
        uint32_t addr = lba();
        if (sectors_remaining > 0 && data_idx >= data_len)
        {
            // Advance LBA for multi-sector reads
            addr = lba();
        }
        off_t offset = (off_t)addr * 512;
        if (offset + 512 > (off_t)file_size)
        {
            // Beyond end of file — return zeros
            memset(data_buf, 0, 512);
        }
        else
        {
            lseek(fd, offset, SEEK_SET);
            ssize_t n = ::read(fd, data_buf, 512);
            if (n < 512)
            {
                memset(data_buf + (n > 0 ? n : 0), 0, 512 - (n > 0 ? n : 0));
            }
        }
        data_idx = 0;
        data_len = 512;
    }

    void flush_sector()
    {
        if (read_only)
        {
            error = 0x04;  // ABRT
            status = CF_STATUS_DRDY | CF_STATUS_ERR;
            return;
        }
        uint32_t addr = lba();
        off_t offset = (off_t)addr * 512;
        if (offset + 512 <= (off_t)file_size)
        {
            lseek(fd, offset, SEEK_SET);
            ::write(fd, data_buf, 512);
        }
    }

    void advance_lba()
    {
        // Increment the LBA stored across the registers
        uint32_t a = lba() + 1;
        sector_num = a & 0xFF;
        cyl_lo = (a >> 8) & 0xFF;
        cyl_hi = (a >> 16) & 0xFF;
        drive_head = (drive_head & 0xF0) | ((a >> 24) & 0x0F);
    }

    void execute_command(uint8_t cmd)
    {
        command = cmd;
        switch (cmd)
        {
            case CF_CMD_IDENTIFY:
                memcpy(data_buf, identify_buf, 512);
                data_idx = 0;
                data_len = 512;
                status = CF_STATUS_DRDY | CF_STATUS_DRQ;
                error = 0;
                if (debug & DEBUG_CF)
                {
                    printf("[CF: IDENTIFY]\n");
                }
                break;

            case CF_CMD_READ_SECTORS:
                sectors_remaining = sector_count == 0 ? 256 : sector_count;
                is_write = false;
                load_sector();
                sectors_remaining--;
                status = CF_STATUS_DRDY | CF_STATUS_DRQ;
                error = 0;
                if (debug & DEBUG_CF)
                {
                    printf("[CF: READ %d sector(s) at LBA %u]\n",
                           sector_count == 0 ? 256 : sector_count, lba());
                }
                break;

            case CF_CMD_WRITE_SECTORS:
                sectors_remaining = sector_count == 0 ? 256 : sector_count;
                is_write = true;
                data_idx = 0;
                data_len = 512;
                status = CF_STATUS_DRDY | CF_STATUS_DRQ;
                error = 0;
                if (debug & DEBUG_CF)
                {
                    printf("[CF: WRITE %d sector(s) at LBA %u]\n",
                           sector_count == 0 ? 256 : sector_count, lba());
                }
                break;

            case CF_CMD_SET_FEATURES:
                // Acknowledge; 8-bit mode is implicit in emulation
                status = CF_STATUS_DRDY;
                error = 0;
                if (debug & DEBUG_CF)
                {
                    printf("[CF: SET FEATURES 0x%02X]\n", features);
                }
                break;

            default:
                // Unknown command — set ABRT error
                error = 0x04;
                status = CF_STATUS_DRDY | CF_STATUS_ERR;
                if (debug & DEBUG_CF)
                {
                    printf("[CF: unknown command 0x%02X]\n", cmd);
                }
                break;
        }
    }

    uint8_t read_reg(uint32_t abs_addr)
    {
        if (!is_present())
        {
            return 0xFF;  // no device
        }

        if (abs_addr == CF_DATA)
        {
            if (!(status & CF_STATUS_DRQ) || is_write)
            {
                return 0xFF;
            }
            uint8_t val = data_buf[data_idx++];
            if (data_idx >= data_len)
            {
                if (sectors_remaining > 0)
                {
                    // Multi-sector: advance and load next
                    advance_lba();
                    load_sector();
                    sectors_remaining--;
                }
                else
                {
                    // Transfer complete
                    status = CF_STATUS_DRDY;
                }
            }
            return val;
        }
        else if (abs_addr == CF_ERROR)
        {
            return error;
        }
        else if (abs_addr == CF_SECTOR_COUNT)
        {
            return sector_count;
        }
        else if (abs_addr == CF_SECTOR_NUM)
        {
            return sector_num;
        }
        else if (abs_addr == CF_CYL_LO)
        {
            return cyl_lo;
        }
        else if (abs_addr == CF_CYL_HI)
        {
            return cyl_hi;
        }
        else if (abs_addr == CF_DRIVE_HEAD)
        {
            return drive_head;
        }
        else if (abs_addr == CF_STATUS)
        {
            return status;
        }
        return 0xFF;
    }

    void write_reg(uint32_t abs_addr, uint8_t val)
    {
        if (!is_present())
        {
            return;
        }

        if (abs_addr == CF_DATA)
        {
            if (!(status & CF_STATUS_DRQ) || !is_write)
            {
                return;
            }
            data_buf[data_idx++] = val;
            if (data_idx >= data_len)
            {
                // Sector buffer full — write it out
                flush_sector();
                sectors_remaining--;
                if (sectors_remaining > 0)
                {
                    advance_lba();
                    data_idx = 0;
                    data_len = 512;
                    status = CF_STATUS_DRDY | CF_STATUS_DRQ;
                }
                else
                {
                    status = CF_STATUS_DRDY;
                }
            }
        }
        else if (abs_addr == CF_FEATURES)
        {
            features = val;
        }
        else if (abs_addr == CF_SECTOR_COUNT)
        {
            sector_count = val;
        }
        else if (abs_addr == CF_SECTOR_NUM)
        {
            sector_num = val;
        }
        else if (abs_addr == CF_CYL_LO)
        {
            cyl_lo = val;
        }
        else if (abs_addr == CF_CYL_HI)
        {
            cyl_hi = val;
        }
        else if (abs_addr == CF_DRIVE_HEAD)
        {
            drive_head = val;
        }
        else if (abs_addr == CF_COMMAND)
        {
            execute_command(val);
        }
    }
};

// ---------------------------------------------------------------------------
// GLUE timer emulation — ÷8 prescaler + 5-bit auto-reload counter
//
// On real hardware, arming the timer blocks ALL DTACK until the next
// zero-crossing, freezing the CPU.  Moira executes whole instructions,
// so we approximate by advancing the clock (sync) at the ARM write.
// The total cycle count between I/O operations matches hardware.
// ---------------------------------------------------------------------------

struct TimerState
{
    uint8_t period = 0;       // 8-bit register value (0 = stopped)
    uint64_t start_clock = 0; // SYSCLK when timer was last loaded

    bool running() const { return period != 0; }
    // Hardware counts N+1 states (N down to 0), so effective period = (N+1) SYSCLK
    uint32_t period_clocks() const { return static_cast<uint32_t>(period) + 1; }

    // Cycles from 'now' until the next zero-crossing.
    // Returns 0 if stopped or exactly on a zero-crossing.
    uint32_t cycles_to_zero(uint64_t now) const
    {
        if (!running())
        {
            return 0;
        }
        uint32_t pc = period_clocks();
        uint64_t elapsed = (now - start_clock) % pc;
        if (elapsed == 0)
        {
            return 0;
        }
        return pc - static_cast<uint32_t>(elapsed);
    }
};

// ---------------------------------------------------------------------------
// Systick timer — fixed rate ~183 Hz = SYSCLK / 65536.
// The pending flag sets every 65536 SYSCLK cycles.  Reading
// SYSTICK_STATUS clears pending and deasserts the IRQ.
// SYSTICK_IRQ_ENABLE in GLUE_CONFIG gates the IRQ output but the
// timer always runs and the pending flag always sets.
// ---------------------------------------------------------------------------

struct SystickState
{
    static constexpr uint64_t PERIOD = 65536;  // SYSCLK / 65536 ≈ 183 Hz
    bool pending = false;
    bool irq_enabled = false;

    // Called once per PERIOD clocks.
    void tick()
    {
        pending = true;
    }

    uint8_t read_status()
    {
        uint8_t val = pending ? GLUE_SYSTICK_STATUS_PENDING_MASK : 0;
        pending = false;
        return val;
    }
};

// ---------------------------------------------------------------------------
// SoftUART TX synthesizer — generates an 8N1 bitstream on DEBUG_IN from
// bytes received on the PTY.  The emulator's firmware bit-bang RX routine
// polls DEBUG_IN, so we need to present a properly-timed serial waveform.
// ---------------------------------------------------------------------------

struct SoftUARTTX
{
    static constexpr int BAUD = 115200;
    static constexpr int CLOCKS_PER_BIT = SYSCLK_HZ / BAUD;  // 104

    enum State { IDLE, SENDING };
    State state = IDLE;
    uint16_t shift_reg = 0;   // 10-bit frame: start(0) + 8 data + stop(1)
    int bits_remaining = 0;
    uint64_t next_bit_clock = 0;

    // Queue of bytes waiting to be transmitted
    static constexpr size_t QUEUE_SIZE = 256;
    uint8_t queue[QUEUE_SIZE];
    size_t head = 0;
    size_t count = 0;

    int current_bit = 1;  // idle = HIGH (mark)

    void enqueue(uint8_t byte)
    {
        if (count >= QUEUE_SIZE)
        {
            return;  // drop on overflow
        }
        queue[(head + count) % QUEUE_SIZE] = byte;
        count++;
    }

    // Advance the bitstream to the given clock.  Call before reading current_bit.
    void advance(uint64_t clock_now)
    {
        while (true)
        {
            if (state == IDLE)
            {
                if (count == 0)
                {
                    current_bit = 1;  // idle HIGH
                    return;
                }
                // Start sending next byte
                uint8_t byte = queue[head % QUEUE_SIZE];
                head = (head + 1) % QUEUE_SIZE;
                count--;
                // Build 10-bit frame: start bit (0), 8 data bits LSB first, stop bit (1)
                shift_reg = (1 << 9) | (static_cast<uint16_t>(byte) << 1) | 0;
                bits_remaining = 10;
                state = SENDING;
                next_bit_clock = clock_now;
            }

            if (state == SENDING)
            {
                if (clock_now < next_bit_clock)
                {
                    return;  // not time for next bit yet
                }
                // Shift out current bit
                current_bit = shift_reg & 1;
                shift_reg >>= 1;
                bits_remaining--;
                next_bit_clock += CLOCKS_PER_BIT;
                if (bits_remaining == 0)
                {
                    state = IDLE;
                    // Continue loop to check for queued bytes
                }
            }
        }
    }
};

// ---------------------------------------------------------------------------
// Real-time clock governor
//
// Paces emulation to wall-clock speed.  Called every THROTTLE_INTERVAL
// instructions; sleeps if the emulator is running ahead of real time.
// ---------------------------------------------------------------------------

static constexpr int THROTTLE_INTERVAL = 1000;

struct ClockGovernor
{
    uint64_t base_clock = 0;
    double clock_hz = SYSCLK_HZ;
    std::chrono::steady_clock::time_point base_time;

    void reset(uint64_t emu_clock, uint32_t hz)
    {
        base_clock = emu_clock;
        clock_hz = hz;
        base_time = std::chrono::steady_clock::now();
    }

    void throttle(uint64_t emu_clock)
    {
        using namespace std::chrono;
        double emu_seconds = static_cast<double>(emu_clock - base_clock) / clock_hz;
        duration<double> wall_elapsed = steady_clock::now() - base_time;
        double ahead = emu_seconds - wall_elapsed.count();
        if (ahead > 0.001)
        {
            std::this_thread::sleep_for(duration<double>(ahead));
        }
    }
};

// ---------------------------------------------------------------------------
// Video display — tracks VIDEO/ENGINE timing, renders framebuffer to SDL3
//
// Derives a line counter from SYSCLK via the 25.175 MHz pixel clock ratio.
// Each time a line boundary is crossed, the just-completed visible line's
// framebuffer data is expanded from 1bpp to ARGB8888 in a staging buffer.
// At frame end the staging buffer is presented to the SDL window.
// ---------------------------------------------------------------------------

struct VideoDisplay
{
    // VGA 640x480@60 progressive timing (from video.v)
    static constexpr uint32_t PIXEL_CLK_HZ = 25175000;
    static constexpr uint32_t H_TOTAL = 800;
    static constexpr uint32_t V_TOTAL = 525;
    static constexpr uint32_t V_ACTIVE = 480;
    static constexpr uint32_t V_SYNC_START = 490;
    static constexpr uint32_t V_SYNC_END = 492;
    static constexpr uint32_t WORDS_PER_LINE = 40;
    static constexpr uint32_t WIDTH = 640;
    static constexpr uint32_t HEIGHT = 480;

    // Fixed-point pixel/SYSCLK ratio (32.16): pixels per SYSCLK cycle
    static constexpr uint32_t PIXEL_RATIO_FP =
        ((uint64_t)PIXEL_CLK_HZ << 16) / SYSCLK_HZ;

    // Timing state
    uint64_t prev_sysclk = 0;
    uint64_t pixel_accum_fp = 0;  // 48.16 fixed-point absolute pixel count
    uint64_t abs_line = 0;        // monotonically increasing line counter
    bool irq_active = false;

    // ENGINE DMA mirror state
    bool dma_enabled = false;
    uint8_t fb_base = 0;         // A[21:14] of framebuffer base
    uint16_t stride_field = 0;   // 2-bit stride / 64
    uint16_t fb_ptr = 0;         // 15-bit word offset, mirrors engine.v

    // Staging buffer (640x480 ARGB8888)
    uint32_t staging[HEIGHT][WIDTH];

    // SDL handles
    SDL_Window *window = nullptr;
    SDL_Renderer *renderer = nullptr;
    SDL_Texture *texture = nullptr;

    bool init()
    {
        window = SDL_CreateWindow("Griffin", WIDTH, HEIGHT, 0);
        if (!window)
        {
            fprintf(stderr, "SDL_CreateWindow: %s\n", SDL_GetError());
            return false;
        }
        renderer = SDL_CreateRenderer(window, nullptr);
        if (!renderer)
        {
            fprintf(stderr, "SDL_CreateRenderer: %s\n", SDL_GetError());
            return false;
        }
        SDL_SetRenderVSync(renderer, 0);
        texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ARGB8888,
                                    SDL_TEXTUREACCESS_STREAMING, WIDTH, HEIGHT);
        if (!texture)
        {
            fprintf(stderr, "SDL_CreateTexture: %s\n", SDL_GetError());
            return false;
        }
        SDL_SetTextureScaleMode(texture, SDL_SCALEMODE_NEAREST);
        memset(staging, 0, sizeof(staging));
        return true;
    }

    void shutdown()
    {
        if (texture)
        {
            SDL_DestroyTexture(texture);
            texture = nullptr;
        }
        if (renderer)
        {
            SDL_DestroyRenderer(renderer);
            renderer = nullptr;
        }
        if (window)
        {
            SDL_DestroyWindow(window);
            window = nullptr;
        }
    }

    // Compute RAM byte address from ENGINE fb_base + fb_ptr
    // Matches engine.v: dma_addr = {2'b00, fb_base + fb_ptr[14:13], fb_ptr[12:0]}
    static uint32_t fb_byte_addr(uint8_t base, uint16_t ptr)
    {
        uint32_t hi = ((uint32_t)base + (ptr >> 13)) & 0xFF;
        uint32_t lo = ptr & 0x1FFF;
        return (hi << 14) | (lo << 1);
    }

    // EOL advance — matches engine.v row_advanced wire
    void eol_advance()
    {
        uint16_t row_part = (fb_ptr >> 6) + stride_field;
        fb_ptr = (row_part << 6) & 0x7FFF;
    }

    void present()
    {
        if (!texture)
        {
            return;
        }
        void *pixels;
        int pitch;
        if (SDL_LockTexture(texture, nullptr, &pixels, &pitch))
        {
            for (uint32_t y = 0; y < HEIGHT; y++)
            {
                memcpy((uint8_t *)pixels + y * pitch, staging[y], WIDTH * 4);
            }
            SDL_UnlockTexture(texture);
        }
        SDL_RenderClear(renderer);
        SDL_RenderTexture(renderer, texture, nullptr, nullptr);
        SDL_RenderPresent(renderer);
    }
};

class GriffinEmulator : public moira::Moira
{
    mutable std::vector<uint8_t> RAM_bank1;
    mutable std::vector<uint8_t> RAM_bank2;
    mutable std::vector<uint8_t> RAM_bank3;
    mutable std::vector<uint8_t> RAM_bank4;
    mutable std::array<uint8_t, ROM_SIZE> ROM{};
    mutable int debug_out_latch = 0;
    mutable bool ROMoverlay = true;
    PTYConsole pty_console;
    mutable CFState cf;
    mutable TimerState timer;
    mutable SystickState systick;
    mutable SoftUARTTX debug_in_tx;
    mutable VideoDisplay video_display;
    mutable uint8_t dac_value = 0x0;

    // VIDEO register shadow state (minimal — just enough to not crash)
    // Reset defaults: ENTRY_0=0x00 (black), ENTRY_1=0xFF (white)
    mutable uint16_t video_palette = 0xFF00;

    // Expand an R3G3B2 palette entry to ARGB8888 (opaque).
    static uint32_t rgb332_to_argb(uint8_t c)
    {
        uint8_t r3 = (c >> 5) & 0x7;
        uint8_t g3 = (c >> 2) & 0x7;
        uint8_t b2 = c & 0x3;
        // Replicate high bits into low bits for full 0..255 range
        uint8_t r = (r3 << 5) | (r3 << 2) | (r3 >> 1);
        uint8_t g = (g3 << 5) | (g3 << 2) | (g3 >> 1);
        uint8_t b = (b2 << 6) | (b2 << 4) | (b2 << 2) | b2;
        return 0xFF000000u | (r << 16) | (g << 8) | b;
    }

    // ENGINE register shadow state (minimal — just enough to not crash)
    mutable uint16_t engine_control = 0;
    mutable uint16_t engine_fb_base = 0;
    mutable uint16_t engine_row_stride = 0;
    mutable uint16_t engine_status = 0;

    static bool is_cf_addr(uint32_t io_offset)
    {
        uint32_t abs = io_offset + IO_BASE;
        return abs >= CF_BASE && abs < CF_BASE + CF_SIZE;
    }

    uint8_t IO_read8(uint32_t addr) const
    {
        if(is_cf_addr(addr)) {
            return cf.read_reg(addr + IO_BASE);
        } else if(addr == GLUE_SYSTICK_STATUS - IO_BASE) {
            uint8_t val = systick.read_status();
            const_cast<GriffinEmulator*>(this)->update_ipl();
            return val;
        } else if(addr == GLUE_DEBUG_IN - IO_BASE) {
            // Eagerly check PTY for incoming data so the bit-bang RX
            // routine sees the start bit promptly.
            if (debug_in_tx.count == 0 && debug_in_tx.state == SoftUARTTX::IDLE)
            {
                uint8_t ch;
                if (pty_console.is_data_ready() && pty_console.receive(&ch))
                {
                    debug_in_tx.enqueue(ch);
                }
            }
            debug_in_tx.advance(getClock());
            return debug_in_tx.current_bit & GLUE_DEBUG_IN_MASK;
        }
        if(debug & DEBUG_IO)
        {
            printf("read of uint8_t at unhandled IO %06X\n", addr + IO_BASE);
        }
        return 0;
    }

    uint16_t IO_read16(uint32_t addr) const
    {
        if(is_cf_addr(addr))
        {
            printf("WARNING: 16-bit read from CF at %06X (firmware should use 8-bit only)\n", addr + IO_BASE);
            return cf.read_reg(addr + IO_BASE);
        }
        if(debug & DEBUG_IO)
        {
            printf("read of uint16_t at unhandled IO %06X\n", addr + IO_BASE);
        }
        return 0;
    }

    void IO_write8(uint32_t addr, uint8_t val) const
    {
        if(is_cf_addr(addr)) {
            cf.write_reg(addr + IO_BASE, val);
        } else if(addr == GLUE_DEBUG_OUT - IO_BASE) {
            auto oldbit = debug_out_latch & GLUE_DEBUG_OUT_MASK;
            auto bit = val & GLUE_DEBUG_OUT_MASK;
            if(bit != oldbit)
            {
                if(debug & DEBUG_DEBUG_BIT) printf("debug_out, %" PRIu64 ", %d\n", getClock(), bit);
            }
            debug_out_latch = val;
        } else if(addr == GLUE_CONFIG - IO_BASE) {
            if(val & GLUE_CONFIG_ROM_OVERLAY_DISABLE_MASK)
            {
                if(debug & DEBUG_IO) printf("ROM overlay disabled\n");
                ROMoverlay = false;
            }
            systick.irq_enabled = !!(val & GLUE_CONFIG_SYSTICK_IRQ_ENABLE_MASK);
            if (debug & DEBUG_IO)
            {
                printf("[GLUE CONFIG: 0x%02X overlay=%s systick_irq=%s]\n", val,
                       (val & GLUE_CONFIG_ROM_OVERLAY_DISABLE_MASK) ? "off" : "on",
                       systick.irq_enabled ? "on" : "off");
            }
        } else if(addr == GLUE_TIMER - IO_BASE) {
            timer.period = val;
            timer.start_clock = getClock();
            if (debug & DEBUG_IO)
            {
                printf("[TIMER: period=%u (%u clks)]\n", timer.period, timer.period_clocks());
            }
        } else if(addr == GLUE_TIMER_ARM - IO_BASE) {
            uint32_t stall = timer.cycles_to_zero(getClock());
            if (stall > 0)
            {
                const_cast<GriffinEmulator*>(this)->sync(stall);
            }
            if (debug & DEBUG_IO)
            {
                printf("[TIMER ARM: stall=%u clks]\n", stall);
            }
        } else if(addr + IO_BASE >= AUDIO_BASE && addr + IO_BASE < AUDIO_BASE + AUDIO_SIZE) {
            dac_value = val;
        } else {
            if(debug & DEBUG_IO) {
                if(isprint(val))
                {
                    printf("IO write: %06" PRIx32 " = %02x (%c)\n", addr + IO_BASE, val, val);
                }
                else
                {
                    printf("IO write: %06" PRIx32 " = %02x\n", addr + IO_BASE, val);
                }
            }
        }
    }

    void IO_write16(uint32_t addr, uint16_t val) const
    {
        if(is_cf_addr(addr))
        {
            printf("WARNING: 16-bit write 0x%04X to CF at %06X (firmware should use 8-bit only)\n", val, addr + IO_BASE);
            cf.write_reg(addr + IO_BASE, val & 0xFF);
            return;
        }
        if(debug & DEBUG_IO)
        {
            printf("write of uint16_t %04X at unhandled IO %06X\n", val, addr + IO_BASE);
        }
    }

    // --------------- VIDEO register stubs ---------------

    uint8_t VIDEO_read8(uint32_t addr) const
    {
        if(debug & DEBUG_IO)
        {
            printf("read of uint8_t at unhandled VIDEO %06X\n", addr);
        }
        return 0;
    }

    uint16_t VIDEO_read16(uint32_t addr) const
    {
        if(debug & DEBUG_IO)
        {
            printf("read of uint16_t at unhandled VIDEO %06X\n", addr);
        }
        return 0;
    }

    void VIDEO_write8(uint32_t addr, uint8_t val) const
    {
        if(debug & DEBUG_IO)
        {
            printf("write of uint8_t %02X at unhandled VIDEO %06X\n", val, addr);
        }
    }

    void VIDEO_write16(uint32_t addr, uint16_t val) const
    {
        if(addr == VIDEO_PALETTE) {
            if(debug & DEBUG_VIDEO)
            {
                printf("write of uint16_t %04X to video palette\n", val);
            }
            video_palette = val;
        } else {
            if(debug & DEBUG_IO)
            {
                printf("write of uint16_t %04X at unhandled VIDEO %06X\n", val, addr);
            }
        }
    }

    // --------------- ENGINE register stubs ---------------

    uint16_t ENGINE_read16(uint32_t addr) const
    {
        if(addr == ENGINE_STATUS) {
            return engine_status;
        }
        if(debug & DEBUG_IO)
        {
            printf("read of uint16_t at unhandled ENGINE %06X\n", addr);
        }
        return 0;
    }

    uint8_t ENGINE_read8(uint32_t addr) const
    {
        if(debug & DEBUG_IO)
        {
            printf("read of uint8_t at unhandled ENGINE %06X\n", addr);
        }
        return 0;
    }

    void ENGINE_write16(uint32_t addr, uint16_t val) const
    {
        if(addr == ENGINE_CONTROL) {
            engine_control = val;
            video_display.dma_enabled = !!(val & ENGINE_CONTROL_ENABLE_MASK);
        } else if(addr == ENGINE_FB_BASE) {
            engine_fb_base = val;
            video_display.fb_base = val & 0xFF;
        } else if(addr == ENGINE_ROW_STRIDE) {
            engine_row_stride = val;
            video_display.stride_field = val & ENGINE_ROW_STRIDE_MASK;
        } else if(addr == ENGINE_STATUS) {
            engine_status = 0;  // write clears sticky error bits
        } else if(addr == ENGINE_ADVANCE) {
            video_display.eol_advance();
        } else {
            if(debug & DEBUG_IO)
            {
                printf("write of uint16_t %04X at unhandled ENGINE %06X\n", val, addr);
            }
        }
    }

    void ENGINE_write8(uint32_t addr, uint8_t val) const
    {
        if(debug & DEBUG_IO)
        {
            printf("write of uint8_t %02X at unhandled ENGINE %06X\n", val, addr);
        }
    }

    // Wait state penalty (extra SYSCLK cycles) for a memory access,
    // derived from griffin.yml dtack entries via codegen.py.
    // Note: read16 for RAM calls read8 twice, but RAM penalty is 0
    // so double-application is harmless.
    int wait_state_penalty(uint32_t addr) const
    {
        if ((ROMoverlay && addr < ROM_SIZE) ||
            (addr >= ROM_BASE && addr < ROM_BASE + ROM_WINDOW))
        {
            return ROM_DTACK_PENALTY;
        }
        if (addr >= ENGINE_BASE && addr < ENGINE_BASE + ENGINE_SIZE)
        {
            return ENGINE_DTACK_PENALTY;
        }
        if (addr >= VIDEO_BASE && addr < VIDEO_BASE + VIDEO_SIZE)
        {
            return VIDEO_DTACK_PENALTY;
        }
        if (addr >= IO_BASE && addr < IO_BASE + IO_SIZE)
        {
            unsigned sub = (addr >> 18) & 0x3;
            if (sub == 1)       // CF: 0xF40000
            {
                return CF_DTACK_PENALTY;
            }
            if (sub == 3)       // AUDIO: 0xFC0000
            {
                return AUDIO_DTACK_PENALTY;
            }
        }
        return 0;
    }

    void apply_wait_states(uint32_t addr) const
    {
        int penalty = wait_state_penalty(addr);
        if (penalty > 0)
        {
            const_cast<GriffinEmulator*>(this)->sync(penalty);
        }
    }

public:

    uint32_t clock_hz = SYSCLK_HZ;
    ClockGovernor governor;

    enum RAMConfig {RAM_1_BANK_256K, RAM_1M, RAM_2M, RAM_3M, RAM_4M };

    uint8_t GetAudioDACValue() const
    {
        return dac_value;
    }

    GriffinEmulator(RAMConfig ram_config)
    {
        switch(ram_config)
        {
            case RAM_1_BANK_256K:
                RAM_bank1.resize(256 * 1024, 0);
                break;
            case RAM_4M:
                RAM_bank4.resize(1024 * 1024, 0);
            case RAM_3M:
                RAM_bank3.resize(1024 * 1024, 0);
            case RAM_2M:
                RAM_bank2.resize(1024 * 1024, 0);
            case RAM_1M:
                RAM_bank1.resize(1024 * 1024, 0);
                break;
        }
    }

    uint8_t read8(uint32_t addr) const override
    {
        apply_wait_states(addr);
        if(debug & DEBUG_BUS) { printf("read of uint8_t at %06X\n", addr); }
        if (ROMoverlay && (addr < ROM_SIZE)) {
            return ROM[addr];
        } else if (RAM_BANK_1.contains(addr)) {
            if(RAM_bank1.size() == 0) {
                return 0;
            } else {
                return RAM_bank1[RAM_BANK_1.offset(addr) % RAM_bank1.size()];
            }
        } else if (RAM_BANK_2.contains(addr)) {
            if(RAM_bank2.size() == 0) {
                return 0;
            } else {
                return RAM_bank2[RAM_BANK_2.offset(addr) % RAM_bank2.size()];
            }
        } else if (RAM_BANK_3.contains(addr)) {
            if(RAM_bank3.size() == 0) {
                return 0;
            } else {
                return RAM_bank3[RAM_BANK_3.offset(addr) % RAM_bank3.size()];
            }
        } else if (RAM_BANK_4.contains(addr)) {
            if(RAM_bank4.size() == 0) {
                return 0;
            } else {
                return RAM_bank4[RAM_BANK_4.offset(addr) % RAM_bank4.size()];
            }
        } else if (addr >= ROM_BASE && addr < ROM_BASE + ROM_WINDOW) {
            return ROM[(addr - ROM_BASE) % ROM_SIZE];
        } else if (addr >= ENGINE_BASE && addr < ENGINE_BASE + ENGINE_SIZE) {
            return ENGINE_read8(addr);
        } else if (addr >= VIDEO_BASE && addr < VIDEO_BASE + VIDEO_SIZE) {
            return VIDEO_read8(addr);
        } else if (addr >= IO_BASE && addr < (IO_BASE + IO_SIZE)) {
            return IO_read8(addr - IO_BASE);
        } else {
            printf("read of uint8_t at unhandled %06X\n", addr);
            abort();
        }
    }

    uint16_t read16(uint32_t addr) const override
    {
        apply_wait_states(addr);
        if(debug & DEBUG_BUS) { printf("read of uint16_t at %06X\n", addr); }
        if (ROMoverlay && (addr < ROM_SIZE)) {
            return (ROM[addr] << 8) | ROM[addr + 1];
        } else if (RAM_BANK_1.contains(addr)) {
            return (read8(addr) << 8) | read8(addr + 1);
        } else if (RAM_BANK_2.contains(addr)) {
            return (read8(addr) << 8) | read8(addr + 1);
        } else if (RAM_BANK_3.contains(addr)) {
            return (read8(addr) << 8) | read8(addr + 1);
        } else if (RAM_BANK_4.contains(addr)) {
            return (read8(addr) << 8) | read8(addr + 1);
        } else if (addr >= ROM_BASE && addr < ROM_BASE + ROM_WINDOW) {
            return (ROM[(addr - ROM_BASE) % ROM_SIZE] << 8) | ROM[(addr - ROM_BASE + 1) % ROM_SIZE];
        } else if (addr >= ENGINE_BASE && addr < ENGINE_BASE + ENGINE_SIZE) {
            return ENGINE_read16(addr);
        } else if (addr >= VIDEO_BASE && addr < VIDEO_BASE + VIDEO_SIZE) {
            return VIDEO_read16(addr);
        } else if (addr >= IO_BASE && addr < (IO_BASE + IO_SIZE)) {
            return IO_read16(addr - IO_BASE);
        } else {
            printf("read of uint16_t at unhandled %06X\n", addr);
            abort();
        }
    }

    void write8(uint32_t addr, uint8_t val) const override
    {
        apply_wait_states(addr);
        if(debug & DEBUG_BUS) { printf("write of uint8_t %02X at %06X\n", val, addr); }
        if (RAM_BANK_1.contains(addr)) {
            if(RAM_bank1.size() != 0) {
                RAM_bank1[RAM_BANK_1.offset(addr) % RAM_bank1.size()] = val;
            } 
        } else if (RAM_BANK_2.contains(addr)) {
            if(RAM_bank2.size() != 0) {
                RAM_bank2[RAM_BANK_2.offset(addr) % RAM_bank2.size()] = val;
            } 
        } else if (RAM_BANK_3.contains(addr)) {
            if(RAM_bank3.size() != 0) {
                RAM_bank3[RAM_BANK_3.offset(addr) % RAM_bank3.size()] = val;
            } 
        } else if (RAM_BANK_4.contains(addr)) {
            if(RAM_bank4.size() != 0) {
                RAM_bank4[RAM_BANK_4.offset(addr) % RAM_bank4.size()] = val;
            } 
        } else if (addr >= ROM_BASE && addr < ROM_BASE + ROM_WINDOW) {
            return;
        } else if (addr >= ENGINE_BASE && addr < ENGINE_BASE + ENGINE_SIZE) {
            ENGINE_write8(addr, val);
        } else if (addr >= VIDEO_BASE && addr < VIDEO_BASE + VIDEO_SIZE) {
            VIDEO_write8(addr, val);
        } else if (addr >= IO_BASE && addr < (IO_BASE + IO_SIZE)) {
            IO_write8(addr - IO_BASE, val);
        } else {
            printf("write of uint8_t %02X to unhandled %06X\n", val, addr);
            abort();
        }
    }

    void write16(uint32_t addr, uint16_t val) const override
    {
        apply_wait_states(addr);
        if(debug & DEBUG_BUS) { printf("write of uint16_t %04X at %06X\n", val, addr); }
        uint8_t high = (val >> 8);
        uint8_t low = (val & 0xFF);

        if (RAM_BANK_1.contains(addr)) {
            if(RAM_bank1.size() != 0) {
                RAM_bank1[RAM_BANK_1.offset(addr) % RAM_bank1.size()] = high;
                RAM_bank1[RAM_BANK_1.offset(addr + 1) % RAM_bank1.size()] = low;
            }
        } else if (RAM_BANK_2.contains(addr)) {
            if(RAM_bank2.size() != 0) {
                RAM_bank2[RAM_BANK_2.offset(addr) % RAM_bank2.size()] = high;
                RAM_bank2[RAM_BANK_2.offset(addr + 1) % RAM_bank2.size()] = low;
            }
        } else if (RAM_BANK_3.contains(addr)) {
            if(RAM_bank3.size() != 0) {
                RAM_bank3[RAM_BANK_3.offset(addr) % RAM_bank3.size()] = high;
                RAM_bank3[RAM_BANK_3.offset(addr + 1) % RAM_bank3.size()] = low;
            }
        } else if (RAM_BANK_4.contains(addr)) {
            if(RAM_bank4.size() != 0) {
                RAM_bank4[RAM_BANK_4.offset(addr) % RAM_bank4.size()] = high;
                RAM_bank4[RAM_BANK_4.offset(addr + 1) % RAM_bank4.size()] = low;
            }
        } else if (addr >= ROM_BASE && addr < ROM_BASE + ROM_WINDOW) {
            return;
        } else if (addr >= ENGINE_BASE && addr < ENGINE_BASE + ENGINE_SIZE) {
            ENGINE_write16(addr, val);
        } else if (addr >= VIDEO_BASE && addr < VIDEO_BASE + VIDEO_SIZE) {
            VIDEO_write16(addr, val);
        } else if (addr >= IO_BASE && addr < (IO_BASE + IO_SIZE)) {
            IO_write16(addr - IO_BASE, val);
        } else {
            printf("write of uint16_t %04X to unhandled %06X\n", val, addr);
            abort();
        }
    }

    uint8_t* romData() { return ROM.data(); }
    size_t romSize() { return ROM.size(); }

    int get_debug_latch() const
    {
        return debug_out_latch;
    }

    bool init_pty()
    {
        return pty_console.open();
    }

    bool open_cf(const char *path, bool read_only)
    {
        return cf.open(path, read_only);
    }

    // Poll the PTY for incoming data, feed the DEBUG_IN bitstream
    // synthesizer, and update IPL.
    // Call this periodically from the main loop, not on every bus cycle.
    void poll_io()
    {
        // Feed PTY input into DEBUG_IN bitstream synthesizer
        uint8_t ch;
        while (pty_console.is_data_ready() && pty_console.receive(&ch))
        {
            debug_in_tx.enqueue(ch);
        }

        update_ipl();
    }

    // Tick the systick timer.  Called every 65536 SYSCLK cycles.
    void tick_systick()
    {
        systick.tick();
    }

    // ----------------------------------------------------------------
    // Video display
    // ----------------------------------------------------------------

    // Read a byte directly from RAM without bus penalties (for DMA emulation)
    uint8_t read_ram_direct(uint32_t addr) const
    {
        if (addr < RAM_BANK_1_BASE + RAM_BANK_1_SIZE)
        {
            return RAM_bank1.size() > 0 ? RAM_bank1[addr % RAM_bank1.size()] : 0;
        }
        if (addr < RAM_BANK_2_BASE + RAM_BANK_2_SIZE)
        {
            return RAM_bank2.size() > 0 ? RAM_bank2[(addr - RAM_BANK_2_BASE) % RAM_bank2.size()] : 0;
        }
        if (addr < RAM_BANK_3_BASE + RAM_BANK_3_SIZE)
        {
            return RAM_bank3.size() > 0 ? RAM_bank3[(addr - RAM_BANK_3_BASE) % RAM_bank3.size()] : 0;
        }
        if (addr < RAM_BANK_4_BASE + RAM_BANK_4_SIZE)
        {
            return RAM_bank4.size() > 0 ? RAM_bank4[(addr - RAM_BANK_4_BASE) % RAM_bank4.size()] : 0;
        }
        return 0;
    }

    // Unified IPL management — picks highest active interrupt source
    void update_ipl()
    {
        if (video_display.irq_active)
        {
            setIPL(VIDEO_IRQ_LEVEL);
        }
        else if (systick.pending && systick.irq_enabled)
        {
            setIPL(5);
        }
        else
        {
            setIPL(0);
        }
    }

    // Process a single video line crossing
    void process_video_line(uint32_t vline)
    {
        if (vline < VideoDisplay::V_ACTIVE && video_display.dma_enabled)
        {
            // Sample palette at line start (firmware may rewrite per-line).
            uint32_t color0 = rgb332_to_argb(video_palette & 0xFF);
            uint32_t color1 = rgb332_to_argb((video_palette >> 8) & 0xFF);
            // Read 40 words (80 bytes) from framebuffer, expand 1bpp to ARGB
            uint32_t base_addr = VideoDisplay::fb_byte_addr(
                video_display.fb_base, video_display.fb_ptr);
            uint32_t *row = video_display.staging[vline];
            for (uint32_t word = 0; word < VideoDisplay::WORDS_PER_LINE; word++)
            {
                uint32_t addr = base_addr + word * 2;
                uint8_t hi = read_ram_direct(addr);
                uint8_t lo = read_ram_direct(addr + 1);
                uint16_t pixels = (hi << 8) | lo;
                // MSB first: video.v word_reg[15 - pixel_cnt]
                for (int bit = 15; bit >= 0; bit--)
                {
                    *row++ = (pixels & (1 << bit)) ? color1 : color0;
                }
            }
            video_display.eol_advance();
        }
        else if (vline < VideoDisplay::V_ACTIVE)
        {
            // DMA disabled — fill with ENTRY_0
            uint32_t color0 = rgb332_to_argb(video_palette & 0xFF);
            uint32_t *row = video_display.staging[vline];
            for (uint32_t i = 0; i < VideoDisplay::WIDTH; i++)
            {
                row[i] = color0;
            }
        }

        // SOF at vsync start — reset fb_ptr, assert VIDEO IRQ
        if (vline == VideoDisplay::V_SYNC_START)
        {
            video_display.fb_ptr = 0;
            video_display.irq_active = true;
            update_ipl();
        }
        else if (vline == VideoDisplay::V_SYNC_END)
        {
            video_display.irq_active = false;
            update_ipl();
        }
    }

    // Advance video timing to current SYSCLK, process any crossed lines
    void advance_video()
    {
        uint64_t sysclk = getClock();
        uint64_t elapsed = sysclk - video_display.prev_sysclk;
        video_display.prev_sysclk = sysclk;
        video_display.pixel_accum_fp += elapsed * VideoDisplay::PIXEL_RATIO_FP;

        uint64_t cur_abs_line =
            (video_display.pixel_accum_fp >> 16) / VideoDisplay::H_TOTAL;

        while (video_display.abs_line < cur_abs_line)
        {
            video_display.abs_line++;
            uint32_t vline = video_display.abs_line % VideoDisplay::V_TOTAL;
            if (vline == 0)
            {
                video_display.present();
            }
            process_video_line(vline);
        }
    }

    bool init_video()
    {
        if (!SDL_Init(SDL_INIT_VIDEO))
        {
            fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
            return false;
        }
        return video_display.init();
    }

    void shutdown_video()
    {
        video_display.shutdown();
        SDL_Quit();
    }
};

// Courtesy Claude Opus 4.6

static constexpr int OVERSAMPLE = 16;
static constexpr int BAUDRATE = 115200;
// Use 16.16 fixed-point to avoid integer truncation drift at high baud rates.
// Sample interval = SYSCLK_HZ / (BAUDRATE * OVERSAMPLE), in 16.16 fixed point.
static constexpr uint32_t SOFT_UART_SAMPLE_INTERVAL_FP = ((uint64_t)SYSCLK_HZ << 16) / (BAUDRATE * OVERSAMPLE);

// TODO parameterize this on SYSCLOCK and OVERSAMPLE and BAUDRATE
struct SoftUART
{
    int state = 0;          // 0 = idle, 1 = receiving
    int sample_count = 0;
    int bit_index = 0;
    uint8_t shift_reg = 0;
    int last_level = 1;

    SoftUART(int start_level) : last_level(start_level) { }

    // Call at 16x baud rate; sample interval derived from SYSCLK_HZ and BAUDRATE
    void clock(int level)
    {
        if(debug & DEBUG_UART) printf("clock(%d)\n", level);
        if(debug & DEBUG_UART) printf("    state %d, sample_count %d, bit_index %d, shift_reg %d\n",
            state, sample_count, bit_index, shift_reg);
        if (state == 0) {
            // Idle — watch for falling edge (start bit)
            if (last_level == 1 && level == 0) {
                state = 1;
                sample_count = 0;
                bit_index = 0;
                shift_reg = 0;
            }
        } else {
            sample_count++;

            if (bit_index == 0) {
                // Start bit — verify it's still low at midpoint
                if (sample_count == 8) {
                    if (level == 0) {
                        // Valid start bit, advance to data bits
                        bit_index = 1;
                        sample_count = 0;
                    } else {
                        // False start, back to idle
                        state = 0;
                    }
                }
            } else if (bit_index <= 8) {
                // Data bits — sample at midpoint
                if (sample_count == 16) {
                    shift_reg |= (level << (bit_index - 1));
                    bit_index++;
                    sample_count = 0;
                }
            } else {
                // Stop bit — sample at midpoint
                if (sample_count == 16) {
                    if (level == 1) {
                        // Valid frame — emit character
                        printf("%c", shift_reg);
                        if(debug & DEBUG_UART) printf("(%d)", shift_reg);
                        fflush(stdout);
                    } else {
                        // Framing error — surface it instead of silently dropping
                        fprintf(stderr,
                            "\n[SoftUART: framing error, stop=0, partial SR=0x%02X '%c']\n",
                            shift_reg, isprint(shift_reg) ? shift_reg : '?');
                        fflush(stderr);
                    }
                    state = 0;
                }
            }
        }
        if(debug & DEBUG_UART) printf("    -> state %d, sample_count %d, bit_index %d, shift_reg %d\n",
            state, sample_count, bit_index, shift_reg);
        last_level = level;
    }
};

void usage(const char *progname)
{
    printf("%s [-m {256,1024,2048,3072,4096}] [--cf disk.img] [--cf-ro disk.img] rom-filename\n", progname);
}

int main(int argc, const char** argv)
{
    const char *progname = argv[0];
    argc -= 1;
    argv += 1;
    auto ram_config = GriffinEmulator::RAM_1_BANK_256K;
    const char *cf_path = nullptr;
    bool cf_ro = false;

    while((argc > 0) && (argv[0][0] == '-')) {
	if(strcmp(argv[0], "--cf") == 0) {
            if(argc < 2) {
                fprintf(stderr, "--cf option requires a disk image path.\n");
                exit(EXIT_FAILURE);
            }
            cf_path = argv[1];
            cf_ro = false;
            argv += 2;
            argc -= 2;
        } else if(strcmp(argv[0], "--cf-ro") == 0) {
            if(argc < 2) {
                fprintf(stderr, "--cf-ro option requires a disk image path.\n");
                exit(EXIT_FAILURE);
            }
            cf_path = argv[1];
            cf_ro = true;
            argv += 2;
            argc -= 2;
        } else if(strcmp(argv[0], "-m") == 0) {
            if(argc < 2) {
                fprintf(stderr, "-m option requires a memory config in K (256, 1024, 2048, 3072, 4096).\n");
                exit(EXIT_FAILURE);
            }
            static std::map <int, GriffinEmulator::RAMConfig> ram_configs = {
                {256, GriffinEmulator::RAM_1_BANK_256K},
                {1024, GriffinEmulator::RAM_1M},
                {2048, GriffinEmulator::RAM_2M}, 
                {3072, GriffinEmulator::RAM_3M},
                {4096, GriffinEmulator::RAM_4M}
            };
            int k = atoi(argv[1]);
            if(!ram_configs.contains(k)) {
                if(argc < 2) {
                    fprintf(stderr, "-m size %s unknown, expected 256, 1024, 2048, 3072, or 4096.\n", argv[1]);
                    exit(EXIT_FAILURE);
                }
            }
            ram_config = ram_configs.at(k);
            argv += 2;
            argc -= 2;
        } else if(
            (strcmp(argv[0], "-help") == 0) ||
            (strcmp(argv[0], "-h") == 0) ||
            (strcmp(argv[0], "-?") == 0))
        {
            usage(progname);
            exit(EXIT_SUCCESS);
	} else {
	    fprintf(stderr, "unknown parameter \"%s\"\n", argv[0]);
            usage(progname);
	    exit(EXIT_FAILURE);
	}
    }

    if(argc < 1) {
        usage(progname);
        exit(EXIT_FAILURE);
    }

    if (argc < 1) {
        fprintf(stderr, "Usage: %s <rom-file>\n", progname);
        exit(EXIT_FAILURE);
    }

    const char *romname = argv[0];

    GriffinEmulator emulator(ram_config);

    if (cf_path)
    {
        if (!emulator.open_cf(cf_path, cf_ro))
        {
            exit(EXIT_FAILURE);
        }
    }

    FILE* fp = fopen(romname, "rb");
    if (fp == NULL) {
        fprintf(stderr, "Couldn't open \"%s\" for reading\n", romname);
        exit(EXIT_FAILURE);
    }
    fseek(fp, 0, SEEK_END);
    long size = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    if (size > static_cast<long>(emulator.romSize())) {
        fprintf(stderr, "ROM file too large (%ld bytes, max %zu)\n", size, emulator.romSize());
        fclose(fp);
        exit(EXIT_FAILURE);
    }
    [[maybe_unused]] size_t was_read = fread(emulator.romData(), 1, size, fp);
    assert(was_read == static_cast<size_t>(size));
    fclose(fp);

    if(!emulator.init_pty())
    {
        fprintf(stderr, "Failed to open PTY for console\n");
        exit(EXIT_FAILURE);
    }

    if(!emulator.init_video())
    {
        fprintf(stderr, "Failed to initialize video display\n");
        exit(EXIT_FAILURE);
    }

    SoftUART debug_uart(emulator.get_debug_latch() & 1); // Bit 0 = DEBUG_OUT serial line

    emulator.setDasmSyntax(moira::Syntax::GNU_MIT);
    emulator.reset();
    emulator.governor.reset(emulator.getClock(), emulator.clock_hz);

    uint64_t previous_uart_sample_fp = 0; // 16.16 fixed-point clock
    uint64_t previous_io_poll = 0;
    static constexpr uint64_t IO_POLL_INTERVAL = SYSCLK_HZ / 1000; // ~1ms
    uint64_t previous_systick_tick = 0;
    static constexpr uint64_t SYSTICK_TICK_INTERVAL = SystickState::PERIOD; // SYSCLK / 65536 ≈ 183 Hz

    auto clock_then = emulator.getClock();
    auto then = time(0);

    int throttle_counter = 0;

    static constexpr uint64_t audio_rate_hertz = 11025;
    static constexpr uint64_t sysclk_per_audio = SYSCLK_HZ / audio_rate_hertz;
    uint64_t clock_next_audio = sysclk_per_audio;
    FILE *audio = fopen("audio.raw", "wb");

    bool running = true;

    while (running) {
        if(debug & DEBUG_DISASSEMBLE) {
            static char str[1024];
            emulator.disassemble(str, emulator.getPC());
            printf("%04X: %s\n", emulator.getPC(), str);
        }
        emulator.execute();
        emulator.advance_video();
        auto clock_now = emulator.getClock();
        auto now = time(0);

        while(clock_now > clock_next_audio) {
            uint8_t dac_value = emulator.GetAudioDACValue();
            fwrite(&dac_value, 1, 1, audio);
            clock_next_audio += sysclk_per_audio;
        }

        if (++throttle_counter >= THROTTLE_INTERVAL)
        {
            emulator.governor.throttle(clock_now);
            throttle_counter = 0;
        }

        {
            uint64_t clock_now_fp = clock_now << 16;
            // If we've fallen too far behind (e.g. halt-settle loop), skip ahead
            // rather than churning through thousands of idle samples.
            // 16 * 10 bit-times at 115200 = ~1389 clocks is plenty of margin.
            uint64_t max_behind_fp = (uint64_t)SOFT_UART_SAMPLE_INTERVAL_FP * OVERSAMPLE * 10;
            if (clock_now_fp > previous_uart_sample_fp + max_behind_fp)
            {
                previous_uart_sample_fp = clock_now_fp - max_behind_fp;
            }
            while (previous_uart_sample_fp + SOFT_UART_SAMPLE_INTERVAL_FP <= clock_now_fp)
            {
                debug_uart.clock(emulator.get_debug_latch() & 1);
                previous_uart_sample_fp += SOFT_UART_SAMPLE_INTERVAL_FP;
            }
        }

        // Tick systick at prescaler rate (every 1024 SYSCLK cycles)
        while (clock_now - previous_systick_tick >= SYSTICK_TICK_INTERVAL)
        {
            emulator.tick_systick();
            previous_systick_tick += SYSTICK_TICK_INTERVAL;
        }

        if (clock_now - previous_io_poll >= IO_POLL_INTERVAL)
        {
            emulator.poll_io();
            previous_io_poll = clock_now;

            SDL_Event event;
            while (SDL_PollEvent(&event))
            {
                if (event.type == SDL_EVENT_QUIT)
                {
                    running = false;
                }
            }
        }

        if(now != then)
        {
            printf("%" PRIu64 " clocks\n", clock_now - clock_then);
            clock_then = clock_now;
            then = now;
        }
    }

    if (audio)
    {
        fclose(audio);
    }
    emulator.shutdown_video();
    return 0;
}
