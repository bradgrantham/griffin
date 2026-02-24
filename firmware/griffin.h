#include <cstdint>

namespace Griffin
{
    static constexpr uint32_t RAMbase = 0x0;
    static constexpr uint32_t RAMsize = 256 * 1024; // 4 * 1024 * 1024;

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
    static constexpr uint32_t GLUE_UNSHADOW   = GLUEbase + 0x07;

    static constexpr uint32_t GLUE_DEBUG_OUT_BIT  = 0x01;
};
