#ifndef PS2_H
#define PS2_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

#include "../griffin.generated.h"

void ps2_isr(void) __attribute__((interrupt_handler));
void ps2_send_byte(uint8_t b);
bool ps2_received_ready();
uint8_t ps2_getchar();
uint16_t ps2_get_err_data(void);

#define PS2_ERROR_FRAMING 0x01u
#define PS2_ERROR_PARITY  0x02u
#define PS2_ERROR_OVERRUN 0x04u

uint8_t ps2_get_err_flags(void);

#ifdef __cplusplus
}
#endif

#endif
