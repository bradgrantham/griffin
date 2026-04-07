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
// for BSY to clear after power-on; 2 seconds covers most cards.
// Each iteration is ~50 SYSCLK (~4 µs at 12 MHz), so 600K ≈ 2.4 s.
static constexpr uint32_t CF_INIT_POLL_LIMIT = 600000;

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

// ENGINE DMA control — defined in crt0.s
extern "C" void video_start_dma(uint16_t fb_base_val, uint16_t stride_val);
extern "C" void video_stop_dma(void);

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

// Timer tick handler — called from ISR context at IPL 5
void timer_tick()
{
    // TODO: increment tick counter, feed watchdog, etc.
}

}; // extern "C"

static void dump_hex(uint32_t base_addr, const uint8_t *data, int size)
{
    int offset = 0;
    while (size > 0)
    {
        int howmany = (size < 16) ? size : 16;

        debug_printf("  0x%06lX: ", (unsigned long)(base_addr + offset));
        for (int i = 0; i < howmany; i++)
        {
            debug_printf("%02X ", data[i]);
        }
        debug_printf("\n");

        debug_printf("            ");
        for (int i = 0; i < howmany; i++)
        {
            char c = data[i];
            debug_printf(" %c ", (c >= 0x20 && c <= 0x7E) ? c : '.');
        }
        debug_printf("\n");

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
        debug_printf("CF: init failed (err=%d) status=0x%02X error=0x%02X\n",
                     err, cf_status, cf_error_reg);
        return;
    }
    debug_printf("CF: init OK\n");

    uint8_t id_buf[512];
    err = cf_identify(id_buf);
    if (err != CF_OK)
    {
        debug_printf("CF: identify failed (err=%d) status=0x%02X error=0x%02X\n",
                     err, cf_status, cf_error_reg);
        return;
    }

    cf_info info;
    cf_parse_identify(id_buf, &info);
    debug_printf("CF: %s, firmware %s, serial %s\n", info.model, info.firmware_rev, info.serial);
    debug_printf("CF: sectors:  %lu, capacity: %lu KB\n", (unsigned long)info.lba_sectors, (unsigned long)(info.lba_sectors / 2));

    // Mount filesystem
    FRESULT res = f_mount(&fatfs, "", 1);
    if (res != FR_OK)
    {
        debug_printf("CF: mount failed (FatFS err=%d)\n", res);
        return;
    }
    debug_printf("CF: filesystem mounted\n");

    // Print volume label
    char label[12];
    DWORD vsn;
    res = f_getlabel("", label, &vsn);
    if (res == FR_OK)
    {
        if (label[0])
        {
            debug_printf("Volume: %s (S/N %04X-%04X)\n",
                         label, (unsigned)(vsn >> 16), (unsigned)(vsn & 0xFFFF));
        }
        else
        {
            debug_printf("Volume: (no label) (S/N %04X-%04X)\n",
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
        debug_printf("  %lu KB free / %lu KB total\n", free_kb, total_kb);
    }

    // List root directory
    DIR dir;
    FILINFO fno;
    res = f_opendir(&dir, "/");
    if (res == FR_OK)
    {
        debug_printf("Root directory:\n");
        for (;;)
        {
            res = f_readdir(&dir, &fno);
            if (res != FR_OK || fno.fname[0] == '\0')
            {
                break;
            }
            debug_printf("  %c %7lu  %s\n",
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

static constexpr uint32_t FB_ALIGN = 16384;  // ENGINE_FB_BASE is A[21:14]
uint8_t *framebuffer_allocation;
extern volatile uint32_t framebuffer_stride_words;
extern volatile uint32_t framebuffer_height_lines;
extern volatile uint32_t framebuffer_width_pixels;
extern volatile uint32_t framebuffer_width_words;
extern volatile uint16_t *framebuffer_base;

bool video_allocate(/* mode parameters */)
{
    framebuffer_stride_words = 64;
    framebuffer_width_pixels = 640;
    framebuffer_width_words = 40;
    framebuffer_height_lines = 240;
    auto framebuffer_size = framebuffer_stride_words * framebuffer_height_lines;
    // Allocate framebuffer with room to align to 16KB
    uint8_t *framebuffer_allocation = static_cast<uint8_t *>(malloc(framebuffer_size + FB_ALIGN - 1));
    if (!framebuffer_allocation)
    {
        debug_printf("video: framebuffer allocation failed\n");
        return false;
    }
    auto raw_addr = reinterpret_cast<uintptr_t>(framebuffer_allocation);
    auto fb_addr = (raw_addr + FB_ALIGN - 1) & ~(FB_ALIGN - 1);
    framebuffer_base = reinterpret_cast<volatile uint16_t *>(fb_addr);

    debug_printf("video: splash displayed at 0x%06lX\n", (unsigned long)fb_addr);

    return true;
}

void video_start()
{
    auto fb_addr = reinterpret_cast<uintptr_t>(framebuffer_base);
    video_start_dma(fb_addr >> 14, Griffin::ENGINE_ROW_STRIDE_STRIDE_64);
}

void video_stop()
{
    video_stop_dma();
}

int main()
{
    debug_printf("Firmware Build: %s, GIT %s\n", build_date, build_provenance);

    // -----------------------------------------------------------------
    // Display splash bitmap via ENGINE DMA
    // -----------------------------------------------------------------
    {
        int STRIDE_BYTES = framebuffer_stride_words * 2;
        static constexpr int WORDS_PER_LINE = SPLASH_WIDTH / 16;  // 40

        bool success = video_allocate(/* ... */);
        if(!success)
        {
            debug_printf("video configuration failed\n");
        }
        else
        {
            volatile uint16_t *fb = framebuffer_base;
            const uint16_t *src = splash_bitmap;

            for (int y = 0; y < SPLASH_HEIGHT; y++)
            {
                for (int x = 0; x < framebuffer_width_words; x++)
                {
                    fb[x] = src[x];
                }
                for (int x = framebuffer_width_words; x < framebuffer_stride_words; x++)
                {
                    fb[x] = 0;
                }
                src += WORDS_PER_LINE;
                fb += framebuffer_stride_words;
            }
            video_start();
        }
    }

    volatile uint8_t &dac       = *reinterpret_cast<volatile uint8_t *>(Griffin::AUDIO_DAC);

    // Play startup sound
    // uint32_t audio_len = _binary_startup_raw_end - _binary_startup_raw_start;
    // play_audio(_binary_startup_raw_start, audio_len, 11025);

    cf_mount_and_list();

    // Polled UART RX loop via DEBUG_IN
    for (;;)
    {
        int ch = debug_getchar();
        if (ch >= 0)
        {
            debug_printf("received: 0x%02X '%c'\n", ch,
                         (ch >= 0x20 && ch < 0x7F) ? ch : '.');
        }
    }
}
