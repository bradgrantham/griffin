#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cinttypes>
#include <cstdlib>
#include <deque>
#include <thread>
#include <vector>

#include <SDL3/SDL.h>

#include "../griffin.generated.h"

// The display-list pipeline is not re-implemented here.  super-engine/ is the
// semantics authority — its EngineWalker is edma3.v's descriptor state machine
// and its PixelUnit/CompositorUnit are pixel.v and compositor.v, all validated
// against cpld/compositor/compositor_tb.v by `make -C super-engine check`.  The
// emulator is one of that suite's two drivers; the suite itself is the other,
// so a behaviour that only exists here is a bug rather than a convenience.
#include "descriptor.h"
#include "interpret.h"
#include "render.h"

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
#include <sys/disk.h>
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

// Direct-bus peripheral region (griffin.yml "Direct-bus peripheral region"):
// one 1 MB window split into four 256 KB quadrants by A19:18, fanned out by a
// 74155 from GLUE's ~IO_RD_EN / ~IO_WR_EN strobes.  Quadrant 0 is AUDIO; the
// other three are reserved.  GLUE answers DTACK for the whole window, so an
// access to a reserved quadrant must not fault -- it just reads back the
// floating bus.  Not a griffin.yml peripheral of its own, so no generated
// constant: derived here from the AUDIO base that anchors it.
static constexpr uint32_t DIRECT_BUS_BASE = AUDIO_BASE;
static constexpr uint32_t DIRECT_BUS_SIZE = 0x100000UL;

// PTY-based console for serial emulation.
// The master fd acts like the UART: write() sends to the terminal,
// read() receives keystrokes.  Works on macOS and Linux.
struct PTYConsole
{
    // The console funnels reads through read_fd and writes through write_fd.
    // For the default pty both are the master fd; for automation they can be
    // stdin/stdout or input/output files.
    int master_fd = -1;             // pty master (owned), -1 if not a pty
    int read_fd = -1;
    int write_fd = -1;
    bool close_read_fd = false;     // close read_fd in dtor (file mode)
    bool close_write_fd = false;    // close write_fd in dtor (file mode)
    mutable bool have_pending = false;
    mutable uint8_t pending = 0;
    mutable bool input_eof = false; // read() returned 0 — no more console input

    // Default interactive console: allocate a pty and print the slave path.
    bool open(const char *label = "Console PTY")
    {
        int slave_fd;
        if(openpty(&master_fd, &slave_fd, NULL, NULL, NULL) < 0)
        {
            perror("openpty");
            return false;
        }
        fprintf(stderr, "%s: %s\n", label, ttyname(slave_fd));
        close(slave_fd);
        fcntl(master_fd, F_SETFL, O_NONBLOCK);
        read_fd = master_fd;
        write_fd = master_fd;
        return true;
    }

    // Console on stdin/stdout, for piped automation.
    bool open_stdio()
    {
        read_fd = STDIN_FILENO;
        write_fd = STDOUT_FILENO;
        fcntl(read_fd, F_SETFL, O_NONBLOCK);
        return true;
    }

    // Connect to an EXISTING device/pty bidirectionally (e.g. the host end
    // of a socat pty bridge).  Unlike open() this doesn't allocate a pty --
    // it opens a stable path someone else owns, so the peer (host pppd) can
    // be set up once and outlive many emulator runs.  Raw mode so no line
    // discipline mangles PPP's HDLC bytes.
    bool open_dev(const char* path)
    {
        int fd = ::open(path, O_RDWR | O_NONBLOCK | O_NOCTTY);
        if (fd < 0) { perror(path); return false; }
        struct termios t;
        if (tcgetattr(fd, &t) == 0)
        {
            cfmakeraw(&t);
            tcsetattr(fd, TCSANOW, &t);
        }
        read_fd = fd;
        write_fd = fd;
        close_read_fd = true;   // sole owner of this fd
        return true;
    }

    // Console backed by files; either path may be null to disable that side.
    bool open_files(const char* in_path, const char* out_path)
    {
        if(in_path)
        {
            read_fd = ::open(in_path, O_RDONLY | O_NONBLOCK);
            if(read_fd < 0) { perror(in_path); return false; }
            close_read_fd = true;
        }
        if(out_path)
        {
            write_fd = ::open(out_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
            if(write_fd < 0) { perror(out_path); return false; }
            close_write_fd = true;
        }
        return true;
    }

    ~PTYConsole()
    {
        if(master_fd >= 0)
        {
            close(master_fd);
        }
        if(close_read_fd && read_fd >= 0)
        {
            close(read_fd);
        }
        if(close_write_fd && write_fd >= 0)
        {
            close(write_fd);
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
        if(read_fd < 0)
        {
            return false;
        }
        uint8_t b;
        ssize_t n = read(read_fd, &b, 1);
        if(n == 1)
        {
            pending = b;
            have_pending = true;
            return true;
        }
        if(n == 0)
        {
            input_eof = true;
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
        if(read_fd < 0)
        {
            return false;
        }
        ssize_t n = read(read_fd, out, 1);
        if(n == 0)
        {
            input_eof = true;
        }
        return n == 1;
    }

    void send(uint8_t ch) const
    {
        if(write_fd >= 0)
        {
            write(write_fd, &ch, 1);
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
        if (S_ISBLK(st.st_mode) || S_ISCHR(st.st_mode))
        {
#ifdef __APPLE__
            uint64_t block_count = 0;
            uint32_t block_size = 0;
            if (ioctl(fd, DKIOCGETBLOCKCOUNT, &block_count) < 0
                || ioctl(fd, DKIOCGETBLOCKSIZE, &block_size) < 0)
            {
                perror("ioctl(DKIOCGETBLOCK*)");
                ::close(fd);
                fd = -1;
                return false;
            }
            file_size = (off_t)block_count * (off_t)block_size;
#else
            // Fall back to seeking to end for other platforms.
            off_t end = lseek(fd, 0, SEEK_END);
            if (end < 0)
            {
                perror("lseek");
                ::close(fd);
                fd = -1;
                return false;
            }
            file_size = end;
            lseek(fd, 0, SEEK_SET);
#endif
        }
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
// Raw stdin watcher for the interactive exit escape: ~. at line start exits
// (like ssh).  Other typed bytes are discarded — console I/O is the DUART
// PTY, not the emulator's own stdin.
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
    uint64_t ctr_start_clock = 0; // SYSCLK at STARTCC; origin for the live count
    uint16_t ctr_latch = 0;       // value latched by a CUR read (68681 protocol)

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

    // Live 16-bit down-counter value (preload -> 0, repeating).  The 68681
    // counter itself always spans one preload countdown regardless of C/T mode,
    // so this is independent of the ACR square-wave doubling used for IRQs.
    uint16_t live_count(uint64_t clock_now) const
    {
        if (!ctr_running)
        {
            return preload();
        }
        uint32_t p = preload();
        if (p == 0)
        {
            p = 0x10000;
        }
        uint64_t countdown = static_cast<uint64_t>(p) * SYSCLK_HZ / DUART_CLOCK;
        if (countdown == 0)
        {
            return 0;
        }
        uint64_t elapsed = (clock_now >= ctr_start_clock)
                               ? (clock_now - ctr_start_clock) : 0;
        uint64_t phase = elapsed % countdown;              // 0 .. countdown-1
        uint64_t remaining = countdown - phase;            // countdown .. 1
        return static_cast<uint16_t>(static_cast<uint64_t>(p) * remaining / countdown);
    }

    // CUR (0x0D) latches the live count and returns its high byte; CLR (0x0F)
    // returns the low byte of the value latched by the preceding CUR read.
    uint8_t read_counter_byte(uint32_t abs, uint64_t clock_now)
    {
        if (abs == DUART_CUR)
        {
            ctr_latch = live_count(clock_now);
            return static_cast<uint8_t>(ctr_latch >> 8);
        }
        return static_cast<uint8_t>(ctr_latch & 0xFF);
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

    // Compute ISR from live state (a = channel A console, b = channel B)
    uint8_t isr(const PTYConsole &a, const PTYConsole &b) const
    {
        uint8_t val = 0;
        if (tx_enabled)
        {
            val |= DUART_ISR_TXRDYA_MASK;   // TX always ready
        }
        if (rx_enabled && a.is_data_ready())
        {
            val |= DUART_ISR_RXRDYA_MASK;
        }
        if (tx_enabled_b)
        {
            val |= DUART_ISR_TXRDYB_MASK;
        }
        if (rx_enabled_b && b.is_data_ready())
        {
            val |= DUART_ISR_RXRDYB_MASK;
        }
        if (ctr_ready)
        {
            val |= DUART_ISR_CTR_READY_MASK;
        }
        return val;
    }

    bool irq_pending(const PTYConsole &a, const PTYConsole &b) const
    {
        return (isr(a, b) & imr) != 0;
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

    uint8_t read_reg(uint32_t abs_addr, const PTYConsole &pty,
                     const PTYConsole &pty_b)
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
            case DUART_ISR:     return isr(pty, pty_b);
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
            case DUART_SRB:
            {
                uint8_t val = 0;
                if (rx_enabled_b && pty_b.is_data_ready())
                {
                    val |= DUART_SRA_RXRDY_MASK;    // SRB shares SRA's layout
                }
                if (tx_enabled_b)
                {
                    val |= DUART_SRA_TXRDY_MASK | DUART_SRA_TXEMT_MASK;
                }
                return val;
            }
            case DUART_RBB:
            {
                uint8_t ch = 0;
                if (rx_enabled_b)
                {
                    pty_b.receive(&ch);
                }
                return ch;
            }
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

    void write_reg(uint32_t abs_addr, uint8_t val, const PTYConsole &pty,
                   const PTYConsole &pty_b)
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
            case DUART_TBB:
                if (tx_enabled_b)
                {
                    pty_b.send(val);
                }
                break;
            case DUART_IVR:     ivr = val; break;
            case DUART_OPCR:    opcr = val; break;
            case DUART_OPR_SET: opr |= val; break;
            case DUART_OPR_CLR: opr &= ~val; break;
        }
    }
};

// R4G4B4 to ARGB8888.  Nibble replication (x17) is the DAC's own transfer
// function, and it is the same rule super-engine's PPM writer uses, so a
// --screenshot and a suite artifact of the same list are directly comparable.
// (r3g3b2_to_argb is gone with rev-1 VIDEO: the pipeline is 12-bit end to end.)
static uint32_t rgb444_to_argb(SuperEngine::Rgb444 c)
{
    const uint8_t r8 = SuperEngine::rgb444_channel_to_8(SuperEngine::rgb444_r(c));
    const uint8_t g8 = SuperEngine::rgb444_channel_to_8(SuperEngine::rgb444_g(c));
    const uint8_t b8 = SuperEngine::rgb444_channel_to_8(SuperEngine::rgb444_b(c));
    return 0xFF000000u | (uint32_t(r8) << 16) | (uint32_t(g8) << 8) | b8;
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

    uint64_t clock_next_line = 0;
    uint64_t frac_accum = 0;
    int v_cnt = 0;

    // Free-running count of scanlines emitted.  TIMING toggles PADDLE_TICK and
    // AUDIO_TICK once per scanline (at h_cnt == 0, from reset, with no
    // frame-boundary exception), and PORTS counts their falling edges, so the
    // delta of this counter is PORTS' entire timebase: its paddle and audio
    // rates cannot drift from the raster the way a private timer would.
    uint64_t line_count = 0;

    // TIMING's vsync pulse is latched in GLUE now, not in VIDEO — there is no
    // VIDEO peripheral any more and PIXEL/COMPOSITOR have no bus at all.  The
    // latch lives here because this is where vsync is generated; GLUE's
    // VSYNC_STATUS/VSYNC_CLEAR registers read and W1C it (see IO_read8).
    bool vsync_pending = false;

    SDL_Window *window = nullptr;
    SDL_Renderer *renderer = nullptr;
    SDL_Texture *texture = nullptr;
    bool sdl_ok = false;
    bool headless = false;          // skip SDL window; framebuffer still filled
    bool want_pixels = true;        // false => nobody views the fb: skip PIXEL,
                                     // COMPOSITOR and the ARGB conversion
                                     // (headless with no screenshot).  The
                                     // descriptor walk, its DMA stall and the
                                     // audio deposits still run, so guest
                                     // timing is unchanged — see service_video.
    std::vector<uint32_t> framebuffer;

    bool init()
    {
        // The framebuffer and scanline timing must exist even when no SDL
        // window is created (headless), because the DMA engine still fills it.
        framebuffer.resize(H_ACTIVE * V_ACTIVE, 0xFF000000u);

        // Seed the timing accumulator
        frac_accum = LINE_NUM;
        clock_next_line = frac_accum / LINE_DEN;
        frac_accum %= LINE_DEN;

        if (headless)
        {
            return true;
        }

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
        sdl_ok = true;

        return true;
    }

    // Save the current framebuffer to a BMP file.  Works headless — wraps the
    // existing ARGB8888 pixels in a surface, which needs no window or renderer.
    void dump_framebuffer(const char* path)
    {
        if (framebuffer.empty())
        {
            return;
        }
        SDL_Surface* surface = SDL_CreateSurfaceFrom(
            H_ACTIVE, V_ACTIVE, SDL_PIXELFORMAT_ARGB8888,
            framebuffer.data(), H_ACTIVE * sizeof(uint32_t));
        if (!surface)
        {
            fprintf(stderr, "SDL_CreateSurfaceFrom failed: %s\n", SDL_GetError());
            return;
        }
        if (!SDL_SaveBMP(surface, path))
        {
            fprintf(stderr, "SDL_SaveBMP(%s) failed: %s\n", path, SDL_GetError());
        }
        else
        {
            fprintf(stderr, "Wrote screenshot: %s\n", path);
        }
        SDL_DestroySurface(surface);
    }

    ~VideoState()
    {
        if (texture)  { SDL_DestroyTexture(texture); }
        if (renderer) { SDL_DestroyRenderer(renderer); }
        if (window)   { SDL_DestroyWindow(window); }
        if (sdl_ok)   { SDL_Quit(); }
    }

    // COMPOSITOR hands over one line of R4G4B4; this is only the DAC.
    void store_scanline(int line, const SuperEngine::Rgb444 *src)
    {
        uint32_t *row = &framebuffer[line * H_ACTIVE];
        for (int i = 0; i < H_ACTIVE; i++)
        {
            row[i] = rgb444_to_argb(src[i]);
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

    // `service_line(v_cnt, stall)` does everything one scanline needs: advance
    // the ENGINE walker over that line's HBLANK edge, run PIXEL/COMPOSITOR
    // through the blanking interval and then the 640 active slots, and store
    // the result.  It is a callback because all of that needs the emulator's
    // RAM and PORTS, which VideoState deliberately does not know about.
    template<typename ServiceFn>
    uint32_t check_timer(uint64_t clock_now, ServiceFn&& service_line)
    {
        uint32_t stall_cycles = 0;
        while (clock_now >= clock_next_line)
        {
            service_line(v_cnt, stall_cycles);

            // VSYNC IRQ: hardware latches at (v_cnt==V_SYNC_START && h_last) —
            // the END of line 490.  In this model that is the current iteration,
            // before v_cnt is advanced.  Latching after the increment (i.e. when
            // v_cnt becomes V_SYNC_START) fires one line early and shifts the
            // per-line VIDCMD packet's effect down by a scanline.
            if (v_cnt == V_SYNC_START)
            {
                vsync_pending = true;
                present_frame();
            }

            v_cnt++;
            line_count++;       // one scanline == one PADDLE_TICK/AUDIO_TICK toggle

            if (v_cnt >= V_TOTAL)
            {
                v_cnt = 0;
            }

            frac_accum += LINE_NUM;
            uint64_t advance = frac_accum / LINE_DEN;
            frac_accum %= LINE_DEN;
            clock_next_line += advance;
        }
        return stall_cycles;
    }

    // There is no VIDEO register file any more: rev-1's CTRL/CLRINT/CLRERR
    // went with the chip.  Everything that used to configure video —
    // enable, palette, mode — is now in-band as VIDCMD SET instructions, and
    // the only CPU-visible bit left is the vsync latch, which GLUE owns.
    bool irq_pending() const { return vsync_pending; }
    void clear_irq() { vsync_pending = false; }
};

// ---------------------------------------------------------------------------
// ENGINE — display-list DMA engine (ATF1508AS CPLD, cpld/engine/edma3.v).
//
// Rev-1's ENGINE streamed a fixed 42 words per scanline from {SOURCE_PAGE,
// line*stride}.  This one walks a descriptor table in RAM's top 64K: the CPU
// writes the first descriptor's word address to DESC, which arms it, and the
// engine bus-masters its way through the list, strobing each payload word into
// one of three FIFOs.  There is no read path — no STATUS, no SOURCE_PAGE — so
// reads of the region return open bus.
//
// The state machine itself is NOT reimplemented here.  SuperEngine::EngineWalker
// is that machine, shared verbatim with super-engine/, and this struct is only
// the register decode plus the emulator's per-line driver bookkeeping.
//
// KNOWN SIMPLIFICATION, in the same spirit as rev-1's DMA-lump note: the walker
// is advanced once per scanline boundary and a whole line's deposits land at
// once, rather than spreading across the bus cycles that carried them.  Lumping
// is strictly EARLIER than hardware, so it cannot manufacture a FIFO fill that
// real timing would have missed — it can only hide a late one.  Lists that are
// marginal on delivery therefore look cleaner here than on the bench; the
// suite's clock-accurate driver is where that gets checked.  Intra-line tearing
// is not modelled for the same reason.
// ---------------------------------------------------------------------------

struct EngineState
{
    // Per-descriptor bus cost comes from descriptor.h's model (arbitration +
    // assert + 4-word fetch + 2 SYSCLK/payload word + release), so the stall
    // the CPU is charged is derived from the actual list rather than from a
    // fixed per-line constant the way rev-1's WORDS_PER_LINE was.
    SuperEngine::EngineWalker walker{SuperEngine::ENGINE_ARBITRATION_CYCLES};

    // DESC carries a 15-bit WORD address inside the descriptor page; edma3.v
    // forms {8'h3F, desc_ptr} from it.
    static uint32_t desc_byte_address(uint16_t val)
    {
        return SuperEngine::DESC_TABLE_BASE |
               ((static_cast<uint32_t>(val) & ENGINE_DESC_ADDR_MASK) << 1);
    }

    // No CPU-readable state exists; GLUE answers DTACK and the bus floats.
    static constexpr uint8_t OPEN_BUS = 0xFF;
};

// ---------------------------------------------------------------------------
// PS/2 keyboard emulation — mirrors the GLUE PS/2 frame engine.
//
// GLUE assembles a whole frame and raises IRQ4 once per byte (RX_READY),
// validating parity/framing in hardware.  Here we model the device side:
// translate SDL key events into PS/2 Set 2 byte streams and deliver one
// assembled byte (RX_READY + PS2_RX_DATA) per ~1 ms as emulated time
// advances.  A PS2_TX_DATA write is treated as an instant, acknowledged
// host->device transmission (TX_DONE).
// ---------------------------------------------------------------------------

// Returns the set-2 make code, with 0xE0-prefixed (extended) keys encoded as
// 0xE0xx in the high/low bytes.  0 = unmapped.
static uint16_t sdl_to_ps2_set2(SDL_Scancode sc)
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

        case SDL_SCANCODE_LSHIFT:    return 0x12;
        case SDL_SCANCODE_RSHIFT:    return 0x59;
        case SDL_SCANCODE_LCTRL:     return 0x14;
        case SDL_SCANCODE_LALT:      return 0x11;
        case SDL_SCANCODE_CAPSLOCK:  return 0x58;

        case SDL_SCANCODE_MINUS:        return 0x4E;
        case SDL_SCANCODE_EQUALS:       return 0x55;
        case SDL_SCANCODE_LEFTBRACKET:  return 0x54;
        case SDL_SCANCODE_RIGHTBRACKET: return 0x5B;
        case SDL_SCANCODE_SEMICOLON:    return 0x4C;
        case SDL_SCANCODE_APOSTROPHE:   return 0x52;
        case SDL_SCANCODE_GRAVE:        return 0x0E;
        case SDL_SCANCODE_COMMA:        return 0x41;
        case SDL_SCANCODE_PERIOD:       return 0x49;
        case SDL_SCANCODE_SLASH:        return 0x4A;
        case SDL_SCANCODE_BACKSLASH:    return 0x5D;

        case SDL_SCANCODE_RCTRL:     return 0xE014;
        case SDL_SCANCODE_RALT:      return 0xE011;
        case SDL_SCANCODE_UP:        return 0xE075;
        case SDL_SCANCODE_DOWN:      return 0xE072;
        case SDL_SCANCODE_RIGHT:     return 0xE074;
        case SDL_SCANCODE_LEFT:      return 0xE06B;
        case SDL_SCANCODE_HOME:      return 0xE06C;
        case SDL_SCANCODE_END:       return 0xE069;
        case SDL_SCANCODE_PAGEUP:    return 0xE07D;
        case SDL_SCANCODE_PAGEDOWN:  return 0xE07A;
        case SDL_SCANCODE_INSERT:    return 0xE070;
        case SDL_SCANCODE_DELETE:    return 0xE071;

        default: return 0;
    }
}

// Odd-parity bit for x: 0 if x already has an odd number of 1s, else 1.
// Matches the firmware's odd_parity_bit() so the parity GLUE reports for
// a received byte makes the firmware's check pass.
static inline bool ps2_odd_parity_bit(uint8_t x)
{
    x ^= x >> 4;
    x ^= x >> 2;
    x ^= x >> 1;
    return ((x & 1u) ^ 1u) != 0;
}

struct PS2State;

// Device side of one PS/2 channel.  The frame engine above/below the hook is
// identical for both channels; only the attached device's command set and
// its self-initiated traffic differ.
struct PS2Device
{
    virtual ~PS2Device() = default;

    // Host->device byte (a PS2_TX_DATA write).  Responses are queued back
    // through the channel's normal RX pacing.
    virtual void command(uint64_t clock_now, uint8_t byte, PS2State &channel) = 0;

    // Device-initiated traffic, polled from the channel's timer service.  A
    // keyboard is purely reactive and needs nothing here; a streaming mouse
    // emits report packets on its own sample-rate cadence.
    virtual void service(uint64_t clock_now, PS2State &channel)
    {
        (void)clock_now;
        (void)channel;
    }
};

// Frame-level PS/2 model, mirroring the GLUE PS/2 frame engine: GLUE
// assembles whole frames, so we deliver one full byte (RX_READY +
// PS2_RX_DATA) per ~1 ms and treat a PS2_TX_DATA write as an instant,
// always-acknowledged host->device transmission (TX_DONE).
//
// The engine is address-agnostic: griffin.yml deliberately gives the PORTS
// mouse channel the same register offsets as the GLUE keyboard channel, so
// one instance serves either by taking the channel base address.  The device
// behaviour hangs off the PS2Device hook.
struct PS2State
{
    static constexpr uint64_t byte_period_sysclk = SYSCLK_HZ / 1000; // ~1 ms

    // Channel register offsets.  Taken from the GLUE keyboard channel and
    // asserted equal to the PORTS mouse channel's, so the shared layout that
    // makes one engine serve both is checked by the compiler, not by comment.
    static constexpr uint32_t OFFSET_TX_DATA = GLUE_PS2_TX_DATA - GLUE_BASE;
    static constexpr uint32_t OFFSET_STATUS  = GLUE_PS2_STATUS  - GLUE_BASE;
    static constexpr uint32_t OFFSET_CLEAR   = GLUE_PS2_CLEAR   - GLUE_BASE;
    static constexpr uint32_t OFFSET_CTRL    = GLUE_PS2_CTRL    - GLUE_BASE;
    static constexpr uint32_t OFFSET_RX_DATA = GLUE_PS2_RX_DATA - GLUE_BASE;
    static_assert(PORTS_PS2_MOUSE_TX_DATA - PORTS_BASE == OFFSET_TX_DATA);
    static_assert(PORTS_PS2_MOUSE_STATUS  - PORTS_BASE == OFFSET_STATUS);
    static_assert(PORTS_PS2_MOUSE_CLEAR   - PORTS_BASE == OFFSET_CLEAR);
    static_assert(PORTS_PS2_MOUSE_CTRL    - PORTS_BASE == OFFSET_CTRL);
    static_assert(PORTS_PS2_MOUSE_RX_DATA - PORTS_BASE == OFFSET_RX_DATA);

    uint32_t   base = GLUE_BASE;
    PS2Device *device = nullptr;

    std::deque<uint8_t> rx_bytes;
    uint64_t next_deliver_clock = 0;
    uint8_t  rx_data = 0;
    bool     rx_ready = false;
    bool     tx_done = false;
    // false = device acknowledged.  Resets true to match the RTL, which comes
    // up with TX_ACK set (glue.v `tx_ack <= 1'b1`, ports.v
    // `transmit_acknowledge <= 1'b1`) so an idle channel does not claim an
    // acknowledgement that never happened.
    bool     tx_ack_failed = true;
    uint8_t  ctrl = 0;

    bool init(uint32_t channel_base, PS2Device &attached)
    {
        base = channel_base;
        device = &attached;
        return true;
    }

    // Queue a device->host byte for timed delivery.
    void enqueue_byte(uint64_t clock_now, uint8_t byte)
    {
        if (rx_bytes.empty())
        {
            next_deliver_clock = clock_now + byte_period_sysclk;
        }
        rx_bytes.push_back(byte);
    }

    // Bytes still waiting to be shifted in.  A device that generates its own
    // traffic uses this to throttle itself instead of growing the queue
    // without bound when the guest is not draining the channel.
    size_t backlog() const { return rx_bytes.size(); }

    void check_timer(uint64_t clock_now)
    {
        if (device)
        {
            device->service(clock_now, *this);
        }
        if (!rx_ready && !rx_bytes.empty()
            && clock_now >= next_deliver_clock)
        {
            rx_data = rx_bytes.front();
            rx_bytes.pop_front();
            rx_ready = true;
            if (!rx_bytes.empty())
            {
                next_deliver_clock = clock_now + byte_period_sysclk;
            }
        }
    }

    // True for any address this channel decodes, including the parity-1 alias
    // of PS2_TX_DATA (parity rides address bit 1).
    bool is_addr(uint32_t abs_addr) const
    {
        uint32_t offset = abs_addr - base;
        return offset == OFFSET_TX_DATA
            || offset == OFFSET_TX_DATA + PS2_TX_DATA_PARITY
            || offset == OFFSET_STATUS
            || offset == OFFSET_CTRL
            || offset == OFFSET_RX_DATA;
    }

    uint8_t read_reg(uint32_t abs_addr) const
    {
        uint32_t offset = abs_addr - base;
        if (offset == OFFSET_STATUS)
        {
            uint8_t v = 0;
            if (rx_ready)      { v |= GLUE_PS2_STATUS_RX_READY_MASK; }
            if (tx_done)       { v |= GLUE_PS2_STATUS_TX_DONE_MASK; }
            if (tx_ack_failed) { v |= GLUE_PS2_STATUS_TX_ACK_MASK; }
            if (rx_ready && ps2_odd_parity_bit(rx_data))
            {
                v |= GLUE_PS2_STATUS_RX_PARITY_MASK;
            }
            // Idle-high live pin bits (debug).
            v |= GLUE_PS2_STATUS_DATA_LIVE_MASK;
            v |= GLUE_PS2_STATUS_CLK_LIVE_MASK;
            return v;
        }
        if (offset == OFFSET_RX_DATA)
        {
            return rx_data;
        }
        return 0;
    }

    void write_reg(uint32_t abs_addr, uint8_t val, uint64_t clock_now)
    {
        uint32_t offset = abs_addr - base;
        if (offset == OFFSET_CLEAR)
        {
            if (val & GLUE_PS2_CLEAR_RX_READY_MASK) { rx_ready = false; }
            if (val & GLUE_PS2_CLEAR_TX_DONE_MASK)  { tx_done = false; }
        }
        else if (offset == OFFSET_CTRL)
        {
            ctrl = val;
        }
        else if (offset == OFFSET_TX_DATA
                 || offset == OFFSET_TX_DATA + PS2_TX_DATA_PARITY)
        {
            // PS2_TX_DATA (0x09) or its parity-1 alias (0x0B): instant,
            // always-acked host->device transmission; the attached device
            // then responds to the command.
            tx_ack_failed = false;
            tx_done = true;
            if (device)
            {
                device->command(clock_now, val, *this);
            }
        }
    }

    bool irq_pending() const { return rx_ready || tx_done; }
};

// Generic PS/2 keyboard: host commands get real responses (0xFA ACK, ident
// bytes, BAT after reset) so guests that fully initialize the keyboard (Linux
// atkbd probe, u-boot's enable/LED writes) behave as they will on hardware.
// Responses ride the channel's 1 ms/byte RX pacing along with scancodes.
struct PS2Keyboard : PS2Device
{
    // Reset -> BAT completion delay.  Real keyboards take 500-750 ms; use
    // 300 ms so atkbd's reset timeout (guest jiffies) is comfortably met
    // while bounded test runs stay short.
    static constexpr uint64_t bat_delay_sysclk = SYSCLK_HZ * 3 / 10;

    uint64_t bat_due_clock = 0;        // nonzero: BAT (0xAA) pending after reset
    uint8_t  pending_cmd = 0;          // 0xED/0xF3 awaiting their data byte
    uint8_t  leds = 0;
    uint8_t  typematic = 0;
    bool     scanning = true;

    void service(uint64_t clock_now, PS2State &channel) override
    {
        if (bat_due_clock != 0 && clock_now >= bat_due_clock)
        {
            bat_due_clock = 0;
            channel.enqueue_byte(clock_now, 0xAA);      // BAT passed
        }
    }

    void command(uint64_t clock_now, uint8_t byte, PS2State &channel) override
    {
        if (pending_cmd == 0xED)        // set LEDs: data byte
        {
            leds = byte;
            pending_cmd = 0;
            channel.enqueue_byte(clock_now, 0xFA);
            return;
        }
        if (pending_cmd == 0xF3)        // set typematic: data byte
        {
            typematic = byte;
            pending_cmd = 0;
            channel.enqueue_byte(clock_now, 0xFA);
            return;
        }
        switch (byte)
        {
            case 0xFF:                  // reset
                channel.rx_bytes.clear();
                pending_cmd = 0;
                leds = 0;
                scanning = true;
                channel.enqueue_byte(clock_now, 0xFA);
                bat_due_clock = clock_now + bat_delay_sysclk;
                break;
            case 0xF2:                  // read ID: MF2 keyboard
                channel.enqueue_byte(clock_now, 0xFA);
                channel.enqueue_byte(clock_now, 0xAB);
                channel.enqueue_byte(clock_now, 0x83);
                break;
            case 0xED:                  // set LEDs (data byte follows)
            case 0xF3:                  // set typematic (data byte follows)
                pending_cmd = byte;
                channel.enqueue_byte(clock_now, 0xFA);
                break;
            case 0xF4:                  // enable scanning
                scanning = true;
                channel.enqueue_byte(clock_now, 0xFA);
                break;
            case 0xF5:                  // disable scanning
                scanning = false;
                channel.enqueue_byte(clock_now, 0xFA);
                break;
            case 0xEE:                  // echo
                channel.enqueue_byte(clock_now, 0xEE);
                break;
            default:                    // unrecognized: resend
                channel.enqueue_byte(clock_now, 0xFE);
                break;
        }
    }
};

// Standard PS/2 mouse with the Microsoft IntelliMouse extension.  Unlike the
// keyboard it is not purely reactive: once data reporting is enabled in
// stream mode it emits 3- (or 4-, with the wheel) byte movement packets at
// its programmed sample rate whenever the accumulated state has changed.
struct PS2Mouse : PS2Device
{
    // Reset -> BAT completion delay, same rationale as the keyboard's.
    static constexpr uint64_t bat_delay_sysclk = SYSCLK_HZ * 3 / 10;

    // Post-reset defaults from the PS/2 mouse spec.
    static constexpr uint8_t DEFAULT_SAMPLE_RATE = 100;   // reports/second
    static constexpr uint8_t DEFAULT_RESOLUTION  = 2;     // 4 counts/mm

    // Device IDs: 0x00 = plain 3-button, 0x03 = IntelliMouse with a wheel.
    static constexpr uint8_t ID_STANDARD    = 0x00;
    static constexpr uint8_t ID_INTELLIMOUSE = 0x03;

    // Button bits, in PS/2 packet-byte-0 order.
    static constexpr uint8_t BUTTON_LEFT   = 0x01;
    static constexpr uint8_t BUTTON_RIGHT  = 0x02;
    static constexpr uint8_t BUTTON_MIDDLE = 0x04;

    // Movement fields are 9-bit two's complement: 8 bits in the data byte and
    // the sign in packet byte 0.
    static constexpr int MOVE_MIN = -256;
    static constexpr int MOVE_MAX = 255;

    // Don't pile up packets the guest is not draining; the channel delivers
    // one byte per millisecond, so a slow ISR must be allowed to fall behind
    // exactly as a real mouse would be inhibited.
    static constexpr size_t MAX_BACKLOG = PS2_RX_QUEUE_SIZE;

    uint8_t  sample_rate = DEFAULT_SAMPLE_RATE;
    uint8_t  resolution = DEFAULT_RESOLUTION;
    bool     scaling_2_1 = false;
    bool     stream_mode = true;       // false = remote (host polls with 0xEB)
    bool     reporting = false;        // data reporting enabled (0xF4)
    uint8_t  device_id = ID_STANDARD;
    uint8_t  pending_cmd = 0;          // 0xF3/0xE8 awaiting their data byte
    uint64_t bat_due_clock = 0;
    uint64_t next_report_clock = 0;

    // IntelliMouse "knock": set-sample-rate 200, 100, 80 in a row switches the
    // device to ID 3 and 4-byte packets with a wheel.
    int knock_stage = 0;

    // Host-side state waiting to be turned into packets.
    int     accum_dx = 0;
    int     accum_dy = 0;              // PS/2 convention: positive is up
    int     accum_dz = 0;              // wheel detents
    uint8_t buttons = 0;
    bool    state_changed = false;

    uint64_t report_period_sysclk() const
    {
        uint8_t rate = sample_rate != 0 ? sample_rate : DEFAULT_SAMPLE_RATE;
        return SYSCLK_HZ / rate;
    }

    void set_defaults()
    {
        sample_rate = DEFAULT_SAMPLE_RATE;
        resolution = DEFAULT_RESOLUTION;
        scaling_2_1 = false;
        stream_mode = true;
        reporting = false;
        knock_stage = 0;
    }

    // --- host-side input, from SDL or the --mouse-in script ---
    void move(int dx, int dy, int dz = 0)
    {
        accum_dx += dx;
        accum_dy += dy;
        accum_dz += dz;
        state_changed = true;
    }

    void set_buttons(uint8_t mask)
    {
        buttons = mask & (BUTTON_LEFT | BUTTON_RIGHT | BUTTON_MIDDLE);
        state_changed = true;
    }

    static int clamp_move(int v)
    {
        return v < MOVE_MIN ? MOVE_MIN : (v > MOVE_MAX ? MOVE_MAX : v);
    }

    void send_packet(uint64_t clock_now, PS2State &channel)
    {
        int dx = clamp_move(accum_dx);
        int dy = clamp_move(accum_dy);
        uint8_t byte0 = 0x08 | (buttons & 0x07)
                      | ((dx < 0) ? 0x10 : 0)
                      | ((dy < 0) ? 0x20 : 0)
                      | ((accum_dx != dx) ? 0x40 : 0)   // X overflow
                      | ((accum_dy != dy) ? 0x80 : 0);  // Y overflow
        accum_dx -= dx;
        accum_dy -= dy;
        channel.enqueue_byte(clock_now, byte0);
        channel.enqueue_byte(clock_now, static_cast<uint8_t>(dx & 0xFF));
        channel.enqueue_byte(clock_now, static_cast<uint8_t>(dy & 0xFF));
        if (device_id == ID_INTELLIMOUSE)
        {
            int dz = accum_dz < -8 ? -8 : (accum_dz > 7 ? 7 : accum_dz);
            accum_dz -= dz;
            channel.enqueue_byte(clock_now, static_cast<uint8_t>(dz & 0x0F));
        }
        else
        {
            accum_dz = 0;   // no wheel in the packet: discard it
        }
        state_changed = (accum_dx != 0) || (accum_dy != 0) || (accum_dz != 0);
    }

    void service(uint64_t clock_now, PS2State &channel) override
    {
        if (bat_due_clock != 0 && clock_now >= bat_due_clock)
        {
            bat_due_clock = 0;
            channel.enqueue_byte(clock_now, 0xAA);          // BAT passed
            channel.enqueue_byte(clock_now, device_id);     // then the ID
        }
        if (!reporting || !stream_mode)
        {
            return;
        }
        if (clock_now < next_report_clock)
        {
            return;
        }
        next_report_clock = clock_now + report_period_sysclk();
        if (!state_changed || channel.backlog() >= MAX_BACKLOG)
        {
            return;
        }
        send_packet(clock_now, channel);
    }

    void command(uint64_t clock_now, uint8_t byte, PS2State &channel) override
    {
        if (pending_cmd == 0xF3)         // set sample rate: data byte
        {
            sample_rate = byte;
            pending_cmd = 0;
            // IntelliMouse knock: 200, 100, 80 in that order.
            if (byte == 200)      { knock_stage = 1; }
            else if (byte == 100) { knock_stage = (knock_stage == 1) ? 2 : 0; }
            else if (byte == 80)
            {
                if (knock_stage == 2) { device_id = ID_INTELLIMOUSE; }
                knock_stage = 0;
            }
            else                  { knock_stage = 0; }
            channel.enqueue_byte(clock_now, 0xFA);
            return;
        }
        if (pending_cmd == 0xE8)         // set resolution: data byte
        {
            resolution = byte & 0x03;
            pending_cmd = 0;
            channel.enqueue_byte(clock_now, 0xFA);
            return;
        }
        switch (byte)
        {
            case 0xFF:                   // reset
                channel.rx_bytes.clear();
                pending_cmd = 0;
                set_defaults();
                device_id = ID_STANDARD;
                accum_dx = accum_dy = accum_dz = 0;
                buttons = 0;
                state_changed = false;
                channel.enqueue_byte(clock_now, 0xFA);
                bat_due_clock = clock_now + bat_delay_sysclk;
                break;
            case 0xF6:                   // set defaults
                set_defaults();
                channel.enqueue_byte(clock_now, 0xFA);
                break;
            case 0xF5:                   // disable data reporting
                reporting = false;
                channel.enqueue_byte(clock_now, 0xFA);
                break;
            case 0xF4:                   // enable data reporting
                reporting = true;
                next_report_clock = clock_now + report_period_sysclk();
                channel.enqueue_byte(clock_now, 0xFA);
                break;
            case 0xF3:                   // set sample rate (data byte follows)
            case 0xE8:                   // set resolution (data byte follows)
                pending_cmd = byte;
                channel.enqueue_byte(clock_now, 0xFA);
                break;
            case 0xF2:                   // get device ID
                channel.enqueue_byte(clock_now, 0xFA);
                channel.enqueue_byte(clock_now, device_id);
                break;
            case 0xF0:                   // set remote (polled) mode
                stream_mode = false;
                channel.enqueue_byte(clock_now, 0xFA);
                break;
            case 0xEA:                   // set stream mode
                stream_mode = true;
                next_report_clock = clock_now + report_period_sysclk();
                channel.enqueue_byte(clock_now, 0xFA);
                break;
            case 0xEB:                   // read data (one packet on demand)
                channel.enqueue_byte(clock_now, 0xFA);
                send_packet(clock_now, channel);
                break;
            case 0xE9:                   // status request
                channel.enqueue_byte(clock_now, 0xFA);
                channel.enqueue_byte(clock_now,
                    static_cast<uint8_t>((reporting ? 0x20 : 0)
                                         | (stream_mode ? 0 : 0x40)
                                         | (scaling_2_1 ? 0x10 : 0)
                                         | (buttons & 0x07)));
                channel.enqueue_byte(clock_now, resolution);
                channel.enqueue_byte(clock_now, sample_rate);
                break;
            case 0xE7:                   // set scaling 2:1
                scaling_2_1 = true;
                channel.enqueue_byte(clock_now, 0xFA);
                break;
            case 0xE6:                   // set scaling 1:1
                scaling_2_1 = false;
                channel.enqueue_byte(clock_now, 0xFA);
                break;
            default:                     // unrecognized: resend
                channel.enqueue_byte(clock_now, 0xFE);
                break;
        }
    }
};

// ---------------------------------------------------------------------------
// Audio sink — host capture (--wav-out) and host playback.
//
// PORTS' two R2R DACs are loaded by the FIFO pop strobe at
// AUDIO_SAMPLES_PER_SECOND and hold their value between pops, so a faithful
// recording is "whatever the output registers hold, sampled once per pop
// tick" — including the ticks where the FIFO was empty and the DAC simply
// held its last sample.  That makes the sample rate a property of the raster
// line rate alone (TIMING's AUDIO_TICK, one pop per two lines), so a capture
// is exactly as deterministic as the run.
//
// This is a sink only: nothing here is ever read back into the emulation, so
// neither the capture nor the playback can perturb guest timing.  Playback
// therefore never blocks — if the host device queue backs up (it always will
// under --no-throttle, where the guest outruns real time) the samples are
// dropped, not waited on.
// ---------------------------------------------------------------------------

class AudioOutput
{
public:
    ~AudioOutput() { close(); }

    // Canonical 44-byte PCM WAV, 8-bit unsigned (the DAC's own format, so the
    // samples are stored verbatim), stereo.  The header is rewritten at close
    // with the final frame count.
    bool open_wav(const char *path)
    {
        wav = fopen(path, "wb");
        if (!wav)
        {
            fprintf(stderr, "--wav-out: cannot open %s for writing\n", path);
            return false;
        }
        wav_path = path;
        wav_frames = 0;
        write_wav_header();
        return true;
    }

    // Best-effort host playback: any failure is a single warning, never fatal.
    // SDL is asked for the DAC's own format and rate and converts as needed.
    void open_playback()
    {
        if (!SDL_InitSubSystem(SDL_INIT_AUDIO))
        {
            fprintf(stderr, "Warning: SDL audio init failed (%s); no sound\n",
                    SDL_GetError());
            return;
        }
        SDL_AudioSpec spec = {};
        spec.format = SDL_AUDIO_U8;
        spec.channels = CHANNELS;
        spec.freq = SAMPLE_RATE_HZ;
        stream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK,
                                           &spec, nullptr, nullptr);
        if (!stream)
        {
            fprintf(stderr, "Warning: SDL_OpenAudioDeviceStream failed (%s); "
                    "no sound\n", SDL_GetError());
            SDL_QuitSubSystem(SDL_INIT_AUDIO);
            return;
        }
        SDL_ResumeAudioStreamDevice(stream);
    }

    bool active() const { return wav != nullptr || stream != nullptr; }

    // One DAC update, called from the PORTS pop strobe.
    void sample(uint8_t left, uint8_t right)
    {
        if (!active())
        {
            return;
        }
        pending[pending_bytes++] = left;
        pending[pending_bytes++] = right;
        if (wav)
        {
            wav_frames++;
        }
        if (pending_bytes >= pending.size())
        {
            flush();
        }
    }

    void close()
    {
        flush();
        if (stream)
        {
            SDL_DestroyAudioStream(stream);   // also closes the bound device
            stream = nullptr;
            SDL_QuitSubSystem(SDL_INIT_AUDIO);
        }
        if (wav)
        {
            fseek(wav, 0, SEEK_SET);
            write_wav_header();
            fclose(wav);
            wav = nullptr;
            fprintf(stderr, "Wrote %" PRIu32 " audio frames: %s\n",
                    wav_frames, wav_path);
        }
    }

private:
    static constexpr int SAMPLE_RATE_HZ = static_cast<int>(AUDIO_SAMPLES_PER_SECOND);
    static constexpr int CHANNELS = 2;
    static constexpr uint32_t BYTES_PER_FRAME = CHANNELS;    // u8 per channel

    // Roughly a quarter second of host queue.  Past that the device is behind
    // the guest and catching up would only add latency, so drop instead.
    static constexpr int QUEUE_LIMIT_BYTES = SAMPLE_RATE_HZ * BYTES_PER_FRAME / 4;

    FILE *wav = nullptr;
    const char *wav_path = nullptr;
    uint32_t wav_frames = 0;
    SDL_AudioStream *stream = nullptr;
    bool drop_reported = false;
    std::array<uint8_t, 2048> pending{};
    size_t pending_bytes = 0;

    void flush()
    {
        if (pending_bytes == 0)
        {
            return;
        }
        if (wav)
        {
            fwrite(pending.data(), 1, pending_bytes, wav);
        }
        if (stream)
        {
            if (SDL_GetAudioStreamQueued(stream) > QUEUE_LIMIT_BYTES)
            {
                if (!drop_reported)
                {
                    drop_reported = true;
                    fprintf(stderr, "Audio: host queue behind the guest, "
                            "dropping samples\n");
                }
            }
            else
            {
                SDL_PutAudioStreamData(stream, pending.data(),
                                       static_cast<int>(pending_bytes));
            }
        }
        pending_bytes = 0;
    }

    void put_u32(uint32_t v)
    {
        uint8_t b[4] = {static_cast<uint8_t>(v), static_cast<uint8_t>(v >> 8),
                        static_cast<uint8_t>(v >> 16), static_cast<uint8_t>(v >> 24)};
        fwrite(b, 1, sizeof(b), wav);
    }

    void put_u16(uint16_t v)
    {
        uint8_t b[2] = {static_cast<uint8_t>(v), static_cast<uint8_t>(v >> 8)};
        fwrite(b, 1, sizeof(b), wav);
    }

    void write_wav_header()
    {
        uint32_t data_bytes = wav_frames * BYTES_PER_FRAME;
        fwrite("RIFF", 1, 4, wav);
        put_u32(36 + data_bytes);
        fwrite("WAVEfmt ", 1, 8, wav);
        put_u32(16);                                    // PCM fmt chunk size
        put_u16(1);                                     // PCM
        put_u16(CHANNELS);
        put_u32(SAMPLE_RATE_HZ);
        put_u32(SAMPLE_RATE_HZ * BYTES_PER_FRAME);      // byte rate
        put_u16(BYTES_PER_FRAME);                       // block align
        put_u16(8);                                     // bits per sample
        fwrite("data", 1, 4, wav);
        put_u32(data_bytes);
    }
};

// ---------------------------------------------------------------------------
// PORTS — the fourth ATF1508AS CPLD (cpld/ports/ports.v), at PORTS_BASE.
//
// Byte-wide registers, zero wait states (GLUE decodes the region and answers
// DTACK for it, as it does for CF), and a single wire-ORed level-2 IRQ.  It
// holds a PS/2 mouse frame engine, two joystick ports, two paddle counters
// with a shared dump control, and the audio FIFO pop strobe with its
// half-full IRQ latch.
//
// PORTS has NO timebase of its own: everything periodic runs off the two
// square waves TIMING emits for it, PADDLE_TICK and AUDIO_TICK.  Each toggles
// once per scanline and PORTS counts only their FALLING edges (a both-edges
// detector does not fit in the CPLD), so each consumer sees one event every
// two lines: paddle steps at 15.734 kHz -- half the old line-rate stepping,
// deliberately, because full scale 255 counts is then 16.2 ms, one frame,
// which is the Atari 2600's scanline-counted pot measurement window -- and
// audio pops at 15.734 kHz, unchanged (AUDIO_SAMPLES_PER_SECOND).  The two
// ticks are identical today, so one shared phase bit below serves both and
// paddle steps land on the same lines as audio pops.  Here that means
// advance_scanlines() is driven from VideoState's scanline counter, never
// from a private timer -- a private timer would drift against the raster and
// destroy the design property.  The mouse channel is the exception: it is in
// the SYSCLK domain and rides the existing per-instruction timer service.
// ---------------------------------------------------------------------------

// One paddle: an 8-bit upcounter advanced by each PADDLE_TICK falling edge
// (one per two scanlines) while the pot's comparator sense line is still low,
// saturating at 0xFF and held at zero while PADDLE_CONTROL.DUMP drives the
// discharge FET.  This models what the RTL does, not RC physics: the host
// supplies a knob position in the same units as the count, and the sense line
// goes high once the ramp reaches it, which freezes the counter there.
struct PaddleCounter
{
    uint8_t position = 0;      // host knob position, 0..255 tick times of ramp
    uint8_t count = 0;

    // The comparator output as PORTS sees it: low while the cap is still
    // charging (dump asserted, or the ramp has not reached the knob yet).
    bool sense_high(bool dump) const { return !dump && count >= position; }

    void tick(bool dump)
    {
        if (dump)
        {
            count = 0;
            return;
        }
        if (count == 0xFF || sense_high(dump))
        {
            return;             // saturated, or frozen at the crossing
        }
        count++;
    }
};

struct PortsState
{
    // --- PS/2 mouse channel (same engine as the GLUE keyboard) ---
    PS2State mouse;
    PS2Mouse mouse_device;

    // --- joystick ports: 1 bits = switch closed (registers read active low) ---
    static constexpr int JOYSTICK_PORTS = 2;
    uint8_t joystick_pressed[JOYSTICK_PORTS] = {0, 0};

    // --- paddles ---
    PaddleCounter paddle_a;
    PaddleCounter paddle_b;
    bool paddle_dump = false;

    // --- audio FIFO (the 7202 pair) and its pop strobe ---
    std::deque<uint16_t> audio_fifo;    // one entry per stereo pair, L<<8 | R
    bool audio_enable = false;
    bool audio_hf_irq = false;          // latched, W1C via AUDIO_CONTROL
    bool audio_half_full_prev = false;  // starts empty: no edge at boot
    bool tick_phase = false;            // level of TIMING's ticks: they toggle
                                        // once per line and the falling edge
                                        // (phase 1 -> 0) is the event
    uint8_t dac_left = 0;
    uint8_t dac_right = 0;              // 7202 output registers hold last sample

    // Optional host sink for what the DACs present (--wav-out / playback).
    // Write-only from here: PORTS never reads it back, so attaching one cannot
    // change guest behaviour or timing.
    AudioOutput *audio_out = nullptr;

    bool init()
    {
        return mouse.init(PORTS_BASE, mouse_device);
    }

    // --- audio FIFO ------------------------------------------------------

    bool audio_half_full() const { return audio_fifo.size() >= AUDIO_FIFO_DEPTH / 2; }

    // PORTS latches HF_IRQ on the falling edge of "half full or more" (the
    // rising edge of the active-low /HF pin), meaning the FIFO just drained
    // below half and firmware should refill.
    void update_half_full_edge()
    {
        bool half_full = audio_half_full();
        if (audio_half_full_prev && !half_full)
        {
            audio_hf_irq = true;
        }
        audio_half_full_prev = half_full;
    }

    void audio_fifo_push(uint16_t pair)
    {
        if (audio_fifo.size() >= AUDIO_FIFO_DEPTH)
        {
            return;             // 7202 full: the write is swallowed
        }
        audio_fifo.push_back(pair);
        update_half_full_edge();
    }

    void audio_fifo_pop()
    {
        if (audio_fifo.empty())
        {
            return;             // output register holds the last sample
        }
        uint16_t pair = audio_fifo.front();
        audio_fifo.pop_front();
        dac_left = static_cast<uint8_t>(pair >> 8);
        dac_right = static_cast<uint8_t>(pair & 0xFF);
    }

    // --- PADDLE_TICK / AUDIO_TICK ----------------------------------------

    // Advance by `lines` VGA scanlines and deliver the tick events they carry.
    // TIMING toggles both ticks at the start of every line, PORTS counts the
    // falling edges, so an event lands at the start of every even-numbered
    // line counted from reset: `tick_phase` carries the tick's current level
    // between calls and the count of falling edges crossed is just the number
    // of times that level returns to 0.  PADDLE_TICK and AUDIO_TICK are the
    // same waveform today, so one event stream feeds both consumers; if TIMING
    // ever divides the audio rate down, this is where the two split.
    void advance_scanlines(uint32_t lines)
    {
        if (lines == 0)
        {
            return;
        }
        uint32_t phased = lines + (tick_phase ? 1u : 0u);
        uint32_t events = phased / 2;
        tick_phase = (phased & 1u) != 0;
        if (events == 0)
        {
            return;
        }

        // Paddle counters saturate, so more than a full-scale ramp of events
        // can never change them further.
        uint32_t paddle_steps = std::min<uint32_t>(events, 256);
        for (uint32_t i = 0; i < paddle_steps; i++)
        {
            paddle_a.tick(paddle_dump);
            paddle_b.tick(paddle_dump);
        }

        for (uint32_t i = 0; i < events; i++)
        {
            if (audio_enable)
            {
                audio_fifo_pop();
            }
            // The DACs are sampled on every pop tick whether or not the pop
            // strobe is enabled and whether or not the FIFO had anything: the
            // converters are always converting whatever their registers hold.
            // That keeps the capture rate exactly AUDIO_SAMPLES_PER_SECOND.
            if (audio_out)
            {
                audio_out->sample(dac_left, dac_right);
            }
        }
        if (audio_enable)
        {
            update_half_full_edge();
        }
    }

    // --- SYSCLK-domain service (mouse only) ------------------------------

    void check_timer(uint64_t clock_now) { mouse.check_timer(clock_now); }

    // --- registers -------------------------------------------------------

    uint8_t joystick_byte(int port) const
    {
        // Bit 7 reads 1; switch bits are active low straight from the DE-9.
        uint8_t v = static_cast<uint8_t>(0xFF & ~joystick_pressed[port]);
        if (port == 0)
        {
            // Port 1 pins 9 and 5 double as the paddle pot comparator levels:
            // the paddle pulls the line low while its cap charges, and so does
            // a button on the same pin, so the bit is the AND of both.
            if (!paddle_a.sense_high(paddle_dump))
            {
                v &= ~PORTS_JOYSTICK_PORT_1_PIN9_MASK;
            }
            if (!paddle_b.sense_high(paddle_dump))
            {
                v &= ~PORTS_JOYSTICK_PORT_1_PIN5_MASK;
            }
        }
        return v;
    }

    uint8_t read_reg(uint32_t abs_addr) const
    {
        switch (abs_addr)
        {
            case PORTS_JOYSTICK_PORT_1: return joystick_byte(0);
            case PORTS_JOYSTICK_PORT_2: return joystick_byte(1);
            case PORTS_PADDLE_A_COUNT:  return paddle_a.count;
            case PORTS_PADDLE_B_COUNT:  return paddle_b.count;
            case PORTS_AUDIO_STATUS:
                return static_cast<uint8_t>(
                      (audio_hf_irq      ? PORTS_AUDIO_STATUS_HF_IRQ_MASK : 0)
                    | (audio_half_full() ? PORTS_AUDIO_STATUS_HALF_FULL_MASK : 0)
                    | (audio_enable      ? PORTS_AUDIO_STATUS_ENABLE_MASK : 0));
            default:
                break;
        }
        if (mouse.is_addr(abs_addr))
        {
            return mouse.read_reg(abs_addr);
        }
        return 0;
    }

    void write_reg(uint32_t abs_addr, uint8_t val, uint64_t clock_now)
    {
        if (abs_addr == PORTS_PADDLE_CONTROL)
        {
            paddle_dump = (val & PORTS_PADDLE_CONTROL_DUMP_MASK) != 0;
            if (paddle_dump)
            {
                paddle_a.count = 0;
                paddle_b.count = 0;
            }
            return;
        }
        if (abs_addr == PORTS_AUDIO_CONTROL)
        {
            audio_enable = (val & PORTS_AUDIO_CONTROL_ENABLE_MASK) != 0;
            if (val & PORTS_AUDIO_CONTROL_CLEAR_HF_IRQ_MASK)
            {
                audio_hf_irq = false;
            }
            return;
        }
        if (mouse.is_addr(abs_addr))
        {
            mouse.write_reg(abs_addr, val, clock_now);
        }
    }

    // PORTS wire-ORs its interrupt sources onto one level-2 pin.
    bool irq_pending() const { return mouse.irq_pending() || audio_hf_irq; }
};

// ---------------------------------------------------------------------------
// Scripted PS/2 keystroke injection (--ps2-in): drives the PS2State model
// from a file so keyboard-input paths can be tested unattended (headless
// runs get no SDL key events).  Directives, one per line:
//   # comment
//   delay MS         wait MS emulated milliseconds before the next directive
//   text STRING      type STRING as set-2 make/break sequences (\r, \\ escapes)
//   raw HH HH ...    inject raw set-2 bytes (e.g. "raw AA" = a stray BAT)
// Bytes go through PS2State::enqueue_byte, so the normal 1 ms/byte RX
// pacing and IRQ flow apply.  Scripts should delay past boot stages they
// don't target: whatever guest is live consumes the scancodes.
// ---------------------------------------------------------------------------

struct PS2Script
{
    struct Event
    {
        uint64_t delay_sysclk = 0;          // nonzero: pause
        std::deque<uint8_t> bytes;          // else: inject these
    };

    std::deque<Event> events;
    uint64_t resume_clock = 0;

    // ASCII -> set-2 make code (+ shift requirement).  Codes match
    // sdl_to_ps2_set2 above.
    static bool ascii_to_set2(char c, uint8_t &code, bool &shift)
    {
        static const uint8_t letters[26] = {
            0x1C,0x32,0x21,0x23,0x24,0x2B,0x34,0x33,0x43,0x3B,0x42,0x4B,0x3A,
            0x31,0x44,0x4D,0x15,0x2D,0x1B,0x2C,0x3C,0x2A,0x1D,0x22,0x35,0x1A,
        };
        static const uint8_t digits[10] = {
            0x45,0x16,0x1E,0x26,0x25,0x2E,0x36,0x3D,0x3E,0x46,
        };
        // Plain and shifted punctuation, same make code.
        struct Punct { char plain, shifted; uint8_t code; };
        static const Punct puncts[] = {
            { '-','_',0x4E }, { '=','+',0x55 }, { '[','{',0x54 },
            { ']','}',0x5B }, { '\\','|',0x5D }, { ';',':',0x4C },
            { '\'','"',0x52 }, { '`','~',0x0E }, { ',','<',0x41 },
            { '.','>',0x49 }, { '/','?',0x4A },
        };
        // Shifted digits: )!@#$%^&*(
        static const char shifted_digits[11] = ")!@#$%^&*(";

        shift = false;
        if (c >= 'a' && c <= 'z') { code = letters[c - 'a']; return true; }
        if (c >= 'A' && c <= 'Z') { code = letters[c - 'A']; shift = true; return true; }
        if (c >= '0' && c <= '9') { code = digits[c - '0']; return true; }
        switch (c)
        {
            case ' ':  code = 0x29; return true;
            case '\r': case '\n': code = 0x5A; return true;
            case '\t': code = 0x0D; return true;
            case '\b': code = 0x66; return true;
            case 0x1B: code = 0x76; return true;    // ESC
        }
        for (const Punct &p : puncts)
        {
            if (c == p.plain)   { code = p.code; return true; }
            if (c == p.shifted) { code = p.code; shift = true; return true; }
        }
        for (int i = 0; i < 10; i++)
        {
            if (c == shifted_digits[i]) { code = digits[i]; shift = true; return true; }
        }
        return false;
    }

    static void emit_char(std::deque<uint8_t> &out, char c)
    {
        uint8_t code;
        bool shift;
        if (!ascii_to_set2(c, code, shift))
        {
            fprintf(stderr, "ps2 script: no set-2 mapping for 0x%02X; skipped\n",
                    (unsigned char)c);
            return;
        }
        if (shift) { out.push_back(0x12); }             // LShift make
        out.push_back(code);                            // make
        out.push_back(0xF0); out.push_back(code);       // break
        if (shift) { out.push_back(0xF0); out.push_back(0x12); }
    }

    bool load(const char *path)
    {
        FILE *fp = fopen(path, "r");
        if (!fp)
        {
            fprintf(stderr, "ps2 script: cannot open %s\n", path);
            return false;
        }
        char line[1024];
        int lineno = 0;
        bool ok = true;
        while (fgets(line, sizeof line, fp))
        {
            lineno++;
            char *p = line;
            while (*p == ' ' || *p == '\t') { p++; }
            size_t len = strlen(p);
            while (len > 0 && (p[len-1] == '\n' || p[len-1] == '\r')) { p[--len] = '\0'; }
            if (*p == '\0' || *p == '#') { continue; }

            if (strncmp(p, "delay ", 6) == 0)
            {
                Event ev;
                ev.delay_sysclk = strtoull(p + 6, nullptr, 0) * (SYSCLK_HZ / 1000);
                events.push_back(ev);
            }
            else if (strncmp(p, "text ", 5) == 0)
            {
                Event ev;
                for (const char *s = p + 5; *s; s++)
                {
                    char c = *s;
                    if (c == '\\' && s[1] != '\0')
                    {
                        s++;
                        switch (*s)
                        {
                            case 'r': c = '\r'; break;
                            case 'n': c = '\n'; break;
                            case 't': c = '\t'; break;
                            case 'e': c = 0x1B; break;
                            case '\\': c = '\\'; break;
                            default:
                                fprintf(stderr, "ps2 script:%d: unknown escape \\%c\n",
                                        lineno, *s);
                                ok = false;
                                continue;
                        }
                    }
                    emit_char(ev.bytes, c);
                }
                events.push_back(ev);
            }
            else if (strncmp(p, "raw ", 4) == 0)
            {
                Event ev;
                const char *s = p + 4;
                char *end;
                while (*s)
                {
                    unsigned long v = strtoul(s, &end, 16);
                    if (end == s) { break; }
                    ev.bytes.push_back(static_cast<uint8_t>(v));
                    s = end;
                }
                events.push_back(ev);
            }
            else
            {
                fprintf(stderr, "ps2 script:%d: unknown directive: %s\n", lineno, p);
                ok = false;
            }
        }
        fclose(fp);
        return ok;
    }

    void service(uint64_t clock_now, PS2State &ps2)
    {
        while (!events.empty() && clock_now >= resume_clock)
        {
            Event &ev = events.front();
            if (ev.delay_sysclk != 0)
            {
                resume_clock = clock_now + ev.delay_sysclk;
            }
            else
            {
                for (uint8_t b : ev.bytes)
                {
                    ps2.enqueue_byte(clock_now, b);
                }
            }
            events.pop_front();
        }
    }
};

// ---------------------------------------------------------------------------
// Scripted PS/2 mouse input (--mouse-in): the mouse counterpart of PS2Script,
// driving the PS2Mouse device model so mouse paths can be tested unattended
// (headless runs get no SDL events).  Directives, one per line:
//   # comment
//   delay MS          wait MS emulated milliseconds before the next directive
//   move DX DY        accumulate movement (PS/2 sign: +Y is up)
//   button LMR        hold exactly these buttons ("none" releases all)
//   click L           press, hold CLICK_HOLD_MS, release
//   raw HH HH ...     inject raw device->host bytes into the mouse channel
// Movement and button changes only become packets when the guest has enabled
// data reporting, exactly as on hardware.
// ---------------------------------------------------------------------------

struct MouseScript
{
    static constexpr uint64_t CLICK_HOLD_MS = 50;

    struct Event
    {
        uint64_t delay_sysclk = 0;          // nonzero: pause
        bool     have_move = false;
        int      dx = 0;
        int      dy = 0;
        bool     have_buttons = false;
        uint8_t  buttons = 0;
        std::deque<uint8_t> raw;
    };

    std::deque<Event> events;
    uint64_t resume_clock = 0;

    // "LMR", "lr", "none" -> a PS/2 button mask.
    static bool parse_buttons(const char *s, uint8_t &mask)
    {
        mask = 0;
        if (strncmp(s, "none", 4) == 0)
        {
            return true;
        }
        for (; *s; s++)
        {
            switch (*s)
            {
                case 'L': case 'l': mask |= PS2Mouse::BUTTON_LEFT; break;
                case 'M': case 'm': mask |= PS2Mouse::BUTTON_MIDDLE; break;
                case 'R': case 'r': mask |= PS2Mouse::BUTTON_RIGHT; break;
                case ' ': case '\t': break;
                default: return false;
            }
        }
        return true;
    }

    bool load(const char *path)
    {
        FILE *fp = fopen(path, "r");
        if (!fp)
        {
            fprintf(stderr, "mouse script: cannot open %s\n", path);
            return false;
        }
        char line[1024];
        int lineno = 0;
        bool ok = true;
        while (fgets(line, sizeof line, fp))
        {
            lineno++;
            char *p = line;
            while (*p == ' ' || *p == '\t') { p++; }
            size_t len = strlen(p);
            while (len > 0 && (p[len-1] == '\n' || p[len-1] == '\r')) { p[--len] = '\0'; }
            if (*p == '\0' || *p == '#') { continue; }

            if (strncmp(p, "delay ", 6) == 0)
            {
                Event ev;
                ev.delay_sysclk = strtoull(p + 6, nullptr, 0) * (SYSCLK_HZ / 1000);
                events.push_back(ev);
            }
            else if (strncmp(p, "move ", 5) == 0)
            {
                Event ev;
                if (sscanf(p + 5, "%d %d", &ev.dx, &ev.dy) != 2)
                {
                    fprintf(stderr, "mouse script:%d: move wants DX DY\n", lineno);
                    ok = false;
                    continue;
                }
                ev.have_move = true;
                events.push_back(ev);
            }
            else if (strncmp(p, "button ", 7) == 0)
            {
                Event ev;
                if (!parse_buttons(p + 7, ev.buttons))
                {
                    fprintf(stderr, "mouse script:%d: button wants L/M/R or none\n", lineno);
                    ok = false;
                    continue;
                }
                ev.have_buttons = true;
                events.push_back(ev);
            }
            else if (strncmp(p, "click ", 6) == 0)
            {
                uint8_t mask;
                if (!parse_buttons(p + 6, mask) || mask == 0)
                {
                    fprintf(stderr, "mouse script:%d: click wants L/M/R\n", lineno);
                    ok = false;
                    continue;
                }
                Event press;
                press.have_buttons = true;
                press.buttons = mask;
                events.push_back(press);
                Event hold;
                hold.delay_sysclk = CLICK_HOLD_MS * (SYSCLK_HZ / 1000);
                events.push_back(hold);
                Event release;
                release.have_buttons = true;
                release.buttons = 0;
                events.push_back(release);
            }
            else if (strncmp(p, "raw ", 4) == 0)
            {
                Event ev;
                const char *s = p + 4;
                char *end;
                while (*s)
                {
                    unsigned long v = strtoul(s, &end, 16);
                    if (end == s) { break; }
                    ev.raw.push_back(static_cast<uint8_t>(v));
                    s = end;
                }
                events.push_back(ev);
            }
            else
            {
                fprintf(stderr, "mouse script:%d: unknown directive: %s\n", lineno, p);
                ok = false;
            }
        }
        fclose(fp);
        return ok;
    }

    void service(uint64_t clock_now, PortsState &ports)
    {
        while (!events.empty() && clock_now >= resume_clock)
        {
            Event &ev = events.front();
            if (ev.delay_sysclk != 0)
            {
                resume_clock = clock_now + ev.delay_sysclk;
            }
            else
            {
                if (ev.have_move)
                {
                    ports.mouse_device.move(ev.dx, ev.dy);
                }
                if (ev.have_buttons)
                {
                    ports.mouse_device.set_buttons(ev.buttons);
                }
                for (uint8_t b : ev.raw)
                {
                    ports.mouse.enqueue_byte(clock_now, b);
                }
            }
            events.pop_front();
        }
    }
};

// ---------------------------------------------------------------------------
// Scripted joystick / paddle input (--joystick-in).  Same shape as the PS/2
// scripts.  Directives, one per line:
//   # comment
//   delay MS            wait MS emulated milliseconds
//   stick1 NAME...      hold exactly these port-1 switches ("none" releases)
//   stick2 NAME...      same for port 2
//   paddle-a N          set paddle A knob position, 0..255
//   paddle-b N          set paddle B knob position
// Switch names: up down left right fire pin9 pin5 (and "none").
// ---------------------------------------------------------------------------

struct JoystickScript
{
    struct Event
    {
        uint64_t delay_sysclk = 0;      // nonzero: pause
        int      stick = -1;            // 0/1: set that port's pressed mask
        uint8_t  pressed = 0;
        int      paddle = -1;           // 0 = A, 1 = B
        uint8_t  position = 0;
    };

    std::deque<Event> events;
    uint64_t resume_clock = 0;

    static bool parse_switches(const char *s, uint8_t &mask)
    {
        struct Name { const char *name; uint32_t bit; };
        static const Name names[] = {
            { "up",    PORTS_JOYSTICK_PORT_1_UP_MASK },
            { "down",  PORTS_JOYSTICK_PORT_1_DOWN_MASK },
            { "left",  PORTS_JOYSTICK_PORT_1_LEFT_MASK },
            { "right", PORTS_JOYSTICK_PORT_1_RIGHT_MASK },
            { "fire",  PORTS_JOYSTICK_PORT_1_FIRE_MASK },
            { "pin9",  PORTS_JOYSTICK_PORT_1_PIN9_MASK },
            { "pin5",  PORTS_JOYSTICK_PORT_1_PIN5_MASK },
        };
        // Port 2 uses the same bit order, so one table serves both.
        static_assert(PORTS_JOYSTICK_PORT_1_FIRE_MASK == PORTS_JOYSTICK_PORT_2_FIRE_MASK);

        mask = 0;
        while (*s)
        {
            while (*s == ' ' || *s == '\t') { s++; }
            if (*s == '\0') { break; }
            size_t len = 0;
            while (s[len] && s[len] != ' ' && s[len] != '\t') { len++; }
            bool found = false;
            if (len == 4 && strncmp(s, "none", 4) == 0)
            {
                found = true;
            }
            for (const Name &n : names)
            {
                if (strlen(n.name) == len && strncmp(s, n.name, len) == 0)
                {
                    mask |= static_cast<uint8_t>(n.bit);
                    found = true;
                    break;
                }
            }
            if (!found)
            {
                return false;
            }
            s += len;
        }
        return true;
    }

    bool load(const char *path)
    {
        FILE *fp = fopen(path, "r");
        if (!fp)
        {
            fprintf(stderr, "joystick script: cannot open %s\n", path);
            return false;
        }
        char line[1024];
        int lineno = 0;
        bool ok = true;
        while (fgets(line, sizeof line, fp))
        {
            lineno++;
            char *p = line;
            while (*p == ' ' || *p == '\t') { p++; }
            size_t len = strlen(p);
            while (len > 0 && (p[len-1] == '\n' || p[len-1] == '\r')) { p[--len] = '\0'; }
            if (*p == '\0' || *p == '#') { continue; }

            if (strncmp(p, "delay ", 6) == 0)
            {
                Event ev;
                ev.delay_sysclk = strtoull(p + 6, nullptr, 0) * (SYSCLK_HZ / 1000);
                events.push_back(ev);
            }
            else if (strncmp(p, "stick1 ", 7) == 0 || strncmp(p, "stick2 ", 7) == 0)
            {
                Event ev;
                ev.stick = (p[5] == '1') ? 0 : 1;
                if (!parse_switches(p + 7, ev.pressed))
                {
                    fprintf(stderr, "joystick script:%d: unknown switch name in: %s\n",
                            lineno, p);
                    ok = false;
                    continue;
                }
                events.push_back(ev);
            }
            else if (strncmp(p, "paddle-a ", 9) == 0 || strncmp(p, "paddle-b ", 9) == 0)
            {
                Event ev;
                ev.paddle = (p[7] == 'a') ? 0 : 1;
                unsigned long v = strtoul(p + 9, nullptr, 0);
                if (v > 255)
                {
                    fprintf(stderr, "joystick script:%d: paddle position %lu > 255\n",
                            lineno, v);
                    ok = false;
                    continue;
                }
                ev.position = static_cast<uint8_t>(v);
                events.push_back(ev);
            }
            else
            {
                fprintf(stderr, "joystick script:%d: unknown directive: %s\n", lineno, p);
                ok = false;
            }
        }
        fclose(fp);
        return ok;
    }

    void service(uint64_t clock_now, PortsState &ports)
    {
        while (!events.empty() && clock_now >= resume_clock)
        {
            Event &ev = events.front();
            if (ev.delay_sysclk != 0)
            {
                resume_clock = clock_now + ev.delay_sysclk;
            }
            else if (ev.stick >= 0)
            {
                ports.joystick_pressed[ev.stick] = ev.pressed;
            }
            else if (ev.paddle >= 0)
            {
                PaddleCounter &paddle = (ev.paddle == 0) ? ports.paddle_a : ports.paddle_b;
                paddle.position = ev.position;
            }
            events.pop_front();
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

class GriffinEmulator : public moira::Moira
{
    // Fully-populated 4MB RAM (4 banks of 2x AS6C4008, one contiguous region from 0).
    static_assert(RAM_BANK_1_BASE == 0);
    static_assert(RAM_BANK_1_SIZE + RAM_BANK_2_SIZE + RAM_BANK_3_SIZE + RAM_BANK_4_SIZE ==
                  RAM_TOTAL_SIZE);
    // RAM is stored as BUS WORDS, not bytes.  The display-list engine reads it
    // through SuperEngine::Memory, which is a span of 16-bit words indexed by
    // byte_address/2 — exactly what a 68000 bus master sees — and making that
    // the storage means there is no second copy to keep in step and no way for
    // the engine's view of memory to drift from the CPU's.  The byte accessors
    // below do the big-endian split: byte 2n is the high half of word n.
    mutable std::vector<uint16_t> RAM = std::vector<uint16_t>(RAM_TOTAL_SIZE / 2, 0);

    uint8_t ram_read8(uint32_t addr) const
    {
        const uint16_t w = RAM[addr >> 1];
        return (addr & 1) ? static_cast<uint8_t>(w & 0xFF) : static_cast<uint8_t>(w >> 8);
    }

    void ram_write8(uint32_t addr, uint8_t v) const
    {
        uint16_t &w = RAM[addr >> 1];
        w = (addr & 1) ? static_cast<uint16_t>((w & 0xFF00) | v)
                       : static_cast<uint16_t>((w & 0x00FF) | (static_cast<uint16_t>(v) << 8));
    }

    // The engine's view.  Const-cast because Moira's bus callbacks are const
    // and RAM is already mutable for the same reason.
    SuperEngine::Memory engine_memory() const
    {
        return SuperEngine::Memory{std::span<uint16_t>(RAM)};
    }
    mutable std::array<uint8_t, ROM_SIZE> ROM{};
    mutable int debug_out_latch = 0;
    mutable bool ROMoverlay = true;
    // GLUE CONFIG bit VSYNC_IRQ_EN.  Gates only the level-6 IPL term; the
    // vsync latch, VSYNC_STATUS and VSYNC_CLEAR stay live so the guest can
    // poll vblank.  Resets to 0, matching GLUE.
    mutable bool vsync_irq_en = false;
    PTYConsole pty_console;
    PTYConsole pty_console_b;   // DUART channel B (unconnected by default)
    mutable CFState cf;
    mutable DUARTState duart;
    mutable VideoState video;
    mutable EngineState engine;
    mutable PS2State ps2;
    mutable PS2Keyboard ps2_keyboard;
    mutable PortsState ports;
    StdinConsole stdin_console;

    // SDL gamepads mapped onto the two joystick ports.  Deliberately NOT the
    // host keyboard: every key goes to the PS/2 keyboard model, and stealing
    // keys here would break console input.
    SDL_Gamepad *gamepads[PortsState::JOYSTICK_PORTS] = {};
    SDL_JoystickID gamepad_ids[PortsState::JOYSTICK_PORTS] = {};

    // --keys-to-joystick, and which of the two FIRE keys are currently held.
    bool keys_to_joystick = false;
    uint8_t fire_keys_held = 0;

    // Scanline count already handed to PORTS as PADDLE_TICK/AUDIO_TICK toggles.
    uint64_t ports_line_count = 0;

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

    static bool is_ports_addr(uint32_t io_offset)
    {
        uint32_t abs = io_offset + IO_BASE;
        return abs >= PORTS_BASE && abs < PORTS_BASE + PORTS_SIZE;
    }

    uint8_t IO_read8(uint32_t addr) const
    {
        if(is_cf_addr(addr)) {
            return cf.read_reg(addr + IO_BASE);
        }
        if (is_duart_addr(addr))
        {
            uint32_t abs = addr + IO_BASE;
            uint8_t val = duart.read_reg(abs, pty_console, pty_console_b);
            // STARTCC restarts the counter from the preload (68681 semantics),
            // so (re)initialize the fire time and the live-count origin here.
            if (abs == DUART_STARTCC)
            {
                duart.ctr_next_fire = getClock() + duart.ctr_period_sysclk();
                duart.ctr_start_clock = getClock();
            }
            // CUR/CLR return the live counter (68681 latch protocol); the
            // DUARTState stub returns 0, so override with the clock-derived value.
            if (abs == DUART_CUR || abs == DUART_CLR)
            {
                val = duart.read_counter_byte(abs, getClock());
            }
            if (debug & DEBUG_DUART)
            {
                printf("[DUART read %06X → 0x%02X]\n", abs, val);
            }
            return val;
        }
        if (addr + IO_BASE == GLUE_PS2_STATUS
            || addr + IO_BASE == GLUE_PS2_RX_DATA)
        {
            return ps2.read_reg(addr + IO_BASE);
        }
        if (addr + IO_BASE == GLUE_VSYNC_STATUS)
        {
            // TIMING's vsync pulse, latched in GLUE.  This replaces rev-1
            // VIDEO's CLRINT: the interrupt source moved chips when VIDEO did.
            return video.vsync_pending ? GLUE_VSYNC_STATUS_VSYNC_PENDING_MASK : 0;
        }
        if (is_ports_addr(addr))
        {
            return ports.read_reg(addr + IO_BASE);
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
            if (addr + IO_BASE == CF_DATA)
            {
                // 16-bit True IDE data port: IDE word is low byte first in
                // the sector byte stream, and rides D7-D0 on the bus.
                uint16_t lo = cf.read_reg(CF_DATA);
                uint16_t hi = cf.read_reg(CF_DATA);
                return static_cast<uint16_t>(lo | (hi << 8));
            }
            printf("WARNING: 16-bit read from CF task-file register at %06X (byte registers)\n", addr + IO_BASE);
            return cf.read_reg(addr + IO_BASE);
        }
        if (is_duart_addr(addr))
        {
            printf("WARNING: 16-bit read from DUART at %06X (firmware should use 8-bit only)\n", addr + IO_BASE);
            return duart.read_reg(addr + IO_BASE, pty_console, pty_console_b);
        }
        if (is_ports_addr(addr))
        {
            printf("WARNING: 16-bit read from PORTS at %06X (byte registers only)\n", addr + IO_BASE);
            return ports.read_reg(addr + IO_BASE);
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
            duart.write_reg(addr + IO_BASE, val, pty_console, pty_console_b);
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
            // Not sticky, unlike the overlay: GLUE reloads this bit from the
            // data bus on every CONFIG write.  Re-evaluate the IPL because
            // enabling it can expose an already-latched vsync.
            vsync_irq_en = (val & GLUE_CONFIG_VSYNC_IRQ_EN_MASK) != 0;
            const_cast<GriffinEmulator*>(this)->update_ipl();
            if (debug & DEBUG_IO)
            {
                printf("[GLUE CONFIG: 0x%02X overlay=%s vsync_irq=%s]\n", val,
                       (val & GLUE_CONFIG_ROM_OVERLAY_DISABLE_MASK) ? "off" : "on",
                       vsync_irq_en ? "on" : "off");
            }
        } else if(addr + IO_BASE == GLUE_VSYNC_CLEAR) {
            // Write-1-to-clear, and it must re-evaluate the IPL: level 6 is
            // level-triggered off this latch, so an ack that does not lower IPL
            // re-enters the ISR immediately.
            if (val & GLUE_VSYNC_CLEAR_VSYNC_PENDING_MASK)
            {
                video.clear_irq();
            }
            const_cast<GriffinEmulator*>(this)->update_ipl();
        } else if(addr + IO_BASE == GLUE_PS2_CLEAR
                  || addr + IO_BASE == GLUE_PS2_CTRL
                  || addr + IO_BASE == GLUE_PS2_TX_DATA
                  || addr + IO_BASE == GLUE_PS2_TX_DATA + PS2_TX_DATA_PARITY) {
            // PS2_TX_DATA spans 0x09/0x0B (parity in address bit 1); both
            // alias to the same TX trigger in the frame model.
            ps2.write_reg(addr + IO_BASE, val, getClock());
            const_cast<GriffinEmulator*>(this)->update_ipl();
        } else if(is_ports_addr(addr)) {
            ports.write_reg(addr + IO_BASE, val, getClock());
            const_cast<GriffinEmulator*>(this)->update_ipl();
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
            if (addr + IO_BASE == CF_DATA)
            {
                cf.write_reg(CF_DATA, val & 0xFF);
                cf.write_reg(CF_DATA, static_cast<uint8_t>(val >> 8));
                return;
            }
            printf("WARNING: 16-bit write 0x%04X to CF task-file register at %06X (byte registers)\n", val, addr + IO_BASE);
            cf.write_reg(addr + IO_BASE, val & 0xFF);
            return;
        }
        if (is_duart_addr(addr))
        {
            printf("WARNING: 16-bit write 0x%04X to DUART at %06X (firmware should use 8-bit only)\n", val, addr + IO_BASE);
            duart.write_reg(addr + IO_BASE, val & 0xFF, pty_console, pty_console_b);
            return;
        }
        if (is_ports_addr(addr))
        {
            printf("WARNING: 16-bit write 0x%04X to PORTS at %06X (byte registers only)\n", val, addr + IO_BASE);
            ports.write_reg(addr + IO_BASE, val & 0xFF, getClock());
            const_cast<GriffinEmulator*>(this)->update_ipl();
            return;
        }
        if(debug & DEBUG_IO)
        {
            printf("write of uint16_t %04X at unhandled IO %06X\n", val, addr + IO_BASE);
        }
    }

    // --- Direct-bus region (0xC00000-0xCFFFFF) -----------------------------
    // GLUE decodes the region and emits fully-timed ~IO_RD_EN / ~IO_WR_EN; a
    // 74155 fans them out by A19:18.  GLUE never touches the data path, so a
    // read of any quadrant (including AUDIO, which is write-only) sees the
    // undriven bus.

    uint8_t direct_bus_read8(uint32_t addr) const
    {
        if (debug & DEBUG_IO)
        {
            printf("read of uint8_t at write-only direct-bus %06X\n", addr);
        }
        return 0xFF;
    }

    uint16_t direct_bus_read16(uint32_t addr) const
    {
        if (debug & DEBUG_IO)
        {
            printf("read of uint16_t at write-only direct-bus %06X\n", addr);
        }
        return 0xFFFF;
    }

    void direct_bus_write8(uint32_t addr, uint8_t val) const
    {
        if (AUDIO.contains(addr))
        {
            // ~IO_WR_EN = region & UDS & LDS & ~R/W, so a byte write physically
            // cannot strobe the 7202s.  Drop it loudly rather than silently
            // honouring an access the hardware guards against.
            printf("WARNING: 8-bit write 0x%02X to AUDIO FIFO at %06X dropped"
                   " (~IO_WR_EN needs UDS and LDS; use a word write)\n",
                   val, addr);
            return;
        }
        if (debug & DEBUG_IO)
        {
            printf("write of uint8_t %02X at unpopulated direct-bus %06X\n", val, addr);
        }
    }

    void direct_bus_write16(uint32_t addr, uint16_t val) const
    {
        if (AUDIO.contains(addr))
        {
            // One stereo pair per full-word write: L = D15-D8, R = D7-D0.
            ports.audio_fifo_push(val);
            return;
        }
        if (debug & DEBUG_IO)
        {
            printf("write of uint16_t %04X at unpopulated direct-bus %06X\n", val, addr);
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
        // AUDIO is at 0xC00000, BELOW IO_BASE -- its penalty must be tested
        // outside the IO region, not nested inside it (where it was dead code).
        if (addr >= AUDIO_BASE && addr < AUDIO_BASE + AUDIO_SIZE)
        {
            return AUDIO_DTACK_PENALTY;
        }
        if (addr >= IO_BASE && addr < IO_BASE + IO_SIZE)
        {
            if (addr >= CF_BASE && addr < CF_BASE + CF_SIZE)
            {
                return CF_DTACK_PENALTY;
            }
            if (addr >= PORTS_BASE && addr < PORTS_BASE + PORTS_SIZE)
            {
                return PORTS_DTACK_PENALTY;
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

    uint8_t peek_ram8(uint32_t addr) const
    {
        if (addr < RAM_TOTAL_SIZE)
        {
            return ram_read8(addr);
        }
        return 0;
    }

public:

    uint32_t clock_hz = SYSCLK_HZ;
    bool exit_requested = false;
    uint64_t dma_stall_debt = 0;   // smooth DMA-stall model: unpaid stall cycles

    // PIXEL and COMPOSITOR, held across scanlines exactly as the chips are.
    // Their state is the frame: FIFO contents, held colours, the run in
    // progress.  Only /RS at vsync clears them.
    SuperEngine::PixelUnit      pixel_unit;
    SuperEngine::CompositorUnit compositor_unit;

    // Bus time the engine spent outside a scanline boundary — i.e. the burst a
    // DESC write kicks off immediately — waiting to be folded into the next
    // service_video() stall payment.
    uint64_t engine_stall_pending = 0;

    // --- PC-sampling profiler (GRIFFIN_PROFILE=START:END, SYSCLK cycles) ---
    // Deterministic runs make this exact: histogram the PC once per
    // instruction while the clock is inside [START,END), dump the hottest
    // 64-byte buckets at exit for symbolization against System.map.
    static constexpr uint32_t PROFILE_SHIFT = 6;   // 64-byte buckets
    std::vector<uint32_t> profile_hist;
    uint64_t profile_start = 0, profile_end = 0;
    uint64_t profile_samples = 0;

    void profile_init(uint64_t start, uint64_t end)
    {
        profile_start = start;
        profile_end = end;
        profile_hist.assign(RAM_TOTAL_SIZE >> PROFILE_SHIFT, 0);
    }

    void profile_sample()
    {
        uint64_t now = getClock();
        if (now < profile_start || now >= profile_end)
        {
            return;
        }
        uint32_t pc = getPC();
        if (pc < RAM_TOTAL_SIZE)
        {
            profile_hist[pc >> PROFILE_SHIFT]++;
            profile_samples++;
        }
    }

    void profile_dump() const
    {
        if (profile_hist.empty() || profile_samples == 0)
        {
            return;
        }
        // Top 50 buckets.
        std::vector<uint32_t> idx(profile_hist.size());
        for (uint32_t i = 0; i < idx.size(); i++) idx[i] = i;
        std::partial_sort(idx.begin(), idx.begin() + 50, idx.end(),
            [this](uint32_t a, uint32_t b)
            { return profile_hist[a] > profile_hist[b]; });
        fprintf(stderr, "=== PC profile: %" PRIu64 " samples in [%" PRIu64
                ", %" PRIu64 ") ===\n",
                profile_samples, profile_start, profile_end);
        for (int i = 0; i < 50; i++)
        {
            uint32_t b = idx[i];
            if (profile_hist[b] == 0) break;
            fprintf(stderr, "  %08X %8u %5.2f%%\n",
                    b << PROFILE_SHIFT, profile_hist[b],
                    100.0 * profile_hist[b] / profile_samples);
        }
    }
    ClockGovernor governor;

    // Left-channel R2R DAC level: whatever the 7202 output register last
    // presented, held between pops.  Feeds the audio.raw capture.
    uint8_t GetAudioDACValue() const
    {
        return ports.dac_left;
    }

    // Scripted keystroke injection (--ps2-in) feeds the PS/2 model directly.
    PS2State &ps2_model() { return ps2; }

    // Scripted mouse (--mouse-in) and joystick/paddle (--joystick-in) input.
    PortsState &ports_model() { return ports; }

    // Attach each PS/2 frame engine to its channel base and device model here
    // rather than in an init() the caller could forget: the GLUE channel is a
    // keyboard, the PORTS channel a mouse, and neither is optional.
    GriffinEmulator()
    {
        ps2.init(GLUE_BASE, ps2_keyboard);
        ports.init();
    }

    // Full CPU state dump, shared by the unhandled-access abort() paths and
    // the GRIFFIN_DUMP_ON_EXIT env-gated dump at normal exit.
    void dump_ram_ascii(uint32_t addr, uint32_t len) const
    {
        fprintf(stderr, "\n=== RAM %06X..%06X (ASCII) ===\n", addr, addr + len);
        std::string run;
        for (uint32_t a = addr; a < addr + len && a < RAM_TOTAL_SIZE; a++)
        {
            uint8_t b = peek_ram8(a);
            run += (b >= 0x20 && b < 0x7f) ? (char)b : (b == '\n' ? '\n' : '.');
        }
        fprintf(stderr, "%s\n", run.c_str());
        fflush(stderr);
    }

    void dump_cpu_state(const char *why) const
    {
        uint32_t pc = getPC();
        fprintf(stderr, "\n=== CPU state (%s) ===\n", why);
        fprintf(stderr, "PC=%06X  SR=%04X  SP=%06X\n", pc, getSR(), getSP());
        for (int i = 0; i < 8; i++)
            fprintf(stderr, "D%d=%08X%s", i, getD(i), (i % 4 == 3) ? "\n" : "  ");
        for (int i = 0; i < 8; i++)
            fprintf(stderr, "A%d=%08X%s", i, getA(i), (i % 4 == 3) ? "\n" : "  ");
        // Frame-pointer backtrace (kernel builds with -fno-omit-frame-pointer):
        // a6 chain: [fp] = caller fp, [fp+4] = return address.
        {
            uint32_t fp = getA(6);
            fprintf(stderr, "--- fp backtrace ---\n");
            for (int i = 0; i < 40 && fp >= 0x1000 && fp < RAM_TOTAL_SIZE - 8; i++)
            {
                uint32_t next = (peek_ram8(fp) << 24) | (peek_ram8(fp + 1) << 16)
                              | (peek_ram8(fp + 2) << 8) | peek_ram8(fp + 3);
                uint32_t ra   = (peek_ram8(fp + 4) << 24) | (peek_ram8(fp + 5) << 16)
                              | (peek_ram8(fp + 6) << 8) | peek_ram8(fp + 7);
                fprintf(stderr, "  fp=%06X ra=%08X\n", fp, ra);
                if (next <= fp) break;
                fp = next;
            }
        }
        fprintf(stderr, "--- disassembly from PC ---\n");
        char str[1024];
        uint32_t addr = pc;
        for (int i = 0; i < 16; i++)
        {
            int len = disassemble(str, addr);
            fprintf(stderr, "%06X: %s\n", addr, str);
            addr += (len > 0) ? (uint32_t)len : 2;
        }
        fflush(stderr);
    }

    uint8_t read8(uint32_t addr) const override
    {
        apply_wait_states(addr);
        if(debug & DEBUG_BUS) { printf("read of uint8_t at %06X\n", addr); }
        if (ROMoverlay && (addr < ROM_SIZE)) {
            return ROM[addr];
        } else if (addr < RAM_TOTAL_SIZE) {
            return ram_read8(addr);
        } else if (addr >= ROM_BASE && addr < ROM_BASE + ROM_WINDOW) {
            return ROM[(addr - ROM_BASE) % ROM_SIZE];
        } else if (addr >= DIRECT_BUS_BASE && addr < DIRECT_BUS_BASE + DIRECT_BUS_SIZE) {
            return direct_bus_read8(addr);
        } else if (addr >= VIDEO_BASE && addr < VIDEO_BASE + VIDEO_SIZE) {
            // Freed region: rev-1 VIDEO is gone and nothing answers here.
            // GLUE still decodes and DTACKs it, so the bus floats.
            return EngineState::OPEN_BUS;
        } else if (addr >= ENGINE_BASE && addr < ENGINE_BASE + ENGINE_SIZE) {
            return EngineState::OPEN_BUS;   // write-only: no STATUS, no readback
        } else if (addr >= IO_BASE && addr < (IO_BASE + IO_SIZE)) {
            return IO_read8(addr - IO_BASE);
        } else {
            fprintf(stderr, "read of uint8_t at unhandled %06X\n", addr);
            dump_cpu_state("unhandled read8"); abort();
        }
    }

    uint16_t read16(uint32_t addr) const override
    {
        apply_wait_states(addr);
        if(debug & DEBUG_BUS) { printf("read of uint16_t at %06X\n", addr); }
        if (ROMoverlay && (addr < ROM_SIZE)) {
            return (ROM[addr] << 8) | ROM[addr + 1];
        } else if (addr < RAM_TOTAL_SIZE) {
            // Aligned word reads are the common case and hit one entry.
            return (addr & 1) ? static_cast<uint16_t>((ram_read8(addr) << 8) | ram_read8(addr + 1))
                              : RAM[addr >> 1];
        } else if (addr >= ROM_BASE && addr < ROM_BASE + ROM_WINDOW) {
            return (ROM[(addr - ROM_BASE) % ROM_SIZE] << 8) | ROM[(addr - ROM_BASE + 1) % ROM_SIZE];
        } else if (addr >= DIRECT_BUS_BASE && addr < DIRECT_BUS_BASE + DIRECT_BUS_SIZE) {
            return direct_bus_read16(addr);
        } else if (addr >= VIDEO_BASE && addr < VIDEO_BASE + VIDEO_SIZE) {
            return (EngineState::OPEN_BUS << 8) | EngineState::OPEN_BUS;
        } else if (addr >= ENGINE_BASE && addr < ENGINE_BASE + ENGINE_SIZE) {
            return (EngineState::OPEN_BUS << 8) | EngineState::OPEN_BUS;
        } else if (addr >= IO_BASE && addr < (IO_BASE + IO_SIZE)) {
            return IO_read16(addr - IO_BASE);
        } else {
            fprintf(stderr, "read of uint16_t at unhandled %06X\n", addr);
            dump_cpu_state("unhandled read16"); abort();
        }
    }

    void write8(uint32_t addr, uint8_t val) const override
    {
        apply_wait_states(addr);
        if(debug & DEBUG_BUS) { printf("write of uint8_t %02X at %06X\n", val, addr); }
        if (addr < RAM_TOTAL_SIZE) {
            ram_write8(addr, val);
        } else if (addr >= ROM_BASE && addr < ROM_BASE + ROM_WINDOW) {
            return;
        } else if (addr >= DIRECT_BUS_BASE && addr < DIRECT_BUS_BASE + DIRECT_BUS_SIZE) {
            direct_bus_write8(addr, val);
        } else if (addr >= IO_BASE && addr < (IO_BASE + IO_SIZE)) {
            IO_write8(addr - IO_BASE, val);
        } else if (addr >= VIDEO_BASE && addr < VIDEO_BASE + VIDEO_SIZE) {
            return;   // freed region, no chip behind it
        } else if (addr >= ENGINE_BASE && addr < ENGINE_BASE + ENGINE_SIZE) {
            const_cast<GriffinEmulator*>(this)->engine_write8(addr, val);
        } else {
            fprintf(stderr, "write of uint8_t %02X to unhandled %06X\n", val, addr);
            dump_cpu_state("unhandled write8"); abort();
        }
    }

    void write16(uint32_t addr, uint16_t val) const override
    {
        apply_wait_states(addr);
        if(debug & DEBUG_BUS) { printf("write of uint16_t %04X at %06X\n", val, addr); }
        uint8_t high = (val >> 8);
        uint8_t low = (val & 0xFF);

        if (addr < RAM_TOTAL_SIZE) {
            if (addr & 1) {
                ram_write8(addr, high);
                ram_write8(addr + 1, low);
            } else {
                RAM[addr >> 1] = val;
            }
        } else if (addr >= ROM_BASE && addr < ROM_BASE + ROM_WINDOW) {
            return;
        } else if (addr >= DIRECT_BUS_BASE && addr < DIRECT_BUS_BASE + DIRECT_BUS_SIZE) {
            direct_bus_write16(addr, val);
        } else if (addr >= VIDEO_BASE && addr < VIDEO_BASE + VIDEO_SIZE) {
            return;   // freed region, no chip behind it
        } else if (addr >= ENGINE_BASE && addr < ENGINE_BASE + ENGINE_SIZE) {
            // DESC is a TRUE 16-bit register — edma3.v latches D[14:0] on one
            // word write.  Rev-1 folded every ENGINE word write down to its low
            // byte because every ENGINE register was 8 bits wide; doing that
            // here would throw away the top seven bits of the descriptor
            // pointer and arm the engine at the wrong address.
            const_cast<GriffinEmulator*>(this)->engine_write16(addr, val);
        } else if (addr >= IO_BASE && addr < (IO_BASE + IO_SIZE)) {
            IO_write16(addr - IO_BASE, val);
        } else {
            fprintf(stderr, "write of uint16_t %04X to unhandled %06X\n", val, addr);
            dump_cpu_state("unhandled write16"); abort();
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

    bool init_console_stdio()
    {
        return pty_console.open_stdio();
    }

    bool init_console_files(const char* in_path, const char* out_path)
    {
        return pty_console.open_files(in_path, out_path);
    }

    // DUART channel B endpoint (--serialb-pty / --serialb-in/-out).  Note:
    // channel B input EOF is deliberately NOT wired to emulator exit --
    // only the channel A console governs run lifetime.
    bool init_serialb_pty()
    {
        return pty_console_b.open("Serial B PTY");
    }

    bool init_serialb_files(const char* in_path, const char* out_path)
    {
        return pty_console_b.open_files(in_path, out_path);
    }

    bool init_serialb_dev(const char* path)
    {
        return pty_console_b.open_dev(path);
    }

    bool console_input_eof() const
    {
        return pty_console.input_eof;
    }

    bool init_stdin()
    {
        return stdin_console.open();
    }

    // --keys-to-joystick: send the host arrow/fire keys to joystick port 1
    // instead of to the PS/2 keyboard.  Interactive only (headless runs get no
    // SDL key events; they use --joystick-in).
    void set_keys_to_joystick(bool on) { keys_to_joystick = on; }

    // Interactive joystick input comes from SDL gamepads.  Only meaningful
    // when SDL is up (i.e. not headless); scripted input needs none of this.
    bool init_gamepads()
    {
        if (!SDL_InitSubSystem(SDL_INIT_GAMEPAD))
        {
            fprintf(stderr, "SDL gamepad init failed: %s\n", SDL_GetError());
            return false;
        }
        return true;
    }

    ~GriffinEmulator()
    {
        for (SDL_Gamepad *pad : gamepads)
        {
            if (pad)
            {
                SDL_CloseGamepad(pad);
            }
        }
    }

    bool init_video(bool headless = false, bool want_pixels = true)
    {
        video.headless = headless;
        video.want_pixels = want_pixels;
        return video.init();
    }

    void dump_framebuffer(const char* path)
    {
        video.dump_framebuffer(path);
    }

    bool open_cf(const char *path, bool read_only)
    {
        return cf.open(path, read_only);
    }

    // Watch stdin for the ~. exit escape, service SDL events, advance the
    // DUART timer, and update IPL.
    // Call this periodically from the main loop, not on every bus cycle.
    void poll_io()
    {
        {
            uint8_t ch;
            int rc;
            while ((rc = stdin_console.poll(&ch)) == 1)
            {
                // discard — stdin exists only for the exit escape
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
                if (keys_to_joystick && apply_key_to_joystick(ev))
                {
                    continue;
                }
                uint16_t code = sdl_to_ps2_set2(ev.key.scancode);
                if (code != 0)
                {
                    // Extended keys are 0xE0xx: E0 xx for make, E0 F0 xx
                    // for break (the E0 prefix precedes the F0).
                    if ((code & 0xFF00u) == 0xE000u)
                    {
                        ps2.enqueue_byte(getClock(), 0xE0);
                    }
                    if (ev.type == SDL_EVENT_KEY_UP)
                    {
                        ps2.enqueue_byte(getClock(), 0xF0);
                    }
                    ps2.enqueue_byte(getClock(), static_cast<uint8_t>(code & 0xFFu));
                }
            }
            else
            {
                handle_gamepad_event(ev);
            }
        }

        duart.check_timer(getClock());
        ps2.check_timer(getClock());
        ports.check_timer(getClock());
        update_ipl();
    }

    // --keys-to-joystick: divert the host arrow keys and space/left-control to
    // joystick port 1 rather than to the PS/2 keyboard, for testing a game
    // without a gamepad plugged in.  Returns true when the event was consumed
    // as a joystick input, so the caller drops it; every other key still types
    // normally.  Two keys share FIRE, so the button is the OR of them —
    // releasing one while the other is held must not release the button.
    bool apply_key_to_joystick(const SDL_Event &ev)
    {
        static constexpr uint8_t FIRE_SPACE = 1;
        static constexpr uint8_t FIRE_CTRL = 2;

        uint8_t bit = 0;
        uint8_t fire_key = 0;
        switch (ev.key.scancode)
        {
            case SDL_SCANCODE_UP:    bit = PORTS_JOYSTICK_PORT_1_UP_MASK; break;
            case SDL_SCANCODE_DOWN:  bit = PORTS_JOYSTICK_PORT_1_DOWN_MASK; break;
            case SDL_SCANCODE_LEFT:  bit = PORTS_JOYSTICK_PORT_1_LEFT_MASK; break;
            case SDL_SCANCODE_RIGHT: bit = PORTS_JOYSTICK_PORT_1_RIGHT_MASK; break;
            case SDL_SCANCODE_SPACE: fire_key = FIRE_SPACE; break;
            case SDL_SCANCODE_LCTRL: fire_key = FIRE_CTRL; break;
            default: return false;
        }

        bool down = (ev.type == SDL_EVENT_KEY_DOWN);
        if (fire_key != 0)
        {
            if (down)
            {
                fire_keys_held |= fire_key;
            }
            else
            {
                fire_keys_held &= static_cast<uint8_t>(~fire_key);
            }
            bit = PORTS_JOYSTICK_PORT_1_FIRE_MASK;
            down = (fire_keys_held != 0);
        }

        if (down)
        {
            ports.joystick_pressed[0] |= bit;
        }
        else
        {
            ports.joystick_pressed[0] &= static_cast<uint8_t>(~bit);
        }
        return true;
    }

    // Map an SDL gamepad onto a Griffin DE-9 joystick port.  Gamepad 0 becomes
    // port 1 and gamepad 1 port 2; the left stick's X axis also drives paddle
    // A and the right stick's paddle B, so a pad exercises the paddle counters
    // too.  Host keyboard keys belong to the PS/2 keyboard model by default;
    // --keys-to-joystick is the explicit opt-out (see apply_key_to_joystick).
    int gamepad_port(SDL_JoystickID which) const
    {
        for (int i = 0; i < PortsState::JOYSTICK_PORTS; i++)
        {
            if (gamepads[i] && gamepad_ids[i] == which)
            {
                return i;
            }
        }
        return -1;
    }

    void handle_gamepad_event(const SDL_Event &ev)
    {
        static constexpr int AXIS_DEADZONE = 12000;

        if (ev.type == SDL_EVENT_GAMEPAD_ADDED)
        {
            for (int i = 0; i < PortsState::JOYSTICK_PORTS; i++)
            {
                if (!gamepads[i])
                {
                    gamepads[i] = SDL_OpenGamepad(ev.gdevice.which);
                    if (gamepads[i])
                    {
                        gamepad_ids[i] = ev.gdevice.which;
                        fprintf(stderr, "Gamepad on joystick port %d: %s\n",
                                i + 1, SDL_GetGamepadName(gamepads[i]));
                    }
                    return;
                }
            }
            return;
        }
        if (ev.type == SDL_EVENT_GAMEPAD_REMOVED)
        {
            int port = gamepad_port(ev.gdevice.which);
            if (port >= 0)
            {
                SDL_CloseGamepad(gamepads[port]);
                gamepads[port] = nullptr;
                ports.joystick_pressed[port] = 0;
            }
            return;
        }
        if (ev.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN
            || ev.type == SDL_EVENT_GAMEPAD_BUTTON_UP)
        {
            int port = gamepad_port(ev.gbutton.which);
            if (port < 0)
            {
                return;
            }
            uint8_t bit = 0;
            switch (ev.gbutton.button)
            {
                case SDL_GAMEPAD_BUTTON_DPAD_UP:    bit = PORTS_JOYSTICK_PORT_1_UP_MASK; break;
                case SDL_GAMEPAD_BUTTON_DPAD_DOWN:  bit = PORTS_JOYSTICK_PORT_1_DOWN_MASK; break;
                case SDL_GAMEPAD_BUTTON_DPAD_LEFT:  bit = PORTS_JOYSTICK_PORT_1_LEFT_MASK; break;
                case SDL_GAMEPAD_BUTTON_DPAD_RIGHT: bit = PORTS_JOYSTICK_PORT_1_RIGHT_MASK; break;
                case SDL_GAMEPAD_BUTTON_SOUTH:      bit = PORTS_JOYSTICK_PORT_1_FIRE_MASK; break;
                case SDL_GAMEPAD_BUTTON_EAST:       bit = PORTS_JOYSTICK_PORT_1_PIN9_MASK; break;
                default: return;
            }
            if (ev.gbutton.down)
            {
                ports.joystick_pressed[port] |= bit;
            }
            else
            {
                ports.joystick_pressed[port] &= static_cast<uint8_t>(~bit);
            }
            return;
        }
        if (ev.type == SDL_EVENT_GAMEPAD_AXIS_MOTION)
        {
            int port = gamepad_port(ev.gaxis.which);
            if (port < 0)
            {
                return;
            }
            // Sticks map to the paddle knobs; the D-pad already covers the
            // digital directions.
            if (port == 0 && ev.gaxis.axis == SDL_GAMEPAD_AXIS_LEFTX)
            {
                ports.paddle_a.position =
                    static_cast<uint8_t>((ev.gaxis.value + 32768) >> 8);
            }
            else if (port == 0 && ev.gaxis.axis == SDL_GAMEPAD_AXIS_RIGHTX)
            {
                ports.paddle_b.position =
                    static_cast<uint8_t>((ev.gaxis.value + 32768) >> 8);
            }
            else if (ev.gaxis.axis == SDL_GAMEPAD_AXIS_LEFTY)
            {
                uint8_t up = PORTS_JOYSTICK_PORT_1_UP_MASK;
                uint8_t down = PORTS_JOYSTICK_PORT_1_DOWN_MASK;
                ports.joystick_pressed[port] &= static_cast<uint8_t>(~(up | down));
                if (ev.gaxis.value < -AXIS_DEADZONE) { ports.joystick_pressed[port] |= up; }
                if (ev.gaxis.value >  AXIS_DEADZONE) { ports.joystick_pressed[port] |= down; }
            }
            else if (ev.gaxis.axis == SDL_GAMEPAD_AXIS_LEFTX)
            {
                // Port 2 has no paddles, so its left stick is digital L/R.
                uint8_t left = PORTS_JOYSTICK_PORT_1_LEFT_MASK;
                uint8_t right = PORTS_JOYSTICK_PORT_1_RIGHT_MASK;
                ports.joystick_pressed[port] &= static_cast<uint8_t>(~(left | right));
                if (ev.gaxis.value < -AXIS_DEADZONE) { ports.joystick_pressed[port] |= left; }
                if (ev.gaxis.value >  AXIS_DEADZONE) { ports.joystick_pressed[port] |= right; }
            }
        }
    }

    // ------------------------------------------------------------------
    // Display-list pipeline plumbing
    // ------------------------------------------------------------------

    // Where a deposited word goes.  ENGINE only strobes; the mask decides which
    // consumer latches D[15:0].  This is the emulator's half of the suite's
    // DepositSink contract — the other half is the suite's own recording sink,
    // and both drive the identical EngineWalker.
    struct EmulatorSink final : SuperEngine::DepositSink
    {
        GriffinEmulator *emu = nullptr;

        void deposit(uint64_t, uint8_t signal_mask, uint16_t word) override
        {
            // AUDIO is guest-visible through PORTS' half-full status, so it is
            // fed unconditionally.  PIXELS and VIDCMD reach nothing the guest
            // can read, so they are skipped when no one is looking at the
            // framebuffer — see want_pixels below.
            if ((signal_mask & SuperEngine::SIGNAL_AUDIO_FIFO_W) != 0)
            {
                emu->ports.audio_fifo_push(word);
            }
            if (!emu->video.want_pixels)
            {
                return;
            }
            if ((signal_mask & SuperEngine::SIGNAL_PIXELS_FIFO_W) != 0)
            {
                emu->pixel_unit.push_word(word);
            }
            if ((signal_mask & SuperEngine::SIGNAL_VIDCMD_FIFO_W) != 0)
            {
                emu->compositor_unit.push_word(word);
            }
        }
    };

    // Bring-up self-check for the display-list path, with no firmware
    // involved: build a descriptor list by hand, arm it the way a DESC write
    // does, and let the raster run.  It exercises the whole chain the ROM will
    // depend on — arm, one wait_hblank release per scanline, VIDCMD deposits,
    // eager SETs in blanking, RUN playback against the 640-slot frame, and
    // stop_after — and it verifies its own output, so it is a test rather than
    // a screenshot to squint at.
    //
    // The picture is left half held_fg (red), right half held_bg (blue), on
    // every line.  Each line's packet is four words:
    //     SET cmp_held_fg, SET cmp_held_bg, RUN(held_fg,320), RUN(held_bg,320)
    // The two SETs are consumed during the preceding HBLANK and cost no slot;
    // the two RUNs total exactly H_ACTIVE.
    static constexpr uint32_t SELFTEST_PACKET = 0x100000;
    static constexpr SuperEngine::Rgb444 SELFTEST_FG = 0x0F00;
    static constexpr SuperEngine::Rgb444 SELFTEST_BG = 0x000F;

    void selftest_build_list()
    {
        using namespace SuperEngine;

        // One shared four-word packet; every line's descriptor points at it.
        uint16_t *p = &RAM[SELFTEST_PACKET >> 1];
        p[0] = vidcmd_set(SET_CMP_HELD_FG, SELFTEST_FG);
        p[1] = vidcmd_set(SET_CMP_HELD_BG, SELFTEST_BG);
        p[2] = vidcmd_run(RUN_SRC_HELD_FG, H_ACTIVE / 2);
        p[3] = vidcmd_run(RUN_SRC_HELD_BG, H_ACTIVE / 2);

        uint32_t at = DESC_TABLE_BASE;
        for (uint32_t line = 0; line < V_ACTIVE; line++)
        {
            Descriptor d;
            d.src         = SELFTEST_PACKET;
            d.count       = 4;
            d.signal_mask = SIGNAL_VIDCMD_FIFO_W;
            d.wait_hblank = true;
            const DescriptorWords w = encode_descriptor(d);
            for (uint32_t i = 0; i < DESC_WORDS; i++)
            {
                RAM[(at + i * 2) >> 1] = w.w[i];
            }
            at += DESC_BYTES;
        }

        // Trailing wait/stop pacer: pins the IRQ to a fixed point in the frame
        // instead of letting it drift with the last line's weight.
        Descriptor stop;
        stop.src         = SELFTEST_PACKET;
        stop.count       = 1;
        stop.signal_mask = SIGNAL_NONE;
        stop.wait_hblank = true;
        stop.stop_after  = true;
        const DescriptorWords sw = encode_descriptor(stop);
        for (uint32_t i = 0; i < DESC_WORDS; i++)
        {
            RAM[(at + i * 2) >> 1] = sw.w[i];
        }

        // Arm exactly as the CPU would: a true 16-bit write to DESC.
        engine_write16(ENGINE_DESC, 0);
    }

    // Stand in for the ENGINE ISR the firmware will have: the list stops itself
    // at the end of every frame, so something has to ack and re-arm or the
    // screen goes black after one frame.  Doing it here also exercises the
    // frame handshake — stop_after, level-3 IRQ, CTRL ack, DESC re-arm.
    void selftest_rearm_if_idle()
    {
        if (!engine.walker.irq_pending())
        {
            return;
        }
        engine_write8(ENGINE_CTRL, ENGINE_CTRL_ENABLE_MASK);   // ack, stay enabled
        engine_write16(ENGINE_DESC, 0);
    }

    // Returns true if the rendered frame is what the list asked for.
    bool selftest_check() const
    {
        uint32_t wrong = 0;
        for (int y = 0; y < VideoState::V_ACTIVE; y++)
        {
            for (int x = 0; x < VideoState::H_ACTIVE; x++)
            {
                const uint32_t want =
                    rgb444_to_argb(x < VideoState::H_ACTIVE / 2 ? SELFTEST_FG : SELFTEST_BG);
                if (video.framebuffer[y * VideoState::H_ACTIVE + x] != want)
                {
                    wrong++;
                }
            }
        }
        fprintf(stderr, "display-list self-test: %u of %d pixels wrong; "
                        "engine ran %u descriptors, %u words, %u IRQ\n",
                wrong, VideoState::V_ACTIVE * VideoState::H_ACTIVE,
                engine.walker.total_descriptors(), engine.walker.total_words(),
                engine.walker.irq_count());
        for (const std::string &v : engine.walker.violations())
        {
            fprintf(stderr, "display-list self-test: VIOLATION %s\n", v.c_str());
        }
        return wrong == 0 && engine.walker.violations().empty();
    }

    void engine_write8(uint32_t addr, uint8_t val)
    {
        if (addr == ENGINE_CTRL)
        {
            // Any write clears a pending ~ENGINE_IRQ; D0 low aborts the list.
            engine.walker.clear_irq();
            if ((val & ENGINE_CTRL_ENABLE_MASK) == 0)
            {
                engine.walker.reset();
            }
            update_ipl();
        }
    }

    void engine_write16(uint32_t addr, uint16_t val)
    {
        if (addr == ENGINE_DESC)
        {
            // Arming runs the list immediately, exactly as the hardware does:
            // everything up to the first wait_hblank descriptor executes now
            // (that is where a frame's VBLANK preamble lives), and the rest is
            // released one HBLANK edge at a time by service_video().
            EmulatorSink sink;
            sink.emu = this;
            const SuperEngine::WalkResult w =
                engine.walker.arm(engine_memory(), EngineState::desc_byte_address(val),
                                  getClock(), sink);
            engine_stall_pending += w.busy_cycles;
            update_ipl();
        }
        else
        {
            engine_write8(addr + 1, static_cast<uint8_t>(val & 0xFF));
        }
    }

    // Advance the video scanline timer.  Must be called every CPU step, not
    // batched at the ~1 ms poll_io() rate: a scanline is only ~445 SYSCLK, so
    // polling at 1 ms would run ~31 lines per call, collapsing per-line VIDCMD
    // packets into coarse bands.  check_timer() early-returns when no line
    // boundary has been crossed, and the SDL present fires only once per frame
    // (at v_cnt == V_SYNC_START), so calling this every step is cheap.
    void service_video()
    {
        EmulatorSink sink;
        sink.emu = this;

        uint32_t dma_stall = static_cast<uint32_t>(engine_stall_pending);
        engine_stall_pending = 0;

        video.check_timer(getClock(),
            [this, &sink](int line, uint32_t &stall)
            {
                // /RS.  TIMING pulses it during vertical sync: both FIFOs
                // clear, COMPOSITOR's held colours return to 0xFFF/0x000 and
                // its source to passthrough, and PIXEL's registers return to
                // their defaults.  Anything the previous frame left staged is
                // gone, which is what contains a mis-framed list to one frame.
                if (line == VideoState::V_SYNC_START)
                {
                    pixel_unit.reset();
                    compositor_unit.reset();
                }

                // One HBLANK edge of the descriptor list.  The walker releases
                // the descriptor parked on wait_hblank, then runs on until the
                // next one parks or stop_after fires.
                const SuperEngine::WalkResult w =
                    engine.walker.advance(engine_memory(), getClock(), sink);
                stall += static_cast<uint32_t>(w.busy_cycles);

                if (!video.want_pixels)
                {
                    // Nobody will look at the framebuffer.  The walk above
                    // still ran, so the DMA stall, the audio deposits and the
                    // stop_after IRQ are all unchanged — only PIXEL and
                    // COMPOSITOR, which no guest-readable register touches, are
                    // skipped.  Guest timing is therefore identical either way.
                    return;
                }

                // Blanking first, then the active line: a line's eager SETs are
                // consumed during the HBLANK that precedes it, which is what
                // makes them cost no active slot.  This is the same sequence of
                // unit calls as super-engine's render_line_at_a_time driver,
                // which `make check` holds pixel-identical to its clock-accurate
                // one.
                SuperEngine::run_blanking(pixel_unit, compositor_unit);
                if (line < VideoState::V_ACTIVE)
                {
                    SuperEngine::Rgb444 row[SuperEngine::H_ACTIVE];
                    SuperEngine::render_active_line(pixel_unit, compositor_unit, row);
                    video.store_scanline(line, row);
                }
            });

        if (!getenv("GRIFFIN_NO_DMA_STALL"))
        {
            // How the descriptor list's bus time is charged to the CPU clock.
            //
            // The cost is no longer a fixed per-line constant: EngineWalker
            // returns the SYSCLK it actually spent, derived per descriptor from
            // descriptor.h's model (arbitration + assert + four descriptor words
            // + two cycles per payload word + release).  A frame of the console
            // list and a frame of a per-line-palette list therefore charge
            // different amounts, as they should.
            //
            // Real ENGINE DMA steals the bus at bus-cycle granularity: it
            // arbitrates in, streams a descriptor's payload, and releases,
            // stretching many CPU instructions by a little each.  The CPU's time
            // reference and its interrupt sampling stay continuous with the
            // instruction stream.
            //
            // The SMOOTH model (default) mirrors that: it accrues the stall as a
            // debt and pays it down a few cycles per CPU step, so getClock()
            // never leaps between two whole instructions.
            //
            // The LUMP model (GRIFFIN_DMA_STALL_LUMP) injects a whole line's
            // burst as one sync() in the gap between two instructions.  That is
            // cheaper but unphysical: it fast-forwards the shared clock a dozen
            // instructions' worth in a single interrupt-blind gap, at the video
            // scanline cadence.  On the nommu/UP Linux kernel that coarse,
            // video-phased discontinuity mis-times a scheduler wakeup (a freshly
            // created kthread never gets run) and wedges the fbcon boot right
            // after "crng init done" — even though the *total* stall, and thus
            // the CPU-vs-jiffies slowdown, is identical to SMOOTH.  Kept only
            // for A/B and intra-line tearing studies; do not use it to boot the
            // kernel.
            if (getenv("GRIFFIN_DMA_STALL_LUMP"))
            {
                if (dma_stall > 0)
                {
                    sync(static_cast<int>(dma_stall));
                }
            }
            else
            {
                dma_stall_debt += dma_stall;
                if (dma_stall_debt > 0)
                {
                    // Pay at most STEP cycles per CPU step.  A scanline is ~445
                    // sysclks of guest time and service_video() runs once per
                    // instruction, so many steps elapse per line and the debt
                    // drains well within a line at STEP=4 — keeping the clock
                    // effectively continuous.  GRIFFIN_DMA_STALL_STEP overrides
                    // for experiments.
                    static const char *stepenv = getenv("GRIFFIN_DMA_STALL_STEP");
                    int step = stepenv ? atoi(stepenv) : 4;
                    if (step <= 0) step = 4;
                    int pay = static_cast<int>(std::min<uint64_t>(dma_stall_debt, step));
                    sync(pay);
                    dma_stall_debt -= pay;
                }
            }
        }

        // Hand PORTS exactly the scanlines the raster just emitted; TIMING's
        // PADDLE_TICK and AUDIO_TICK toggle on each of them and PORTS takes an
        // event on every second one.  PORTS has no timer of its own by design,
        // so this is its only clock for the paddle counters and the audio pop.
        if (video.line_count != ports_line_count)
        {
            ports.advance_scanlines(static_cast<uint32_t>(video.line_count - ports_line_count));
            ports_line_count = video.line_count;
        }
        update_ipl();
    }

    // Unified IPL management — picks highest active interrupt source
    void update_ipl()
    {
        // Level 6 is TIMING's vsync, latched in GLUE (VSYNC_STATUS/VSYNC_CLEAR),
        // and gated onto IPL by CONFIG bit VSYNC_IRQ_EN — the latch itself keeps
        // running when that is clear so the guest can poll VSYNC_STATUS.
        if (video.irq_pending() && vsync_irq_en) {
            setIPL(VSYNC_IRQ_LEVEL);
        } else if (duart.irq_pending(pty_console, pty_console_b)) {
            setIPL(DUART_IRQ_LEVEL);
        } else if (ps2.irq_pending()) {
            setIPL(GLUE_IRQ_LEVEL);
        } else if (engine.walker.irq_pending()) {
            // stop_after on the last descriptor: the list has finished and
            // disarmed itself.  Level 3 is LEVEL-triggered, so the ISR must
            // write ENGINE_CTRL to clear it or it re-enters forever.
            static_assert(ENGINE_IRQ_LEVEL < GLUE_IRQ_LEVEL);
            setIPL(ENGINE_IRQ_LEVEL);
        } else if (ports.irq_pending()) {
            // Lowest of the four CPLDs: below ENGINE's level 3.
            static_assert(PORTS_IRQ_LEVEL < ENGINE_IRQ_LEVEL);
            setIPL(PORTS_IRQ_LEVEL);
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
    printf("%s [options] rom-filename\n", progname);
    printf("  --cf disk.img                 attach CompactFlash image (read/write)\n");
    printf("  --cf-ro disk.img              attach CompactFlash image (read-only)\n");
    printf("\n");
    printf("  Automation (default is interactive pty console + SDL window):\n");
    printf("  --console-stdio               DUART console on stdin/stdout instead of a pty\n");
    printf("  --console-in FILE             feed DUART console input from FILE\n");
    printf("  --console-out FILE            write DUART console output to FILE\n");
    printf("  --ps2-in FILE                 inject PS/2 keystrokes from a script file\n");
    printf("                                (directives: delay MS | text STRING | raw HH..)\n");
    printf("  --mouse-in FILE               inject PS/2 mouse input from a script file\n");
    printf("                                (directives: delay MS | move DX DY |\n");
    printf("                                 button LMR|none | click L | raw HH..)\n");
    printf("  --joystick-in FILE            inject joystick/paddle input from a script file\n");
    printf("                                (directives: delay MS | stick1 NAMES |\n");
    printf("                                 stick2 NAMES | paddle-a N | paddle-b N;\n");
    printf("                                 names: up down left right fire pin9 pin5 none)\n");
    printf("  --serialb-pty                 DUART channel B on a fresh host pty (path printed)\n");
    printf("  --serialb-dev PATH            DUART channel B on an existing device (e.g. a\n");
    printf("                                socat pty bridge to a persistent host pppd)\n");
    printf("  --serialb-in FILE             feed DUART channel B input from FILE\n");
    printf("  --serialb-out FILE            write DUART channel B output to FILE\n");
    printf("  --keys-to-joystick            host arrows + space/left-ctrl drive joystick\n");
    printf("                                port 1 instead of the PS/2 keyboard\n");
    printf("  --wav-out FILE                record the audio DACs to FILE (8-bit stereo WAV)\n");
    printf("  --headless                    do not open an SDL video window (also no sound)\n");
    printf("  --screenshot FILE             write the framebuffer to FILE (BMP) on exit\n");
    printf("  --run-cycles N                stop after N emulated SYSCLK cycles\n");
    printf("  --no-throttle                 run as fast as possible (no real-time pacing)\n");
}

int main(int argc, const char** argv)
{
    const char *progname = argv[0];
    argc -= 1;
    argv += 1;
    const char *cf_path = nullptr;
    bool cf_ro = false;

    // Automation options (default: interactive pty + SDL window, run forever).
    enum { CONSOLE_PTY, CONSOLE_STDIO, CONSOLE_FILES } console_mode = CONSOLE_PTY;
    const char *console_in_path = nullptr;
    const char *console_out_path = nullptr;
    const char *ps2_in_path = nullptr;
    const char *mouse_in_path = nullptr;
    const char *joystick_in_path = nullptr;
    bool serialb_pty = false;
    const char *serialb_dev_path = nullptr;
    const char *serialb_in_path = nullptr;
    const char *serialb_out_path = nullptr;
    const char *screenshot_path = nullptr;
    const char *wav_out_path = nullptr;
    bool keys_to_joystick = false;
    bool selftest_displaylist = false;
    bool headless = false;
    bool no_throttle = false;
    uint64_t run_cycles = 0; // 0 = unlimited

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
        } else if(strcmp(argv[0], "--console-stdio") == 0) {
            console_mode = CONSOLE_STDIO;
            argv += 1;
            argc -= 1;
        } else if(strcmp(argv[0], "--console-in") == 0) {
            if(argc < 2) {
                fprintf(stderr, "--console-in option requires a file path.\n");
                exit(EXIT_FAILURE);
            }
            console_mode = CONSOLE_FILES;
            console_in_path = argv[1];
            argv += 2;
            argc -= 2;
        } else if(strcmp(argv[0], "--console-out") == 0) {
            if(argc < 2) {
                fprintf(stderr, "--console-out option requires a file path.\n");
                exit(EXIT_FAILURE);
            }
            console_mode = CONSOLE_FILES;
            console_out_path = argv[1];
            argv += 2;
            argc -= 2;
        } else if(strcmp(argv[0], "--ps2-in") == 0) {
            if(argc < 2) {
                fprintf(stderr, "--ps2-in option requires a file path.\n");
                exit(EXIT_FAILURE);
            }
            ps2_in_path = argv[1];
            argv += 2;
            argc -= 2;
        } else if(strcmp(argv[0], "--mouse-in") == 0) {
            if(argc < 2) {
                fprintf(stderr, "--mouse-in option requires a file path.\n");
                exit(EXIT_FAILURE);
            }
            mouse_in_path = argv[1];
            argv += 2;
            argc -= 2;
        } else if(strcmp(argv[0], "--joystick-in") == 0) {
            if(argc < 2) {
                fprintf(stderr, "--joystick-in option requires a file path.\n");
                exit(EXIT_FAILURE);
            }
            joystick_in_path = argv[1];
            argv += 2;
            argc -= 2;
        } else if(strcmp(argv[0], "--serialb-pty") == 0) {
            serialb_pty = true;
            argv += 1;
            argc -= 1;
        } else if(strcmp(argv[0], "--serialb-dev") == 0) {
            if(argc < 2) {
                fprintf(stderr, "--serialb-dev option requires a device path.\n");
                exit(EXIT_FAILURE);
            }
            serialb_dev_path = argv[1];
            argv += 2;
            argc -= 2;
        } else if(strcmp(argv[0], "--serialb-in") == 0) {
            if(argc < 2) {
                fprintf(stderr, "--serialb-in option requires a file path.\n");
                exit(EXIT_FAILURE);
            }
            serialb_in_path = argv[1];
            argv += 2;
            argc -= 2;
        } else if(strcmp(argv[0], "--serialb-out") == 0) {
            if(argc < 2) {
                fprintf(stderr, "--serialb-out option requires a file path.\n");
                exit(EXIT_FAILURE);
            }
            serialb_out_path = argv[1];
            argv += 2;
            argc -= 2;
        } else if(strcmp(argv[0], "--selftest-displaylist") == 0) {
            selftest_displaylist = true;
            argv += 1;
            argc -= 1;
        } else if(strcmp(argv[0], "--keys-to-joystick") == 0) {
            keys_to_joystick = true;
            argv += 1;
            argc -= 1;
        } else if(strcmp(argv[0], "--wav-out") == 0) {
            if(argc < 2) {
                fprintf(stderr, "--wav-out option requires a file path.\n");
                exit(EXIT_FAILURE);
            }
            wav_out_path = argv[1];
            argv += 2;
            argc -= 2;
        } else if(strcmp(argv[0], "--headless") == 0) {
            headless = true;
            argv += 1;
            argc -= 1;
        } else if(strcmp(argv[0], "--screenshot") == 0) {
            if(argc < 2) {
                fprintf(stderr, "--screenshot option requires a file path.\n");
                exit(EXIT_FAILURE);
            }
            screenshot_path = argv[1];
            argv += 2;
            argc -= 2;
        } else if(strcmp(argv[0], "--no-throttle") == 0) {
            no_throttle = true;
            argv += 1;
            argc -= 1;
        } else if(strcmp(argv[0], "--run-cycles") == 0) {
            if(argc < 2) {
                fprintf(stderr, "--run-cycles option requires a cycle count.\n");
                exit(EXIT_FAILURE);
            }
            run_cycles = strtoull(argv[1], nullptr, 0);
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

    GriffinEmulator emulator;

    PS2Script ps2_script;
    if (ps2_in_path && !ps2_script.load(ps2_in_path))
    {
        exit(EXIT_FAILURE);
    }

    MouseScript mouse_script;
    if (mouse_in_path && !mouse_script.load(mouse_in_path))
    {
        exit(EXIT_FAILURE);
    }

    JoystickScript joystick_script;
    if (joystick_in_path && !joystick_script.load(joystick_in_path))
    {
        exit(EXIT_FAILURE);
    }

    if (const char *prof = getenv("GRIFFIN_PROFILE"))
    {
        uint64_t s = 0, e = 0;
        if (sscanf(prof, "%" SCNu64 ":%" SCNu64, &s, &e) == 2 && e > s)
        {
            emulator.profile_init(s, e);
        }
        else
        {
            fprintf(stderr, "GRIFFIN_PROFILE wants START:END (SYSCLK cycles)\n");
            exit(EXIT_FAILURE);
        }
    }

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

    switch (console_mode)
    {
        case CONSOLE_STDIO:
            if(!emulator.init_console_stdio())
            {
                fprintf(stderr, "Failed to set up stdio console\n");
                exit(EXIT_FAILURE);
            }
            break;
        case CONSOLE_FILES:
            if(!emulator.init_console_files(console_in_path, console_out_path))
            {
                exit(EXIT_FAILURE);
            }
            break;
        case CONSOLE_PTY:
        default:
            if(!emulator.init_pty())
            {
                fprintf(stderr, "Failed to open PTY for console\n");
                exit(EXIT_FAILURE);
            }
            break;
    }

    // DUART channel B endpoint (default: unconnected).
    if (serialb_pty)
    {
        if (!emulator.init_serialb_pty())
        {
            fprintf(stderr, "Failed to open PTY for serial B\n");
            exit(EXIT_FAILURE);
        }
    }
    else if (serialb_dev_path)
    {
        if (!emulator.init_serialb_dev(serialb_dev_path))
        {
            exit(EXIT_FAILURE);
        }
    }
    else if (serialb_in_path || serialb_out_path)
    {
        if (!emulator.init_serialb_files(serialb_in_path, serialb_out_path))
        {
            exit(EXIT_FAILURE);
        }
    }

    // The raw-stdin ~. exit watcher would fight --console-stdio over stdin, so
    // only enable it when the DUART console is not already using stdin.
    if (console_mode != CONSOLE_STDIO)
    {
        emulator.init_stdin();
    }

    /* Render the framebuffer only if something will look at it: an SDL
     * window (not headless) or a requested screenshot.  Headless console-
     * only runs skip PIXEL/COMPOSITOR entirely; the descriptor walk, its DMA
     * stall and the audio deposits still run, so guest timing is unaffected. */
    bool want_pixels = !headless || (screenshot_path != nullptr) || selftest_displaylist;
    if (!emulator.init_video(headless, want_pixels))
    {
        fprintf(stderr, "Warning: SDL/Video init failed, display disabled\n");
    }
    else if (!headless)
    {
        // Gamepads are the interactive joystick source; headless runs use
        // --joystick-in instead and need no SDL at all.
        emulator.init_gamepads();
    }
    emulator.set_keys_to_joystick(keys_to_joystick);

    // Attach the DAC sink before the CPU is released so a capture always
    // starts at reset and is reproducible for a given --run-cycles.  Host
    // playback follows the window: an SDL audio device makes no sense for an
    // unattended headless run, and a capture is what those runs want anyway.
    AudioOutput audio_output;
    if (wav_out_path && !audio_output.open_wav(wav_out_path))
    {
        exit(EXIT_FAILURE);
    }
    if (!headless)
    {
        audio_output.open_playback();
    }
    if (audio_output.active())
    {
        emulator.ports_model().audio_out = &audio_output;
    }

    // Bring-up self-check: poke a descriptor list into RAM and arm ENGINE
    // before the guest runs.  Whatever the CPU does afterwards is irrelevant —
    // the engine bus-masters on its own once armed, which is the point.
    bool selftest_ok = true;
    if (selftest_displaylist)
    {
        emulator.selftest_build_list();
    }

    SoftUART debug_uart(emulator.get_debug_latch() & 1); // Bit 0 = DEBUG_OUT serial line

    emulator.setDasmSyntax(moira::Syntax::GNU_MIT);
    // Griffin's CPU is a 68010 (VBR/movec, format-8 bus-error frames).
    emulator.setModel(moira::Model::M68010);
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
        if (!emulator.profile_hist.empty())
        {
            emulator.profile_sample();
        }
        emulator.service_video();
        auto clock_now = emulator.getClock();
        // time(2) once per instruction was measurable host overhead; the
        // once-per-second stats only need a coarse check.
        static unsigned time_check_countdown = 0;
        static time_t now = time(0);
        if (++time_check_countdown >= 65536)
        {
            time_check_countdown = 0;
            now = time(0);
        }

        while(clock_now > clock_next_audio) {
            uint8_t dac_value = emulator.GetAudioDACValue();
            fwrite(&dac_value, 1, 1, audio);
            clock_next_audio += sysclk_per_audio;
        }

        if (selftest_displaylist)
        {
            emulator.selftest_rearm_if_idle();
        }

        if (run_cycles != 0 && clock_now >= run_cycles)
        {
            running = false;
        }

        if (!no_throttle && ++throttle_counter >= THROTTLE_INTERVAL)
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
            ps2_script.service(clock_now, emulator.ps2_model());
            mouse_script.service(clock_now, emulator.ports_model());
            joystick_script.service(clock_now, emulator.ports_model());
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

    if (getenv("GRIFFIN_DUMP_ON_EXIT"))
    {
        emulator.dump_cpu_state("at exit");
    }
    emulator.profile_dump();
    if (const char *rd = getenv("GRIFFIN_DUMP_RAM"))
    {
        uint32_t addr = 0, len = 0;
        if (sscanf(rd, "%x:%x", &addr, &len) == 2)
            emulator.dump_ram_ascii(addr, len);
    }


    if (audio)
    {
        fclose(audio);
    }

    // Patch the WAV header with the final frame count and let go of the host
    // audio device while SDL is still up (VideoState's destructor calls
    // SDL_Quit).
    emulator.ports_model().audio_out = nullptr;
    audio_output.close();

    if (selftest_displaylist)
    {
        selftest_ok = emulator.selftest_check();
    }

    if (screenshot_path)
    {
        emulator.dump_framebuffer(screenshot_path);
    }

    if (selftest_displaylist && !selftest_ok)
    {
        fflush(stdout);
        return 1;
    }

    fflush(stdout);

    return 0;
}
