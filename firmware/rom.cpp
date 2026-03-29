#include <string>
#include <cstdio>
#include <cstdint>
#include <cstdarg>
#include <cstring>

#include "../griffin.generated.h"


// ---------------------------------------------------------------------------
// Compact Flash (True IDE, 8-bit PIO)
// ---------------------------------------------------------------------------

enum cf_error : int { CF_OK = 0, CF_TIMEOUT, CF_ERR };

struct cf_info
{
    char     model[41];       // words 27-46, null-terminated
    char     serial[21];      // words 10-19, null-terminated
    char     firmware_rev[9]; // words 23-26, null-terminated
    uint32_t lba_sectors;     // words 60-61
};

static constexpr uint32_t CF_POLL_LIMIT = 500000;

static volatile uint8_t &cf_data       = *reinterpret_cast<volatile uint8_t *>(Griffin::CF_DATA);
static volatile uint8_t &cf_error_reg  = *reinterpret_cast<volatile uint8_t *>(Griffin::CF_ERROR);
static volatile uint8_t &cf_features   = *reinterpret_cast<volatile uint8_t *>(Griffin::CF_FEATURES);
static volatile uint8_t &cf_sec_count  = *reinterpret_cast<volatile uint8_t *>(Griffin::CF_SECTOR_COUNT);
static volatile uint8_t &cf_sec_num    = *reinterpret_cast<volatile uint8_t *>(Griffin::CF_SECTOR_NUM);
static volatile uint8_t &cf_cyl_lo     = *reinterpret_cast<volatile uint8_t *>(Griffin::CF_CYL_LO);
static volatile uint8_t &cf_cyl_hi     = *reinterpret_cast<volatile uint8_t *>(Griffin::CF_CYL_HI);
static volatile uint8_t &cf_drive_head = *reinterpret_cast<volatile uint8_t *>(Griffin::CF_DRIVE_HEAD);
static volatile uint8_t &cf_status     = *reinterpret_cast<volatile uint8_t *>(Griffin::CF_STATUS);
static volatile uint8_t &cf_command    = *reinterpret_cast<volatile uint8_t *>(Griffin::CF_COMMAND);

// Wait for BSY to clear.  Returns CF_TIMEOUT if limit exceeded.
static cf_error cf_wait_ready()
{
    for (uint32_t i = 0; i < CF_POLL_LIMIT; i++)
    {
        if (!(cf_status & Griffin::CF_STATUS_BSY))
        {
            return CF_OK;
        }
    }
    return CF_TIMEOUT;
}

// Wait for BSY clear and DRQ set.  Returns CF_ERR if device signals error.
static cf_error cf_wait_drq()
{
    for (uint32_t i = 0; i < CF_POLL_LIMIT; i++)
    {
        uint8_t s = cf_status;
        if (s & Griffin::CF_STATUS_ERR)
        {
            return CF_ERR;
        }
        if (!(s & Griffin::CF_STATUS_BSY) && (s & Griffin::CF_STATUS_DRQ))
        {
            return CF_OK;
        }
    }
    return CF_TIMEOUT;
}

// Set LBA address and sector count into CF registers.
static void cf_set_lba(uint32_t lba, uint8_t count)
{
    cf_sec_count  = count;
    cf_sec_num    = lba & 0xFF;
    cf_cyl_lo     = (lba >> 8) & 0xFF;
    cf_cyl_hi     = (lba >> 16) & 0xFF;
    cf_drive_head = Griffin::CF_DH_LBA | ((lba >> 24) & 0x0F);
}

// Initialize CF card: wait for ready, set 8-bit PIO mode.
cf_error cf_init()
{
    cf_error err = cf_wait_ready();
    if (err != CF_OK)
    {
        return err;
    }

    // Wait for DRDY
    for (uint32_t i = 0; i < CF_POLL_LIMIT; i++)
    {
        if (cf_status & Griffin::CF_STATUS_DRDY)
        {
            goto drdy_ok;
        }
    }
    return CF_TIMEOUT;

drdy_ok:
    // Set 8-bit transfer mode
    cf_features = Griffin::CF_CMD_SET_8BIT;
    cf_command  = Griffin::CF_CMD_SET_FEATURES;
    return cf_wait_ready();
}

// Read the 512-byte IDENTIFY DEVICE block into caller-provided buffer.
cf_error cf_identify(uint8_t buf[512])
{
    cf_error err = cf_wait_ready();
    if (err != CF_OK)
    {
        return err;
    }

    cf_drive_head = Griffin::CF_DH_LBA;
    cf_command    = Griffin::CF_CMD_IDENTIFY;

    err = cf_wait_drq();
    if (err != CF_OK)
    {
        return err;
    }

    for (int i = 0; i < 512; i++)
    {
        buf[i] = cf_data;
    }
    return CF_OK;
}

// Extract an ATA string from the identify block.
// ATA strings store first char of each word-pair in the high byte,
// but 8-bit PIO reads low byte first, so adjacent bytes are swapped.
static void cf_extract_string(const uint8_t *buf, int word_start, int word_count, char *out)
{
    int byte_off = word_start * 2;
    int len = word_count * 2;
    for (int i = 0; i < len; i += 2)
    {
        out[i]     = buf[byte_off + i + 1];
        out[i + 1] = buf[byte_off + i];
    }
    // Trim trailing spaces and null-terminate
    int end = len;
    while (end > 0 && out[end - 1] == ' ')
    {
        end--;
    }
    out[end] = '\0';
}

// Parse the raw 512-byte identify block into a cf_info struct.
void cf_parse_identify(const uint8_t buf[512], cf_info *info)
{
    cf_extract_string(buf, 27, 20, info->model);
    cf_extract_string(buf, 10, 10, info->serial);
    cf_extract_string(buf, 23, 4,  info->firmware_rev);

    // Words 60-61: total LBA sectors (little-endian words, low byte first in 8-bit PIO)
    info->lba_sectors = static_cast<uint32_t>(buf[120])
                      | (static_cast<uint32_t>(buf[121]) << 8)
                      | (static_cast<uint32_t>(buf[122]) << 16)
                      | (static_cast<uint32_t>(buf[123]) << 24);
}

// Read count sectors starting at lba into buf.
// buf must be at least count * 512 bytes.
cf_error cf_read_sectors(uint32_t lba, uint8_t count, uint8_t *buf)
{
    cf_error err = cf_wait_ready();
    if (err != CF_OK)
    {
        return err;
    }

    cf_set_lba(lba, count);
    cf_command = Griffin::CF_CMD_READ_SECTORS;

    for (int sec = 0; sec < count; sec++)
    {
        err = cf_wait_drq();
        if (err != CF_OK)
        {
            return err;
        }
        for (int i = 0; i < 512; i++)
        {
            *buf++ = cf_data;
        }
    }
    return CF_OK;
}

// Write count sectors starting at lba from buf.
// buf must be at least count * 512 bytes.
cf_error cf_write_sectors(uint32_t lba, uint8_t count, const uint8_t *buf)
{
    cf_error err = cf_wait_ready();
    if (err != CF_OK)
    {
        return err;
    }

    cf_set_lba(lba, count);
    cf_command = Griffin::CF_CMD_WRITE_SECTORS;

    for (int sec = 0; sec < count; sec++)
    {
        err = cf_wait_drq();
        if (err != CF_OK)
        {
            return err;
        }
        for (int i = 0; i < 512; i++)
        {
            cf_data = *buf++;
        }
    }

    // Wait for the card to finish writing
    return cf_wait_ready();
}

// ---------------------------------------------------------------------------
// Debug serial output
// ---------------------------------------------------------------------------

/**
 * Send one character via the GLUE hardware UART TX (115200 baud 8N1).
 * Polls UART_STATUS until not busy, then writes UART_TX_DATA.
 */
extern "C" void debug_serial_putchar(const char s);

asm(
    ".global debug_serial_putchar     \n"
    "debug_serial_putchar:            \n"
    "    move.b  7(%sp), %d0 \n"
    "    lea     .Lret_stub(%pc), %a5 \n"
    "    jmp     uart_putchar \n"
    ".Lret_stub:                   \n"
    "    rts                  \n"
);

/**
 * Bitbang one character at 9600 baud (8N1) through DEBUG_OUT.
 * Kept as a fallback for debugging the GLUE UART itself.
 */
extern "C" void debug_serial_putchar_bitbang(const char s);

asm(
    ".global debug_serial_putchar_bitbang \n"
    "debug_serial_putchar_bitbang:        \n"
    "    move.l  %d2, -(%sp) \n"
    "    move.b  11(%sp), %d0 \n"
    "    lea     .Lret_stub_bb(%pc), %a5 \n"
    "    jmp     early_putchar \n"
    ".Lret_stub_bb:                   \n"
    "    move.l  (%sp)+, %d2\n"
    "    rts                  \n"
);

extern "C" void panic(const char *s);

asm(
    ".global panic\n"
    "panic:\n"
    "    move.l 4(%sp), %a1\n"
    "    jmp monitor_panic\n"
);

extern "C" {

void debug_printf(const char *fmt, ...)
{
    va_list args;
    char dummy[512];

    va_start(args, fmt);
    vsprintf(dummy, fmt, args);
    va_end(args);

    for(const char* s = dummy; *s; s++)
    {
        debug_serial_putchar(*s);
    }
}

extern uint32_t memory_size;
extern const char *build_date;
extern const char *build_provenance;

// IO_MCU event ring buffer (written by ISR in crt0.s, read by main)
constexpr size_t IO_EVT_QUEUE_SIZE = 256;  // must match crt0.s
extern volatile uint8_t io_evt_queue[IO_EVT_QUEUE_SIZE];
extern volatile uint32_t io_evt_head;
extern volatile uint32_t io_evt_tail;
extern volatile uint8_t io_evt_overflow;

// Timer tick handler — called from ISR context at IPL 5
void timer_tick()
{
    // TODO: increment tick counter, feed watchdog, etc.
}

}; // extern "C"

static volatile uint8_t &io_mcu_tx = *reinterpret_cast<volatile uint8_t *>(Griffin::IO_MCU_TX_DATA);

static void io_mcu_putchar(uint8_t ch)
{
    io_mcu_tx = ch;
}

// Pop one byte from the event ring buffer.  Returns false if empty.
// No interrupt masking needed — head is only modified here (single consumer).
static bool evt_pop(uint8_t *out)
{
    uint32_t h = io_evt_head;
    if(h == io_evt_tail)
    {
        return false;
    }
    *out = io_evt_queue[h];
    io_evt_head = (h + 1) & (IO_EVT_QUEUE_SIZE - 1);
    return true;
}

int main()
{
    debug_printf("Firmware Build: %s, GIT %s\n", build_date, build_provenance);
    debug_printf("IO_MCU console ready\n");

    uint8_t evt;
    for (;;)
    {
        if(!evt_pop(&evt))
        {
            continue;
        }

        switch(evt)
        {
            case Griffin::IO_MCU_EVT_UART_RX:
            {
                uint8_t ch;
                if(evt_pop(&ch))
                {
                    debug_serial_putchar(ch);
                    io_mcu_putchar(ch);
                }
                break;
            }

            case Griffin::IO_MCU_EVT_KBD:
            {
                uint8_t scancode;
                if(evt_pop(&scancode))
                {
                    debug_printf("[KBD: 0x%02X]\n", scancode);
                }
                break;
            }

            case Griffin::IO_MCU_EVT_MOUSE:
            {
                uint8_t data;
                if(evt_pop(&data))
                {
                    debug_printf("[MOUSE: 0x%02X]\n", data);
                }
                break;
            }

            case Griffin::IO_MCU_EVT_TIMER:
                // Timer tick already handled in ISR; nothing to do here
                break;

            default:
                debug_printf("[IO EVT: 0x%02X]\n", evt);
                break;
        }
    }
}
