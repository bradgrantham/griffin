#ifndef RTC_H
#define RTC_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ====================================================================
 * MCP7940N real-time clock on the DUART's spare OP/IP pins (griffin.yml
 * DUART pin wiring): SCL = OP2, SDA = OP3 through the N-FET, SDA readback =
 * IP2, MFP = IP3.  This layer only talks to the chip; it never prints.
 *
 * Wall time itself is NOT read from the chip after boot: firmware programs
 * MFP as a 1 Hz square wave and the DUART IP3 change-of-state interrupt
 * (_duart_isr in crt0.s) advances `rtc_seconds` on every rising edge.  The
 * chip is read once at boot (rtc_read_time) and written on a set.
 *
 * All times are UTC.  The chip holds a two-digit year, read as 2000-2099.
 * ==================================================================== */

typedef struct {
    uint16_t year;      /* 1970.. */
    uint8_t  month;     /* 1..12 */
    uint8_t  day;       /* 1..31 */
    uint8_t  hour;      /* 0..23 */
    uint8_t  minute;    /* 0..59 */
    uint8_t  second;    /* 0..59 */
    uint8_t  weekday;   /* 0 = Sunday .. 6 = Saturday */
} rtc_civil_t;

/* Proleptic Gregorian, no leap seconds, no time zone. */
void     rtc_civil_from_epoch(uint32_t epoch, rtc_civil_t *out);
uint32_t rtc_epoch_from_civil(const rtc_civil_t *c);

/* Release the I2C bus (OPR bit 3; reset leaves SDA held low through the FET),
 * recover any transfer a reset interrupted, probe the chip, start its
 * oscillator (ST) and battery switchover (VBATEN) if a fresh part has them
 * clear, and program MFP as the 1 Hz square wave.  Returns true if the chip
 * acknowledged its address.  Call before the DUART ACR/IMR are written so the
 * caller can enable the IP3 interrupt only when there is something on it. */
bool rtc_init(void);

/* Result of the last rtc_init. */
bool rtc_present(void);

/* Read the timekeeping registers into an epoch.  False on a NACK. */
bool rtc_read_time(uint32_t *epoch);

/* Write the timekeeping registers from an epoch (oscillator halted around the
 * write as the datasheet asks, ST and VBATEN left set).  False on a NACK. */
bool rtc_write_time(uint32_t epoch);

/* Raw register access for a monitor dump.  False on a NACK. */
bool rtc_read_regs(uint8_t reg, uint8_t *buf, unsigned n);

/* Level of the MFP pin right now (IP3), for the boot-time edge alignment. */
bool rtc_mfp_level(void);

#ifdef __cplusplus
}
#endif

#endif /* RTC_H */
