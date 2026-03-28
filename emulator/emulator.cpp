#include <array>
#include <cstdint>
#include <cinttypes>

#include "../griffin.generated.h"

// pty.h / util.h pull in termios.h which #defines EXTB, colliding with
// a Moira enum member.  Include Moira first, then the PTY header.
#include "Moira.h"

#include <fcntl.h>
#include <sys/select.h>
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
constexpr uint32_t DEBUG_IO_MCU = 0x0010;
constexpr uint32_t debug = 0; // DEBUG_BUS | DEBUG_IO | DEBUG_UART;

using namespace Griffin;

// PTY-based console for IO_MCU serial emulation.
// The master fd acts like the IO_MCU's UART: write() sends to the terminal,
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
        fprintf(stderr, "IO_MCU console PTY: %s\n", ttyname(slave_fd));
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

// IO_MCU event queue — mirrors the real MCU's event FIFO.
// Firmware reads IO_MCU_RX_DATA to dequeue; IO_MCU_STATUS reports QUEUE_NOTEMPTY.
struct IOmcuState
{
    static constexpr size_t QUEUE_SIZE = 256;
    uint8_t queue[QUEUE_SIZE];
    size_t head = 0;
    size_t count = 0;
    bool overflow = false;
    bool io_reset_released = false;

    void push(uint8_t byte)
    {
        if(count >= QUEUE_SIZE)
        {
            overflow = true;
            return;
        }
        queue[(head + count) % QUEUE_SIZE] = byte;
        count++;
    }

    uint8_t pop()
    {
        if(count == 0)
        {
            return IO_MCU_EVT_EMPTY;
        }
        uint8_t byte = queue[head % QUEUE_SIZE];
        head = (head + 1) % QUEUE_SIZE;
        count--;
        return byte;
    }

    bool notempty() const
    {
        return count > 0;
    }

    uint8_t status() const
    {
        uint8_t s = 0;
        if(notempty())
        {
            s |= IO_MCU_STATUS_QUEUE_NOTEMPTY_MASK;
        }
        s |= IO_MCU_STATUS_TX_READY_MASK; // TX is always ready in emulation
        if(overflow)
        {
            s |= IO_MCU_STATUS_OVERFLOW_MASK;
        }
        return s;
    }

    // Poll the PTY and enqueue any received bytes as UART_RX events.
    void poll(const PTYConsole &pty)
    {
        if(!io_reset_released)
        {
            return;
        }
        uint8_t ch;
        while(pty.is_data_ready() && pty.receive(&ch))
        {
            push(IO_MCU_EVT_UART_RX);
            push(ch);
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
    mutable IOmcuState io_mcu;

    uint8_t IO_read8(uint32_t addr) const
    {
        if(addr == GLUE_UART_STATUS - IO_BASE) {
            // Hardware UART is never busy in emulation (instant TX)
            return 0;
        } else if(addr == GLUE_DEBUG_IN - IO_BASE) {
            return 0;
        } else if(addr == IO_MCU_RX_DATA - IO_BASE) {
            uint8_t val = io_mcu.pop();
            if(debug & DEBUG_IO_MCU)
            {
                printf("[IO_MCU RX_DATA: 0x%02X]\n", val);
            }
            // Deassert interrupt immediately when queue drains
            if(!io_mcu.notempty())
            {
                const_cast<GriffinEmulator*>(this)->setIPL(0);
            }
            return val;
        } else if(addr == IO_MCU_STATUS - IO_BASE) {
            return io_mcu.status();
        }
        if(debug & DEBUG_IO)
        {
            printf("read of uint8_t at unhandled IO %06X\n", addr + IO_BASE);
        }
        return 0;
    }

    uint16_t IO_read16(uint32_t addr) const
    {
        if(debug & DEBUG_IO)
        {
            printf("read of uint16_t at unhandled IO %06X\n", addr + IO_BASE);
        }
        return 0;
    }

    void IO_write8(uint32_t addr, uint8_t val) const
    {
        if(addr == GLUE_DEBUG_OUT - IO_BASE) {
            auto oldbit = debug_out_latch & GLUE_DEBUG_OUT_MASK;
            auto bit = val & GLUE_DEBUG_OUT_MASK;
            if(bit != oldbit)
            {
                if(debug & DEBUG_IO) printf("debug_out, %" PRIu64 ", %d\n", getClock(), bit);
            }
            debug_out_latch = val;
        } else if(addr == GLUE_UART_TX_DATA - IO_BASE) {
            // Hardware UART TX: emit character immediately to stdout
            printf("%c", val);
            if(debug & DEBUG_UART) printf("[UART TX: 0x%02X]", val);
            fflush(stdout);
        } else if(addr == GLUE_CONFIG - IO_BASE) {
            if(val & GLUE_CONFIG_ROM_OVERLAY_DISABLE_MASK)
            {
                if(debug & DEBUG_IO) printf("ROM overlay disabled\n");
                ROMoverlay = false;
            }
            if(val & GLUE_CONFIG_IO_RESET_RELEASE_MASK)
            {
                if(debug & DEBUG_IO) printf("IO_MCU reset released\n");
                io_mcu.io_reset_released = true;
            }
        } else if(addr == IO_MCU_TX_DATA - IO_BASE) {
            // IO_MCU UART TX: send to PTY console
            if(debug & DEBUG_IO_MCU) printf("[IO_MCU TX: 0x%02X]\n", val);
            pty_console.send(val);
        } else if(addr == IO_MCU_CONFIG - IO_BASE) {
            if(debug & DEBUG_IO_MCU) printf("[IO_MCU CONFIG: 0x%02X]\n", val);
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
        if(debug & DEBUG_IO)
        {
            printf("write of uint16_t %04X at unhandled IO %06X\n", val, addr + IO_BASE);
        }
    }

    // Wait state penalty (extra SYSCLK cycles) for a memory access,
    // matching the GLUE CPLD DTACK generation thresholds.
    //   RAM, GLUE, VIDEO: ws_cnt >= 2  (baseline, 0 penalty)
    //   ROM, AUDIO:       ws_cnt >= 4  (+2 clocks)
    //   CF:               ws_cnt >= 14 (+12 clocks)
    // Note: read16 for RAM calls read8 twice, but RAM penalty is 0
    // so double-application is harmless.
    int wait_state_penalty(uint32_t addr) const
    {
        if ((ROMoverlay && addr < ROM_SIZE) ||
            (addr >= ROM_BASE && addr < ROM_BASE + ROM_WINDOW))
        {
            return 2;
        }
        if (addr >= IO_BASE && addr < IO_BASE + IO_SIZE)
        {
            unsigned sub = (addr >> 18) & 0x3;
            if (sub == 1)       // CF: 0xF40000
            {
                return 12;
            }
            if (sub == 3)       // AUDIO: 0xFC0000
            {
                return 2;
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

    enum RAMConfig {RAM_1_BANK_256K, RAM_1M, RAM_2M, RAM_3M, RAM_4M };

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

    // Poll the PTY for incoming data and update IPL.
    // Call this periodically from the main loop, not on every bus cycle.
    void poll_io_mcu()
    {
        io_mcu.poll(pty_console);
        if(io_mcu.notempty())
        {
            setIPL(IO_MCU_IRQ_LEVEL);
        }
        else
        {
            setIPL(0);
        }
    }
};

// Courtesy Claude Opus 4.6

static constexpr int OVERSAMPLE = 16;
static constexpr int BAUDRATE = 9600;
static constexpr int SOFT_UART_SAMPLE_INTERVAL = SYSCLK_HZ / (BAUDRATE * OVERSAMPLE);

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
    printf("%s [-m {256,1024,2048,3072,4096}] rom-filename\n", progname);
}

int main(int argc, const char** argv)
{
    const char *progname = argv[0];
    argc -= 1;
    argv += 1;
    auto ram_config = GriffinEmulator::RAM_1_BANK_256K;

    while((argc > 0) && (argv[0][0] == '-')) {
	if(strcmp(argv[0], "-m") == 0) {
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
        fprintf(stderr, "Failed to open PTY for IO_MCU console\n");
        exit(EXIT_FAILURE);
    }

    SoftUART debug_uart(emulator.get_debug_latch()); // This represents an FTDI 232 attached to the debug out pin
    emulator.setDasmSyntax(moira::Syntax::GNU_MIT);
    emulator.reset();
    printf("begin execution\n");
    uint64_t previous_uart_sample = 0;
    uint64_t previous_io_mcu_poll = 0;
    static constexpr uint64_t IO_MCU_POLL_INTERVAL = SYSCLK_HZ / 1000; // ~1ms
    auto clock_then = emulator.getClock();
    auto then = time(0);
    while (1) {
        if(debug & DEBUG_DISASSEMBLE) {
            static char str[1024];
            emulator.disassemble(str, emulator.getPC());
            printf("%04X: %s\n", emulator.getPC(), str);
        }
        emulator.execute();
        auto clock_now = emulator.getClock();
        auto now = time(0);

        while(clock_now / SOFT_UART_SAMPLE_INTERVAL != previous_uart_sample / SOFT_UART_SAMPLE_INTERVAL)
        {
            debug_uart.clock(emulator.get_debug_latch());
            previous_uart_sample = previous_uart_sample + SOFT_UART_SAMPLE_INTERVAL;
        }

        if(clock_now - previous_io_mcu_poll >= IO_MCU_POLL_INTERVAL)
        {
            emulator.poll_io_mcu();
            previous_io_mcu_poll = clock_now;
        }

        if(now != then)
        {
            printf("%" PRIu64 " clocks\n", clock_now - clock_then);
            clock_then = clock_now;
            then = now;
        }
    }
}
