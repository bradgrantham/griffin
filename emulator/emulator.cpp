#include <array>
#include <chrono>
#include <cstdint>
#include <cinttypes>
#include <deque>
#include <thread>

#include <SDL3/SDL.h>

#include "../griffin.generated.h"

// pty.h / util.h pull in termios.h which #defines EXTB, colliding with
// a Moira enum member.  Include Moira first, then the PTY header.
#include "Moira.h"

#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <termios.h>
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
constexpr uint32_t DEBUG_DEBUG_BIT = 0x0010;
constexpr uint32_t DEBUG_CF = 0x0020;
constexpr uint32_t DEBUG_DUART = 0x0040;
constexpr uint32_t DEBUG_SPEED = 0x0080;
constexpr uint32_t debug = 0; // DEBUG_BUS | DEBUG_IO | DEBUG_UART;

using namespace Griffin;

// PTY-based console for serial emulation.
// The master fd acts like the UART: write() sends to the terminal,
// read() receives keystrokes.  Works on macOS and Linux.
struct PTYConsole
{
    int master_fd = -1;
    mutable bool have_pending = false;
    mutable uint8_t pending = 0;

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

    // On macOS, FIONREAD on a PTY master is unreliable: it can return a
    // positive count when no readable bytes are actually available (it
    // appears to reflect pending master-write data, not master-readable
    // data).  The resulting non-blocking read() then returns EAGAIN and
    // the DUART emulation would enqueue spurious 0x00 bytes, drowning
    // real keystrokes.  Instead, actually attempt a non-blocking read
    // and cache a single byte.
    bool is_data_ready() const
    {
        if(have_pending)
        {
            return true;
        }
        if(master_fd < 0)
        {
            return false;
        }
        uint8_t b;
        ssize_t n = read(master_fd, &b, 1);
        if(n == 1)
        {
            pending = b;
            have_pending = true;
            return true;
        }
        return false;
    }

    // Read one byte.  Returns true if a byte was read.
    bool receive(uint8_t *out) const
    {
        if(have_pending)
        {
            *out = pending;
            have_pending = false;
            return true;
        }
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
// Raw stdin console for DEBUG_IN bitstream — bypasses PTY/DUART path.
// Escape sequence: ~. at line start exits (like ssh).
// ---------------------------------------------------------------------------

struct StdinConsole
{
    struct termios orig_termios;
    bool raw_mode = false;
    int escape_state = 1;  // 0=normal, 1=line start, 2=saw tilde
    bool have_pending = false;
    uint8_t pending = 0;

    bool open()
    {
        if (!isatty(STDIN_FILENO))
        {
            return false;
        }
        tcgetattr(STDIN_FILENO, &orig_termios);
        struct termios raw = orig_termios;
        cfmakeraw(&raw);
        raw.c_oflag |= OPOST | ONLCR;
        tcsetattr(STDIN_FILENO, TCSANOW, &raw);
        fcntl(STDIN_FILENO, F_SETFL, O_NONBLOCK);
        raw_mode = true;
        fprintf(stderr, "stdin: raw mode, ~. to exit\n");
        return true;
    }

    ~StdinConsole()
    {
        if (raw_mode)
        {
            tcsetattr(STDIN_FILENO, TCSANOW, &orig_termios);
        }
    }

    // Returns: 1 = byte in *out, 0 = no data, -1 = exit requested (~.)
    int poll(uint8_t *out)
    {
        if (have_pending)
        {
            *out = pending;
            have_pending = false;
            return 1;
        }

        if (!raw_mode)
        {
            return 0;
        }

        uint8_t ch;
        ssize_t n = ::read(STDIN_FILENO, &ch, 1);
        if (n != 1)
        {
            return 0;
        }

        if (escape_state == 2)
        {
            if (ch == '.')
            {
                return -1;
            }
            if (ch == '~')
            {
                escape_state = 0;
                *out = '~';
                return 1;
            }
            pending = ch;
            have_pending = true;
            escape_state = (ch == '\r' || ch == '\n') ? 1 : 0;
            *out = '~';
            return 1;
        }

        if (escape_state == 1 && ch == '~')
        {
            escape_state = 2;
            return 0;
        }

        escape_state = (ch == '\r' || ch == '\n') ? 1 : 0;
        *out = ch;
        return 1;
    }
};

// ---------------------------------------------------------------------------
// 68681 DUART emulation — Channel A UART via PTY, counter/timer, interrupts
//
// Minimal subset: Channel A TX/RX through the PTY, counter/timer for
// periodic interrupts (e.g. 59.95 Hz tick), ISR/IMR for IPL assertion.
// Channel B, I/O ports, and BRG details are stubs.
// ---------------------------------------------------------------------------

struct DUARTState
{
    // Channel A
    bool tx_enabled = false;
    bool rx_enabled = false;
    int mr_pointer_a = 0;       // 0 = next rw hits MR1, 1 = MR2+
    uint8_t mr_a[2] = {0, 0};  // stored but not interpreted

    // Channel B (stub)
    bool tx_enabled_b = false;
    bool rx_enabled_b = false;
    int mr_pointer_b = 0;
    uint8_t mr_b[2] = {0, 0};

    // Interrupt
    uint8_t imr = 0;
    bool ctr_ready = false;     // latched; clears on STOPCC read

    // Counter/timer
    uint8_t acr = 0;
    uint8_t ctur = 0xFF;
    uint8_t ctlr = 0xFF;
    bool ctr_running = false;
    uint64_t ctr_next_fire = 0; // in SYSCLK units

    // IVR (accepted but unused — autovector mode)
    uint8_t ivr = 0x0F;

    // Output port
    uint8_t opcr = 0;
    uint8_t opr = 0;

    uint16_t preload() const
    {
        return (static_cast<uint16_t>(ctur) << 8) | ctlr;
    }

    // Counter/timer period between IRQs in SYSCLK cycles.
    // Counter mode: one IRQ per preload countdown.
    // Timer mode: one IRQ per full square-wave period = 2*preload input cycles
    // (matches MC68681 hardware — preload alone gives only the half-period).
    uint64_t ctr_period_sysclk() const
    {
        uint32_t p = preload();
        if (p == 0)
        {
            p = 0x10000;  // 68681 treats 0 as 65536
        }
        uint64_t cycles = p;
        if ((acr & 0x40) != 0)
        {
            cycles *= 2;
        }
        return cycles * SYSCLK_HZ / DUART_CLOCK;
    }

    // Advance timer state — call periodically from poll_io
    void check_timer(uint64_t clock_now)
    {
        if (!ctr_running)
        {
            return;
        }
        while (clock_now >= ctr_next_fire)
        {
            ctr_ready = true;
            ctr_next_fire += ctr_period_sysclk();
        }
    }

    // Compute ISR from live state
    uint8_t isr(const PTYConsole &pty) const
    {
        uint8_t val = 0;
        if (tx_enabled)
        {
            val |= DUART_ISR_TXRDYA_MASK;   // TX always ready
        }
        if (rx_enabled && pty.is_data_ready())
        {
            val |= DUART_ISR_RXRDYA_MASK;
        }
        if (ctr_ready)
        {
            val |= DUART_ISR_CTR_READY_MASK;
        }
        return val;
    }

    bool irq_pending(const PTYConsole &pty) const
    {
        return (isr(pty) & imr) != 0;
    }

    // --- Command register decode (shared by CRA and CRB) ---
    static void apply_command(uint8_t val, bool &tx_en, bool &rx_en,
                              int &mr_ptr, uint8_t mr[2])
    {
        uint8_t ec = (val & DUART_CRA_EC_MASK) >> DUART_CRA_EC_SHIFT;
        uint8_t tc = (val & DUART_CRA_TC_MASK) >> DUART_CRA_TC_SHIFT;
        uint8_t mc = (val & DUART_CRA_MC_MASK) >> DUART_CRA_MC_SHIFT;

        if (ec == 1) { rx_en = true; }
        if (ec == 2) { rx_en = false; }
        if (tc == 1) { tx_en = true; }
        if (tc == 2) { tx_en = false; }

        switch (mc)
        {
            case 1: mr_ptr = 0; break;                     // reset MR pointer
            case 2: rx_en = false; break;                   // reset receiver
            case 3: tx_en = false; break;                   // reset transmitter
            case 4: break;                                  // reset error status (no-op)
            case 5: case 6: case 7: break;                  // break commands (no-op)
        }
    }

    uint8_t read_reg(uint32_t abs_addr, const PTYConsole &pty)
    {
        switch (abs_addr)
        {
            case DUART_MR1A:  // MR1A/MR2A share address; pointer auto-advances
            {
                uint8_t val = mr_a[mr_pointer_a];
                if (mr_pointer_a == 0)
                {
                    mr_pointer_a = 1;
                }
                return val;
            }
            case DUART_SRA:
            {
                uint8_t val = 0;
                if (rx_enabled && pty.is_data_ready())
                {
                    val |= DUART_SRA_RXRDY_MASK;
                }
                if (tx_enabled)
                {
                    val |= DUART_SRA_TXRDY_MASK | DUART_SRA_TXEMT_MASK;
                }
                return val;
            }
            case DUART_RBA:
            {
                uint8_t ch = 0;
                if (rx_enabled)
                {
                    pty.receive(&ch);
                }
                return ch;
            }
            case DUART_IPCR:    return 0;
            case DUART_ISR:     return isr(pty);
            case DUART_CUR:     return 0;
            case DUART_CLR:     return 0;
            case DUART_MR1B:  // MR1B/MR2B share address
            {
                uint8_t val = mr_b[mr_pointer_b];
                if (mr_pointer_b == 0)
                {
                    mr_pointer_b = 1;
                }
                return val;
            }
            case DUART_SRB:     return 0;
            case DUART_RBB:     return 0;
            case DUART_IVR:     return ivr;
            case DUART_IP:      return 0;
            case DUART_STARTCC:
                ctr_running = true;
                // ctr_next_fire == 0 signals "needs initialization" — caller
                // sets it using getClock() after this returns.
                return 0;
            case DUART_STOPCC:
                // In Timer mode (ACR[6]=1), STOPCC only clears the IRQ
                // status; the counter keeps free-running.  In Counter
                // mode it fully halts the counter.
                ctr_ready = false;
                if ((acr & 0x40) == 0)
                {
                    ctr_running = false;
                    ctr_next_fire = 0;
                }
                return 0;
            default:
                return 0;
        }
    }

    void write_reg(uint32_t abs_addr, uint8_t val, const PTYConsole &pty)
    {
        switch (abs_addr)
        {
            case DUART_MR1A:  // MR1A/MR2A share address
                mr_a[mr_pointer_a] = val;
                if (mr_pointer_a == 0)
                {
                    mr_pointer_a = 1;
                }
                break;
            case DUART_CSRA:
                break;  // accept and ignore
            case DUART_CRA:
                apply_command(val, tx_enabled, rx_enabled, mr_pointer_a, mr_a);
                if (debug & DEBUG_DUART)
                {
                    printf("[DUART CRA: 0x%02X → tx=%d rx=%d]\n", val, tx_enabled, rx_enabled);
                }
                break;
            case DUART_TBA:
                if (tx_enabled)
                {
                    pty.send(val);
                }
                break;
            case DUART_ACR:     acr = val; break;
            case DUART_IMR:
                imr = val;
                if (debug & DEBUG_DUART)
                {
                    printf("[DUART IMR: 0x%02X]\n", val);
                }
                break;
            case DUART_CTUR:    ctur = val; break;
            case DUART_CTLR:    ctlr = val; break;
            case DUART_MR1B:  // MR1B/MR2B share address
                mr_b[mr_pointer_b] = val;
                if (mr_pointer_b == 0)
                {
                    mr_pointer_b = 1;
                }
                break;
            case DUART_CSRB:    break;
            case DUART_CRB:
                apply_command(val, tx_enabled_b, rx_enabled_b, mr_pointer_b, mr_b);
                break;
            case DUART_TBB:     break;
            case DUART_IVR:     ivr = val; break;
            case DUART_OPCR:    opcr = val; break;
            case DUART_OPR_SET: opr |= val; break;
            case DUART_OPR_CLR: opr &= ~val; break;
        }
    }
};

static uint32_t r3g3b2_to_argb(uint8_t c)
{
    uint8_t r3 = (c >> 5) & 0x7;
    uint8_t g3 = (c >> 2) & 0x7;
    uint8_t b2 = c & 0x3;
    uint8_t r8 = (r3 << 5) | (r3 << 2) | (r3 >> 1);
    uint8_t g8 = (g3 << 5) | (g3 << 2) | (g3 >> 1);
    uint8_t b8 = (b2 << 6) | (b2 << 4) | (b2 << 2) | b2;
    return 0xFF000000u | (r8 << 16) | (g8 << 8) | b8;
}

struct VideoState
{
    // 800 pixel clocks per line at 25.175 MHz, converted to SYSCLK (14 MHz)
    // via Bresenham: GCD(14000000,25175000)=25000 → 560/1007
    static constexpr uint64_t LINE_NUM = 800ULL * 560;  // 448000
    static constexpr uint64_t LINE_DEN = 1007;
    static constexpr int V_TOTAL = 525;
    static constexpr int V_ACTIVE = 480;
    static constexpr int V_SYNC_START = 490;
    static constexpr int H_ACTIVE = 640;
    static constexpr int BYTES_PER_LINE = 80;

    uint64_t clock_next_line = 0;
    uint64_t frac_accum = 0;
    int v_cnt = 0;

    bool irq_latched = false;

    bool video_enable = false;
    uint8_t palette_fg = 0xFF;
    uint8_t palette_bg = 0x00;
    uint8_t background_color = 0x00;
    bool fifo_error = false;

    SDL_Window *window = nullptr;
    SDL_Renderer *renderer = nullptr;
    SDL_Texture *texture = nullptr;
    bool sdl_ok = false;
    std::vector<uint32_t> framebuffer;

    uint32_t rng_state = 0x12345678;

    uint8_t next_random_byte()
    {
        static int count = 0;
        if(count++ < 5) {
            rng_state ^= rng_state << 13;
            rng_state ^= rng_state >> 17;
            rng_state ^= rng_state << 5;
        }
        return static_cast<uint8_t>(rng_state);
    }

    bool init()
    {
        if (!SDL_Init(SDL_INIT_VIDEO))
        {
            fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
            return false;
        }
        window = SDL_CreateWindow("Griffin Video", H_ACTIVE, V_ACTIVE, 0);
        if (!window)
        {
            fprintf(stderr, "SDL_CreateWindow failed: %s\n", SDL_GetError());
            SDL_Quit();
            return false;
        }
        renderer = SDL_CreateRenderer(window, nullptr);
        if (!renderer)
        {
            fprintf(stderr, "SDL_CreateRenderer failed: %s\n", SDL_GetError());
            SDL_DestroyWindow(window);
            SDL_Quit();
            return false;
        }
        texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ARGB8888,
                                    SDL_TEXTUREACCESS_STREAMING,
                                    H_ACTIVE, V_ACTIVE);
        if (!texture)
        {
            fprintf(stderr, "SDL_CreateTexture failed: %s\n", SDL_GetError());
            SDL_DestroyRenderer(renderer);
            SDL_DestroyWindow(window);
            SDL_Quit();
            return false;
        }
        framebuffer.resize(H_ACTIVE * V_ACTIVE, 0xFF000000u);
        sdl_ok = true;

        // Seed the timing accumulator
        frac_accum = LINE_NUM;
        clock_next_line = frac_accum / LINE_DEN;
        frac_accum %= LINE_DEN;

        return true;
    }

    ~VideoState()
    {
        if (texture)  { SDL_DestroyTexture(texture); }
        if (renderer) { SDL_DestroyRenderer(renderer); }
        if (window)   { SDL_DestroyWindow(window); }
        if (sdl_ok)   { SDL_Quit(); }
    }

    void render_scanline(int line)
    {
        uint32_t *row = &framebuffer[line * H_ACTIVE];
        if (!video_enable)
        {
            uint32_t bg = r3g3b2_to_argb(background_color);
            for (int i = 0; i < H_ACTIVE; i++)
            {
                row[i] = bg;
            }
            return;
        }
        uint32_t fg_argb = r3g3b2_to_argb(palette_fg);
        uint32_t bg_argb = r3g3b2_to_argb(palette_bg);
        for (int b = 0; b < BYTES_PER_LINE; b++)
        {
            uint8_t shift = next_random_byte();
            for (int bit = 7; bit >= 0; bit--)
            {
                row[b * 8 + (7 - bit)] = (shift & (1 << bit)) ? fg_argb : bg_argb;
            }
        }
    }

    void present_frame()
    {
        if (!sdl_ok)
        {
            return;
        }
        SDL_UpdateTexture(texture, nullptr, framebuffer.data(),
                          H_ACTIVE * sizeof(uint32_t));
        SDL_RenderTexture(renderer, texture, nullptr, nullptr);
        SDL_RenderPresent(renderer);
    }

    void check_timer(uint64_t clock_now)
    {
        while (clock_now >= clock_next_line)
        {
            if (v_cnt < V_ACTIVE)
            {
                render_scanline(v_cnt);
            }

            v_cnt++;

            if (v_cnt == V_SYNC_START)
            {
                irq_latched = true;
                present_frame();
            }

            if (v_cnt >= V_TOTAL)
            {
                v_cnt = 0;
            }

            frac_accum += LINE_NUM;
            uint64_t advance = frac_accum / LINE_DEN;
            frac_accum %= LINE_DEN;
            clock_next_line += advance;
        }
    }

    bool irq_pending() const { return irq_latched; }
    void clear_irq() { irq_latched = false; }

    uint8_t read_reg(uint32_t addr) const
    {
        if (addr == VIDEO_CTRL_RB)
        {
            return (fifo_error ? VIDEO_CTRL_RB_FIFO_ERROR_MASK : 0)
                 | (video_enable ? VIDEO_CTRL_RB_ENABLE_MASK : 0);
        }
        if (addr == VIDEO_STATUS)
        {
            return v_cnt & 1;
        }
        return 0;
    }

    void write_reg8(uint32_t addr, uint8_t val)
    {
        if (addr == VIDEO_CLRINT)
        {
            irq_latched = false;
        }
        else if (addr == VIDEO_CTRL)
        {
            video_enable = (val & VIDEO_CTRL_ENABLE_MASK) != 0;
        }
        else if (addr == VIDEO_CLRERR)
        {
            fifo_error = false;
        }
        else if (addr == VIDEO_BACKGROUND)
        {
            background_color = val;
        }
    }

    void write_reg16(uint32_t addr, uint16_t val)
    {
        if (addr == VIDEO_PALETTE)
        {
            palette_fg = (val >> 8) & 0xFF;
            palette_bg = val & 0xFF;
        }
    }
};

// ---------------------------------------------------------------------------
// PS/2 keyboard emulation — mirrors the minimal CPLD primitive.
//
// The CPLD latches one PS/2 DATA bit on each synchronized PS2_CLK falling
// edge and raises IRQ4.  Firmware does all framing/parity in software.
// Here we model the device side: translate SDL key events into PS/2 Set 2
// byte streams, then into individual bit events timed at ~80 µs each, and
// feed them into the latched-bit + BIT_READY register as emulated time
// advances.
// ---------------------------------------------------------------------------

static uint8_t sdl_to_ps2_set2(SDL_Scancode sc)
{
    switch (sc)
    {
        case SDL_SCANCODE_A: return 0x1C;
        case SDL_SCANCODE_B: return 0x32;
        case SDL_SCANCODE_C: return 0x21;
        case SDL_SCANCODE_D: return 0x23;
        case SDL_SCANCODE_E: return 0x24;
        case SDL_SCANCODE_F: return 0x2B;
        case SDL_SCANCODE_G: return 0x34;
        case SDL_SCANCODE_H: return 0x33;
        case SDL_SCANCODE_I: return 0x43;
        case SDL_SCANCODE_J: return 0x3B;
        case SDL_SCANCODE_K: return 0x42;
        case SDL_SCANCODE_L: return 0x4B;
        case SDL_SCANCODE_M: return 0x3A;
        case SDL_SCANCODE_N: return 0x31;
        case SDL_SCANCODE_O: return 0x44;
        case SDL_SCANCODE_P: return 0x4D;
        case SDL_SCANCODE_Q: return 0x15;
        case SDL_SCANCODE_R: return 0x2D;
        case SDL_SCANCODE_S: return 0x1B;
        case SDL_SCANCODE_T: return 0x2C;
        case SDL_SCANCODE_U: return 0x3C;
        case SDL_SCANCODE_V: return 0x2A;
        case SDL_SCANCODE_W: return 0x1D;
        case SDL_SCANCODE_X: return 0x22;
        case SDL_SCANCODE_Y: return 0x35;
        case SDL_SCANCODE_Z: return 0x1A;
        case SDL_SCANCODE_1: return 0x16;
        case SDL_SCANCODE_2: return 0x1E;
        case SDL_SCANCODE_3: return 0x26;
        case SDL_SCANCODE_4: return 0x25;
        case SDL_SCANCODE_5: return 0x2E;
        case SDL_SCANCODE_6: return 0x36;
        case SDL_SCANCODE_7: return 0x3D;
        case SDL_SCANCODE_8: return 0x3E;
        case SDL_SCANCODE_9: return 0x46;
        case SDL_SCANCODE_0: return 0x45;
        case SDL_SCANCODE_RETURN:    return 0x5A;
        case SDL_SCANCODE_SPACE:     return 0x29;
        case SDL_SCANCODE_BACKSPACE: return 0x66;
        case SDL_SCANCODE_ESCAPE:    return 0x76;
        case SDL_SCANCODE_TAB:       return 0x0D;
        default: return 0;
    }
}

struct PS2BitEvent
{
    uint64_t fire_clock;
    uint8_t bit;
};

struct PS2State
{
    static constexpr uint64_t bit_period_sysclk = SYSCLK_HZ / 12500; // ~80 µs

    std::deque<PS2BitEvent> bit_queue;
    uint8_t data_in_latched = 1;       // idle-high
    bool bit_ready = false;
    uint8_t ctrl = 0;
    bool init() { return true; }

    // Append an 11-bit PS/2 frame to the pending bit event queue.
    void enqueue_byte(uint64_t clock_now, uint8_t byte)
    {
        uint64_t t = bit_queue.empty()
            ? clock_now + bit_period_sysclk
            : bit_queue.back().fire_clock + bit_period_sysclk;

        bit_queue.push_back({t, 0});                          // start
        t += bit_period_sysclk;

        uint8_t parity = 1;                                   // odd parity
        for (int i = 0; i < 8; i++)
        {
            uint8_t b = (byte >> i) & 1;
            bit_queue.push_back({t, b});
            parity ^= b;
            t += bit_period_sysclk;
        }

        bit_queue.push_back({t, parity});                     // parity
        t += bit_period_sysclk;
        bit_queue.push_back({t, 1});                          // stop
    }

    void check_timer(uint64_t clock_now)
    {
        while (!bit_ready && !bit_queue.empty()
               && clock_now >= bit_queue.front().fire_clock)
        {
            data_in_latched = bit_queue.front().bit;
            bit_ready = true;
            bit_queue.pop_front();
        }
    }

    uint8_t read_reg(uint32_t abs_addr) const
    {
        if (abs_addr == GLUE_PS2_STATUS)
        {
            uint8_t v = 0;
            if (bit_ready)       { v |= GLUE_PS2_STATUS_BIT_READY_MASK; }
            if (data_in_latched) { v |= GLUE_PS2_STATUS_DATA_IN_MASK; }
            v |= GLUE_PS2_STATUS_DATA_LIVE_MASK;
            v |= GLUE_PS2_STATUS_CLK_LIVE_MASK;
            return v;
        }
        return 0;
    }

    void write_reg(uint32_t abs_addr, uint8_t val)
    {
        if (abs_addr == GLUE_PS2_CLEAR)
        {
            if (val & GLUE_PS2_CLEAR_BIT_READY_MASK)
            {
                bit_ready = false;
            }
        }
        else if (abs_addr == GLUE_PS2_CTRL)
        {
            ctrl = val;
        }
    }

    bool irq_pending() const { return bit_ready; }
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
    mutable DUARTState duart;
    mutable VideoState video;
    mutable PS2State ps2;
    mutable TimerState timer;
    mutable SoftUARTTX debug_in_tx;
    StdinConsole stdin_console;
    mutable uint8_t dac_value = 0x0;

    static bool is_cf_addr(uint32_t io_offset)
    {
        uint32_t abs = io_offset + IO_BASE;
        return abs >= CF_BASE && abs < CF_BASE + CF_SIZE;
    }

    static bool is_duart_addr(uint32_t io_offset)
    {
        uint32_t abs = io_offset + IO_BASE;
        return abs >= DUART_BASE && abs < DUART_BASE + DUART_SIZE;
    }

    uint8_t IO_read8(uint32_t addr) const
    {
        if(is_cf_addr(addr)) {
            return cf.read_reg(addr + IO_BASE);
        }
        if (is_duart_addr(addr))
        {
            uint32_t abs = addr + IO_BASE;
            uint8_t val = duart.read_reg(abs, pty_console);
            // STARTCC side-effect: initialize ctr_next_fire with current clock
            if (abs == DUART_STARTCC && duart.ctr_next_fire == 0)
            {
                duart.ctr_next_fire = getClock() + duart.ctr_period_sysclk();
            }
            if (debug & DEBUG_DUART)
            {
                printf("[DUART read %06X → 0x%02X]\n", abs, val);
            }
            return val;
        }
        if (addr == GLUE_DEBUG_IN - IO_BASE)
        {
            debug_in_tx.advance(getClock());
            // PLATFORM_ID (bit 1) reads 0 in the emulator; firmware uses it
            // to skip Rev 1 bringup hacks (bitbang serial, VIDEO-IRQ systick).
            return (debug_in_tx.current_bit & GLUE_DEBUG_IN_MASK);
        }
        if (addr + IO_BASE == GLUE_PS2_STATUS)
        {
            return ps2.read_reg(GLUE_PS2_STATUS);
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
        if (is_duart_addr(addr))
        {
            printf("WARNING: 16-bit read from DUART at %06X (firmware should use 8-bit only)\n", addr + IO_BASE);
            return duart.read_reg(addr + IO_BASE, pty_console);
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
        } else if(is_duart_addr(addr)) {
            if (debug & DEBUG_DUART)
            {
                printf("[DUART write %06X ← 0x%02X]\n", addr + IO_BASE, val);
            }
            duart.write_reg(addr + IO_BASE, val, pty_console);
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
            if (debug & DEBUG_IO)
            {
                printf("[GLUE CONFIG: 0x%02X overlay=%s]\n", val,
                       (val & GLUE_CONFIG_ROM_OVERLAY_DISABLE_MASK) ? "off" : "on");
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
        } else if(addr + IO_BASE == GLUE_PS2_CLEAR || addr + IO_BASE == GLUE_PS2_CTRL) {
            ps2.write_reg(addr + IO_BASE, val);
            const_cast<GriffinEmulator*>(this)->update_ipl();
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
        if (is_duart_addr(addr))
        {
            printf("WARNING: 16-bit write 0x%04X to DUART at %06X (firmware should use 8-bit only)\n", val, addr + IO_BASE);
            duart.write_reg(addr + IO_BASE, val & 0xFF, pty_console);
            return;
        }
        if(debug & DEBUG_IO)
        {
            printf("write of uint16_t %04X at unhandled IO %06X\n", val, addr + IO_BASE);
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
    bool exit_requested = false;
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
        } else if (addr >= VIDEO_BASE && addr < VIDEO_BASE + VIDEO_SIZE) {
            return video.read_reg(addr);
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
        } else if (addr >= VIDEO_BASE && addr < VIDEO_BASE + VIDEO_SIZE) {
            return (video.read_reg(addr) << 8) | video.read_reg(addr + 1);
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
        } else if (addr >= IO_BASE && addr < (IO_BASE + IO_SIZE)) {
            IO_write8(addr - IO_BASE, val);
        } else if (addr >= VIDEO_BASE && addr < VIDEO_BASE + VIDEO_SIZE) {
            video.write_reg8(addr, val);
            const_cast<GriffinEmulator*>(this)->update_ipl();
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
        } else if (addr >= VIDEO_BASE && addr < VIDEO_BASE + VIDEO_SIZE) {
            video.write_reg16(addr, val);
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

    bool init_stdin()
    {
        return stdin_console.open();
    }

    bool init_ps2()
    {
        return ps2.init();
    }

    bool init_video()
    {
        return video.init();
    }

    bool open_cf(const char *path, bool read_only)
    {
        return cf.open(path, read_only);
    }

    // Poll the PTY for incoming data, feed the DEBUG_IN bitstream
    // synthesizer, advance DUART timer, and update IPL.
    // Call this periodically from the main loop, not on every bus cycle.
    void poll_io()
    {
        {
            uint8_t ch;
            int rc;
            while ((rc = stdin_console.poll(&ch)) == 1)
            {
                debug_in_tx.enqueue(ch);
            }
            if (rc == -1)
            {
                fprintf(stderr, "\n");
                exit_requested = true;
            }
        }

        SDL_Event ev;
        while (SDL_PollEvent(&ev))
        {
            if (ev.type == SDL_EVENT_QUIT)
            {
                exit_requested = true;
            }
            else if (ev.type == SDL_EVENT_KEY_DOWN || ev.type == SDL_EVENT_KEY_UP)
            {
                if (ev.key.repeat)
                {
                    continue;
                }
                uint8_t code = sdl_to_ps2_set2(ev.key.scancode);
                if (code != 0)
                {
                    if (ev.type == SDL_EVENT_KEY_UP)
                    {
                        ps2.enqueue_byte(getClock(), 0xF0);
                    }
                    ps2.enqueue_byte(getClock(), code);
                }
            }
        }

        duart.check_timer(getClock());
        video.check_timer(getClock());
        ps2.check_timer(getClock());
        update_ipl();
    }

    // Unified IPL management — picks highest active interrupt source
    void update_ipl()
    {
        if (video.irq_pending()) {
            setIPL(VIDEO_IRQ_LEVEL);
        } else if (duart.irq_pending(pty_console)) {
            setIPL(DUART_IRQ_LEVEL);
        } else if (ps2.irq_pending()) {
            setIPL(GLUE_IRQ_LEVEL);
        } else {
            setIPL(0);
        }
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

    emulator.init_stdin();

    if (!emulator.init_video())
    {
        fprintf(stderr, "Warning: SDL/Video init failed, display disabled\n");
    }

    SoftUART debug_uart(emulator.get_debug_latch() & 1); // Bit 0 = DEBUG_OUT serial line

    emulator.setDasmSyntax(moira::Syntax::GNU_MIT);
    emulator.reset();
    emulator.governor.reset(emulator.getClock(), emulator.clock_hz);

    uint64_t previous_uart_sample_fp = 0; // 16.16 fixed-point clock
    uint64_t previous_io_poll = 0;
    static constexpr uint64_t IO_POLL_INTERVAL = SYSCLK_HZ / 1000; // ~1ms

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

        if (clock_now - previous_io_poll >= IO_POLL_INTERVAL)
        {
            emulator.poll_io();
            if (emulator.exit_requested)
            {
                running = false;
            }
            previous_io_poll = clock_now;
        }

        if(now != then)
        {
            if(debug & DEBUG_SPEED)
            {
                printf("%" PRIu64 " clocks\n", clock_now - clock_then);
            }
            clock_then = clock_now;
            then = now;
        }
    }

    return 0;
}
