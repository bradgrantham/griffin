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
#include "mouse.h"
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
    // 16-bit True IDE PIO is the power-on default; no SET FEATURES step.
    return CF_OK;
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

    for (int i = 0; i < 512; i += 2)
    {
        uint16_t w = CF_DATA;
        buf[i]     = static_cast<uint8_t>(w & 0xFF);
        buf[i + 1] = static_cast<uint8_t>(w >> 8);
    }
    return CF_OK;
}

// Extract an ATA string from the identify block.
// ATA strings store first char of each word-pair in the high byte, but the
// driver stores each 16-bit data word low byte first, so adjacent bytes are
// swapped.
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

    // Words 60-61: total LBA sectors (little-endian words, stored low byte first)
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
        for (int i = 0; i < 256; i++)
        {
            uint16_t w = CF_DATA;
            *buf++ = static_cast<uint8_t>(w & 0xFF);
            *buf++ = static_cast<uint8_t>(w >> 8);
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
        for (int i = 0; i < 256; i++)
        {
            uint16_t lo = *buf++;
            uint16_t hi = *buf++;
            CF_DATA = static_cast<uint16_t>(lo | (hi << 8));
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

// Wall clock.  There is no wired RTC yet, so the epoch at power-on defaults to
// the firmware build time (BUILD_EPOCH, stamped by the Makefile).  It is a
// variable so a monitor command, a syscall, or a future DS3231 driver can set
// it once at boot without touching anything that reads the clock.
extern "C" { uint32_t boot_epoch = BUILD_EPOCH; }

// Seconds since the Unix epoch.
extern "C" uint32_t get_epoch_seconds()
{
    return boot_epoch + get_milliseconds() / 1000U;
}

extern "C" void delay_milliseconds(uint32_t millis)
{
    uint32_t then = get_milliseconds();
    while(get_milliseconds() - then < millis);
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

extern "C" bool duart_received_ready()
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

// List one directory of the mounted filesystem.  Shared by the boot-time
// CF report and the monitor's "ls" command.
static void list_directory(const char *path)
{
    DIR dir;
    FILINFO fno;
    FRESULT res = f_opendir(&dir, path);
    if (res != FR_OK)
    {
        printf("ls: cannot open %s (FatFS err %d)\n", path, res);
        return;
    }
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

    printf("Root directory:\n");
    list_directory("/");
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

// Parse an ASCII hex string; if end is non-null it receives a pointer to the
// first non-hex character.  Used by the monitor's ADDR/VAL/LEN arguments.
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

// ---------------------------------------------------------------------------
// Video framebuffer — palette-and-pixels layout, start ENGINE DMA, enable scanout
//
// Each scanline in memory is a 4-byte header (word 0 = {fg, bg} R3G3B2
// palette, word 1 reserved) followed by 80 pixel bytes.  ENGINE streams the
// header through the FIFO so VIDEO latches the per-line palette in-band — no
// CPU palette register, no per-line timing.  The stride is a multiple of 4 so
// the textport's long-word framebuffer movers stay valid.
// ---------------------------------------------------------------------------

// The framebuffer is a 64K-page-aligned allocation from the firmware heap,
// made once at video init by fb_alloc().  ENGINE_SOURCE_PAGE is page-granular
// (A[23:16]) and the engine DMAs exactly FB_BYTES from page<<16, so the base
// must be 64K-aligned and the whole frame (FB_BYTES < 64K) lives in that page.
// FB_ADDR/FB_PAGE are filled in at runtime — there is no fixed framebuffer
// address anywhere.
static uint32_t FB_ADDR                   = 0;
static uint8_t  FB_PAGE                   = 0;
static constexpr unsigned FB_LINES        = 480;
static constexpr unsigned FB_PIXEL_BYTES  = Griffin::VIDEO_PIXEL_BYTES_PER_LINE; // 80
static constexpr unsigned FB_PIXEL_OFFSET = Griffin::VIDEO_LINE_PIXEL_OFFSET;    // 4
static constexpr unsigned FB_STRIDE       = Griffin::VIDEO_LINE_STRIDE_BYTES;    // 84
static constexpr unsigned FB_BYTES        = FB_LINES * FB_STRIDE;                // 40320, < 64K
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

// Allocate the framebuffer from the firmware heap, rounded up to a 64K page
// boundary so its base is a clean ENGINE source page (A[23:16]).  malloc +
// round-up avoids an aligned_alloc/memalign dependency; it is never freed.
static void *fb_raw = nullptr;   // retained base of the framebuffer allocation (never freed)

static void fb_alloc()
{
    if (FB_ADDR != 0)
    {
        return;
    }
    fb_raw = malloc(FB_BYTES + 0xFFFFu);
    if (fb_raw == nullptr)
    {
        printf("FB: alloc of %u bytes FAILED -- video disabled\n", FB_BYTES);
        return;
    }
    FB_ADDR = (reinterpret_cast<uint32_t>(fb_raw) + 0xFFFFu) & ~0xFFFFu;
    FB_PAGE = static_cast<uint8_t>(FB_ADDR >> 16);
}

static void video_framebuffer_init()
{
    fb_alloc();

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

void process_ps2_inputs()
{
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

    bool saw_BAT_code = false;
    while(ps2_received_ready())
    {
        uint8_t byte = ps2_getchar();
        printf("ps2: 0x%02X\n", byte);
        if(byte == 0xAA)
        {
            saw_BAT_code = true;
        }
    }
    if(saw_BAT_code)
    {
        printf("ps2: BAT OK, sending 0xED\n");
        ps2_send_byte(0xED);
        printf("ps2: BAT OK, sending 0x00\n");
        ps2_send_byte(0x00);
        printf("ps2: BAT OK, sent 0xED, enqueued 0x00\n");
    }
}

// Mainline mouse drain, the counterpart of process_ps2_inputs().  mouse.cpp
// only assembles packets; all reporting happens here.  Printing every packet
// would flood the console (a moving mouse reports ~40-100 times a second), so
// only button transitions are announced, carrying the current position with
// them.
static void process_mouse_input()
{
    static uint8_t last_buttons = 0;

    mouse_poll();

    mouse_report_t report;
    mouse_get_report(&report);
    if (report.buttons != last_buttons)
    {
        last_buttons = report.buttons;
        printf("mouse: x=%ld y=%ld wheel=%ld buttons=%c%c%c\n",
               static_cast<long>(report.x), static_cast<long>(report.y),
               static_cast<long>(report.wheel),
               (report.buttons & MOUSE_BUTTON_LEFT) ? 'L' : '-',
               (report.buttons & MOUSE_BUTTON_MIDDLE) ? 'M' : '-',
               (report.buttons & MOUSE_BUTTON_RIGHT) ? 'R' : '-');
    }
}

extern "C" int load_and_run_app(const char *path, int argc, char **argv);  // firmware/loader.cpp
extern "C" bool console_input_ready(void);           // firmware/syscalls.c

// ---------------------------------------------------------------------------
// Monitor — the firmware's top-level command loop, and a minimal shell.
//
// Line editing, echo, and CR->NL live in the console layer (syscalls.c), so
// this just waits for a completed line, tokenizes it, and dispatches.  While
// idle it polls console_input_ready() -- which pumps the line editor -- and
// keeps the textport cursor blinking.
//
// Dispatch is shell-like: built-in commands always win, and a first word that
// matches no built-in is looked up as "<word>.bin" on the CF card.  Either way
// the tokenized line is handed to the app as argc/argv, so "basic FOO.BAS"
// runs BASIC.BIN with a real argument vector.
// ---------------------------------------------------------------------------

// The console line discipline caps a typed line at 128 bytes (CONS_LINE_MAX in
// syscalls.c), so a line can hold at most that many tokens' worth of text.
constexpr size_t MONITOR_LINE_MAX = 128;
constexpr size_t MONITOR_MAX_ARGS = 16;

// Drain one completed line from fd 0.  Only called when console_input_ready()
// says read() won't block; bytes of a completed line return immediately.
// read() returning 0 is Ctrl-D EOF, treated as an empty line.
static size_t monitor_read_line(char *buf, size_t maxlen)
{
    size_t n = 0;
    for (;;)
    {
        char c;
        if (read(0, &c, 1) != 1)
        {
            break;
        }
        if (c == '\n')
        {
            break;
        }
        if (n < maxlen - 1)
        {
            buf[n++] = c;
        }
    }
    buf[n] = '\0';
    return n;
}

// Split line in place on spaces.  Returns the argument count.
static int split_args(char *line, char *argv[], int max_args)
{
    int argc = 0;
    char *p = line;
    while (argc < max_args)
    {
        while (*p == ' ')
        {
            p++;
        }
        if (*p == '\0')
        {
            break;
        }
        argv[argc++] = p;
        while (*p != '\0' && *p != ' ')
        {
            p++;
        }
        if (*p == '\0')
        {
            break;
        }
        *p++ = '\0';
    }
    return argc;
}

static void monitor_help()
{
    printf("read  ADDR [LEN]     (rd)  dump LEN bytes (default 1, max 256)\n");
    printf("write ADDR VAL       (wr)  write byte VAL at ADDR\n");
    printf("ls [PATH]                  list directory (default /)\n");
    printf("time                       print time since boot\n");
    printf("run FILE [ARGS...]         load and run FILE from CF\n");
    printf("view IMAGE PALETTE         show image until a key is pressed\n");
    printf("help                 (?)   this text\n");
    printf("CMD [ARGS...]              run CMD.bin from CF with those args\n");
}

static void monitor_cmd_read(int argc, char *argv[])
{
    if (argc < 2)
    {
        printf("usage: read ADDR [LEN]\n");
        return;
    }
    uint32_t addr = parse_hex(argv[1], nullptr);
    uint32_t len = (argc > 2) ? parse_hex(argv[2], nullptr) : 1;
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
        printf("%06lX:", static_cast<unsigned long>(addr + off));
        for (uint32_t i = 0; i < row; i++)
        {
            uint8_t val = *reinterpret_cast<volatile uint8_t *>(addr + off + i);
            printf(" %02X", val);
        }
        printf("\n");
    }
}

static void monitor_cmd_write(int argc, char *argv[])
{
    if (argc < 3)
    {
        printf("usage: write ADDR VAL\n");
        return;
    }
    uint32_t addr = parse_hex(argv[1], nullptr);
    uint32_t val = parse_hex(argv[2], nullptr);
    *reinterpret_cast<volatile uint8_t *>(addr) = static_cast<uint8_t>(val);
    printf("%06lX <- %02X\n", static_cast<unsigned long>(addr),
           static_cast<unsigned>(val & 0xFF));
}

static void monitor_cmd_time()
{
    uint32_t seconds = get_milliseconds() / 1000;
    uint32_t ss = seconds % 60;
    uint32_t mm = (seconds / 60) % 60;
    uint32_t hh = seconds / 3600;
    printf("%02ld:%02ld:%02ld since boot\n", hh, mm, ss);
}

// Shell fallback: a command word that is no built-in names a binary on the CF
// card.  Probe "<word>.bin" in the root (FAT name matching is case-insensitive,
// so a typed "basic" finds BASIC.BIN) and, if it is there, run it with the line
// as typed -- argv[0] is the word, not the file name, exactly like a shell.
// Returns false if there is no such binary, so the caller can report the
// command as unknown.
static bool monitor_run_cf_command(int argc, char *argv[])
{
    static char binpath[MONITOR_LINE_MAX + 8];

    int n = snprintf(binpath, sizeof(binpath), "%s.bin", argv[0]);
    if (n < 0 || static_cast<size_t>(n) >= sizeof(binpath))
    {
        return false;
    }

    FILINFO fno;
    if (f_stat(binpath, &fno) != FR_OK || (fno.fattrib & AM_DIR))
    {
        return false;
    }

    int result = load_and_run_app(binpath, argc, argv);
    printf("%s: exited with status %d\n", argv[0], result);
    return true;
}

[[noreturn]] static void monitor()
{
    // Static, not automatic: the app runs on this stack below monitor()'s
    // frame and reads argv in place, so the strings must live somewhere the
    // app cannot walk over.
    static char  line[MONITOR_LINE_MAX];
    static char *argv[MONITOR_MAX_ARGS];

    for (;;)
    {
        printf("> ");
        fflush(stdout);   // prompt has no newline; stdout is line-buffered

        while (!console_input_ready())
        {
            gtxt::g_textport.cursor_blink_tick();
            process_mouse_input();
        }
        monitor_read_line(line, sizeof(line));

        int argc = split_args(line, argv, static_cast<int>(MONITOR_MAX_ARGS));
        if (argc == 0)
        {
            continue;
        }
        const char *cmd = argv[0];

        if (strcmp(cmd, "read") == 0 || strcmp(cmd, "rd") == 0)
        {
            monitor_cmd_read(argc, argv);
        }
        else if (strcmp(cmd, "write") == 0 || strcmp(cmd, "wr") == 0)
        {
            monitor_cmd_write(argc, argv);
        }
        else if (strcmp(cmd, "ls") == 0)
        {
            list_directory((argc > 1) ? argv[1] : "/");
        }
        else if (strcmp(cmd, "time") == 0)
        {
            monitor_cmd_time();
        }
        else if (strcmp(cmd, "run") == 0)
        {
            if (argc < 2)
            {
                printf("usage: run FILE [ARGS...]\n");
            }
            else
            {
                // Drop the "run" word: the app sees argv[0] = the path it was
                // launched as, then the remaining tokens.
                int result = load_and_run_app(argv[1], argc - 1, &argv[1]);
                printf("%s: exited with status %d\n", argv[1], result);
            }
        }
        else if (strcmp(cmd, "view") == 0)
        {
            if (argc < 3)
            {
                printf("usage: view IMAGE PALETTE\n");
            }
            else
            {
                view_image(argv[1], argv[2]);
            }
        }
        else if (strcmp(cmd, "help") == 0 || strcmp(cmd, "?") == 0)
        {
            monitor_help();
        }
        else if (!monitor_run_cf_command(argc, argv))
        {
            printf("%s? try \"help\"\n", cmd);
        }
    }
}

int main()
{
    debug_printf("Firmware Build: %s, GIT %s\n", build_date, build_provenance);

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

    video_framebuffer_init();

    // FB is now scanning out a checkerboard; bring up the on-screen
    // console (splash + ring replay + memory size).  Everything below
    // this point appears on both DUART and the textport.
    textport_console_enable();

    // Play startup sound
    // uint32_t audio_len = _binary_startup_raw_end - _binary_startup_raw_start;
    // play_audio(_binary_startup_raw_start, audio_len, 11025);

    cf_mount_and_list();

    // The PS/2 mouse reset/enable handshake spins on TX_DONE from the PORTS
    // level-2 ISR, so it has to run here in mainline with interrupts up --
    // not in crt0.s beside ports_init(), and never from an ISR.
    if (mouse_init())
    {
        mouse_report_t report;
        mouse_get_report(&report);
        printf("PS/2 mouse: ready, %d-byte packets\n", report.packet_bytes);
    }
    else
    {
        printf("PS/2 mouse: not responding\n");
    }

    printf("Monitor ready; 'help' for commands\n");
    monitor();
}
