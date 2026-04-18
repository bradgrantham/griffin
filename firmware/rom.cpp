#include <string>
#include <cstdio>
#include <cstdint>
#include <cstdarg>
#include <cstring>
#include <cstdlib>

#include "../griffin.generated.h"
#include "splash.h"

extern "C" {
#include "ff.h"
}


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

// Power-on poll limit for cf_init.  CF spec allows up to 31 seconds
// for BSY to clear after power-on; ~2 seconds covers most cards.
// Each iteration is ~50 SYSCLK; scale the count from SYSCLK_HZ so the
// wall-clock duration stays constant if the system clock changes.
static constexpr uint32_t CF_INIT_POLL_LIMIT = (Griffin::SYSCLK_HZ / 50) * 2;

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

// ---------------------------------------------------------------------------
// 68681 DUART — register access
// ---------------------------------------------------------------------------

static volatile uint8_t &duart_mra     = *reinterpret_cast<volatile uint8_t *>(Griffin::DUART_MR1A);
static volatile uint8_t &duart_sra     = *reinterpret_cast<volatile uint8_t *>(Griffin::DUART_SRA);
static volatile uint8_t &duart_csra    = *reinterpret_cast<volatile uint8_t *>(Griffin::DUART_CSRA);
static volatile uint8_t &duart_cra     = *reinterpret_cast<volatile uint8_t *>(Griffin::DUART_CRA);
static volatile uint8_t &duart_rba     = *reinterpret_cast<volatile uint8_t *>(Griffin::DUART_RBA);
static volatile uint8_t &duart_tba     = *reinterpret_cast<volatile uint8_t *>(Griffin::DUART_TBA);
static volatile uint8_t &duart_acr     = *reinterpret_cast<volatile uint8_t *>(Griffin::DUART_ACR);
static volatile uint8_t &duart_imr     = *reinterpret_cast<volatile uint8_t *>(Griffin::DUART_IMR);
static volatile uint8_t &duart_ctur    = *reinterpret_cast<volatile uint8_t *>(Griffin::DUART_CTUR);
static volatile uint8_t &duart_ctlr    = *reinterpret_cast<volatile uint8_t *>(Griffin::DUART_CTLR);
static volatile uint8_t &duart_startcc = *reinterpret_cast<volatile uint8_t *>(Griffin::DUART_STARTCC);
static volatile uint8_t &duart_ivr     = *reinterpret_cast<volatile uint8_t *>(Griffin::DUART_IVR);

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
extern "C" cf_error cf_init()
{
    // Wait for BSY clear with extended power-on timeout.
    for (uint32_t i = 0; i < CF_INIT_POLL_LIMIT; i++)
    {
        if (!(cf_status & Griffin::CF_STATUS_BSY))
        {
            goto bsy_clear;
        }
    }
    return CF_TIMEOUT;

bsy_clear:
    // Wait for DRDY
    for (uint32_t i = 0; i < CF_INIT_POLL_LIMIT; i++)
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

    for (uint32_t i = 0; i < CF_INIT_POLL_LIMIT; i++)
    {
        uint8_t s = cf_status;
        if (s & Griffin::CF_STATUS_ERR)
        {
            return CF_ERR;
        }
        if (!(s & Griffin::CF_STATUS_BSY))
        {
            return CF_OK;
        }
    }
    return CF_TIMEOUT;
}

// Read the 512-byte IDENTIFY DEVICE block into caller-provided buffer.
extern "C" cf_error cf_identify(uint8_t buf[512])
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
extern "C" cf_error cf_read_sectors(uint32_t lba, uint8_t count, uint8_t *buf)
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
extern "C" cf_error cf_write_sectors(uint32_t lba, uint8_t count, const uint8_t *buf)
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
 * Send one character at 115200 baud (8N1) via GLUE timer + DEBUG_OUT.
 * Uses the GLUE_TIMER ARM mechanism for precise bit timing.
 */
extern "C" void debug_serial_putchar(const char s);

asm(
    ".global debug_serial_putchar     \n"
    "debug_serial_putchar:            \n"
    "    move.b  7(%sp), %d0 \n"
    "    lea     .Lret_stub(%pc), %a5 \n"
    "    jmp     timer_putchar \n"
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

/**
 * Bit-bang receive one byte at 115200 via DEBUG_IN + GLUE timer.
 * Returns 0–255 on success, -1 on timeout (~1ms).
 */
extern "C" int debug_getchar(void);

asm(
    ".global debug_getchar       \n"
    "debug_getchar:              \n"
    "    move.l  %d2, -(%sp)     \n"
    "    lea     .Lret_gc(%pc), %a5 \n"
    "    jmp     debug_getchar_asm \n"
    ".Lret_gc:                   \n"
    "    move.l  (%sp)+, %d2     \n"
    "    rts                     \n"
);

extern "C" void panic(const char *s);

asm(
    ".global panic\n"
    "panic:\n"
    "    move.l 4(%sp), %a1\n"
    "    jmp monitor_panic\n"
);

// GLUE CONFIG shadow access — defined in crt0.s
extern "C" void glue_config_set_bits(uint8_t mask);
extern "C" void glue_config_clear_bits(uint8_t mask);

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

// event ring buffer (written by ISR in crt0.s, read by main)
constexpr size_t EVT_QUEUE_SIZE = 256;  // must match crt0.s
extern volatile uint8_t evt_queue[EVT_QUEUE_SIZE];
extern volatile uint32_t evt_head;
extern volatile uint32_t evt_tail;
extern volatile uint8_t evt_overflow;

// UART RX ring buffer (written by _duart_isr in crt0.s, read by duart_getchar)
constexpr size_t UART_RX_QUEUE_SIZE = 256;  // must match crt0.s
extern volatile uint8_t uart_rx_queue[UART_RX_QUEUE_SIZE];
extern volatile uint32_t uart_rx_head;
extern volatile uint32_t uart_rx_tail;
extern volatile uint8_t uart_rx_overflow;

// Timer tick handler — called from ISR context at IPL 5
void timer_tick()
{
    // TODO: increment tick counter, feed watchdog, etc.
}

}; // extern "C"

// ---------------------------------------------------------------------------
// 68681 DUART — Channel A console (38400 8N1)
// ---------------------------------------------------------------------------

// CRA/CRB command encodings
static constexpr uint8_t DUART_CMD_RESET_MR_PTR = 0x10;  // MC=1
static constexpr uint8_t DUART_CMD_RESET_RX     = 0x20;  // MC=2
static constexpr uint8_t DUART_CMD_RESET_TX     = 0x30;  // MC=3
static constexpr uint8_t DUART_CMD_RESET_ERR    = 0x40;  // MC=4
static constexpr uint8_t DUART_CMD_ENABLE_TXRX  = 0x05;  // EC=1, TC=1

extern "C" void duart_putchar(uint8_t ch)
{
    while (!(duart_sra & Griffin::DUART_SRA_TXRDY_MASK))
        ;
    duart_tba = ch;
}

extern "C" uint8_t duart_getchar()
{
    // Spin until the ISR has deposited a byte.  Head is only advanced
    // here (single consumer); tail is only advanced by the ISR.  On
    // the 68000 an aligned long-word read is atomic, so no masking
    // of interrupts is required around these indices.
    while (uart_rx_head == uart_rx_tail)
        ;
    uint8_t ch = uart_rx_queue[uart_rx_head];
    uart_rx_head = (uart_rx_head + 1) & (UART_RX_QUEUE_SIZE - 1);
    return ch;
}

// Defined in syscalls.c — switches write()/read() to DUART backend
extern "C" void duart_console_enable();

static void duart_init()
{
    debug_printf("DUART: init\n");

    // Initialize RX queue state before enabling any DUART interrupts.
    // .monitor_data is NOLOAD and not cleared by the BSS init loop.
    uart_rx_head = 0;
    uart_rx_tail = 0;
    uart_rx_overflow = 0;

    // ---- MC68681 Init: Channel A, 38400 8N1, RxRDY interrupt ----

    // Reset Channel A
    duart_cra = DUART_CMD_RESET_RX;
    duart_cra = DUART_CMD_RESET_TX;
    duart_cra = DUART_CMD_RESET_MR_PTR;

    // MR1A: 8 data bits, no parity
    duart_mra = 0x13;
    // MR2A: 1 stop bit, normal mode (MR pointer auto-advanced)
    duart_mra = 0x07;

    // ACR: BRG set 0, timer mode irrelevant (bits 6:4 = 000), no IP change int
    duart_acr = 0x00;

    // CSRA: Tx and Rx both = BRG 38400 (code 1100 = 0xC)
    duart_csra = 0xcc;

    // Enable RXRDYA interrupt — ISR drains bytes into uart_rx_queue.
    // TX stays polled; no TXRDYA interrupt needed.
    duart_imr = Griffin::DUART_ISR_RXRDYA_MASK;

    // Enable TX and RX
    duart_cra = DUART_CMD_ENABLE_TXRX;

    // Report status via bit-bang debug path
    uint8_t sra = duart_sra;
    debug_printf("DUART: SRA=0x%02X", sra);
    if (sra & Griffin::DUART_SRA_TXRDY_MASK)
    {
        debug_printf(" TXRDY");
    }
    if (sra & Griffin::DUART_SRA_TXEMT_MASK)
    {
        debug_printf(" TXEMT");
    }
    debug_printf("\n");

    if (!(sra & Griffin::DUART_SRA_TXRDY_MASK))
    {
        debug_printf("DUART: WARNING — TXRDY not set after init\n");
    }
}

static void dump_hex(uint32_t base_addr, const uint8_t *data, int size)
{
    int offset = 0;
    while (size > 0)
    {
        int howmany = (size < 16) ? size : 16;

        printf("  0x%06lX: ", (unsigned long)(base_addr + offset));
        for (int i = 0; i < howmany; i++)
        {
            printf("%02X ", data[i]);
        }
        printf("\n");

        printf("            ");
        for (int i = 0; i < howmany; i++)
        {
            char c = data[i];
            printf(" %c ", (c >= 0x20 && c <= 0x7E) ? c : '.');
        }
        printf("\n");

        size -= howmany;
        data += howmany;
        offset += howmany;
    }
}

static FATFS fatfs;

static void cf_mount_and_list()
{
    cf_error err = cf_init();
    if (err != CF_OK)
    {
        printf("CF: init failed (err=%d) status=0x%02X error=0x%02X\n",
               err, cf_status, cf_error_reg);
        return;
    }
    printf("CF: init OK\n");

    uint8_t id_buf[512];
    err = cf_identify(id_buf);
    if (err != CF_OK)
    {
        printf("CF: identify failed (err=%d) status=0x%02X error=0x%02X\n",
               err, cf_status, cf_error_reg);
        return;
    }

    cf_info info;
    cf_parse_identify(id_buf, &info);
    printf("CF: %s, firmware %s, serial %s\n", info.model, info.firmware_rev, info.serial);
    printf("CF: sectors:  %lu, capacity: %lu KB\n",
           (unsigned long)info.lba_sectors, (unsigned long)(info.lba_sectors / 2));

    // Mount filesystem
    FRESULT res = f_mount(&fatfs, "", 1);
    if (res != FR_OK)
    {
        printf("CF: mount failed (FatFS err=%d)\n", res);
        return;
    }
    printf("CF: filesystem mounted\n");

    // Print volume label
    char label[12];
    DWORD vsn;
    res = f_getlabel("", label, &vsn);
    if (res == FR_OK)
    {
        if (label[0])
        {
            printf("Volume: %s (S/N %04X-%04X)\n",
                   label, (unsigned)(vsn >> 16), (unsigned)(vsn & 0xFFFF));
        }
        else
        {
            printf("Volume: (no label) (S/N %04X-%04X)\n",
                   (unsigned)(vsn >> 16), (unsigned)(vsn & 0xFFFF));
        }
    }

    // Print free space
    DWORD free_clust;
    FATFS *fs_ptr;
    res = f_getfree("", &free_clust, &fs_ptr);
    if (res == FR_OK)
    {
        unsigned long free_kb = (unsigned long)(free_clust * fs_ptr->csize) / 2;
        unsigned long total_kb = (unsigned long)((fs_ptr->n_fatent - 2) * fs_ptr->csize) / 2;
        printf("  %lu KB free / %lu KB total\n", free_kb, total_kb);
    }

    // List root directory
    DIR dir;
    FILINFO fno;
    res = f_opendir(&dir, "/");
    if (res == FR_OK)
    {
        printf("Root directory:\n");
        for (;;)
        {
            res = f_readdir(&dir, &fno);
            if (res != FR_OK || fno.fname[0] == '\0')
            {
                break;
            }
            printf("  %c %7lu  %s\n",
                   (fno.fattrib & AM_DIR) ? 'd' : '-',
                   (unsigned long)fno.fsize, fno.fname);
        }
        f_closedir(&dir);
    }
}

// ---------------------------------------------------------------------------
// Audio playback via GLUE timer + AUDIO_DAC
// ---------------------------------------------------------------------------

// Embedded startup sound (linked from startup_audio.o)
extern "C" const int8_t _binary_startup_raw_start[];
extern "C" const int8_t _binary_startup_raw_end[];

// Stream signed 8-bit samples to the DAC at the given sample rate.
// Uses the GLUE auto-reload timer to get deterministic sample timing:
// each sample period is broken into 'arms' timer stalls of (period+1)*8
// SYSCLK clocks each.  The stall absorbs instruction overhead.
static void play_audio(const int8_t *buf, uint32_t len, uint32_t sample_rate)
{
    // Find best timer period (1-31) and arm count to match sample_rate.
    uint32_t target = Griffin::SYSCLK_HZ / sample_rate;
    uint8_t best_period = 1;
    uint16_t best_arms = 1;
    uint32_t best_error = UINT32_MAX;

    for (uint8_t n = 1; n <= 31; n++)
    {
        uint32_t tick = (static_cast<uint32_t>(n) + 1) * 8;
        uint16_t arms = (target + tick / 2) / tick;
        if (arms < 1)
        {
            arms = 1;
        }
        uint32_t actual = arms * tick;
        uint32_t err = (actual > target) ? actual - target : target - actual;
        if (err < best_error)
        {
            best_error = err;
            best_period = n;
            best_arms = arms;
        }
    }

    volatile uint8_t &timer_reg = *reinterpret_cast<volatile uint8_t *>(Griffin::GLUE_TIMER);
    volatile uint8_t &timer_arm = *reinterpret_cast<volatile uint8_t *>(Griffin::GLUE_TIMER_ARM);
    volatile uint8_t &dac       = *reinterpret_cast<volatile uint8_t *>(Griffin::AUDIO_DAC);

    timer_reg = best_period;

    // Inner loop in inline asm for tightness.
    // Per sample: write (sample + 128) to DAC, then arm the timer best_arms times.
    // The timer stall freezes the CPU each arm, giving deterministic timing.
    //
    // Local register variables force GCC to allocate buf/count/arms_m1 to
    // specific registers — the asm body references them by name, so they
    // must not be relegated to arbitrary regs by the constraint allocator.
    register const int8_t *buf_ptr asm("a0") = buf;
    register uint16_t count asm("d3") = static_cast<uint16_t>(len - 1);
    register uint16_t arms_m1 asm("d2") = best_arms - 1;

    asm volatile (
        "    bra.s   2f                   \n"  // enter loop
        "1:                               \n"  // .sample_loop
        "    move.b  (%%a0)+, %%d0        \n"  // load signed sample
        "    addi.b  #0x80, %%d0          \n"  // signed -> unsigned
        "    move.b  %%d0, (%[dac])       \n"  // write DAC
        "    move.w  %%d2, %%d1           \n"  // arm counter
        "3:                               \n"  // .arm_loop
        "    move.b  %%d0, (%[arm])       \n"  // arm (CPU stalls)
        "    dbra    %%d1, 3b             \n"
        "2:                               \n"  // .test
        "    dbra    %%d3, 1b             \n"
        : "+a" (buf_ptr), "+d" (count)
        : [dac] "a" (&dac),
          [arm] "a" (&timer_arm),
          "d" (arms_m1)
        : "d0", "d1", "memory"
    );

    timer_reg = 0;
    dac = 0x80;  // silence (center)
}

// Pop one byte from the event ring buffer.  Returns false if empty.
// No interrupt masking needed — head is only modified here (single consumer).
static bool evt_pop(uint8_t *out)
{
    uint32_t h = evt_head;
    if(h == evt_tail)
    {
        return false;
    }
    *out = evt_queue[h];
    evt_head = (h + 1) & (EVT_QUEUE_SIZE - 1);
    return true;
}

extern "C" {
extern long read(int file, void *__buf, size_t len);
};

// ---------------------------------------------------------------------------
// Debug monitor — interactive memory read/write via bitbang serial.
// Runs before DUART init so the DUART can be poked from here.
// ---------------------------------------------------------------------------

static uint32_t parse_hex(const char *s, const char **end)
{
    uint32_t val = 0;
    while (*s)
    {
        char c = *s;
        if (c >= '0' && c <= '9')
        {
            val = (val << 4) | (c - '0');
        }
        else if (c >= 'a' && c <= 'f')
        {
            val = (val << 4) | (c - 'a' + 10);
        }
        else if (c >= 'A' && c <= 'F')
        {
            val = (val << 4) | (c - 'A' + 10);
        }
        else
        {
            break;
        }
        s++;
    }
    if (end)
    {
        *end = s;
    }
    return val;
}

static const char *skip_spaces(const char *s)
{
    while (*s == ' ')
    {
        s++;
    }
    return s;
}

static int debug_getline(char *buf, int maxlen)
{
    int pos = 0;
    for (;;)
    {
        int ch = debug_getchar();
        if (ch < 0)
        {
            continue;
        }
        if (ch == '\r' || ch == '\n')
        {
            debug_serial_putchar('\r');
            debug_serial_putchar('\n');
            buf[pos] = '\0';
            return pos;
        }
        if (ch == 0x7F || ch == 0x08)
        {
            if (pos > 0)
            {
                pos--;
                debug_serial_putchar('\b');
                debug_serial_putchar(' ');
                debug_serial_putchar('\b');
            }
        }
        else if (pos < maxlen - 1)
        {
            buf[pos++] = ch;
            debug_serial_putchar(ch);
        }
    }
}

static void debug_monitor()
{
    debug_printf("Monitor: r ADDR [LEN] | w ADDR VAL | q\n");

    char line[80];

    for (;;)
    {
        debug_printf("> ");
        debug_getline(line, sizeof(line));

        const char *p = skip_spaces(line);
        char cmd = *p;

        if (cmd == '\0')
        {
            continue;
        }

        if (cmd == 'q' || cmd == 'Q')
        {
            debug_printf("Continuing boot...\n");
            return;
        }

        if (cmd == 'r' || cmd == 'R')
        {
            p = skip_spaces(p + 1);
            const char *end;
            uint32_t addr = parse_hex(p, &end);
            p = skip_spaces(end);
            uint32_t len = 1;
            if (*p)
            {
                len = parse_hex(p, &end);
            }
            if (len == 0)
            {
                len = 1;
            }
            if (len > 256)
            {
                len = 256;
            }

            for (uint32_t off = 0; off < len; off += 16)
            {
                uint32_t row = (len - off < 16) ? (len - off) : 16;
                debug_printf("%06lX:", (unsigned long)(addr + off));
                for (uint32_t i = 0; i < row; i++)
                {
                    uint8_t val = *reinterpret_cast<volatile uint8_t *>(addr + off + i);
                    debug_printf(" %02X", val);
                }
                debug_printf("\n");
            }
        }
        else if (cmd == 'w' || cmd == 'W')
        {
            p = skip_spaces(p + 1);
            const char *end;
            uint32_t addr = parse_hex(p, &end);
            p = skip_spaces(end);
            if (*p)
            {
                uint32_t val = parse_hex(p, &end);
                *reinterpret_cast<volatile uint8_t *>(addr) = static_cast<uint8_t>(val);
                debug_printf("%06lX <- %02X\n", (unsigned long)addr, val & 0xFF);
            }
            else
            {
                debug_printf("usage: w ADDR VAL\n");
            }
        }
        else if (cmd == 'h' || cmd == 'H' || cmd == '?')
        {
            debug_printf("r ADDR [LEN] - read bytes (LEN default 1, max 256)\n");
            debug_printf("w ADDR VAL   - write byte\n");
            debug_printf("q            - quit monitor, continue boot\n");
        }
        else
        {
            debug_printf("?\n");
        }
    }
}

int main()
{
    debug_printf("Firmware Build: %s, GIT %s\n", build_date, build_provenance);

    // debug_monitor();

    // Initialize 68681 DUART and switch console output from bit-bang to DUART.
    // Everything before this point prints via debug_printf (GLUE bit-bang).
    // Everything after prints via printf (DUART Channel A, 38400 8N1).
    duart_init();
    for(auto c: "DUART TX\n")
    {
        if(c) duart_putchar(c);
    }
    duart_console_enable();
    printf("Console on DUART Channel A, 38400 8N1\n");

    volatile uint8_t &dac       = *reinterpret_cast<volatile uint8_t *>(Griffin::AUDIO_DAC);

    // Play startup sound
    // uint32_t audio_len = _binary_startup_raw_end - _binary_startup_raw_start;
    // play_audio(_binary_startup_raw_start, audio_len, 11025);

    cf_mount_and_list();

    extern uint32_t video_counter;

    printf("Input check loop...\n");
    uint32_t last_video_print = video_counter;
    for (;;)
    {
        unsigned char ch;
        long result = read(0, &ch, 1);
        if(result == 1)
        {
            printf("received: 0x%02X '%c'\n", ch,
                         (ch >= 0x20 && ch < 0x7F) ? ch : '.');
        }
        if(video_counter >= last_video_print + 60)
        {
            uint32_t seconds = video_counter * 100 / 5994;
            uint32_t ss = seconds % 60;
            uint32_t mm = (seconds / 60) % 60;
            uint32_t hh = seconds / 3600;
            printf("%02d:%02d:%02d\n");
            last_video_print = video_counter;
        }
    }
}
