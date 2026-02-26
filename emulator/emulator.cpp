#include <array>
#include <cstdint>
#include <cinttypes>

#include "griffin.h"

#include "Moira.h"

constexpr uint32_t DEBUG_MEMORY = 0x0001;
constexpr uint32_t DEBUG_IO = 0x0002;
constexpr uint32_t debug = 0; // DEBUG_MEMORY | DEBUG_IO;

using namespace Griffin;

class GriffinEmulator : public moira::Moira
{
    mutable std::vector<uint8_t> RAM_bank1;
    mutable std::vector<uint8_t> RAM_bank2;
    mutable std::vector<uint8_t> RAM_bank3;
    mutable std::vector<uint8_t> RAM_bank4;
    mutable std::array<uint8_t, ROMsize> ROM{};
    mutable int debug_out_latch = 0;
    mutable bool ROMoverlay = true;

    uint8_t IO_read8(uint32_t addr) const
    {
        printf("read of uint8_t at unhandled IO %06X\n", addr);
        abort();
        return 0;
    }

    uint16_t IO_read16(uint32_t addr) const
    {
        printf("read of uint16_t at unhandled IO %06X\n", addr);
        abort();
        return 0;
    }

    void IO_write8(uint32_t addr, uint8_t val) const
    {
        if(addr == GLUE_DEBUG_OUT - GLUEbase) {
            auto bit = val & GLUE_DEBUG_OUT_BIT;
            if(bit != debug_out_latch) {
                if(debug & DEBUG_IO) printf("debug_out, %" PRIu64 ", %d\n", getClock(), bit);
            }
            debug_out_latch = bit;
        } else if(addr == GLUE_OVERLAY_DISABLE - GLUEbase) {
            if(debug & DEBUG_IO) printf("ROM overlay disabled\n");
            ROMoverlay = false;
        } else {
            if(isprint(val)) {
                printf("%" PRIx32 " = %" PRIx8 " (%c)\n", addr, addr, val);
            } else {
                printf("%" PRIx32 " = %" PRIx8 "\n", addr, addr);
            }
            // printf("%c", val);
        }
    }

    void IO_write16(uint32_t addr, uint16_t val) const
    {
        printf("write of uint16_t at unhandled IO %06X\n", addr);
        abort();
    }

public:

    enum RAMConfig {RAM_1_BANK_256K, RAM_1_BANK_1M, RAM_4M };

    GriffinEmulator(RAMConfig ram_config)
    {
        switch(ram_config)
        {
            case RAM_1_BANK_256K:
                RAM_bank1.resize(256 * 1024, 0);
                break;
            case RAM_1_BANK_1M:
                RAM_bank1.resize(1024 * 1024, 0);
                break;
            case RAM_4M:
                RAM_bank1.resize(1024 * 1024, 0);
                RAM_bank2.resize(1024 * 1024, 0);
                RAM_bank3.resize(1024 * 1024, 0);
                RAM_bank4.resize(1024 * 1024, 0);
                break;
        }
    }

    uint8_t read8(uint32_t addr) const override
    {
        if(debug & DEBUG_MEMORY) { printf("read of uint8_t at %06X\n", addr); }
        if (ROMoverlay && (addr < ROMsize)) {
            return ROM[addr];
        } else if (RAM_BANK_1.contains(addr)) {
            if(RAM_bank1.size() == 0) {
                return 0;
            } else {
                return RAM_bank1[RAM_BANK_1.get(addr) % RAM_bank1.size()];
            }
        } else if (RAM_BANK_2.contains(addr)) {
            if(RAM_bank2.size() == 0) {
                return 0;
            } else {
                return RAM_bank2[RAM_BANK_2.get(addr) % RAM_bank2.size()];
            }
        } else if (RAM_BANK_3.contains(addr)) {
            if(RAM_bank3.size() == 0) {
                return 0;
            } else {
                return RAM_bank3[RAM_BANK_3.get(addr) % RAM_bank3.size()];
            }
        } else if (RAM_BANK_4.contains(addr)) {
            if(RAM_bank4.size() == 0) {
                return 0;
            } else {
                return RAM_bank4[RAM_BANK_4.get(addr) % RAM_bank4.size()];
            }
        } else if (addr >= ROMbase && addr < ROMbase + ROMsize) {
            return ROM[addr - ROMbase];
        } else if (addr >= IObase && addr < (IObase + IOsize)) {
            return IO_read8(addr - IObase);
        } else {
            printf("read of uint8_t at unhandled %06X\n", addr);
            abort();
        }
    }

    uint16_t read16(uint32_t addr) const override
    {
        if(debug & DEBUG_MEMORY) { printf("read of uint16_t at %06X\n", addr); }
        if (ROMoverlay && (addr < ROMsize)) {
            return (ROM[addr] << 8) | ROM[addr + 1];
        } else if (RAM_BANK_1.contains(addr)) {
            return (read8(addr) << 8) | read8(addr + 1);
        } else if (RAM_BANK_2.contains(addr)) {
            return (read8(addr) << 8) | read8(addr + 1);
        } else if (RAM_BANK_3.contains(addr)) {
            return (read8(addr) << 8) | read8(addr + 1);
        } else if (RAM_BANK_4.contains(addr)) {
            return (read8(addr) << 8) | read8(addr + 1);
        } else if (addr >= ROMbase && addr < ROMbase + ROMsize) {
            return (ROM[addr - ROMbase] << 8) | ROM[addr - ROMbase + 1];
        } else if (addr >= IObase && addr < (IObase + IOsize)) {
            return IO_read16(addr - IObase);
        } else {
            printf("read of uint16_t at unhandled %06X\n", addr);
            abort();
        }
    }

    void write8(uint32_t addr, uint8_t val) const override
    {
        if(debug & DEBUG_MEMORY) { printf("write of uint8_t %02X at %06X\n", val, addr); }
        if (RAM_BANK_1.contains(addr)) {
            if(RAM_bank1.size() != 0) {
                RAM_bank1[RAM_BANK_1.get(addr) % RAM_bank1.size()] = val;
            } 
        } else if (RAM_BANK_2.contains(addr)) {
            if(RAM_bank2.size() != 0) {
                RAM_bank2[RAM_BANK_2.get(addr) % RAM_bank2.size()] = val;
            } 
        } else if (RAM_BANK_3.contains(addr)) {
            if(RAM_bank3.size() != 0) {
                RAM_bank3[RAM_BANK_3.get(addr) % RAM_bank3.size()] = val;
            } 
        } else if (RAM_BANK_4.contains(addr)) {
            if(RAM_bank4.size() != 0) {
                RAM_bank4[RAM_BANK_4.get(addr) % RAM_bank4.size()] = val;
            } 
        } else if (addr >= ROMbase && addr < ROMbase + ROMsize) {
            return;
        } else if (addr >= IObase && addr < (IObase + IOsize)) {
            IO_write8(addr - IObase, val);
        } else {
            printf("write of uint8_t %02X to unhandled %06X\n", val, addr);
            abort();
        }
    }

    void write16(uint32_t addr, uint16_t val) const override
    {
        if(debug & DEBUG_MEMORY) { printf("write of uint16_t %04X at %06X\n", val, addr); }
        uint8_t high = (val >> 8);
        uint8_t low = (val & 0xFF);

        if (RAM_BANK_1.contains(addr)) {
            if(RAM_bank1.size() != 0) {
                RAM_bank1[RAM_BANK_1.get(addr) % RAM_bank1.size()] = high;
                RAM_bank1[RAM_BANK_1.get(addr + 1) % RAM_bank1.size()] = low;
            } 
        } else if (RAM_BANK_2.contains(addr)) {
            if(RAM_bank2.size() != 0) {
                RAM_bank2[RAM_BANK_2.get(addr) % RAM_bank2.size()] = high;
                RAM_bank2[RAM_BANK_2.get(addr + 1) % RAM_bank2.size()] = low;
            } 
        } else if (RAM_BANK_3.contains(addr)) {
            if(RAM_bank3.size() != 0) {
                RAM_bank3[RAM_BANK_3.get(addr) % RAM_bank3.size()] = high;
                RAM_bank3[RAM_BANK_3.get(addr + 1) % RAM_bank3.size()] = low;
            } 
        } else if (RAM_BANK_4.contains(addr)) {
            if(RAM_bank4.size() != 0) {
                RAM_bank4[RAM_BANK_4.get(addr) % RAM_bank4.size()] = high;
                RAM_bank4[RAM_BANK_4.get(addr + 1) % RAM_bank4.size()] = low;
            } 
        } else if (addr >= ROMbase && addr < ROMbase + ROMsize) {
            return;
        } else if (addr >= IObase && addr < (IObase + IOsize)) {
            IO_write16(addr - IObase, val);
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
};

// Courtesy Claude Opus 4.6

static constexpr int SYSCLK = 12'000'000;
static constexpr int OVERSAMPLE = 16;
static constexpr int BAUDRATE = 9600;
static constexpr int SOFT_UART_SAMPLE_INTERVAL = SYSCLK / (BAUDRATE * OVERSAMPLE);

// TODO parameterize this on SYSCLOCK and OVERSAMPLE and BAUDRATE
struct SoftUART
{
    int state = 0;          // 0 = idle, 1 = receiving
    int sample_count = 0;
    int bit_index = 0;
    uint8_t shift_reg = 0;
    int last_level = 1;

    SoftUART(int start_level) : last_level(start_level) { }

    // Call this at 16x baud rate (9600 * 16 = 153600 Hz)
    // For a 12 MHz system that's every 78.125 cycles — call every 78 cycles
    void clock(int level)
    {
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
                        fflush(stdout);
                    }
                    state = 0;
                }
            }
        }
        last_level = level;
    }
};

int main(int argc, const char** argv)
{
    if (argc < 2) {
        fprintf(stderr, "Usage: %s <rom-file>\n", argv[0]);
        exit(EXIT_FAILURE);
    }

    GriffinEmulator emulator(GriffinEmulator::RAM_1_BANK_256K);

    FILE* fp = fopen(argv[1], "rb");
    if (fp == NULL) {
        fprintf(stderr, "Couldn't open \"%s\" for reading\n", argv[1]);
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

    SoftUART debug_uart(emulator.get_debug_latch()); // This represents an FTDI 232 attached to the debug out pin
    emulator.setDasmSyntax(moira::Syntax::GNU_MIT);
    emulator.reset();
    printf("begin execution\n");
    uint64_t previous_uart_sample = 0;
    while (1) {
        if(0) {
            static char str[1024];
            emulator.disassemble(str, emulator.getPC());
            printf("%04X: %s\n", emulator.getPC(), str);
        }
        emulator.execute();
        auto current_clock = emulator.getClock();
        if(current_clock / SOFT_UART_SAMPLE_INTERVAL != previous_uart_sample / SOFT_UART_SAMPLE_INTERVAL)
        {
            debug_uart.clock(emulator.get_debug_latch());
            previous_uart_sample = current_clock;
        }
    }
}
