#include <string>
#include <cstdio>
#include <cstdint>
#include <cstdarg>
#include <cstring>
#include <cstdlib>

#include "../griffin.generated.h"
#include "../griffin.generated.refs.h"
// #include "splash.h"

#include "ps2.h"
#include "textport.h"
#include "vt102.h"
#include "early_log.h"
#include "splash.h"

using namespace Griffin::reg;

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

// Wait for BSY to clear.  Returns CF_TIMEOUT if limit exceeded.
static cf_error cf_wait_ready()
{
    for (uint32_t i = 0; i < CF_POLL_LIMIT; i++)
    {
        if (!(CF_STATUS & Griffin::CF_STATUS_BSY))
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
        uint8_t s = CF_STATUS;
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
    CF_SECTOR_COUNT  = count;
    CF_SECTOR_NUM    = lba & 0xFF;
    CF_CYL_LO     = (lba >> 8) & 0xFF;
    CF_CYL_HI     = (lba >> 16) & 0xFF;
    CF_DRIVE_HEAD = Griffin::CF_DH_LBA | ((lba >> 24) & 0x0F);
}

// Initialize CF card: wait for ready, set 8-bit PIO mode.
extern "C" cf_error cf_init()
{
    // Wait for BSY clear with extended power-on timeout.
    for (uint32_t i = 0; i < CF_INIT_POLL_LIMIT; i++)
    {
        if (!(CF_STATUS & Griffin::CF_STATUS_BSY))
        {
            goto bsy_clear;
        }
    }
    return CF_TIMEOUT;

bsy_clear:
    // Wait for DRDY
    for (uint32_t i = 0; i < CF_INIT_POLL_LIMIT; i++)
    {
        if (CF_STATUS & Griffin::CF_STATUS_DRDY)
        {
            goto drdy_ok;
        }
    }
    return CF_TIMEOUT;

drdy_ok:
    // Set 8-bit transfer mode
    CF_FEATURES = Griffin::CF_CMD_SET_8BIT;
    CF_COMMAND  = Griffin::CF_CMD_SET_FEATURES;

    for (uint32_t i = 0; i < CF_INIT_POLL_LIMIT; i++)
    {
        uint8_t s = CF_STATUS;
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

    CF_DRIVE_HEAD = Griffin::CF_DH_LBA;
    CF_COMMAND    = Griffin::CF_CMD_IDENTIFY;

    err = cf_wait_drq();
    if (err != CF_OK)
    {
        return err;
    }

    for (int i = 0; i < 512; i++)
    {
        buf[i] = CF_DATA;
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
    CF_COMMAND = Griffin::CF_CMD_READ_SECTORS;

    for (int sec = 0; sec < count; sec++)
    {
        err = cf_wait_drq();
        if (err != CF_OK)
        {
            return err;
        }
        for (int i = 0; i < 512; i++)
        {
            *buf++ = CF_DATA;
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
    CF_COMMAND = Griffin::CF_CMD_WRITE_SECTORS;

    for (int sec = 0; sec < count; sec++)
    {
        err = cf_wait_drq();
        if (err != CF_OK)
        {
            return err;
        }
        for (int i = 0; i < 512; i++)
        {
            CF_DATA = *buf++;
        }
    }

    // Wait for the card to finish writing
    return cf_wait_ready();
}

// ---------------------------------------------------------------------------
// Debug serial output
// ---------------------------------------------------------------------------

// Console putchar.  Routes to the 68681 DUART Channel A, which crt0's
// early init brings up at 115200 8N1 before main() runs.  Replaces the
// former GLUE timer bit-bang serial.
extern "C" void duart_putchar(uint8_t ch);   // defined below

extern "C" void debug_serial_putchar(const char s)
{
    duart_putchar(static_cast<uint8_t>(s));
}

#if 0
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
#endif

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
        // '\n' → '\r\n' so the VT102 replay and any serial-side terminal
        // both advance to column 0.
        if (*s == '\n')
        {
            early_log_push('\r');
            debug_serial_putchar('\r');
        }
        early_log_push(static_cast<uint8_t>(*s));
        debug_serial_putchar(*s);
    }
}

extern volatile uint32_t memory_size;
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

// DUART CTR_READY ISR increments this at 100 Hz (emulator path).
extern volatile uint32_t tick_counter;

// VIDEO ISR increments this on every VBLANK (~60 Hz at VGA 640x480@60).
extern volatile uint32_t video_frame_counter;

}; // extern "C"

// Milliseconds since boot.
extern "C" uint32_t get_milliseconds()
{
//     return (video_frame_counter * 1000U) / 60U; // Possible alternate timebase
   return tick_counter * 10U;
}

// ---------------------------------------------------------------------------
// 68681 DUART — Channel A console (115200 8N1)
// ---------------------------------------------------------------------------

// CRA/CRB command encodings
static constexpr uint8_t DUART_CMD_RESET_MR_PTR = 0x10;  // MC=1
static constexpr uint8_t DUART_CMD_RESET_RX     = 0x20;  // MC=2
static constexpr uint8_t DUART_CMD_RESET_TX     = 0x30;  // MC=3
static constexpr uint8_t DUART_CMD_RESET_ERR    = 0x40;  // MC=4
static constexpr uint8_t DUART_CMD_ENABLE_TXRX  = 0x05;  // EC=1, TC=1

extern "C" void duart_putchar(uint8_t ch)
{
    while (!(DUART_SRA & Griffin::DUART_SRA_TXRDY_MASK))
        ;
    DUART_TBA = ch;
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

bool duart_received_ready()
{
    return uart_rx_head != uart_rx_tail;
}

// Defined in syscalls.c — switches write()/read() to DUART backend
extern "C" void duart_console_enable();

// Runtime DUART setup: RX interrupt + 100 Hz C/T tick.  The baud rate
// and 8N1 framing (115200) are already programmed by crt0's early
// duart_early_tx_init, so this must NOT re-touch CSRA/MR or the BRG
// Extend bits (set for Ch A via CRA 0x80 (Rx) and CRA 0xA0 (Tx) — misc
// commands in the upper nibble; the CRA values used here —
// 0x20/0x40/0x05 — don't disturb them, and ACR is written with bit7=0 so
// Bit Rate Set #1 is preserved).  It only flushes RX, sets the C/T tick,
// and enables interrupts.
static void duart_runtime_init()
{
    debug_printf("DUART: runtime init (RX irq + 100 Hz tick)\n");

    // RX queue state (.monitor_data is NOLOAD, not cleared by BSS init).
    uart_rx_head = 0;
    uart_rx_tail = 0;
    uart_rx_overflow = 0;

    // Flush the RX path (baud/format left as the early init set them).
    // Do NOT reset TX — that would interrupt the live console.
    DUART_CRA = DUART_CMD_RESET_RX;
    DUART_CRA = DUART_CMD_RESET_ERR;

    // ACR: BRG set 0 (bit 7 = 0, baud unchanged), C/T = Timer mode on
    // X1/CLK direct (bits 6:4 = 110) for the system tick.
    DUART_ACR = (0x6U << Griffin::DUART_ACR_CT_MODE_SHIFT);

    // C/T preload: Timer mode fires every square-wave half-period
    // = preload input cycles.  TICK_HZ is a firmware convention.
    static constexpr uint32_t TICK_HZ = 100;
    static constexpr uint32_t TICK_PRELOAD = Griffin::DUART_CLOCK / TICK_HZ / 2;
    static_assert(TICK_PRELOAD > 0 && TICK_PRELOAD < 0x10000,
                  "DUART tick preload must fit in 16 bits");
    DUART_CTUR = (TICK_PRELOAD >> 8) & 0xFF;
    DUART_CTLR =  TICK_PRELOAD       & 0xFF;

    // RXRDYA + CTR_READY share the level-5 autovector; _duart_isr
    // distinguishes them via the ISR snapshot.
    DUART_IMR = Griffin::DUART_ISR_RXRDYA_MASK | Griffin::DUART_ISR_CTR_READY_MASK;
    DUART_IVR = 0x0;

    DUART_CRA = DUART_CMD_ENABLE_TXRX;

    // Read STARTCC to kick off the C/T (the read itself is the side effect).
    uint8_t startcc_discard = DUART_STARTCC;
    (void)startcc_discard;

    uint8_t sra = DUART_SRA;
    debug_printf("DUART: SRA=0x%02X%s\n", sra,
                 (sra & Griffin::DUART_SRA_TXRDY_MASK) ? " TXRDY" : " (no TXRDY!)");
}

[[maybe_unused]] static void dump_hex(uint32_t base_addr, const uint8_t *data, int size)
{
    int offset = 0;
    while (size > 0)
    {
        int howmany = (size < 16) ? size : 16;

        printf("  0x%06lX: ", static_cast<unsigned long>(base_addr + offset));
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
               err, CF_STATUS, CF_ERROR);
        return;
    }
    printf("CF: init OK\n");

    uint8_t id_buf[512];
    err = cf_identify(id_buf);
    if (err != CF_OK)
    {
        printf("CF: identify failed (err=%d) status=0x%02X error=0x%02X\n",
               err, CF_STATUS, CF_ERROR);
        return;
    }

    cf_info info;
    cf_parse_identify(id_buf, &info);
    printf("CF: %s, firmware %s, serial %s\n", info.model, info.firmware_rev, info.serial);
    printf("CF: sectors:  %lu, capacity: %lu KB\n",
           static_cast<unsigned long>(info.lba_sectors), static_cast<unsigned long>(info.lba_sectors / 2));

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
                   label, static_cast<unsigned>(vsn >> 16), static_cast<unsigned>(vsn & 0xFFFF));
        }
        else
        {
            printf("Volume: (no label) (S/N %04X-%04X)\n",
                   static_cast<unsigned>(vsn >> 16), static_cast<unsigned>(vsn & 0xFFFF));
        }
    }

    // Print free space
    DWORD free_clust;
    FATFS *fs_ptr;
    res = f_getfree("", &free_clust, &fs_ptr);
    if (res == FR_OK)
    {
        unsigned long free_kb = static_cast<unsigned long>(free_clust * fs_ptr->csize) / 2;
        unsigned long total_kb = static_cast<unsigned long>((fs_ptr->n_fatent - 2) * fs_ptr->csize) / 2;
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
                   static_cast<unsigned long>(fno.fsize), fno.fname);
        }
        f_closedir(&dir);
    }
}

// NOTE: play_audio() (GLUE timer + AUDIO_DAC sample pacing) was removed
// with the GLUE timer.  Deterministic audio sample timing now needs a
// different source (e.g. the DUART C/T or a future DMA path).

// Pop one byte from the event ring buffer.  Returns false if empty.
// No interrupt masking needed — head is only modified here (single consumer).
[[maybe_unused]] static bool evt_pop(uint8_t *out)
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

#if 0
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
            buf[pos++] = static_cast<char>(ch);
            debug_serial_putchar(static_cast<char>(ch));
        }
    }
}

[[maybe_unused]] static void debug_monitor()
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
                debug_printf("%06lX:", static_cast<unsigned long>(addr + off));
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
                debug_printf("%06lX <- %02X\n", static_cast<unsigned long>(addr), val & 0xFF);
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

#endif

// ---------------------------------------------------------------------------
// Video framebuffer — palette-and-pixels layout, start ENGINE DMA, enable scanout
//
// Each scanline in memory is a 4-byte header (word 0 = {fg, bg} R3G3B2
// palette, word 1 reserved) followed by 80 pixel bytes.  ENGINE streams the
// header through the FIFO so VIDEO latches the per-line palette in-band — no
// CPU palette register, no per-line timing.  The stride is a multiple of 4 so
// the textport's long-word framebuffer movers stay valid.
// ---------------------------------------------------------------------------

static constexpr uint32_t FB_ADDR         = 0x0F0000;
static constexpr uint8_t  FB_PAGE         = 0x0F;
static constexpr unsigned FB_LINES        = 480;
static constexpr unsigned FB_PIXEL_BYTES  = Griffin::VIDEO_PIXEL_BYTES_PER_LINE; // 80
static constexpr unsigned FB_PIXEL_OFFSET = Griffin::VIDEO_LINE_PIXEL_OFFSET;    // 4
static constexpr unsigned FB_STRIDE       = Griffin::VIDEO_LINE_STRIDE_BYTES;    // 84
static constexpr uint16_t FB_DEFAULT_PALETTE = 0xFF00;  // fg=white, bg=black

// ---------------------------------------------------------------------------
// Per-line-palette image viewer
//
// Loads a 640x480 1bpp image and its companion 480-word per-line palette set,
// interleaving them into the palette-and-pixels framebuffer: each scanline's
// palette word lands in the header (bytes 0..1) and the 80 pixel bytes follow
// at FB_PIXEL_OFFSET.  ENGINE + VIDEO then display it with per-line palettes
// automatically (no CPU palette timing), so this just lays out the framebuffer
// and returns once a PS/2 key is pressed.
// ---------------------------------------------------------------------------
static void view_image(const char *image_path, const char *palette_path)
{
    // Per-line palette set: 480 words, D[15:8]=fg, D[7:0]=bg, each byte R3G3B2.
    // malloc, not new[]: the throwing array-new drags in libstdc++ exception
    // machinery and overflows ROM; newlib malloc/free keeps the image lean.
    constexpr size_t PAL_BYTES = FB_LINES * sizeof(uint16_t);
    uint16_t *palettes = static_cast<uint16_t *>(malloc(PAL_BYTES));
    if (!palettes)
    {
        printf("view_image: out of memory for palette set\n");
        return;
    }
    FILE *pal = fopen(palette_path, "rb");
    if (!pal)
    {
        printf("view_image: cannot open %s\n", palette_path);
        free(palettes);
        return;
    }
    size_t pal_got = fread(palettes, 1, PAL_BYTES, pal);
    fclose(pal);
    if (pal_got != PAL_BYTES)
    {
        printf("view_image: %s short read (%lu of %lu bytes)\n", palette_path,
               static_cast<unsigned long>(pal_got),
               static_cast<unsigned long>(PAL_BYTES));
        free(palettes);
        return;
    }

    FILE *img = fopen(image_path, "rb");
    if (!img)
    {
        printf("view_image: cannot open %s\n", image_path);
        free(palettes);
        return;
    }

    // Interleave into the strided framebuffer one scanline at a time: palette
    // word into the header, then the line's 80 pixel bytes after the header.
    bool ok = true;
    for (unsigned line = 0; line < FB_LINES; line++)
    {
        uint8_t *lp = reinterpret_cast<uint8_t *>(FB_ADDR + line * FB_STRIDE);
        *reinterpret_cast<uint16_t *>(lp) = palettes[line];
        size_t got = fread(lp + FB_PIXEL_OFFSET, 1, FB_PIXEL_BYTES, img);
        if (got != FB_PIXEL_BYTES)
        {
            ok = false;
            break;
        }
    }
    fclose(img);
    free(palettes);
    if (!ok)
    {
        printf("view_image: %s short read\n", image_path);
        return;
    }

    // The image now scans out with its per-line palettes in-band — no CPU work.
    // Block until a PS/2 key arrives (interrupts stay enabled), then return.
    while (!ps2_received_ready())
    {
        // idle; ENGINE/VIDEO refresh the screen from the framebuffer
    }
    (void)ps2_getchar();
}

// ---------------------------------------------------------------------------
// Textport demo: drive an 80x30 VT102-compatible textport on the framebuffer
// ---------------------------------------------------------------------------

namespace gtxt = griffin::textport;
namespace griffin::textport {
    extern const FontRenderer font_8x16_renderer;
    extern const FontRenderer font_8x8_renderer;
    extern const FontRenderer font_6x10_renderer;
    extern const uint8_t font_8x16_bits[256 * 16];
}

// ---------------------------------------------------------------------------
// Bitmap-text smoke test: route lorem-ipsum through Vt102Parser::put,
// using only printable ASCII (no ESC, no control chars, no UTF-8).  This
// exercises only the parser's S::Normal printable fast path, which is a
// one-line delegation to Textport::put_glyph.
// Cursor blink is intentionally NOT ticked.
// ---------------------------------------------------------------------------
[[maybe_unused]] static void bitmap_text_test()
{
    static const char lorem[] =
        "Lorem ipsum dolor sit amet, consectetur adipiscing elit, sed do "
        "eiusmod tempor incididunt ut labore et dolore magna aliqua. Ut "
        "enim ad minim veniam, quis nostrud exercitation ullamco laboris "
        "nisi ut aliquip ex ea commodo consequat. Duis aute irure dolor "
        "in reprehenderit in voluptate velit esse cillum dolore eu fugiat "
        "nulla pariatur. Excepteur sint occaecat cupidatat non proident, "
        "sunt in culpa qui officia deserunt mollit anim id est laborum.";

    gtxt::g_textport.configure(
        reinterpret_cast<uint8_t*>(FB_ADDR),
        FB_STRIDE,                        // per-line stride (header + 80 px)
        &gtxt::font_8x16_renderer,
        80U, 24U,
        FB_PIXEL_OFFSET, FB_DEFAULT_PALETTE);
    gtxt::g_vt102.reset();

    for (int rep = 0; rep < 10; ++rep)
    {
        for (const char* s = lorem; *s; ++s)
        {
            gtxt::g_vt102.put(static_cast<uint8_t>(*s));
        }
    }
}

// Caller for the VT102 parser when it needs to send a reply (e.g. cursor
// position report) — routes to the DUART so the host sees it.
extern "C" void textport_uart_responder(const char* s, size_t n)
{
    for (size_t i = 0; i < n; ++i)
    {
        duart_putchar(static_cast<uint8_t>(s[i]));
    }
}

// syscalls.c hook — flips the textport-enabled flag in the console tee.
extern "C" void textport_console_set_enabled(int on);

// Bring up the on-screen console.  After this returns, printf goes to
// DUART AND the textport, the early-log ring is drained onto the textport,
// and the splash (if any) sits at the top of the screen waiting to scroll
// off as text fills.
static void textport_console_enable()
{
    constexpr unsigned CONSOLE_COLS   = 80;
    constexpr unsigned CONSOLE_ROWS   = 30;

    const auto& fr = gtxt::font_8x16_renderer;
    const unsigned font_h = fr.font->height;

    gtxt::g_vt102.set_responder(&textport_uart_responder);
    gtxt::g_textport.configure(
        reinterpret_cast<uint8_t*>(FB_ADDR),
        FB_STRIDE,
        &fr,
        CONSOLE_COLS, CONSOLE_ROWS,
        FB_PIXEL_OFFSET, FB_DEFAULT_PALETTE);
    gtxt::g_vt102.reset();

    // Splash sits at the top of the FB; the Textport's char buffer for
    // those rows is still ' ', so the cursor avoids them until scrolling
    // evicts them.  Blit into the pixel region (past the per-line header).
    splash_blit_topleft(reinterpret_cast<uint8_t*>(FB_ADDR + FB_PIXEL_OFFSET), FB_STRIDE);
    const unsigned splash_rows = splash_rows_for_font_height(font_h);
    gtxt::g_textport.move_to(0, static_cast<int>(splash_rows));

    // Replay everything we captured since boot.  Each byte goes straight
    // into the VT102 parser — the syscalls tee has NOT been told the
    // textport is up yet, so we don't double-emit to DUART.
    early_log_replay(&gtxt::textport_vt102_putchar);
    early_log_freeze();

    // Re-emit info that crt0 sent only via duart_puts (and so isn't in
    // the ring) plus what's now interesting on the visible console.
    textport_console_set_enabled(1);
    if (uint32_t dropped = early_log_dropped_count(); dropped)
    {
        printf("[early-log: %lu bytes dropped to overflow]\n",
               static_cast<unsigned long>(dropped));
    }
    printf("Memory: %ld KB\n", memory_size);
}

static void video_test_init()
{
    debug_printf("VIDEO: palette-and-pixels framebuffer at 0x%06lX (%u-byte stride)\n",
                 static_cast<unsigned long>(FB_ADDR),
                 static_cast<unsigned>(FB_STRIDE));

    // The framebuffer is laid out (palette headers + pixels) by whoever draws
    // into it next — textport_console_enable() clears it immediately after.
    ENGINE_SOURCE_PAGE = FB_PAGE;
    ENGINE_CTRL = Griffin::ENGINE_CTRL_DMA_EN_MASK;
    printf("ENGINE: DMA enabled, page=0x%02X\n", FB_PAGE);

    VIDEO_CLRERR = 0;           // clear any stale FIFO_ERROR
    VIDEO_CTRL = Griffin::VIDEO_CTRL_ENABLE_MASK;

    printf("VIDEO: enabled (CTRL_RB=0x%02X)\n",
                 static_cast<unsigned>(VIDEO_CTRL_RB));
}

int main()
{
    debug_printf("Firmware Build: %s, GIT %s\n", build_date, build_provenance);

    // debug_monitor();

    // The DUART Channel A console was already brought up at 115200 8N1
    // by crt0's early init, so debug_printf above has been going out the
    // DUART all along.  Here we add the RX interrupt + 100 Hz tick and
    // switch the C library console (printf/read) to the DUART backend.
    duart_runtime_init();

    for(auto c: "DUART TX\n")
    {
        if(c) duart_putchar(c);
    }
    duart_console_enable();
    printf("Console on DUART Channel A, 115200 8N1\n");

    video_test_init();

    // FB is now scanning out a checkerboard; bring up the on-screen
    // console (splash + ring replay + memory size).  Everything below
    // this point appears on both DUART and the textport.
    textport_console_enable();

    // Play startup sound
    // uint32_t audio_len = _binary_startup_raw_end - _binary_startup_raw_start;
    // play_audio(_binary_startup_raw_start, audio_len, 11025);

    cf_mount_and_list();

    // Per-line palette image viewer.  Names hardcoded for now; runs until a
    // PS/2 key is pressed, then falls through to the normal input loop.
    view_image("oops.bin", "oops-palette.bin");

    printf("Input check loop...\n");

    uint32_t last_clock_print_ms = get_milliseconds();
    for (;;)
    {
        gtxt::g_textport.cursor_blink_tick();

        if(duart_received_ready())
        {
            unsigned char ch;
            long result = read(0, &ch, 1);
            if(result == 1)
            {
                printf("received: 0x%02X '%c'\n", ch,
                             (ch >= 0x20 && ch < 0x7F) ? ch : '.');
            }
        }

        uint16_t err_data = ps2_get_err_data();
        auto err_flags = ps2_get_err_flags();
        if(err_flags)
        {
            printf("ps2 err: 0x%02X (%s%s%s) data=0x%04X\n",
                err_flags,
                (err_flags & PS2_ERROR_FRAMING) ? "framing " : "",
                (err_flags & PS2_ERROR_PARITY) ? "parity " : "",
                (err_flags & PS2_ERROR_OVERRUN) ? "overrun " : "",
                err_data);
        }

        if(ps2_received_ready())
        {
            uint8_t byte = ps2_getchar();
            printf("ps2: 0x%02X\n", byte);
            if(byte == 0xAA)
            {
                printf("ps2: BAT OK, sending 0xED\n");
                ps2_send_byte(0xED);
                printf("ps2: BAT OK, sending 0x00\n");
                ps2_send_byte(0x00);
                printf("ps2: BAT OK, sent 0xED, enqueued 0x00\n");
            }
        }

        uint32_t now_ms = get_milliseconds();
        if(now_ms >= last_clock_print_ms + 1000)
        {
            uint32_t seconds = now_ms / 1000;
            uint32_t ss = seconds % 60;
            uint32_t mm = (seconds / 60) % 60;
            uint32_t hh = seconds / 3600;
            printf("%02ld:%02ld:%02ld\n", hh, mm, ss);
            last_clock_print_ms = now_ms;
        }
    }
}
