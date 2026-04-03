#ifndef CF_H
#define CF_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Return values match cf_error enum in rom.cpp */
#define CF_OK      0
#define CF_TIMEOUT 1
#define CF_ERR     2

int cf_init(void);
int cf_identify(uint8_t buf[512]);
int cf_read_sectors(uint32_t lba, uint8_t count, uint8_t *buf);
int cf_write_sectors(uint32_t lba, uint8_t count, const uint8_t *buf);

#ifdef __cplusplus
}
#endif

#endif
