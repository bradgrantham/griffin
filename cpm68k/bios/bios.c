/* CP/M-68K BIOS for Griffin: the C half.
 *
 * Everything goes through the ROM's TRAP #15 syscalls (sys.s): the console is
 * the firmware console in character mode (VT102 screen + PS/2 keyboard +
 * DUART channel A at once), and each CP/M drive is a file on the FAT card,
 * A.CPM .. D.CPM, read and written through open/lseek/read/write/fsync.  The
 * BIOS never touches a hardware register, so the ROM's interrupt handlers
 * keep running and the FAT volume stays consistent.
 *
 * Drive geometry (one shape for every drive, so cpmtools' tools/diskdefs
 * matches the DPB here):
 *   512-byte physical sectors, 32 per track  -> spt = 128 logical records
 *   2 KB blocks (bsh 4, blm 15, exm 0: one directory entry per 16 KB)
 *   512 directory entries (drm 511, al0:al1 = 0xFF00), 16-bit block numbers
 *   no skew, no checksum vector (fixed disk), no reserved tracks
 *   dsm = (file size / 2 KB) - 1, read from the file at first select, so any
 *   image that is a whole number of 2 KB blocks works, up to DRIVE_MAX_BLOCKS.
 * The BDOS addresses 128-byte records; this BIOS blocks and deblocks them
 * through one 512-byte sector cache with write-through, which is enough
 * because the FatFs layer in the ROM buffers the sector again.
 *
 * Freestanding C (no libc), -m68000, built by either the podman
 * m68k-unknown-elf wrappers or a plain m68k-linux-gnu cross gcc.
 */

#include <stdint.h>
#include <stddef.h>

/* ---- ROM syscalls (sys.s); negative return = -errno ---------------------- */
extern long sys_exit(long code, long, long);
extern long sys_write(long fd, const void *buf, long len);
extern long sys_read(long fd, void *buf, long len);
extern long sys_open(const char *path, long flags, long mode);
extern long sys_close(long fd, long, long);
extern long sys_lseek(long fd, long off, long whence);
extern long sys_fstat(long fd, void *st, long);
extern long sys_inputready(long, long, long);
extern long sys_getticks(long, long, long);
extern long sys_gettime(long, long, long);
extern long sys_console_mode(long mode, long, long);
extern long sys_fsync(long fd, long, long);

#define O_RDWR          2           /* newlib <fcntl.h>, what the ROM's open() takes */
#define SEEK_SET        0
#define CONSOLE_LINE    0           /* SYS_CONSOLE_RAW modes, griffin.yml */
#define CONSOLE_CHAR    2

/* newlib's generic struct stat as the m68k-unknown-elf firmware lays it out
 * (2-byte alignment): st_dev/st_ino/st_mode/st_nlink/st_uid/st_gid/st_rdev
 * then st_size at offset 16.  Only st_size is read; the buffer is oversized
 * so the firmware's memset of the whole struct stays inside it. */
#define STAT_BUF_LONGS  32
#define STAT_SIZE_OFF   16

/* ---- CP/M-68K structures (system/bdosdef.h, kept byte-identical) --------- */
struct dpb                      /* disk parameter block */
{
    uint16_t spt;               /* logical (128-byte) sectors per track */
    uint8_t  bsh;               /* block shift */
    uint8_t  blm;               /* block mask */
    uint8_t  exm;               /* extent mask */
    uint8_t  dpbdum;
    uint16_t dsm;               /* highest block number */
    uint16_t drm;               /* highest directory entry number */
    uint16_t dir_al;            /* al0:al1 directory allocation bits */
    uint16_t cks;               /* directory records to checksum (0: fixed disk) */
    uint16_t trk_off;           /* reserved tracks */
};

struct dph                      /* disk parameter header */
{
    uint8_t    *xlt;            /* sector translate table (none) */
    uint16_t    hiwater;
    uint16_t    dum1;
    uint16_t    dum2;
    uint8_t    *dbufp;          /* 128-byte directory buffer */
    struct dpb *dpbp;
    uint8_t    *csv;            /* check vector (none) */
    uint8_t    *alv;            /* allocation vector, one bit per block */
};

struct memregion                /* BIOS function 18: memory region table */
{
    uint16_t count;
    uint32_t low;
    uint32_t length;
} __attribute__((packed));

/* ---- geometry ------------------------------------------------------------ */
#define SECTOR_BYTES        512
#define RECORD_BYTES        128
#define RECORDS_PER_SECTOR  (SECTOR_BYTES / RECORD_BYTES)
#define SECTORS_PER_TRACK   32
#define RECORDS_PER_TRACK   (SECTORS_PER_TRACK * RECORDS_PER_SECTOR)
#define BLOCK_BYTES         2048
#define BLOCK_SHIFT         4           /* log2(BLOCK_BYTES / RECORD_BYTES) */
#define BLOCK_MASK          15
#define EXTENT_MASK         0           /* 8 blocks x 2 KB = one 16 KB logical extent per entry */
#define DIR_ENTRIES         512
#define DIR_ALLOC           0xFF00u     /* 512 * 32 bytes = 8 blocks */
#define DRIVE_MAX_BLOCKS    32768       /* 64 MB per drive; sizes the alloc vectors */
#define NDRIVES             4           /* the ROM keeps 4 files open at most */

/* ---- state --------------------------------------------------------------- */
extern char _tpa_start[];       /* cpm.ld: first byte above CPM.BIN's bss */
extern char _tpa_end[];         /* cpm.ld: the firmware's RAM, 0x380000 */

static struct memregion memtab;

static struct dpb   dpbs[NDRIVES];
static struct dph   dphs[NDRIVES];
static uint8_t      alvs[NDRIVES][DRIVE_MAX_BLOCKS / 8];
static uint8_t      dirbuf[RECORD_BYTES];
static long         drive_fd[NDRIVES];      /* 0: not open yet, <0: missing */
static const char  *drive_name[NDRIVES] = { "A.CPM", "B.CPM", "C.CPM", "D.CPM" };

static uint16_t     cur_drive;
static uint16_t     cur_track;
static uint16_t     cur_record;             /* logical sector within the track */
static uint8_t     *cur_dma;
static uint16_t     iobyte;

static uint8_t      cache[SECTOR_BYTES];
static int          cache_drive = -1;
static uint32_t     cache_lba;

/* vectors changed through function 22, restored by function 30 */
static uint32_t     saved_vec[256];
static uint8_t      saved_vec_valid[256];

static const char banner[] =
    "\r\nCP/M-68K for Griffin: console via ROM, drives A.CPM..D.CPM on the FAT card\r\n";

/* ---- helpers ------------------------------------------------------------- */
static void conout(uint8_t c)
{
    sys_write(1, &c, 1);
}

static void conputs(const char *s)
{
    long n = 0;
    while (s[n] != '\0')
    {
        n++;
    }
    sys_write(1, s, n);
}

static uint8_t conin(void)
{
    uint8_t c;
    while (sys_read(0, &c, 1) != 1)
    {
    }
    return c;
}

static void conhex(uint32_t v)
{
    static const char digits[] = "0123456789ABCDEF";
    for (int i = 28; i >= 0; i -= 4)
    {
        conout((uint8_t)digits[(v >> i) & 0xF]);
    }
}

/* "BIOS: A.CPM: <what> (<code>)" */
static void drive_complain(int d, const char *what, long code)
{
    conputs("\r\nBIOS: ");
    conputs(drive_name[d]);
    conputs(": ");
    conputs(what);
    conputs(" (");
    conhex((uint32_t)code);
    conputs(")\r\n");
}

static void *memcpy128(void *dst, const void *src)
{
    uint32_t *d = dst;
    const uint32_t *s = src;
    for (int i = 0; i < RECORD_BYTES / 4; i++)
    {
        d[i] = s[i];
    }
    return dst;
}

/* ---- drives -------------------------------------------------------------- */
static void drive_setup(int d, uint32_t blocks)
{
    struct dpb *p = &dpbs[d];
    p->spt     = RECORDS_PER_TRACK;
    p->bsh     = BLOCK_SHIFT;
    p->blm     = BLOCK_MASK;
    p->exm     = EXTENT_MASK;
    p->dpbdum  = 0;
    p->dsm     = (uint16_t)(blocks - 1);
    p->drm     = DIR_ENTRIES - 1;
    p->dir_al  = DIR_ALLOC;
    p->cks     = 0;
    p->trk_off = 0;

    struct dph *h = &dphs[d];
    h->xlt     = NULL;
    h->hiwater = 0;
    h->dum1    = 0;
    h->dum2    = 0;
    h->dbufp   = dirbuf;
    h->dpbp    = p;
    h->csv     = NULL;
    h->alv     = alvs[d];
}

/* Open the drive's image on first use; 0 on success. */
static int drive_open(int d)
{
    if (drive_fd[d] > 0)
    {
        return 0;
    }
    if (drive_fd[d] < 0)
    {
        return -1;
    }
    long fd = sys_open(drive_name[d], O_RDWR, 0);
    if (fd < 0)
    {
        drive_complain(d, "no such drive image on the card", fd);
        drive_fd[d] = -1;
        return -1;
    }
    long st[STAT_BUF_LONGS];
    long rc = sys_fstat(fd, st, 0);
    if (rc < 0)
    {
        drive_complain(d, "fstat failed", rc);
        sys_close(fd, 0, 0);
        drive_fd[d] = -1;
        return -1;
    }
    uint32_t size   = *(uint32_t *)((char *)st + STAT_SIZE_OFF);
    uint32_t blocks = size / BLOCK_BYTES;
    if (blocks < 16 || blocks > DRIVE_MAX_BLOCKS)
    {
        drive_complain(d, "not a whole number of 2 KB blocks between 32 KB and 64 MB, size", (long)size);
        sys_close(fd, 0, 0);
        drive_fd[d] = -1;
        return -1;
    }
    drive_setup(d, blocks);
    drive_fd[d] = fd;
    return 0;
}

static int sector_load(int d, uint32_t lba)
{
    if (cache_drive == d && cache_lba == lba)
    {
        return 0;
    }
    cache_drive = -1;
    long fd = drive_fd[d];
    if (sys_lseek(fd, (long)(lba * SECTOR_BYTES), SEEK_SET) < 0)
    {
        return -1;
    }
    if (sys_read(fd, cache, SECTOR_BYTES) != SECTOR_BYTES)
    {
        return -1;
    }
    cache_drive = d;
    cache_lba   = lba;
    return 0;
}

static int sector_store(int d, uint32_t lba)
{
    long fd = drive_fd[d];
    if (sys_lseek(fd, (long)(lba * SECTOR_BYTES), SEEK_SET) < 0)
    {
        return -1;
    }
    if (sys_write(fd, cache, SECTOR_BYTES) != SECTOR_BYTES)
    {
        cache_drive = -1;
        return -1;
    }
    return 0;
}

/* The BDOS has already selected the drive, set track and record and the DMA
 * address.  Return 0 on success, 1 on error (the BDOS retries or reports). */
static long disk_read(void)
{
    if (cur_drive >= NDRIVES || drive_fd[cur_drive] <= 0 || cur_dma == NULL)
    {
        return 1;
    }
    uint32_t lba = (uint32_t)cur_track * SECTORS_PER_TRACK + (cur_record / RECORDS_PER_SECTOR);
    if (sector_load(cur_drive, lba) != 0)
    {
        return 1;
    }
    memcpy128(cur_dma, cache + (cur_record % RECORDS_PER_SECTOR) * RECORD_BYTES);
    return 0;
}

static long disk_write(void)
{
    if (cur_drive >= NDRIVES || drive_fd[cur_drive] <= 0 || cur_dma == NULL)
    {
        return 1;
    }
    uint32_t lba = (uint32_t)cur_track * SECTORS_PER_TRACK + (cur_record / RECORDS_PER_SECTOR);
    if (sector_load(cur_drive, lba) != 0)
    {
        return 1;
    }
    memcpy128(cache + (cur_record % RECORDS_PER_SECTOR) * RECORD_BYTES, cur_dma);
    return sector_store(cur_drive, lba) != 0 ? 1 : 0;
}

static long disk_flush(void)
{
    long rc = 0;
    for (int d = 0; d < NDRIVES; d++)
    {
        if (drive_fd[d] > 0 && sys_fsync(drive_fd[d], 0, 0) < 0)
        {
            rc = 0xFFFF;
        }
    }
    return rc;
}

/* ---- exception vectors --------------------------------------------------- */
/* The BDOS installs its handlers for vectors 2..23 and 32..46 through here at
 * every cold and warm boot.  TRAP #15 (47) is the ROM syscall this BIOS lives
 * on, and 24..31 are the ROM's interrupt handlers; both are kept out of reach.
 * The first value overwritten is remembered so exit can put the ROM's back. */
static long set_vector(uint16_t vec, uint32_t value)
{
    volatile uint32_t *slot = (volatile uint32_t *)((uintptr_t)vec * 4);
    uint32_t old = *slot;
    if (vec >= 256 || vec == 47 || (vec >= 24 && vec <= 31))
    {
        return (long)old;
    }
    if (!saved_vec_valid[vec])
    {
        saved_vec[vec]       = old;
        saved_vec_valid[vec] = 1;
    }
    *slot = value;
    return (long)old;
}

static void restore_vectors(void)
{
    for (unsigned vec = 0; vec < 256; vec++)
    {
        if (saved_vec_valid[vec])
        {
            *(volatile uint32_t *)((uintptr_t)vec * 4) = saved_vec[vec];
            saved_vec_valid[vec] = 0;
        }
    }
}

/* ---- entry points from biosa.s ------------------------------------------- */
void bios_init(void)
{
    memtab.count  = 1;
    memtab.low    = (uint32_t)(uintptr_t)_tpa_start;
    memtab.length = (uint32_t)(uintptr_t)_tpa_end - memtab.low;
    sys_console_mode(CONSOLE_CHAR, 0, 0);
    conputs(banner);
}

void bios_wboot(void)
{
    disk_flush();
}

/* Griffin extension: leave CP/M and return to the ROM monitor. */
static void bios_exit(void)
{
    disk_flush();
    for (int d = 0; d < NDRIVES; d++)
    {
        if (drive_fd[d] > 0)
        {
            sys_close(drive_fd[d], 0, 0);
            drive_fd[d] = 0;
        }
    }
    restore_vectors();
    sys_console_mode(CONSOLE_LINE, 0, 0);
    sys_exit(0, 0, 0);
}

long bios_dispatch(long func, long p1, long p2)
{
    switch (func)
    {
        case 0:                                   /* init: handled in biosa.s */
            return 0;
        case 2:                                   /* console status */
            return sys_inputready(0, 0, 0) > 0 ? 0xFF : 0;
        case 3:                                   /* console input */
            return conin();
        case 4:                                   /* console output */
        case 5:                                   /* list output: the console too */
            conout((uint8_t)p1);
            return 0;
        case 6:                                   /* punch output: discarded */
            return 0;
        case 7:                                   /* reader input: end of file */
            return 0x1A;
        case 8:                                   /* home */
            cur_track = 0;
            return 0;
        case 9:                                   /* select disk: dph or 0 */
        {
            uint16_t d = (uint16_t)p1;
            if (d >= NDRIVES || drive_open(d) != 0)
            {
                return 0;
            }
            cur_drive = d;
            return (long)(uintptr_t)&dphs[d];
        }
        case 10:                                  /* set track */
            cur_track = (uint16_t)p1;
            return 0;
        case 11:                                  /* set sector (logical record, 0-based) */
            cur_record = (uint16_t)p1;
            return 0;
        case 12:                                  /* set DMA address */
            cur_dma = (uint8_t *)(uintptr_t)p1;
            return 0;
        case 13:                                  /* read */
            return disk_read();
        case 14:                                  /* write (p1: 0 normal, 1 directory, 2 new block) */
            return disk_write();
        case 15:                                  /* list status: always ready */
            return 0xFF;
        case 16:                                  /* sector translate: identity */
            return (uint16_t)p1;
        case 18:                                  /* memory region table */
            return (long)(uintptr_t)&memtab;
        case 19:                                  /* get I/O byte */
            return iobyte;
        case 20:                                  /* set I/O byte */
            iobyte = (uint16_t)p1;
            return 0;
        case 21:                                  /* flush */
            return disk_flush();
        case 22:                                  /* set exception vector */
            return set_vector((uint16_t)p1, (uint32_t)p2);
        case 30:                                  /* Griffin: exit to the monitor */
            bios_exit();
            return 0;
        case 31:                                  /* Griffin: seconds since the Unix epoch */
            return sys_gettime(0, 0, 0);
        case 32:                                  /* Griffin: milliseconds since boot */
            return sys_getticks(0, 0, 0);
        default:
            return 0;
    }
}
