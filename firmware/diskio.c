/*-----------------------------------------------------------------------*/
/* Low level disk I/O module for FatFs -- Griffin CF card backend        */
/*-----------------------------------------------------------------------*/

#include "ff.h"
#include "diskio.h"
#include "rtc.h"
#include "cf.h"


/*-----------------------------------------------------------------------*/
/* Get Drive Status                                                      */
/*-----------------------------------------------------------------------*/

DSTATUS disk_status(BYTE pdrv)
{
    if (pdrv != 0)
    {
        return STA_NODISK;
    }
    return 0;
}


/*-----------------------------------------------------------------------*/
/* Initialize a Drive                                                    */
/*-----------------------------------------------------------------------*/

DSTATUS disk_initialize(BYTE pdrv)
{
    if (pdrv != 0)
    {
        return RES_ERROR;
    }
    /* CF init is done in main() before f_mount() */
    return RES_OK;
}


/*-----------------------------------------------------------------------*/
/* Read Sector(s)                                                        */
/*-----------------------------------------------------------------------*/

DRESULT disk_read(BYTE pdrv, BYTE *buff, LBA_t sector, UINT count)
{
    if (pdrv != 0)
    {
        return RES_ERROR;
    }

    for (UINT i = 0; i < count; i++)
    {
        if (cf_read_sectors(sector + i, 1, buff + 512 * i) != CF_OK)
        {
            return RES_ERROR;
        }
    }
    return RES_OK;
}


/*-----------------------------------------------------------------------*/
/* Write Sector(s)                                                       */
/*-----------------------------------------------------------------------*/

#if FF_FS_READONLY == 0

DRESULT disk_write(BYTE pdrv, const BYTE *buff, LBA_t sector, UINT count)
{
    if (pdrv != 0)
    {
        return RES_ERROR;
    }

    for (UINT i = 0; i < count; i++)
    {
        if (cf_write_sectors(sector + i, 1, buff + 512 * i) != CF_OK)
        {
            return RES_ERROR;
        }
    }
    return RES_OK;
}

#endif


/*-----------------------------------------------------------------------*/
/* Miscellaneous Functions                                               */
/*-----------------------------------------------------------------------*/

DRESULT disk_ioctl(BYTE pdrv, BYTE cmd, void *buff)
{
    if (pdrv != 0)
    {
        return RES_ERROR;
    }

    switch (cmd)
    {
        case CTRL_SYNC:
            /* CF writes are synchronous; nothing to flush */
            return RES_OK;

        default:
            return RES_ERROR;
    }
}


/* FatFs timestamps from the firmware wall clock (rtc_seconds, UTC; FAT has no
 * zone, so files carry UTC).  FAT dates start at 1980; the clock can't be
 * earlier than the build time, so no clamp is needed. */
extern uint32_t get_epoch_seconds(void);

DWORD get_fattime(void)
{
    rtc_civil_t c;
    rtc_civil_from_epoch(get_epoch_seconds(), &c);
    return
        ((DWORD)(c.year - 1980) << 25) |
        ((DWORD)c.month << 21) |
        ((DWORD)c.day << 16) |
        ((DWORD)c.hour << 11) |
        ((DWORD)c.minute << 5) |
        ((DWORD)(c.second / 2) << 0);
}
