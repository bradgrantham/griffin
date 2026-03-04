// Generated from griffin.yml by codegen.py on 2026-03-03 — do not edit
// yaml-language-server: $schema=hw_schema.yml

#pragma once

#include <cstdint>

// Memory range helper used by the emulator for address decode.
struct MemoryRange {
    uint32_t base;
    uint32_t size;
    constexpr MemoryRange(uint32_t b, uint32_t s) : base(b), size(s) {}
    constexpr bool contains(uint32_t addr) const { return addr >= base && addr < base + size; }
    constexpr uint32_t offset(uint32_t addr) const { return addr - base; }
};

namespace Griffin {

// Project: Griffin
static constexpr uint32_t SYSCLK = 12000000UL;

// ------------------------------------------------------------
// GLUE: System glue logic
static constexpr uint32_t GLUE_BASE = 0xF00000UL;
static constexpr uint32_t GLUE_SIZE = 0x040000UL;
inline constexpr MemoryRange GLUE(0xF00000UL, 0x040000UL);
static constexpr uint32_t GLUE_DEBUG_OUT = 0xF00001UL;  // WRITE: Set or clear DEBUG_OUT signal (debug LED and test point output)
static constexpr uint32_t GLUE_DEBUG_OUT_MASK  = 0x01U;  // bits 0:0
static constexpr uint32_t GLUE_DEBUG_OUT_SHIFT = 0U;
static constexpr uint32_t GLUE_DEBUG_IN = 0xF00001UL;  // READ: Read DEBUG_IN signal state
static constexpr uint32_t GLUE_DEBUG_IN_MASK  = 0x01U;  // bits 0:0
static constexpr uint32_t GLUE_DEBUG_IN_SHIFT = 0U;
static constexpr uint32_t GLUE_UART_TX_STATUS = 0xF00003UL;  // READ: UART transmitter status; poll BUSY until clear before writing UART_TX_DATA
static constexpr uint32_t GLUE_UART_TX_STATUS_BUSY_MASK  = 0x01U;  // bits 0:0
static constexpr uint32_t GLUE_UART_TX_STATUS_BUSY_SHIFT = 0U;
static constexpr uint32_t GLUE_UART_TX_DATA = 0xF00003UL;  // WRITE: Load TX latch and begin 8N1 output on DEBUG_OUT
static constexpr uint32_t GLUE_UART_RX_STATUS = 0xF00005UL;  // READ: UART receiver status
static constexpr uint32_t GLUE_UART_RX_STATUS_READY_MASK  = 0x01U;  // bits 0:0
static constexpr uint32_t GLUE_UART_RX_STATUS_READY_SHIFT = 0U;
static constexpr uint32_t GLUE_UART_RX_STATUS_PIN_MASK  = 0x80U;  // bits 7:7
static constexpr uint32_t GLUE_UART_RX_STATUS_PIN_SHIFT = 7U;
static constexpr uint32_t GLUE_UART_RX_CONFIG = 0xF00005UL;  // WRITE: UART receiver configuration
static constexpr uint32_t GLUE_UART_RX_CONFIG_ARM_MASK  = 0x01U;  // bits 0:0
static constexpr uint32_t GLUE_UART_RX_CONFIG_ARM_SHIFT = 0U;
static constexpr uint32_t GLUE_CONFIG = 0xF00007UL;  // WRITE: GLUE configuration register
static constexpr uint32_t GLUE_CONFIG_ROM_OVERLAY_DISABLE_MASK  = 0x01U;  // bits 0:0
static constexpr uint32_t GLUE_CONFIG_ROM_OVERLAY_DISABLE_SHIFT = 0U;

// ------------------------------------------------------------
// ROM: 2x 64K byte-wide EPROM/Flash
static constexpr uint32_t ROM_BASE = 0xC00000UL;
static constexpr uint32_t ROM_SIZE = 0x020000UL;
static constexpr uint32_t ROM_WINDOW = 0x100000UL;
inline constexpr MemoryRange ROM(0xC00000UL, 0x100000UL);

// ------------------------------------------------------------
// RAM_BANK_1: SRAM
static constexpr uint32_t RAM_BANK_1_BASE = 0x000000UL;
static constexpr uint32_t RAM_BANK_1_SIZE = 0x100000UL;
inline constexpr MemoryRange RAM_BANK_1(0x000000UL, 0x100000UL);

// ------------------------------------------------------------
// RAM_BANK_2: SRAM
static constexpr uint32_t RAM_BANK_2_BASE = 0x100000UL;
static constexpr uint32_t RAM_BANK_2_SIZE = 0x100000UL;
inline constexpr MemoryRange RAM_BANK_2(0x100000UL, 0x100000UL);

// ------------------------------------------------------------
// RAM_BANK_3: SRAM
static constexpr uint32_t RAM_BANK_3_BASE = 0x200000UL;
static constexpr uint32_t RAM_BANK_3_SIZE = 0x100000UL;
inline constexpr MemoryRange RAM_BANK_3(0x200000UL, 0x100000UL);

// ------------------------------------------------------------
// RAM_BANK_4: SRAM
static constexpr uint32_t RAM_BANK_4_BASE = 0x300000UL;
static constexpr uint32_t RAM_BANK_4_SIZE = 0x100000UL;
inline constexpr MemoryRange RAM_BANK_4(0x300000UL, 0x100000UL);

// ------------------------------------------------------------
// VIDEO: On-board video generator
static constexpr uint32_t VIDEO_BASE = 0xE00000UL;
static constexpr uint32_t VIDEO_SIZE = 0x100000UL;
inline constexpr MemoryRange VIDEO(0xE00000UL, 0x100000UL);
static constexpr uint32_t VIDEO_MODE = 0xE00001UL;  // RW: Primary video mode control
static constexpr uint32_t VIDEO_MODE_CLOCK_MASK  = 0x07U;  // bits 2:0
static constexpr uint32_t VIDEO_MODE_CLOCK_SHIFT = 0U;
static constexpr uint32_t VIDEO_MODE_CLOCK_DISABLED = 0U;  // Video disabled; all outputs Hi-Z, VIDEO_STALL reset
static constexpr uint32_t VIDEO_MODE_CLOCK_CLK_14MHZ = 4U;  // 14.318 MHz pixel clock (NTSC composite)
static constexpr uint32_t VIDEO_MODE_CLOCK_CLK_25MHZ = 5U;  // 25.175 MHz pixel clock (VGA 640x480)
static constexpr uint32_t VIDEO_MODE_PALETTE_MASK  = 0x08U;  // bits 3:3
static constexpr uint32_t VIDEO_MODE_PALETTE_SHIFT = 3U;
static constexpr uint32_t VIDEO_MODE_FORMAT_MASK  = 0x10U;  // bits 4:4
static constexpr uint32_t VIDEO_MODE_FORMAT_SHIFT = 4U;
static constexpr uint32_t VIDEO_MODE_ENBVINT_MASK  = 0x20U;  // bits 5:5
static constexpr uint32_t VIDEO_MODE_ENBVINT_SHIFT = 5U;
static constexpr uint32_t VIDEO_MODE_ENBSNP_MASK  = 0x40U;  // bits 6:6
static constexpr uint32_t VIDEO_MODE_ENBSNP_SHIFT = 6U;
static constexpr uint32_t VIDEO_MODE_CBURST_MASK  = 0x80U;  // bits 7:7
static constexpr uint32_t VIDEO_MODE_CBURST_SHIFT = 7U;
static constexpr uint32_t VIDEO_MODE2 = 0xE00003UL;  // RW: Secondary video mode control; can be changed at any time
static constexpr uint32_t VIDEO_MODE2_PPC_MASK  = 0x01U;  // bits 0:0
static constexpr uint32_t VIDEO_MODE2_PPC_SHIFT = 0U;
static constexpr uint32_t VIDEO_MODE2_ENBLINT_MASK  = 0x02U;  // bits 1:1
static constexpr uint32_t VIDEO_MODE2_ENBLINT_SHIFT = 1U;
static constexpr uint32_t VIDEO_WORDS_START = 0xE00005UL;  // WRITE: 16-pixel word count from hblank end to first visible output; can change in hblank
static constexpr uint32_t VIDEO_BORDER_PIXEL = 0xE00009UL;  // RW: Pixel value used outside the visible word range; can change at any time
static constexpr uint32_t VIDEO_BORDER_PIXEL_VALUE_MASK  = 0x01U;  // bits 0:0
static constexpr uint32_t VIDEO_BORDER_PIXEL_VALUE_SHIFT = 0U;
static constexpr uint32_t VIDEO_LINES_START = 0xE0000BUL;  // WRITE: Visible line start count after vblank end; unblocks VIDEO snoop reads after vsync
static constexpr uint32_t VIDEO_LINES_COUNT = 0xE0000CUL;  // WRITE: Number of visible lines
static constexpr uint32_t VIDEO_PALETTE = 0xE0000EUL;  // WRITE: Load palette register immediately; 2x R3G3B2 entries; can change at any time
static constexpr uint32_t VIDEO_PALETTE_ENTRY_0_MASK  = 0xFFU;  // bits 7:0
static constexpr uint32_t VIDEO_PALETTE_ENTRY_0_SHIFT = 0U;
static constexpr uint32_t VIDEO_PALETTE_ENTRY_1_MASK  = 0xFF00U;  // bits 15:8
static constexpr uint32_t VIDEO_PALETTE_ENTRY_1_SHIFT = 8U;
static constexpr uint32_t VIDEO_NEXT_PALETTE = 0xE00010UL;  // WRITE: Load next-palette buffer; promoted to PALETTE on next shift register reload; can change at any time
static constexpr uint32_t VIDEO_NEXT_PALETTE_ENTRY_0_MASK  = 0xFFU;  // bits 7:0
static constexpr uint32_t VIDEO_NEXT_PALETTE_ENTRY_0_SHIFT = 0U;
static constexpr uint32_t VIDEO_NEXT_PALETTE_ENTRY_1_MASK  = 0xFF00U;  // bits 15:8
static constexpr uint32_t VIDEO_NEXT_PALETTE_ENTRY_1_SHIFT = 8U;
static constexpr uint32_t VIDEO_ARM_SNOOP = 0xE00012UL;  // WRITE: Write to arm VIDEO_STALL blocking snoop; write once per visible line before pixel reads
static constexpr uint32_t VIDEO_DISARM_SNOOP = 0xE00014UL;  // WRITE: Write to disarm VIDEO_STALL blocking snoop

// ------------------------------------------------------------
// ENGINE: Application-specific accelerator
static constexpr uint32_t ENGINE_BASE = 0xD00000UL;
static constexpr uint32_t ENGINE_SIZE = 0x100000UL;
inline constexpr MemoryRange ENGINE(0xD00000UL, 0x100000UL);

// ------------------------------------------------------------
// CF: Storage via CF card in True IDE 8-bit PIO mode
static constexpr uint32_t CF_BASE = 0xF40000UL;
static constexpr uint32_t CF_SIZE = 0x040000UL;
inline constexpr MemoryRange CF(0xF40000UL, 0x040000UL);
static constexpr uint32_t CF_DATA = 0xF40001UL;  // RW: Data to/from CF card
static constexpr uint32_t CF_ERROR = 0xF40003UL;  // READ: Error register; valid after a command error
static constexpr uint32_t CF_FEATURES = 0xF40003UL;  // WRITE: Features register; written before issuing SET_FEATURES command
static constexpr uint32_t CF_SECTOR_COUNT = 0xF40005UL;  // RW: Number of sectors to transfer
static constexpr uint32_t CF_SECTOR_NUM = 0xF40007UL;  // RW: LBA bits 7:0
static constexpr uint32_t CF_CYL_LO = 0xF40009UL;  // RW: LBA bits 15:8
static constexpr uint32_t CF_CYL_HI = 0xF4000BUL;  // RW: LBA bits 23:16
static constexpr uint32_t CF_DRIVE_HEAD = 0xF4000DUL;  // RW: LBA bits 27:24 and drive select; OR with CF_DH_LBA for LBA mode
static constexpr uint32_t CF_STATUS = 0xF4000FUL;  // READ: Device status; poll BSY clear and DRQ set before data transfer
static constexpr uint32_t CF_COMMAND = 0xF4000FUL;  // WRITE: Issue command; write after setting all other registers

// ------------------------------------------------------------
// IO_MCU: Keyboard, mouse, serial IO processor
static constexpr uint32_t IO_MCU_BASE = 0xF80000UL;
static constexpr uint32_t IO_MCU_SIZE = 0x040000UL;
inline constexpr MemoryRange IO_MCU(0xF80000UL, 0x040000UL);
static constexpr uint32_t IO_MCU_STATUS = 0xF80001UL;  // READ: IO processor status
static constexpr uint32_t IO_MCU_CONFIG = 0xF80001UL;  // WRITE: IO processor configuration; baud rate and mode
static constexpr uint32_t IO_MCU_TX = 0xF80003UL;  // WRITE: UART transmit data byte
static constexpr uint32_t IO_MCU_RX = 0xF80005UL;  // READ: Receive data; first byte is type/status, subsequent bytes are payload

// ------------------------------------------------------------
// AUDIO: 8-bit latched audio output
static constexpr uint32_t AUDIO_BASE = 0xFC0000UL;
static constexpr uint32_t AUDIO_SIZE = 0x040000UL;
inline constexpr MemoryRange AUDIO(0xFC0000UL, 0x040000UL);
static constexpr uint32_t AUDIO_DAC = 0xFC0001UL;  // WRITE: Write sample to 8-bit R2R DAC output latch

// IO region — span of all non-RAM/ROM memory-mapped peripherals
static constexpr uint32_t IO_BASE = 0xF00000UL;
static constexpr uint32_t IO_SIZE = 0x100000UL;

// Constants
static constexpr uint32_t CF_STATUS_BSY = 0x80U;
static constexpr uint32_t CF_STATUS_DRDY = 0x40U;
static constexpr uint32_t CF_STATUS_DRQ = 0x08U;
static constexpr uint32_t CF_STATUS_ERR = 0x01U;
static constexpr uint32_t CF_CMD_READ_SECTORS = 0x20U;
static constexpr uint32_t CF_CMD_WRITE_SECTORS = 0x30U;
static constexpr uint32_t CF_CMD_IDENTIFY = 0xECU;
static constexpr uint32_t CF_CMD_SET_FEATURES = 0xEFU;
static constexpr uint32_t CF_CMD_SET_8BIT = 0x01U;
static constexpr uint32_t CF_DH_LBA = 0xE0U;

} // namespace Griffin
