#include <cstdint>

struct MemoryRange
{
    uint32_t base;
    uint32_t size;
    MemoryRange(uint32_t base, uint32_t size) : base(base), size(size)
    {
    }
    bool contains(uint32_t addr)
    {
        return (addr >= base) && (addr < base + size);
    }
    uint32_t get(uint32_t addr)
    {
        return addr - base;
    }
};

namespace Griffin
{
    // But these may not be populated
    MemoryRange RAM_BANK_1(0 * 1024 * 1024, 1024 * 1024);
    MemoryRange RAM_BANK_2(1 * 1024 * 1024, 1024 * 1024);
    MemoryRange RAM_BANK_3(2 * 1024 * 1024, 1024 * 1024);
    MemoryRange RAM_BANK_4(3 * 1024 * 1024, 1024 * 1024);

    static constexpr uint32_t ROMbase = 0xC00000;
    static constexpr uint32_t ROMsize = 128 * 1024;

    static constexpr uint32_t OBVIDbase = 0xE00000;
    static constexpr uint32_t OBVIDsize = 0x100000;

    static constexpr uint32_t IObase = 0xF00000;
    static constexpr uint32_t IOsize = 0x100000;

    static constexpr uint32_t GLUEbase  = IObase + 0x00000;
    static constexpr uint32_t CFbase    = IObase + 0x10000;
    static constexpr uint32_t MCUbase   = IObase + 0x20000;
    static constexpr uint32_t AUDIObase = IObase + 0x30000;

    static constexpr uint32_t GLUE_DEBUG_OUT  = GLUEbase + 0x01;
    static constexpr uint32_t GLUE_DEBUG_IN   = GLUEbase + 0x03;
    static constexpr uint32_t GLUE_OVERLAY_DISABLE   = GLUEbase + 0x07;

    static constexpr uint32_t GLUE_DEBUG_OUT_BIT  = 0x01;
};
