// MCP7940N real-time clock behind a bit-banged I2C master on the DUART's
// OP2/OP3/IP2 pins.  See rtc.h for the contract and griffin.yml (DUART pin
// wiring, DUART_OP_RTC_* / DUART_IP_RTC_* masks, RTC_I2C_ADDR) for the pins.
//
// The two inversions, spelled out because they are what gets gotten wrong:
//   OPR_SET drives an OP pin LOW; OPR_CLR drives it HIGH (68681 OPR
//   complement).  OP2 is SCL directly, so SCL high = OPR_CLR.
//   OP3 drives the gate of an N-FET whose drain is SDA, so OP3 HIGH pulls
//   SDA LOW: SDA released = OP3 low = OPR_SET.  At reset OPR = 0, so SDA is
//   held low until rtc_init runs.
//
// The MCP7940N does not clock-stretch and is the only device on the bus, so
// SCL is push-pull and no stretch wait exists.  Bit timing is one DUART bus
// cycle per edge plus i2c_pad(); measure SCL on the bench before trusting
// the rate (griffin.log, RTC prove-out).

#include <stdint.h>

#include "../griffin.generated.h"
#include "../griffin.generated.refs.h"
#include "rtc.h"

using namespace Griffin::reg;

namespace {

// MCP7940N register map (datasheet DS20005010).
constexpr uint8_t REG_RTCSEC   = 0x00;   // bit 7 ST
constexpr uint8_t REG_RTCMIN   = 0x01;
constexpr uint8_t REG_RTCHOUR  = 0x02;   // bit 6 = 12-hour format
constexpr uint8_t REG_RTCWKDAY = 0x03;   // bit 5 OSCRUN (ro), bit 4 PWRFAIL, bit 3 VBATEN
constexpr uint8_t REG_RTCDATE  = 0x04;
constexpr uint8_t REG_RTCMTH   = 0x05;   // bit 5 LPYR (ro)
constexpr uint8_t REG_RTCYEAR  = 0x06;
constexpr uint8_t REG_CONTROL  = 0x07;

constexpr uint8_t RTCSEC_ST      = 0x80;
constexpr uint8_t RTCHOUR_12H    = 0x40;
constexpr uint8_t RTCHOUR_PM     = 0x20;
constexpr uint8_t RTCWKDAY_VBATEN = 0x08;
constexpr uint8_t RTCWKDAY_MASK  = 0x07;
constexpr uint8_t CONTROL_SQWEN  = 0x40;
constexpr uint8_t CONTROL_SQWFS_1HZ = 0x00;

// Chip address on the bus, shifted for the R/W bit.
constexpr uint8_t ADDR_W = static_cast<uint8_t>(Griffin::RTC_I2C_ADDR << 1);
constexpr uint8_t ADDR_R = static_cast<uint8_t>((Griffin::RTC_I2C_ADDR << 1) | 1);

constexpr uint8_t OP_SCL = static_cast<uint8_t>(Griffin::DUART_OP_RTC_SCL);
constexpr uint8_t OP_SDA = static_cast<uint8_t>(Griffin::DUART_OP_RTC_SDA_DRIVE);
constexpr uint8_t IP_SDA = static_cast<uint8_t>(Griffin::DUART_IP_RTC_SDA);
constexpr uint8_t IP_MFP = static_cast<uint8_t>(Griffin::DUART_IP_RTC_MFP);

bool present = false;

// A few bus cycles of padding so each SCL half-period clears the chip's
// 400 kHz minimums (tLOW 1.3 us, tHIGH 0.6 us) with margin even if a future
// build runs from zero-wait RAM.
inline void i2c_pad()
{
    asm volatile("nop\n\tnop\n\tnop\n\tnop" : : : "memory");
}

inline void scl_high()    { DUART_OPR_CLR = OP_SCL; }
inline void scl_low()     { DUART_OPR_SET = OP_SCL; }
inline void sda_release() { DUART_OPR_SET = OP_SDA; }
inline void sda_low()     { DUART_OPR_CLR = OP_SDA; }
inline bool sda_read()    { return (DUART_IP & IP_SDA) != 0; }

void i2c_start()
{
    sda_release();
    scl_high();
    i2c_pad();
    sda_low();
    i2c_pad();
    scl_low();
    i2c_pad();
}

void i2c_stop()
{
    sda_low();
    i2c_pad();
    scl_high();
    i2c_pad();
    sda_release();
    i2c_pad();
}

// Returns true on ACK.
bool i2c_write_byte(uint8_t b)
{
    for (int i = 0; i < 8; i++) {
        if (b & 0x80) {
            sda_release();
        } else {
            sda_low();
        }
        i2c_pad();
        scl_high();
        i2c_pad();
        scl_low();
        b = static_cast<uint8_t>(b << 1);
    }
    sda_release();
    i2c_pad();
    scl_high();
    i2c_pad();
    bool ack = !sda_read();
    scl_low();
    i2c_pad();
    return ack;
}

uint8_t i2c_read_byte(bool ack)
{
    uint8_t b = 0;
    sda_release();
    for (int i = 0; i < 8; i++) {
        i2c_pad();
        scl_high();
        i2c_pad();
        b = static_cast<uint8_t>((b << 1) | (sda_read() ? 1 : 0));
        scl_low();
    }
    if (ack) {
        sda_low();
    } else {
        sda_release();
    }
    i2c_pad();
    scl_high();
    i2c_pad();
    scl_low();
    sda_release();
    i2c_pad();
    return b;
}

// Nine clocks with SDA released, then STOP: finishes any byte a reset cut
// short so the chip lets go of SDA.
void i2c_bus_recover()
{
    sda_release();
    for (int i = 0; i < 9; i++) {
        scl_high();
        i2c_pad();
        scl_low();
        i2c_pad();
    }
    i2c_stop();
}

bool write_regs(uint8_t reg, const uint8_t *buf, unsigned n)
{
    i2c_start();
    bool ok = i2c_write_byte(ADDR_W) && i2c_write_byte(reg);
    for (unsigned i = 0; ok && i < n; i++) {
        ok = i2c_write_byte(buf[i]);
    }
    i2c_stop();
    return ok;
}

bool read_regs(uint8_t reg, uint8_t *buf, unsigned n)
{
    i2c_start();
    bool ok = i2c_write_byte(ADDR_W) && i2c_write_byte(reg);
    if (ok) {
        i2c_start();                            // repeated START
        ok = i2c_write_byte(ADDR_R);
        for (unsigned i = 0; ok && i < n; i++) {
            buf[i] = i2c_read_byte(i + 1 < n);
        }
    }
    i2c_stop();
    return ok;
}

inline uint8_t bcd_to_bin(uint8_t v)
{
    return static_cast<uint8_t>((v >> 4) * 10 + (v & 0x0F));
}

inline uint8_t bin_to_bcd(uint8_t v)
{
    return static_cast<uint8_t>(((v / 10) << 4) | (v % 10));
}

} // namespace

// ---- calendar arithmetic (Howard Hinnant's days_from_civil / civil_from_days)

void rtc_civil_from_epoch(uint32_t epoch, rtc_civil_t *out)
{
    uint32_t days = epoch / 86400U;
    uint32_t rem = epoch % 86400U;
    out->hour = static_cast<uint8_t>(rem / 3600U);
    out->minute = static_cast<uint8_t>((rem / 60U) % 60U);
    out->second = static_cast<uint8_t>(rem % 60U);
    out->weekday = static_cast<uint8_t>((days + 4U) % 7U);     // 1970-01-01 was a Thursday

    uint32_t z = days + 719468U;
    uint32_t era = z / 146097U;
    uint32_t doe = z - era * 146097U;
    uint32_t yoe = (doe - doe / 1460U + doe / 36524U - doe / 146096U) / 365U;
    uint32_t y = yoe + era * 400U;
    uint32_t doy = doe - (365U * yoe + yoe / 4U - yoe / 100U);
    uint32_t mp = (5U * doy + 2U) / 153U;
    uint32_t d = doy - (153U * mp + 2U) / 5U + 1U;
    uint32_t m = (mp < 10U) ? mp + 3U : mp - 9U;
    out->year = static_cast<uint16_t>(y + ((m <= 2U) ? 1U : 0U));
    out->month = static_cast<uint8_t>(m);
    out->day = static_cast<uint8_t>(d);
}

uint32_t rtc_epoch_from_civil(const rtc_civil_t *c)
{
    uint32_t y = c->year;
    uint32_t m = c->month;
    if (m <= 2U) {
        y -= 1U;
    }
    uint32_t era = y / 400U;
    uint32_t yoe = y - era * 400U;
    uint32_t doy = (153U * ((m > 2U) ? m - 3U : m + 9U) + 2U) / 5U + c->day - 1U;
    uint32_t doe = yoe * 365U + yoe / 4U - yoe / 100U + doy;
    uint32_t days = era * 146097U + doe - 719468U;
    return days * 86400U + c->hour * 3600U + c->minute * 60U + c->second;
}

// ---- the chip

bool rtc_init()
{
    DUART_OPCR = 0;                             // OP2..OP7 follow OPR
    i2c_bus_recover();                          // also releases SDA from the reset hold

    uint8_t regs[8];
    present = read_regs(REG_RTCSEC, regs, sizeof(regs));
    if (!present) {
        return false;
    }

    // A fresh part (or one whose battery ran out) has ST and VBATEN clear.
    if ((regs[REG_RTCSEC] & RTCSEC_ST) == 0) {
        uint8_t sec = static_cast<uint8_t>(regs[REG_RTCSEC] | RTCSEC_ST);
        write_regs(REG_RTCSEC, &sec, 1);
    }
    if ((regs[REG_RTCWKDAY] & RTCWKDAY_VBATEN) == 0) {
        uint8_t wk = static_cast<uint8_t>((regs[REG_RTCWKDAY] & RTCWKDAY_MASK) | RTCWKDAY_VBATEN);
        write_regs(REG_RTCWKDAY, &wk, 1);
    }

    // MFP = 1 Hz square wave; the DUART IP3 change-of-state IRQ counts it.
    uint8_t control = CONTROL_SQWEN | CONTROL_SQWFS_1HZ;
    if (regs[REG_CONTROL] != control) {
        write_regs(REG_CONTROL, &control, 1);
    }
    return true;
}

bool rtc_present()
{
    return present;
}

bool rtc_read_regs(uint8_t reg, uint8_t *buf, unsigned n)
{
    return read_regs(reg, buf, n);
}

bool rtc_mfp_level()
{
    return (DUART_IP & IP_MFP) != 0;
}

bool rtc_read_time(uint32_t *epoch)
{
    uint8_t regs[7];
    if (!read_regs(REG_RTCSEC, regs, sizeof(regs))) {
        return false;
    }
    rtc_civil_t c;
    c.second = bcd_to_bin(regs[REG_RTCSEC] & 0x7F);
    c.minute = bcd_to_bin(regs[REG_RTCMIN] & 0x7F);
    uint8_t hour = regs[REG_RTCHOUR];
    if (hour & RTCHOUR_12H) {
        c.hour = bcd_to_bin(hour & 0x1F);
        if (c.hour == 12) {
            c.hour = 0;
        }
        if (hour & RTCHOUR_PM) {
            c.hour = static_cast<uint8_t>(c.hour + 12);
        }
    } else {
        c.hour = bcd_to_bin(hour & 0x3F);
    }
    c.day = bcd_to_bin(regs[REG_RTCDATE] & 0x3F);
    c.month = bcd_to_bin(regs[REG_RTCMTH] & 0x1F);
    c.year = static_cast<uint16_t>(2000 + bcd_to_bin(regs[REG_RTCYEAR]));
    c.weekday = 0;
    if (c.month < 1 || c.month > 12 || c.day < 1 || c.day > 31 ||
        c.hour > 23 || c.minute > 59 || c.second > 59) {
        return false;                           // never-set or corrupt registers
    }
    *epoch = rtc_epoch_from_civil(&c);
    return true;
}

bool rtc_write_time(uint32_t epoch)
{
    rtc_civil_t c;
    rtc_civil_from_epoch(epoch, &c);
    if (c.year < 2000 || c.year > 2099) {
        return false;
    }
    uint8_t regs[7];
    regs[REG_RTCSEC] = bin_to_bcd(c.second);    // ST clear: oscillator halted for the write
    regs[REG_RTCMIN] = bin_to_bcd(c.minute);
    regs[REG_RTCHOUR] = bin_to_bcd(c.hour);     // 24-hour format
    regs[REG_RTCWKDAY] = static_cast<uint8_t>(RTCWKDAY_VBATEN | (c.weekday + 1));
    regs[REG_RTCDATE] = bin_to_bcd(c.day);
    regs[REG_RTCMTH] = bin_to_bcd(c.month);
    regs[REG_RTCYEAR] = bin_to_bcd(static_cast<uint8_t>(c.year - 2000));
    if (!write_regs(REG_RTCSEC, regs, sizeof(regs))) {
        return false;
    }
    uint8_t sec = static_cast<uint8_t>(regs[REG_RTCSEC] | RTCSEC_ST);
    return write_regs(REG_RTCSEC, &sec, 1);
}
