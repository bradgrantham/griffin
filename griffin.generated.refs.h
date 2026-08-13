// Generated from griffin.yml by codegen.py — do not edit (regenerate with: make codegen)
//
// Inline references for memory-mapped registers.  Include this header
// from firmware translation units that read/write peripheral registers
// and add `using namespace Griffin::reg;` to bring the names into scope.
//
// Each reference is a `volatile uintN_t &` bound to the register's
// physical address, where N is the register's declared width.

#pragma once

#include <cstdint>

namespace Griffin::reg {

// GLUE: System glue logic (also hosts PS/2 keyboard RX/TX)
inline volatile uint8_t &GLUE_DEBUG_OUT = *reinterpret_cast<volatile uint8_t *>(0xF00001UL);  // WRITE: Set or clear DEBUG_OUT signal (debug LED and test point output)
inline volatile uint8_t &GLUE_CONFIG = *reinterpret_cast<volatile uint8_t *>(0xF00007UL);  // WRITE: GLUE configuration register
inline volatile uint8_t &GLUE_PS2_TX_DATA = *reinterpret_cast<volatile uint8_t *>(0xF00009UL);  // WRITE: Byte to transmit host->device; the write itself starts the frame
inline volatile uint8_t &GLUE_PS2_STATUS = *reinterpret_cast<volatile uint8_t *>(0xF00011UL);  // READ: PS/2 frame-engine status
inline volatile uint8_t &GLUE_PS2_CLEAR = *reinterpret_cast<volatile uint8_t *>(0xF00011UL);  // WRITE: Write-1-to-clear for PS2_STATUS latched flags / IRQ ack
inline volatile uint8_t &GLUE_PS2_CTRL = *reinterpret_cast<volatile uint8_t *>(0xF00013UL);  // WRITE: Open-drain drive control for PS2_CLK and PS2_DATA for the TX inhibit handshake
inline volatile uint8_t &GLUE_PS2_RX_DATA = *reinterpret_cast<volatile uint8_t *>(0xF00015UL);  // READ: Assembled PS/2 data byte; valid while RX_READY is set
inline volatile uint8_t &GLUE_VSYNC_STATUS = *reinterpret_cast<volatile uint8_t *>(0xF00017UL);  // READ: Latched vsync frame interrupt status
inline volatile uint8_t &GLUE_VSYNC_CLEAR = *reinterpret_cast<volatile uint8_t *>(0xF00017UL);  // WRITE: Write-1-to-clear for VSYNC_PENDING / level-6 IRQ ack

// ENGINE: Video framebuffer DMA engine with 7200 FIFO write interface
inline volatile uint8_t &ENGINE_SOURCE_PAGE = *reinterpret_cast<volatile uint8_t *>(0xD00003UL);  // WRITE: Framebuffer page: A[23:16] of source base address
inline volatile uint8_t &ENGINE_CTRL = *reinterpret_cast<volatile uint8_t *>(0xD00005UL);  // WRITE: DMA control register
inline volatile uint8_t &ENGINE_STATUS = *reinterpret_cast<volatile uint8_t *>(0xD00005UL);  // READ: DMA status readback

// VIDEO: VGA 640x480@60 1bpp video generator with 7200 FIFO read interface
inline volatile uint8_t &VIDEO_CLRINT = *reinterpret_cast<volatile uint8_t *>(0xE00003UL);  // WRITE: Write any value to clear the latched VIDEO vsync IRQ
inline volatile uint8_t &VIDEO_CTRL = *reinterpret_cast<volatile uint8_t *>(0xE00005UL);  // WRITE: Video control register
inline volatile uint8_t &VIDEO_CTRL_RB = *reinterpret_cast<volatile uint8_t *>(0xE00005UL);  // READ: Video control readback
inline volatile uint8_t &VIDEO_CLRERR = *reinterpret_cast<volatile uint8_t *>(0xE00009UL);  // WRITE: Write any value to clear FIFO_ERROR sticky bit

// PORTS: PS/2 mouse, two joystick ports, two paddle counters, and the audio FIFO pop strobe
inline volatile uint8_t &PORTS_JOYSTICK_PORT_1 = *reinterpret_cast<volatile uint8_t *>(0xFC0001UL);  // READ: Joystick port 1 switches, active low (0 = closed)
inline volatile uint8_t &PORTS_JOYSTICK_PORT_2 = *reinterpret_cast<volatile uint8_t *>(0xFC0003UL);  // READ: Joystick port 2 switches, active low (0 = closed)
inline volatile uint8_t &PORTS_PADDLE_A_COUNT = *reinterpret_cast<volatile uint8_t *>(0xFC0005UL);  // READ: Paddle A position, the pot on port 1 pin 9
inline volatile uint8_t &PORTS_PADDLE_B_COUNT = *reinterpret_cast<volatile uint8_t *>(0xFC0007UL);  // READ: Paddle B position, the pot on port 1 pin 5
inline volatile uint8_t &PORTS_PS2_MOUSE_TX_DATA = *reinterpret_cast<volatile uint8_t *>(0xFC0009UL);  // WRITE: Byte to transmit host->mouse; the write itself starts the frame
inline volatile uint8_t &PORTS_PADDLE_CONTROL = *reinterpret_cast<volatile uint8_t *>(0xFC000FUL);  // WRITE: Paddle measurement control, written once per vsync ISR
inline volatile uint8_t &PORTS_PS2_MOUSE_STATUS = *reinterpret_cast<volatile uint8_t *>(0xFC0011UL);  // READ: PS/2 mouse frame-engine status
inline volatile uint8_t &PORTS_PS2_MOUSE_CLEAR = *reinterpret_cast<volatile uint8_t *>(0xFC0011UL);  // WRITE: Write-1-to-clear for PS2_MOUSE_STATUS latched flags / IRQ ack
inline volatile uint8_t &PORTS_PS2_MOUSE_CTRL = *reinterpret_cast<volatile uint8_t *>(0xFC0013UL);  // WRITE: Open-drain drive control for PS2_MOUSE_CLK and PS2_MOUSE_DATA for the TX inhibit handshake
inline volatile uint8_t &PORTS_PS2_MOUSE_RX_DATA = *reinterpret_cast<volatile uint8_t *>(0xFC0015UL);  // READ: Assembled PS/2 mouse data byte; valid while RX_READY is set
inline volatile uint8_t &PORTS_AUDIO_CONTROL = *reinterpret_cast<volatile uint8_t *>(0xFC001FUL);  // WRITE: Audio FIFO pop control
inline volatile uint8_t &PORTS_AUDIO_STATUS = *reinterpret_cast<volatile uint8_t *>(0xFC001FUL);  // READ: Audio FIFO pop status

// CF: Storage via CF card in True IDE 16-bit PIO mode
inline volatile uint16_t &CF_DATA = *reinterpret_cast<volatile uint16_t *>(0xF40000UL);  // RW: 16-bit data port to/from the CF card
inline volatile uint8_t &CF_ERROR = *reinterpret_cast<volatile uint8_t *>(0xF40003UL);  // READ: Error register; valid after a command error
inline volatile uint8_t &CF_FEATURES = *reinterpret_cast<volatile uint8_t *>(0xF40003UL);  // WRITE: Features register; written before issuing SET_FEATURES command
inline volatile uint8_t &CF_SECTOR_COUNT = *reinterpret_cast<volatile uint8_t *>(0xF40005UL);  // RW: Number of sectors to transfer
inline volatile uint8_t &CF_SECTOR_NUM = *reinterpret_cast<volatile uint8_t *>(0xF40007UL);  // RW: LBA bits 7:0
inline volatile uint8_t &CF_CYL_LO = *reinterpret_cast<volatile uint8_t *>(0xF40009UL);  // RW: LBA bits 15:8
inline volatile uint8_t &CF_CYL_HI = *reinterpret_cast<volatile uint8_t *>(0xF4000BUL);  // RW: LBA bits 23:16
inline volatile uint8_t &CF_DRIVE_HEAD = *reinterpret_cast<volatile uint8_t *>(0xF4000DUL);  // RW: LBA bits 27:24 and drive select; OR with CF_DH_LBA for LBA mode
inline volatile uint8_t &CF_STATUS = *reinterpret_cast<volatile uint8_t *>(0xF4000FUL);  // READ: Device status; poll BSY clear and DRQ set before data transfer
inline volatile uint8_t &CF_COMMAND = *reinterpret_cast<volatile uint8_t *>(0xF4000FUL);  // WRITE: Issue command; write after setting all other registers

// DUART: Console (Ch A) and second serial (Ch B), 100 Hz systick C/T, GP pins for flow control / DS3231 I2C
inline volatile uint8_t &DUART_MR1A = *reinterpret_cast<volatile uint8_t *>(0xF80001UL);  // RW: Channel A mode register 1 (after reset or MR pointer reset)
inline volatile uint8_t &DUART_MR2A = *reinterpret_cast<volatile uint8_t *>(0xF80001UL);  // RW: Channel A mode register 2 (second access after MR pointer reset)
inline volatile uint8_t &DUART_SRA = *reinterpret_cast<volatile uint8_t *>(0xF80003UL);  // READ: Channel A status register
inline volatile uint8_t &DUART_CSRA = *reinterpret_cast<volatile uint8_t *>(0xF80003UL);  // WRITE: Channel A clock select register
inline volatile uint8_t &DUART_CRA = *reinterpret_cast<volatile uint8_t *>(0xF80005UL);  // WRITE: Channel A command register
inline volatile uint8_t &DUART_RBA = *reinterpret_cast<volatile uint8_t *>(0xF80007UL);  // READ: Channel A receiver buffer (read to dequeue FIFO)
inline volatile uint8_t &DUART_TBA = *reinterpret_cast<volatile uint8_t *>(0xF80007UL);  // WRITE: Channel A transmitter buffer (write to enqueue)
inline volatile uint8_t &DUART_IPCR = *reinterpret_cast<volatile uint8_t *>(0xF80009UL);  // READ: Input port change register (read clears interrupt)
inline volatile uint8_t &DUART_ACR = *reinterpret_cast<volatile uint8_t *>(0xF80009UL);  // WRITE: Auxiliary control register (timer/counter mode, BRG set select)
inline volatile uint8_t &DUART_ISR = *reinterpret_cast<volatile uint8_t *>(0xF8000BUL);  // READ: Interrupt status register
inline volatile uint8_t &DUART_IMR = *reinterpret_cast<volatile uint8_t *>(0xF8000BUL);  // WRITE: Interrupt mask register (same bit layout as ISR; 1=enabled)
inline volatile uint8_t &DUART_CUR = *reinterpret_cast<volatile uint8_t *>(0xF8000DUL);  // READ: Counter/timer upper byte (current value)
inline volatile uint8_t &DUART_CTUR = *reinterpret_cast<volatile uint8_t *>(0xF8000DUL);  // WRITE: Counter/timer upper preload register
inline volatile uint8_t &DUART_CLR = *reinterpret_cast<volatile uint8_t *>(0xF8000FUL);  // READ: Counter/timer lower byte (current value)
inline volatile uint8_t &DUART_CTLR = *reinterpret_cast<volatile uint8_t *>(0xF8000FUL);  // WRITE: Counter/timer lower preload register
inline volatile uint8_t &DUART_MR1B = *reinterpret_cast<volatile uint8_t *>(0xF80011UL);  // RW: Channel B mode register 1 (after reset or MR pointer reset)
inline volatile uint8_t &DUART_MR2B = *reinterpret_cast<volatile uint8_t *>(0xF80011UL);  // RW: Channel B mode register 2 (second access after MR pointer reset)
inline volatile uint8_t &DUART_SRB = *reinterpret_cast<volatile uint8_t *>(0xF80013UL);  // READ: Channel B status register (same bit layout as SRA)
inline volatile uint8_t &DUART_CSRB = *reinterpret_cast<volatile uint8_t *>(0xF80013UL);  // WRITE: Channel B clock select register
inline volatile uint8_t &DUART_CRB = *reinterpret_cast<volatile uint8_t *>(0xF80015UL);  // WRITE: Channel B command register (same bit layout as CRA)
inline volatile uint8_t &DUART_RBB = *reinterpret_cast<volatile uint8_t *>(0xF80017UL);  // READ: Channel B receiver buffer (read to dequeue FIFO)
inline volatile uint8_t &DUART_TBB = *reinterpret_cast<volatile uint8_t *>(0xF80017UL);  // WRITE: Channel B transmitter buffer (write to enqueue)
inline volatile uint8_t &DUART_IVR = *reinterpret_cast<volatile uint8_t *>(0xF80019UL);  // RW: Interrupt vector register
inline volatile uint8_t &DUART_IP = *reinterpret_cast<volatile uint8_t *>(0xF8001BUL);  // READ: Input port (unlatched)
inline volatile uint8_t &DUART_OPCR = *reinterpret_cast<volatile uint8_t *>(0xF8001BUL);  // WRITE: Output port configuration register
inline volatile uint8_t &DUART_STARTCC = *reinterpret_cast<volatile uint8_t *>(0xF8001DUL);  // READ: Start counter/timer command (read to start; data ignored)
inline volatile uint8_t &DUART_OPR_SET = *reinterpret_cast<volatile uint8_t *>(0xF8001DUL);  // WRITE: Output port bit set (1 bits set corresponding OP pins)
inline volatile uint8_t &DUART_STOPCC = *reinterpret_cast<volatile uint8_t *>(0xF8001FUL);  // READ: Stop counter/timer command (read to stop; data ignored)
inline volatile uint8_t &DUART_OPR_CLR = *reinterpret_cast<volatile uint8_t *>(0xF8001FUL);  // WRITE: Output port bit reset (1 bits clear corresponding OP pins)

// AUDIO: Stereo FIFO audio output, drained at half the VGA line rate by PORTS
inline volatile uint16_t &AUDIO_FIFO = *reinterpret_cast<volatile uint16_t *>(0xC00000UL);  // WRITE: Write one stereo sample pair into the audio FIFOs

} // namespace Griffin::reg
