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

// GLUE: System glue logic, PS/2 keyboard, ENGINE and CF card status readback
inline volatile uint8_t &GLUE_DEBUG_OUT = *reinterpret_cast<volatile uint8_t *>(0xF00001UL);  // WRITE: Set or clear DEBUG_OUT signal (debug LED and test point output)
inline volatile uint8_t &GLUE_CONFIG = *reinterpret_cast<volatile uint8_t *>(0xF00007UL);  // WRITE: GLUE configuration register
inline volatile uint8_t &GLUE_PS2_TX_DATA = *reinterpret_cast<volatile uint8_t *>(0xF00009UL);  // WRITE: Byte to transmit host->device; the write itself starts the frame
inline volatile uint8_t &GLUE_PS2_STATUS = *reinterpret_cast<volatile uint8_t *>(0xF00011UL);  // READ: PS/2 frame-engine status
inline volatile uint8_t &GLUE_PS2_CLEAR = *reinterpret_cast<volatile uint8_t *>(0xF00011UL);  // WRITE: Write-1-to-clear for PS2_STATUS latched flags / IRQ ack
inline volatile uint8_t &GLUE_PS2_CTRL = *reinterpret_cast<volatile uint8_t *>(0xF00013UL);  // WRITE: Open-drain drive control for PS2_CLK and PS2_DATA for the TX inhibit handshake
inline volatile uint8_t &GLUE_PS2_RX_DATA = *reinterpret_cast<volatile uint8_t *>(0xF00015UL);  // READ: Assembled PS/2 data byte; valid while RX_READY is set
inline volatile uint8_t &GLUE_VSYNC_STATUS = *reinterpret_cast<volatile uint8_t *>(0xF00017UL);  // READ: Latched vsync frame interrupt status
inline volatile uint8_t &GLUE_VSYNC_CLEAR = *reinterpret_cast<volatile uint8_t *>(0xF00017UL);  // WRITE: Write-1-to-clear for VSYNC_PENDING / level-6 IRQ ack
inline volatile uint8_t &GLUE_ENGINE_STATUS = *reinterpret_cast<volatile uint8_t *>(0xF00019UL);  // READ: Display-list ENGINE state, read back through GLUE because ENGINE reads see the open bus
inline volatile uint8_t &GLUE_CF_PINS = *reinterpret_cast<volatile uint8_t *>(0xF0001BUL);  // READ: CF card sideband pin levels, read raw

// ENGINE: Display-list DMA engine walking 4-word descriptors from the top 64K of RAM
inline volatile uint16_t &ENGINE_DESC = *reinterpret_cast<volatile uint16_t *>(0xD00002UL);  // WRITE: Descriptor word address within RAM's top 64K; writing arms DMA
inline volatile uint8_t &ENGINE_CTRL = *reinterpret_cast<volatile uint8_t *>(0xD00005UL);  // WRITE: DMA enable and abort; any write clears a pending ~ENGINE_IRQ

// PORTS: PS/2 mouse, two joystick ports, two paddle counters, and the audio FIFO pop/reset control
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
inline volatile uint8_t &PORTS_AUDIO_CONTROL = *reinterpret_cast<volatile uint8_t *>(0xFC001FUL);  // WRITE: Audio FIFO pop and reset control
inline volatile uint8_t &PORTS_AUDIO_STATUS = *reinterpret_cast<volatile uint8_t *>(0xFC001FUL);  // READ: Audio FIFO status; no interrupt, poll it

// CF: Storage via CF card in True IDE 16-bit PIO mode
inline volatile uint16_t &CF_DATA = *reinterpret_cast<volatile uint16_t *>(0xF40000UL);  // RW: 16-bit data port to/from the CF card
inline volatile uint8_t &CF_ERROR = *reinterpret_cast<volatile uint8_t *>(0xF40003UL);  // READ: Error register
inline volatile uint8_t &CF_FEATURES = *reinterpret_cast<volatile uint8_t *>(0xF40003UL);  // WRITE: Features register
inline volatile uint8_t &CF_SECTOR_COUNT = *reinterpret_cast<volatile uint8_t *>(0xF40005UL);  // RW: Number of sectors to transfer
inline volatile uint8_t &CF_SECTOR_NUM = *reinterpret_cast<volatile uint8_t *>(0xF40007UL);  // RW: LBA bits 7:0
inline volatile uint8_t &CF_CYL_LO = *reinterpret_cast<volatile uint8_t *>(0xF40009UL);  // RW: LBA bits 15:8
inline volatile uint8_t &CF_CYL_HI = *reinterpret_cast<volatile uint8_t *>(0xF4000BUL);  // RW: LBA bits 23:16
inline volatile uint8_t &CF_DRIVE_HEAD = *reinterpret_cast<volatile uint8_t *>(0xF4000DUL);  // RW: LBA bits 27:24 and drive select; OR with CF_DH_LBA for LBA mode
inline volatile uint8_t &CF_STATUS = *reinterpret_cast<volatile uint8_t *>(0xF4000FUL);  // READ: Status register
inline volatile uint8_t &CF_COMMAND = *reinterpret_cast<volatile uint8_t *>(0xF4000FUL);  // WRITE: Command register

// DUART: Console (Ch A) and second serial (Ch B), 100 Hz systick C/T, GP pins for flow control / DS3231 I2C
inline volatile uint8_t &DUART_MR1A = *reinterpret_cast<volatile uint8_t *>(0xF80001UL);  // RW: Channel A mode register 1
inline volatile uint8_t &DUART_MR2A = *reinterpret_cast<volatile uint8_t *>(0xF80001UL);  // RW: Channel A mode register 2
inline volatile uint8_t &DUART_SRA = *reinterpret_cast<volatile uint8_t *>(0xF80003UL);  // READ: Channel A status register
inline volatile uint8_t &DUART_CSRA = *reinterpret_cast<volatile uint8_t *>(0xF80003UL);  // WRITE: Channel A clock select register
inline volatile uint8_t &DUART_CRA = *reinterpret_cast<volatile uint8_t *>(0xF80005UL);  // WRITE: Channel A command register
inline volatile uint8_t &DUART_RBA = *reinterpret_cast<volatile uint8_t *>(0xF80007UL);  // READ: Channel A receiver buffer
inline volatile uint8_t &DUART_TBA = *reinterpret_cast<volatile uint8_t *>(0xF80007UL);  // WRITE: Channel A transmitter buffer
inline volatile uint8_t &DUART_IPCR = *reinterpret_cast<volatile uint8_t *>(0xF80009UL);  // READ: Input port change register
inline volatile uint8_t &DUART_ACR = *reinterpret_cast<volatile uint8_t *>(0xF80009UL);  // WRITE: Auxiliary control register (timer/counter mode, BRG set select)
inline volatile uint8_t &DUART_ISR = *reinterpret_cast<volatile uint8_t *>(0xF8000BUL);  // READ: Interrupt status register
inline volatile uint8_t &DUART_IMR = *reinterpret_cast<volatile uint8_t *>(0xF8000BUL);  // WRITE: Interrupt mask register (same bit layout as ISR; 1=enabled)
inline volatile uint8_t &DUART_CUR = *reinterpret_cast<volatile uint8_t *>(0xF8000DUL);  // READ: Counter/timer upper byte (current value)
inline volatile uint8_t &DUART_CTUR = *reinterpret_cast<volatile uint8_t *>(0xF8000DUL);  // WRITE: Counter/timer upper preload register
inline volatile uint8_t &DUART_CLR = *reinterpret_cast<volatile uint8_t *>(0xF8000FUL);  // READ: Counter/timer lower byte (current value)
inline volatile uint8_t &DUART_CTLR = *reinterpret_cast<volatile uint8_t *>(0xF8000FUL);  // WRITE: Counter/timer lower preload register
inline volatile uint8_t &DUART_MR1B = *reinterpret_cast<volatile uint8_t *>(0xF80011UL);  // RW: Channel B mode register 1
inline volatile uint8_t &DUART_MR2B = *reinterpret_cast<volatile uint8_t *>(0xF80011UL);  // RW: Channel B mode register 2
inline volatile uint8_t &DUART_SRB = *reinterpret_cast<volatile uint8_t *>(0xF80013UL);  // READ: Channel B status register (same bit layout as SRA)
inline volatile uint8_t &DUART_CSRB = *reinterpret_cast<volatile uint8_t *>(0xF80013UL);  // WRITE: Channel B clock select register
inline volatile uint8_t &DUART_CRB = *reinterpret_cast<volatile uint8_t *>(0xF80015UL);  // WRITE: Channel B command register (same bit layout as CRA)
inline volatile uint8_t &DUART_RBB = *reinterpret_cast<volatile uint8_t *>(0xF80017UL);  // READ: Channel B receiver buffer
inline volatile uint8_t &DUART_TBB = *reinterpret_cast<volatile uint8_t *>(0xF80017UL);  // WRITE: Channel B transmitter buffer
inline volatile uint8_t &DUART_IVR = *reinterpret_cast<volatile uint8_t *>(0xF80019UL);  // RW: Interrupt vector register
inline volatile uint8_t &DUART_IP = *reinterpret_cast<volatile uint8_t *>(0xF8001BUL);  // READ: Input port (unlatched)
inline volatile uint8_t &DUART_OPCR = *reinterpret_cast<volatile uint8_t *>(0xF8001BUL);  // WRITE: Output port configuration register
inline volatile uint8_t &DUART_STARTCC = *reinterpret_cast<volatile uint8_t *>(0xF8001DUL);  // READ: Start counter/timer command (read)
inline volatile uint8_t &DUART_OPR_SET = *reinterpret_cast<volatile uint8_t *>(0xF8001DUL);  // WRITE: Output port bit set (sets OPR bits; those OP pins go LOW)
inline volatile uint8_t &DUART_STOPCC = *reinterpret_cast<volatile uint8_t *>(0xF8001FUL);  // READ: Stop counter/timer command (read)
inline volatile uint8_t &DUART_OPR_CLR = *reinterpret_cast<volatile uint8_t *>(0xF8001FUL);  // WRITE: Output port bit reset (clears OPR bits; those OP pins go HIGH)

} // namespace Griffin::reg
