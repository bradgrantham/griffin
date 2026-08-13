// Generated from griffin.yml by codegen.py — do not edit (regenerate with: make codegen)
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
static constexpr uint32_t SYSCLK_HZ = 14000000UL;

// ------------------------------------------------------------
// GLUE: System glue logic (also hosts PS/2 keyboard RX/TX)
static constexpr uint32_t GLUE_BASE = 0xF00000UL;
static constexpr uint32_t GLUE_SIZE = 0x040000UL;
inline constexpr MemoryRange GLUE(0xF00000UL, 0x040000UL);
static constexpr uint32_t GLUE_IRQ_LEVEL = 4U;
static constexpr int GLUE_DTACK_WS = 0;  // wait states at 14000000 Hz
static constexpr int GLUE_DTACK_THRESHOLD = 2;  // ws_cnt threshold for Verilog
static constexpr int GLUE_DTACK_PENALTY = 0;  // extra SYSCLK cycles for emulator
static constexpr uint32_t GLUE_DEBUG_OUT = 0xF00001UL;  // WRITE: Set or clear DEBUG_OUT signal (debug LED and test point output)
static constexpr uint32_t GLUE_DEBUG_OUT_MASK  = 0x01U;  // bits 0:0
static constexpr uint32_t GLUE_DEBUG_OUT_SHIFT = 0U;
static constexpr uint32_t GLUE_CONFIG = 0xF00007UL;  // WRITE: GLUE configuration register
static constexpr uint32_t GLUE_CONFIG_ROM_OVERLAY_DISABLE_MASK  = 0x01U;  // bits 0:0
static constexpr uint32_t GLUE_CONFIG_ROM_OVERLAY_DISABLE_SHIFT = 0U;
static constexpr uint32_t GLUE_CONFIG_FLASH_WE_EN_MASK  = 0x02U;  // bits 1:1
static constexpr uint32_t GLUE_CONFIG_FLASH_WE_EN_SHIFT = 1U;
static constexpr uint32_t GLUE_CONFIG_DEFAULT = 0x00U;
static constexpr uint32_t GLUE_PS2_TX_DATA = 0xF00009UL;  // WRITE: Byte to transmit host->device; the write itself starts the frame
static constexpr uint32_t GLUE_PS2_TX_DATA_MASK  = 0xFFU;  // bits 7:0
static constexpr uint32_t GLUE_PS2_TX_DATA_SHIFT = 0U;
static constexpr uint32_t GLUE_PS2_STATUS = 0xF00011UL;  // READ: PS/2 frame-engine status
static constexpr uint32_t GLUE_PS2_STATUS_RX_READY_MASK  = 0x01U;  // bits 0:0
static constexpr uint32_t GLUE_PS2_STATUS_RX_READY_SHIFT = 0U;
static constexpr uint32_t GLUE_PS2_STATUS_TX_DONE_MASK  = 0x02U;  // bits 1:1
static constexpr uint32_t GLUE_PS2_STATUS_TX_DONE_SHIFT = 1U;
static constexpr uint32_t GLUE_PS2_STATUS_TX_ACK_MASK  = 0x04U;  // bits 2:2
static constexpr uint32_t GLUE_PS2_STATUS_TX_ACK_SHIFT = 2U;
static constexpr uint32_t GLUE_PS2_STATUS_RX_PARITY_MASK  = 0x08U;  // bits 3:3
static constexpr uint32_t GLUE_PS2_STATUS_RX_PARITY_SHIFT = 3U;
static constexpr uint32_t GLUE_PS2_STATUS_RX_FRAME_ERR_MASK  = 0x10U;  // bits 4:4
static constexpr uint32_t GLUE_PS2_STATUS_RX_FRAME_ERR_SHIFT = 4U;
static constexpr uint32_t GLUE_PS2_STATUS_DATA_LIVE_MASK  = 0x20U;  // bits 5:5
static constexpr uint32_t GLUE_PS2_STATUS_DATA_LIVE_SHIFT = 5U;
static constexpr uint32_t GLUE_PS2_STATUS_CLK_LIVE_MASK  = 0x40U;  // bits 6:6
static constexpr uint32_t GLUE_PS2_STATUS_CLK_LIVE_SHIFT = 6U;
static constexpr uint32_t GLUE_PS2_CLEAR = 0xF00011UL;  // WRITE: Write-1-to-clear for PS2_STATUS latched flags / IRQ ack
static constexpr uint32_t GLUE_PS2_CLEAR_RX_READY_MASK  = 0x01U;  // bits 0:0
static constexpr uint32_t GLUE_PS2_CLEAR_RX_READY_SHIFT = 0U;
static constexpr uint32_t GLUE_PS2_CLEAR_TX_DONE_MASK  = 0x02U;  // bits 1:1
static constexpr uint32_t GLUE_PS2_CLEAR_TX_DONE_SHIFT = 1U;
static constexpr uint32_t GLUE_PS2_CTRL = 0xF00013UL;  // WRITE: Open-drain drive control for PS2_CLK and PS2_DATA for the TX inhibit handshake
static constexpr uint32_t GLUE_PS2_CTRL_CLK_MASK  = 0x01U;  // bits 0:0
static constexpr uint32_t GLUE_PS2_CTRL_CLK_SHIFT = 0U;
static constexpr uint32_t GLUE_PS2_CTRL_DATA_MASK  = 0x02U;  // bits 1:1
static constexpr uint32_t GLUE_PS2_CTRL_DATA_SHIFT = 1U;
static constexpr uint32_t GLUE_PS2_RX_DATA = 0xF00015UL;  // READ: Assembled PS/2 data byte; valid while RX_READY is set
static constexpr uint32_t GLUE_PS2_RX_DATA_MASK  = 0xFFU;  // bits 7:0
static constexpr uint32_t GLUE_PS2_RX_DATA_SHIFT = 0U;

// ------------------------------------------------------------
// ROM: 2x SST39SF040-70 (512Kx8 DIP-32 5V NOR flash, socketed; 1MB total)
static constexpr uint32_t ROM_BASE = 0x800000UL;
static constexpr uint32_t ROM_SIZE = 0x100000UL;
static constexpr uint32_t ROM_WINDOW = 0x400000UL;
inline constexpr MemoryRange ROM(0x800000UL, 0x400000UL);
static constexpr int ROM_DTACK_WS = 1;  // wait states at 14000000 Hz
static constexpr int ROM_DTACK_THRESHOLD = 4;  // ws_cnt threshold for Verilog
static constexpr int ROM_DTACK_PENALTY = 2;  // extra SYSCLK cycles for emulator

// ------------------------------------------------------------
// RAM_BANK_1: 2x AS6C4008
static constexpr uint32_t RAM_BANK_1_BASE = 0x000000UL;
static constexpr uint32_t RAM_BANK_1_SIZE = 0x100000UL;
inline constexpr MemoryRange RAM_BANK_1(0x000000UL, 0x100000UL);
static constexpr int RAM_BANK_1_DTACK_WS = 0;  // wait states at 14000000 Hz
static constexpr int RAM_BANK_1_DTACK_THRESHOLD = 2;  // ws_cnt threshold for Verilog
static constexpr int RAM_BANK_1_DTACK_PENALTY = 0;  // extra SYSCLK cycles for emulator

// ------------------------------------------------------------
// RAM_BANK_2: 2x AS6C4008
static constexpr uint32_t RAM_BANK_2_BASE = 0x100000UL;
static constexpr uint32_t RAM_BANK_2_SIZE = 0x100000UL;
inline constexpr MemoryRange RAM_BANK_2(0x100000UL, 0x100000UL);
static constexpr int RAM_BANK_2_DTACK_WS = 0;  // wait states at 14000000 Hz
static constexpr int RAM_BANK_2_DTACK_THRESHOLD = 2;  // ws_cnt threshold for Verilog
static constexpr int RAM_BANK_2_DTACK_PENALTY = 0;  // extra SYSCLK cycles for emulator

// ------------------------------------------------------------
// RAM_BANK_3: 2x AS6C4008
static constexpr uint32_t RAM_BANK_3_BASE = 0x200000UL;
static constexpr uint32_t RAM_BANK_3_SIZE = 0x100000UL;
inline constexpr MemoryRange RAM_BANK_3(0x200000UL, 0x100000UL);
static constexpr int RAM_BANK_3_DTACK_WS = 0;  // wait states at 14000000 Hz
static constexpr int RAM_BANK_3_DTACK_THRESHOLD = 2;  // ws_cnt threshold for Verilog
static constexpr int RAM_BANK_3_DTACK_PENALTY = 0;  // extra SYSCLK cycles for emulator

// ------------------------------------------------------------
// RAM_BANK_4: 2x AS6C4008
static constexpr uint32_t RAM_BANK_4_BASE = 0x300000UL;
static constexpr uint32_t RAM_BANK_4_SIZE = 0x100000UL;
inline constexpr MemoryRange RAM_BANK_4(0x300000UL, 0x100000UL);
static constexpr int RAM_BANK_4_DTACK_WS = 0;  // wait states at 14000000 Hz
static constexpr int RAM_BANK_4_DTACK_THRESHOLD = 2;  // ws_cnt threshold for Verilog
static constexpr int RAM_BANK_4_DTACK_PENALTY = 0;  // extra SYSCLK cycles for emulator

// ------------------------------------------------------------
// ENGINE: Video framebuffer DMA engine with 7200 FIFO write interface
static constexpr uint32_t ENGINE_BASE = 0xD00000UL;
static constexpr uint32_t ENGINE_SIZE = 0x100000UL;
inline constexpr MemoryRange ENGINE(0xD00000UL, 0x100000UL);
static constexpr uint32_t ENGINE_IRQ_LEVEL = 3U;
static constexpr int ENGINE_DTACK_WS = 0;  // wait states at 14000000 Hz
static constexpr int ENGINE_DTACK_THRESHOLD = 2;  // ws_cnt threshold for Verilog
static constexpr int ENGINE_DTACK_PENALTY = 0;  // extra SYSCLK cycles for emulator
static constexpr uint32_t ENGINE_SOURCE_PAGE = 0xD00003UL;  // WRITE: Framebuffer page: A[23:16] of source base address
static constexpr uint32_t ENGINE_CTRL = 0xD00005UL;  // WRITE: DMA control register
static constexpr uint32_t ENGINE_CTRL_DMA_EN_MASK  = 0x01U;  // bits 0:0
static constexpr uint32_t ENGINE_CTRL_DMA_EN_SHIFT = 0U;
static constexpr uint32_t ENGINE_CTRL_DEFAULT = 0x00U;
static constexpr uint32_t ENGINE_STATUS = 0xD00005UL;  // READ: DMA status readback
static constexpr uint32_t ENGINE_STATUS_DMA_EN_MASK  = 0x01U;  // bits 0:0
static constexpr uint32_t ENGINE_STATUS_DMA_EN_SHIFT = 0U;
static constexpr uint32_t ENGINE_STATUS_FIFO_HF_MASK  = 0x02U;  // bits 1:1
static constexpr uint32_t ENGINE_STATUS_FIFO_HF_SHIFT = 1U;

// ------------------------------------------------------------
// VIDEO: VGA 640x480@60 1bpp video generator with 7200 FIFO read interface
static constexpr uint32_t VIDEO_BASE = 0xE00000UL;
static constexpr uint32_t VIDEO_SIZE = 0x100000UL;
inline constexpr MemoryRange VIDEO(0xE00000UL, 0x100000UL);
static constexpr uint32_t VIDEO_IRQ_LEVEL = 6U;
static constexpr int VIDEO_DTACK_WS = 0;  // wait states at 14000000 Hz
static constexpr int VIDEO_DTACK_THRESHOLD = 2;  // ws_cnt threshold for Verilog
static constexpr int VIDEO_DTACK_PENALTY = 0;  // extra SYSCLK cycles for emulator
static constexpr uint32_t VIDEO_CLRINT = 0xE00003UL;  // WRITE: Write any value to clear the latched VIDEO vsync IRQ
static constexpr uint32_t VIDEO_CTRL = 0xE00005UL;  // WRITE: Video control register
static constexpr uint32_t VIDEO_CTRL_ENABLE_MASK  = 0x01U;  // bits 0:0
static constexpr uint32_t VIDEO_CTRL_ENABLE_SHIFT = 0U;
static constexpr uint32_t VIDEO_CTRL_IRQENB_MASK  = 0x02U;  // bits 1:1
static constexpr uint32_t VIDEO_CTRL_IRQENB_SHIFT = 1U;
static constexpr uint32_t VIDEO_CTRL_DEFAULT = 0x00U;
static constexpr uint32_t VIDEO_CTRL_RB = 0xE00005UL;  // READ: Video control readback
static constexpr uint32_t VIDEO_CTRL_RB_ENABLE_MASK  = 0x01U;  // bits 0:0
static constexpr uint32_t VIDEO_CTRL_RB_ENABLE_SHIFT = 0U;
static constexpr uint32_t VIDEO_CTRL_RB_FIFO_ERROR_MASK  = 0x02U;  // bits 1:1
static constexpr uint32_t VIDEO_CTRL_RB_FIFO_ERROR_SHIFT = 1U;
static constexpr uint32_t VIDEO_CTRL_RB_IRQENB_MASK  = 0x04U;  // bits 2:2
static constexpr uint32_t VIDEO_CTRL_RB_IRQENB_SHIFT = 2U;
static constexpr uint32_t VIDEO_CLRERR = 0xE00009UL;  // WRITE: Write any value to clear FIFO_ERROR sticky bit

// ------------------------------------------------------------
// PORTS: PS/2 mouse, two joystick ports, two paddle counters, and the audio FIFO pop strobe
static constexpr uint32_t PORTS_BASE = 0xFC0000UL;
static constexpr uint32_t PORTS_SIZE = 0x040000UL;
inline constexpr MemoryRange PORTS(0xFC0000UL, 0x040000UL);
static constexpr uint32_t PORTS_IRQ_LEVEL = 2U;
static constexpr int PORTS_DTACK_WS = 0;  // wait states at 14000000 Hz
static constexpr int PORTS_DTACK_THRESHOLD = 2;  // ws_cnt threshold for Verilog
static constexpr int PORTS_DTACK_PENALTY = 0;  // extra SYSCLK cycles for emulator
static constexpr uint32_t PORTS_JOYSTICK_PORT_1 = 0xFC0001UL;  // READ: Joystick port 1 switches, active low (0 = closed)
static constexpr uint32_t PORTS_JOYSTICK_PORT_1_UP_MASK  = 0x01U;  // bits 0:0
static constexpr uint32_t PORTS_JOYSTICK_PORT_1_UP_SHIFT = 0U;
static constexpr uint32_t PORTS_JOYSTICK_PORT_1_DOWN_MASK  = 0x02U;  // bits 1:1
static constexpr uint32_t PORTS_JOYSTICK_PORT_1_DOWN_SHIFT = 1U;
static constexpr uint32_t PORTS_JOYSTICK_PORT_1_LEFT_MASK  = 0x04U;  // bits 2:2
static constexpr uint32_t PORTS_JOYSTICK_PORT_1_LEFT_SHIFT = 2U;
static constexpr uint32_t PORTS_JOYSTICK_PORT_1_RIGHT_MASK  = 0x08U;  // bits 3:3
static constexpr uint32_t PORTS_JOYSTICK_PORT_1_RIGHT_SHIFT = 3U;
static constexpr uint32_t PORTS_JOYSTICK_PORT_1_FIRE_MASK  = 0x10U;  // bits 4:4
static constexpr uint32_t PORTS_JOYSTICK_PORT_1_FIRE_SHIFT = 4U;
static constexpr uint32_t PORTS_JOYSTICK_PORT_1_PIN9_MASK  = 0x20U;  // bits 5:5
static constexpr uint32_t PORTS_JOYSTICK_PORT_1_PIN9_SHIFT = 5U;
static constexpr uint32_t PORTS_JOYSTICK_PORT_1_PIN5_MASK  = 0x40U;  // bits 6:6
static constexpr uint32_t PORTS_JOYSTICK_PORT_1_PIN5_SHIFT = 6U;
static constexpr uint32_t PORTS_JOYSTICK_PORT_2 = 0xFC0003UL;  // READ: Joystick port 2 switches, active low (0 = closed)
static constexpr uint32_t PORTS_JOYSTICK_PORT_2_UP_MASK  = 0x01U;  // bits 0:0
static constexpr uint32_t PORTS_JOYSTICK_PORT_2_UP_SHIFT = 0U;
static constexpr uint32_t PORTS_JOYSTICK_PORT_2_DOWN_MASK  = 0x02U;  // bits 1:1
static constexpr uint32_t PORTS_JOYSTICK_PORT_2_DOWN_SHIFT = 1U;
static constexpr uint32_t PORTS_JOYSTICK_PORT_2_LEFT_MASK  = 0x04U;  // bits 2:2
static constexpr uint32_t PORTS_JOYSTICK_PORT_2_LEFT_SHIFT = 2U;
static constexpr uint32_t PORTS_JOYSTICK_PORT_2_RIGHT_MASK  = 0x08U;  // bits 3:3
static constexpr uint32_t PORTS_JOYSTICK_PORT_2_RIGHT_SHIFT = 3U;
static constexpr uint32_t PORTS_JOYSTICK_PORT_2_FIRE_MASK  = 0x10U;  // bits 4:4
static constexpr uint32_t PORTS_JOYSTICK_PORT_2_FIRE_SHIFT = 4U;
static constexpr uint32_t PORTS_JOYSTICK_PORT_2_PIN9_MASK  = 0x20U;  // bits 5:5
static constexpr uint32_t PORTS_JOYSTICK_PORT_2_PIN9_SHIFT = 5U;
static constexpr uint32_t PORTS_JOYSTICK_PORT_2_PIN5_MASK  = 0x40U;  // bits 6:6
static constexpr uint32_t PORTS_JOYSTICK_PORT_2_PIN5_SHIFT = 6U;
static constexpr uint32_t PORTS_PADDLE_A_COUNT = 0xFC0005UL;  // READ: Paddle A position, the pot on port 1 pin 9
static constexpr uint32_t PORTS_PADDLE_A_COUNT_MASK  = 0xFFU;  // bits 7:0
static constexpr uint32_t PORTS_PADDLE_A_COUNT_SHIFT = 0U;
static constexpr uint32_t PORTS_PADDLE_B_COUNT = 0xFC0007UL;  // READ: Paddle B position, the pot on port 1 pin 5
static constexpr uint32_t PORTS_PADDLE_B_COUNT_MASK  = 0xFFU;  // bits 7:0
static constexpr uint32_t PORTS_PADDLE_B_COUNT_SHIFT = 0U;
static constexpr uint32_t PORTS_PS2_MOUSE_TX_DATA = 0xFC0009UL;  // WRITE: Byte to transmit host->mouse; the write itself starts the frame
static constexpr uint32_t PORTS_PS2_MOUSE_TX_DATA_MASK  = 0xFFU;  // bits 7:0
static constexpr uint32_t PORTS_PS2_MOUSE_TX_DATA_SHIFT = 0U;
static constexpr uint32_t PORTS_PADDLE_CONTROL = 0xFC000FUL;  // WRITE: Paddle measurement control, written once per vsync ISR
static constexpr uint32_t PORTS_PADDLE_CONTROL_DUMP_MASK  = 0x01U;  // bits 0:0
static constexpr uint32_t PORTS_PADDLE_CONTROL_DUMP_SHIFT = 0U;
static constexpr uint32_t PORTS_PADDLE_CONTROL_DEFAULT = 0x00U;
static constexpr uint32_t PORTS_PS2_MOUSE_STATUS = 0xFC0011UL;  // READ: PS/2 mouse frame-engine status
static constexpr uint32_t PORTS_PS2_MOUSE_STATUS_RX_READY_MASK  = 0x01U;  // bits 0:0
static constexpr uint32_t PORTS_PS2_MOUSE_STATUS_RX_READY_SHIFT = 0U;
static constexpr uint32_t PORTS_PS2_MOUSE_STATUS_TX_DONE_MASK  = 0x02U;  // bits 1:1
static constexpr uint32_t PORTS_PS2_MOUSE_STATUS_TX_DONE_SHIFT = 1U;
static constexpr uint32_t PORTS_PS2_MOUSE_STATUS_TX_ACK_MASK  = 0x04U;  // bits 2:2
static constexpr uint32_t PORTS_PS2_MOUSE_STATUS_TX_ACK_SHIFT = 2U;
static constexpr uint32_t PORTS_PS2_MOUSE_STATUS_RX_PARITY_MASK  = 0x08U;  // bits 3:3
static constexpr uint32_t PORTS_PS2_MOUSE_STATUS_RX_PARITY_SHIFT = 3U;
static constexpr uint32_t PORTS_PS2_MOUSE_STATUS_RX_FRAME_ERR_MASK  = 0x10U;  // bits 4:4
static constexpr uint32_t PORTS_PS2_MOUSE_STATUS_RX_FRAME_ERR_SHIFT = 4U;
static constexpr uint32_t PORTS_PS2_MOUSE_STATUS_DATA_LIVE_MASK  = 0x20U;  // bits 5:5
static constexpr uint32_t PORTS_PS2_MOUSE_STATUS_DATA_LIVE_SHIFT = 5U;
static constexpr uint32_t PORTS_PS2_MOUSE_STATUS_CLK_LIVE_MASK  = 0x40U;  // bits 6:6
static constexpr uint32_t PORTS_PS2_MOUSE_STATUS_CLK_LIVE_SHIFT = 6U;
static constexpr uint32_t PORTS_PS2_MOUSE_CLEAR = 0xFC0011UL;  // WRITE: Write-1-to-clear for PS2_MOUSE_STATUS latched flags / IRQ ack
static constexpr uint32_t PORTS_PS2_MOUSE_CLEAR_RX_READY_MASK  = 0x01U;  // bits 0:0
static constexpr uint32_t PORTS_PS2_MOUSE_CLEAR_RX_READY_SHIFT = 0U;
static constexpr uint32_t PORTS_PS2_MOUSE_CLEAR_TX_DONE_MASK  = 0x02U;  // bits 1:1
static constexpr uint32_t PORTS_PS2_MOUSE_CLEAR_TX_DONE_SHIFT = 1U;
static constexpr uint32_t PORTS_PS2_MOUSE_CTRL = 0xFC0013UL;  // WRITE: Open-drain drive control for PS2_MOUSE_CLK and PS2_MOUSE_DATA for the TX inhibit handshake
static constexpr uint32_t PORTS_PS2_MOUSE_CTRL_CLK_MASK  = 0x01U;  // bits 0:0
static constexpr uint32_t PORTS_PS2_MOUSE_CTRL_CLK_SHIFT = 0U;
static constexpr uint32_t PORTS_PS2_MOUSE_CTRL_DATA_MASK  = 0x02U;  // bits 1:1
static constexpr uint32_t PORTS_PS2_MOUSE_CTRL_DATA_SHIFT = 1U;
static constexpr uint32_t PORTS_PS2_MOUSE_RX_DATA = 0xFC0015UL;  // READ: Assembled PS/2 mouse data byte; valid while RX_READY is set
static constexpr uint32_t PORTS_PS2_MOUSE_RX_DATA_MASK  = 0xFFU;  // bits 7:0
static constexpr uint32_t PORTS_PS2_MOUSE_RX_DATA_SHIFT = 0U;
static constexpr uint32_t PORTS_AUDIO_CONTROL = 0xFC001FUL;  // WRITE: Audio FIFO pop control
static constexpr uint32_t PORTS_AUDIO_CONTROL_ENABLE_MASK  = 0x01U;  // bits 0:0
static constexpr uint32_t PORTS_AUDIO_CONTROL_ENABLE_SHIFT = 0U;
static constexpr uint32_t PORTS_AUDIO_CONTROL_CLEAR_HF_IRQ_MASK  = 0x02U;  // bits 1:1
static constexpr uint32_t PORTS_AUDIO_CONTROL_CLEAR_HF_IRQ_SHIFT = 1U;
static constexpr uint32_t PORTS_AUDIO_CONTROL_DEFAULT = 0x00U;
static constexpr uint32_t PORTS_AUDIO_STATUS = 0xFC001FUL;  // READ: Audio FIFO pop status
static constexpr uint32_t PORTS_AUDIO_STATUS_HF_IRQ_MASK  = 0x01U;  // bits 0:0
static constexpr uint32_t PORTS_AUDIO_STATUS_HF_IRQ_SHIFT = 0U;
static constexpr uint32_t PORTS_AUDIO_STATUS_HALF_FULL_MASK  = 0x02U;  // bits 1:1
static constexpr uint32_t PORTS_AUDIO_STATUS_HALF_FULL_SHIFT = 1U;
static constexpr uint32_t PORTS_AUDIO_STATUS_ENABLE_MASK  = 0x04U;  // bits 2:2
static constexpr uint32_t PORTS_AUDIO_STATUS_ENABLE_SHIFT = 2U;

// ------------------------------------------------------------
// CF: Storage via CF card in True IDE 16-bit PIO mode
static constexpr uint32_t CF_BASE = 0xF40000UL;
static constexpr uint32_t CF_SIZE = 0x040000UL;
inline constexpr MemoryRange CF(0xF40000UL, 0x040000UL);
static constexpr int CF_DTACK_WS = 7;  // wait states at 14000000 Hz
static constexpr int CF_DTACK_THRESHOLD = 14;  // ws_cnt threshold for Verilog
static constexpr int CF_DTACK_PENALTY = 12;  // extra SYSCLK cycles for emulator
static constexpr uint32_t CF_DATA = 0xF40000UL;  // RW: 16-bit data port to/from the CF card
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
// DUART: Console (Ch A) and second serial (Ch B), 100 Hz systick C/T, GP pins for flow control / DS3231 I2C
static constexpr uint32_t DUART_BASE = 0xF80000UL;
static constexpr uint32_t DUART_SIZE = 0x040000UL;
inline constexpr MemoryRange DUART(0xF80000UL, 0x040000UL);
static constexpr uint32_t DUART_IRQ_LEVEL = 5U;
static constexpr uint32_t DUART_CLOCK = 3686400UL;
static constexpr uint32_t DUART_MR1A = 0xF80001UL;  // RW: Channel A mode register 1 (after reset or MR pointer reset)
static constexpr uint32_t DUART_MR2A = 0xF80001UL;  // RW: Channel A mode register 2 (second access after MR pointer reset)
static constexpr uint32_t DUART_SRA = 0xF80003UL;  // READ: Channel A status register
static constexpr uint32_t DUART_SRA_RXRDY_MASK  = 0x01U;  // bits 0:0
static constexpr uint32_t DUART_SRA_RXRDY_SHIFT = 0U;
static constexpr uint32_t DUART_SRA_FFULL_MASK  = 0x02U;  // bits 1:1
static constexpr uint32_t DUART_SRA_FFULL_SHIFT = 1U;
static constexpr uint32_t DUART_SRA_TXRDY_MASK  = 0x04U;  // bits 2:2
static constexpr uint32_t DUART_SRA_TXRDY_SHIFT = 2U;
static constexpr uint32_t DUART_SRA_TXEMT_MASK  = 0x08U;  // bits 3:3
static constexpr uint32_t DUART_SRA_TXEMT_SHIFT = 3U;
static constexpr uint32_t DUART_SRA_OE_MASK  = 0x10U;  // bits 4:4
static constexpr uint32_t DUART_SRA_OE_SHIFT = 4U;
static constexpr uint32_t DUART_SRA_PE_MASK  = 0x20U;  // bits 5:5
static constexpr uint32_t DUART_SRA_PE_SHIFT = 5U;
static constexpr uint32_t DUART_SRA_FE_MASK  = 0x40U;  // bits 6:6
static constexpr uint32_t DUART_SRA_FE_SHIFT = 6U;
static constexpr uint32_t DUART_SRA_RB_MASK  = 0x80U;  // bits 7:7
static constexpr uint32_t DUART_SRA_RB_SHIFT = 7U;
static constexpr uint32_t DUART_CSRA = 0xF80003UL;  // WRITE: Channel A clock select register
static constexpr uint32_t DUART_CRA = 0xF80005UL;  // WRITE: Channel A command register
static constexpr uint32_t DUART_CRA_EC_MASK  = 0x03U;  // bits 1:0
static constexpr uint32_t DUART_CRA_EC_SHIFT = 0U;
static constexpr uint32_t DUART_CRA_TC_MASK  = 0x0CU;  // bits 3:2
static constexpr uint32_t DUART_CRA_TC_SHIFT = 2U;
static constexpr uint32_t DUART_CRA_MC_MASK  = 0x70U;  // bits 6:4
static constexpr uint32_t DUART_CRA_MC_SHIFT = 4U;
static constexpr uint32_t DUART_RBA = 0xF80007UL;  // READ: Channel A receiver buffer (read to dequeue FIFO)
static constexpr uint32_t DUART_TBA = 0xF80007UL;  // WRITE: Channel A transmitter buffer (write to enqueue)
static constexpr uint32_t DUART_IPCR = 0xF80009UL;  // READ: Input port change register (read clears interrupt)
static constexpr uint32_t DUART_ACR = 0xF80009UL;  // WRITE: Auxiliary control register (timer/counter mode, BRG set select)
static constexpr uint32_t DUART_ACR_BRG_SET_MASK  = 0x80U;  // bits 7:7
static constexpr uint32_t DUART_ACR_BRG_SET_SHIFT = 7U;
static constexpr uint32_t DUART_ACR_CT_MODE_MASK  = 0x70U;  // bits 6:4
static constexpr uint32_t DUART_ACR_CT_MODE_SHIFT = 4U;
static constexpr uint32_t DUART_ACR_IP3_INT_EN_MASK  = 0x08U;  // bits 3:3
static constexpr uint32_t DUART_ACR_IP3_INT_EN_SHIFT = 3U;
static constexpr uint32_t DUART_ACR_IP2_INT_EN_MASK  = 0x04U;  // bits 2:2
static constexpr uint32_t DUART_ACR_IP2_INT_EN_SHIFT = 2U;
static constexpr uint32_t DUART_ACR_IP1_INT_EN_MASK  = 0x02U;  // bits 1:1
static constexpr uint32_t DUART_ACR_IP1_INT_EN_SHIFT = 1U;
static constexpr uint32_t DUART_ACR_IP0_INT_EN_MASK  = 0x01U;  // bits 0:0
static constexpr uint32_t DUART_ACR_IP0_INT_EN_SHIFT = 0U;
static constexpr uint32_t DUART_ISR = 0xF8000BUL;  // READ: Interrupt status register
static constexpr uint32_t DUART_ISR_TXRDYA_MASK  = 0x01U;  // bits 0:0
static constexpr uint32_t DUART_ISR_TXRDYA_SHIFT = 0U;
static constexpr uint32_t DUART_ISR_RXRDYA_MASK  = 0x02U;  // bits 1:1
static constexpr uint32_t DUART_ISR_RXRDYA_SHIFT = 1U;
static constexpr uint32_t DUART_ISR_DELTA_BRKA_MASK  = 0x04U;  // bits 2:2
static constexpr uint32_t DUART_ISR_DELTA_BRKA_SHIFT = 2U;
static constexpr uint32_t DUART_ISR_CTR_READY_MASK  = 0x08U;  // bits 3:3
static constexpr uint32_t DUART_ISR_CTR_READY_SHIFT = 3U;
static constexpr uint32_t DUART_ISR_TXRDYB_MASK  = 0x10U;  // bits 4:4
static constexpr uint32_t DUART_ISR_TXRDYB_SHIFT = 4U;
static constexpr uint32_t DUART_ISR_RXRDYB_MASK  = 0x20U;  // bits 5:5
static constexpr uint32_t DUART_ISR_RXRDYB_SHIFT = 5U;
static constexpr uint32_t DUART_ISR_DELTA_BRKB_MASK  = 0x40U;  // bits 6:6
static constexpr uint32_t DUART_ISR_DELTA_BRKB_SHIFT = 6U;
static constexpr uint32_t DUART_ISR_IPC_MASK  = 0x80U;  // bits 7:7
static constexpr uint32_t DUART_ISR_IPC_SHIFT = 7U;
static constexpr uint32_t DUART_IMR = 0xF8000BUL;  // WRITE: Interrupt mask register (same bit layout as ISR; 1=enabled)
static constexpr uint32_t DUART_CUR = 0xF8000DUL;  // READ: Counter/timer upper byte (current value)
static constexpr uint32_t DUART_CTUR = 0xF8000DUL;  // WRITE: Counter/timer upper preload register
static constexpr uint32_t DUART_CLR = 0xF8000FUL;  // READ: Counter/timer lower byte (current value)
static constexpr uint32_t DUART_CTLR = 0xF8000FUL;  // WRITE: Counter/timer lower preload register
static constexpr uint32_t DUART_MR1B = 0xF80011UL;  // RW: Channel B mode register 1 (after reset or MR pointer reset)
static constexpr uint32_t DUART_MR2B = 0xF80011UL;  // RW: Channel B mode register 2 (second access after MR pointer reset)
static constexpr uint32_t DUART_SRB = 0xF80013UL;  // READ: Channel B status register (same bit layout as SRA)
static constexpr uint32_t DUART_CSRB = 0xF80013UL;  // WRITE: Channel B clock select register
static constexpr uint32_t DUART_CRB = 0xF80015UL;  // WRITE: Channel B command register (same bit layout as CRA)
static constexpr uint32_t DUART_RBB = 0xF80017UL;  // READ: Channel B receiver buffer (read to dequeue FIFO)
static constexpr uint32_t DUART_TBB = 0xF80017UL;  // WRITE: Channel B transmitter buffer (write to enqueue)
static constexpr uint32_t DUART_IVR = 0xF80019UL;  // RW: Interrupt vector register
static constexpr uint32_t DUART_IP = 0xF8001BUL;  // READ: Input port (unlatched)
static constexpr uint32_t DUART_OPCR = 0xF8001BUL;  // WRITE: Output port configuration register
static constexpr uint32_t DUART_STARTCC = 0xF8001DUL;  // READ: Start counter/timer command (read to start; data ignored)
static constexpr uint32_t DUART_OPR_SET = 0xF8001DUL;  // WRITE: Output port bit set (1 bits set corresponding OP pins)
static constexpr uint32_t DUART_STOPCC = 0xF8001FUL;  // READ: Stop counter/timer command (read to stop; data ignored)
static constexpr uint32_t DUART_OPR_CLR = 0xF8001FUL;  // WRITE: Output port bit reset (1 bits clear corresponding OP pins)

// ------------------------------------------------------------
// AUDIO: Stereo FIFO audio output, drained at half the VGA line rate by PORTS
static constexpr uint32_t AUDIO_BASE = 0xC00000UL;
static constexpr uint32_t AUDIO_SIZE = 0x040000UL;
inline constexpr MemoryRange AUDIO(0xC00000UL, 0x040000UL);
static constexpr int AUDIO_DTACK_WS = 0;  // wait states at 14000000 Hz
static constexpr int AUDIO_DTACK_THRESHOLD = 2;  // ws_cnt threshold for Verilog
static constexpr int AUDIO_DTACK_PENALTY = 0;  // extra SYSCLK cycles for emulator
static constexpr uint32_t AUDIO_FIFO = 0xC00000UL;  // WRITE: Write one stereo sample pair into the audio FIFOs

// RAM region — total of all RAM banks
static constexpr uint32_t RAM_TOTAL_SIZE = 0x400000UL;

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
static constexpr uint32_t PS2_RX_QUEUE_SIZE = 0x40U;
static constexpr uint32_t AUDIO_FIFO_DEPTH = 0x400U;
static constexpr uint32_t AUDIO_SAMPLES_PER_SECOND = 0x3D76U;
static constexpr uint32_t DUART_OP_RTC_SCL = 0x04U;
static constexpr uint32_t DUART_OP_RTC_SDA_DRIVE = 0x08U;
static constexpr uint32_t DUART_IP_RTC_SDA = 0x04U;
static constexpr uint32_t DUART_IP_RTC_NINT = 0x08U;
static constexpr uint32_t RTC_I2C_ADDR = 0x68U;
static constexpr uint32_t PS2_TX_DATA_PARITY = 0x02U;
static constexpr uint32_t ENGINE_WORDS_PER_BURST = 0x14U;
static constexpr uint32_t VIDEO_PIXEL_BYTES_PER_LINE = 0x50U;
static constexpr uint32_t VIDEO_LINE_PIXEL_OFFSET = 0x04U;
static constexpr uint32_t VIDEO_LINE_STRIDE_BYTES = 0x54U;
static constexpr uint32_t VIDEO_WORDS_PER_LINE = 0x2AU;
static constexpr uint32_t ENGINE_WORDS_PER_FRAME = 0x4EC0U;
static constexpr uint32_t ENGINE_SIGNAL_PIXELS_FIFO_W = 0x01U;
static constexpr uint32_t ENGINE_SIGNAL_VIDCMD_FIFO_W = 0x02U;
static constexpr uint32_t ENGINE_SIGNAL_AUDIO_FIFO_W = 0x04U;
static constexpr uint32_t VIDCMD_SET_CMP_HELD_FG = 0x00U;
static constexpr uint32_t VIDCMD_SET_CMP_HELD_BG = 0x01U;
static constexpr uint32_t VIDCMD_SET_PIX_PAL_FG = 0x02U;
static constexpr uint32_t VIDCMD_SET_PIX_PAL_BG = 0x03U;
static constexpr uint32_t VIDCMD_SET_PIX_HAM_HELD = 0x04U;
static constexpr uint32_t VIDCMD_SET_PIX_MODE = 0x05U;
static constexpr uint32_t VIDCMD_SET_PIX_PIXEL_SKIP = 0x06U;
static constexpr uint32_t PIXELS_FIFO_WORDS = 0x100U;
static constexpr uint32_t VIDCMD_FIFO_WORDS = 0x100U;
static constexpr uint32_t PIXELS_WORDS_PER_LINE_1BPP = 0x28U;
static constexpr uint32_t PIXELS_WORDS_PER_LINE_HAM = 0x50U;
static constexpr uint32_t COMPOSITOR_LEAD = 0x02U;
static constexpr uint32_t PIXEL_OUT_LEAD = 0x01U;
static constexpr uint32_t TIMING_SPLIT_LEAD = 0x01U;
static constexpr uint32_t DAC_LEAD = 0x04U;
static constexpr uint32_t PIXEL_PRELOAD_LEAD = 0x10U;

// Syscalls (TRAP #15 ABI; number in d0, args in d1/d2/d3, return in d0)
static constexpr unsigned SYS_TRAP = 15U;
static constexpr unsigned SYS_EXIT = 0U;  // Terminate the application and return control to the firmware.
static constexpr unsigned SYS_WRITE = 1U;  // write(fd in d1, buf in d2, len in d3) to the console or a file.
static constexpr unsigned SYS_READ = 2U;  // read(fd in d1, buf in d2, len in d3) from the console or a file.
static constexpr unsigned SYS_OPEN = 3U;  // open(path in d1, flags in d2, mode in d3) and return a file descriptor.
static constexpr unsigned SYS_CLOSE = 4U;  // close(fd in d1) a previously opened file descriptor.
static constexpr unsigned SYS_LSEEK = 5U;  // lseek(fd in d1, offset in d2, whence in d3) within an open file.
static constexpr unsigned SYS_FSTAT = 6U;  // fstat(fd in d1, struct stat in d2) for an open file descriptor.
static constexpr unsigned SYS_ISATTY = 7U;  // isatty(fd in d1) returns nonzero if the descriptor is a terminal.
static constexpr unsigned SYS_STAT = 8U;  // stat(path in d1, struct stat in d2) for a named file.
static constexpr unsigned SYS_INPUTREADY = 9U;  // Return 1 if console input is ready so a read from the console would not block.
static constexpr unsigned SYS_READDIR = 10U;  // readdir(path in d1, index in d2, entry buffer in d3) fills one directory entry and returns 0, or 1 past the end.
static constexpr unsigned SYS_GETTICKS = 11U;  // Return milliseconds since boot as an unsigned 32-bit count that wraps.
static constexpr unsigned SYS_GETTIME = 12U;  // Return seconds since the Unix epoch derived from the boot epoch plus milliseconds since boot.

} // namespace Griffin
