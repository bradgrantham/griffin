#include "mouse.h"
#include "ps2.h"
#include "../griffin.generated.h"

/* ====================================================================
 * PS/2 mouse decoder.
 *
 * Bytes arrive one per interrupt from the PORTS mouse frame engine and are
 * queued by ps2.cpp; this layer assembles them into movement packets in
 * mainline.  Nothing here prints or touches any other facility.
 *
 * Standard 3-byte packet:
 *   byte 0: YO XO YS XS 1 MB RB LB
 *   byte 1: X delta, low 8 bits of a 9-bit two's complement value (XS is bit 8)
 *   byte 2: Y delta, likewise with YS
 * IntelliMouse (device ID 3) adds:
 *   byte 3: 4-bit signed wheel delta in the low nibble
 *
 * Bit 3 of byte 0 always reads 1, which is the only framing marker on the
 * wire; a byte with it clear where byte 0 is expected means we lost sync,
 * so it is discarded and counted.
 * ==================================================================== */

namespace {

/* Host->device commands. */
constexpr uint8_t MOUSE_CMD_RESET             = 0xFF;
constexpr uint8_t MOUSE_CMD_GET_DEVICE_ID     = 0xF2;
constexpr uint8_t MOUSE_CMD_SET_SAMPLE_RATE   = 0xF3;
constexpr uint8_t MOUSE_CMD_ENABLE_REPORTING  = 0xF4;

/* Device->host responses. */
constexpr uint8_t MOUSE_REPLY_ACK    = 0xFA;
constexpr uint8_t MOUSE_REPLY_BAT_OK = 0xAA;

/* Packet byte 0 fields. */
constexpr uint8_t MOUSE_FLAG_BUTTONS = 0x07;
constexpr uint8_t MOUSE_FLAG_ALWAYS_ONE = 0x08;
constexpr uint8_t MOUSE_FLAG_X_SIGN = 0x10;
constexpr uint8_t MOUSE_FLAG_Y_SIGN = 0x20;

constexpr uint8_t MOUSE_PACKET_STANDARD = 3;
constexpr uint8_t MOUSE_PACKET_WHEEL = 4;

/* The "magic knock": three sample-rate settings in this order make an
 * IntelliMouse-compatible device switch to 4-byte wheel packets and report
 * device ID 3.  A plain 2-button mouse ignores it and stays ID 0. */
constexpr uint8_t MOUSE_KNOCK_RATE_1 = 200;
constexpr uint8_t MOUSE_KNOCK_RATE_2 = 100;
constexpr uint8_t MOUSE_KNOCK_RATE_3 = 80;
constexpr uint8_t MOUSE_DEVICE_ID_WHEEL = 3;

/* Reply spin budgets.  Same shape as ps2.cpp's TX bound: roughly 16 SYSCLK
 * cycles per iteration out of ROM.  A reset's BAT can take the better part
 * of a second on a real mouse; ordinary command ACKs come back in a couple
 * of frame times. */
constexpr uint32_t MOUSE_SPINS_PER_SECOND = Griffin::SYSCLK_HZ / 16u;
constexpr uint32_t MOUSE_RESET_TIMEOUT_SPINS = MOUSE_SPINS_PER_SECOND;
constexpr uint32_t MOUSE_REPLY_TIMEOUT_SPINS = MOUSE_SPINS_PER_SECOND / 10u;

struct mouse_state_t
{
    uint8_t  packet[MOUSE_PACKET_WHEEL];
    uint8_t  index;
    uint8_t  packet_bytes;
    uint8_t  buttons;
    bool     present;

    int32_t  x;
    int32_t  y;
    int32_t  wheel;
    uint32_t packets;
    uint32_t resyncs;
};

/* Same section as the PS/2 channel state.  It is not zeroed at startup, so
 * mouse_init must set every field before anything else reads it. */
[[gnu::section("monitor_data")]] mouse_state_t mouse;

/* Wait for one byte from the mouse channel.  Returns false on timeout. */
bool mouse_wait_byte(uint8_t *out, uint32_t timeout_spins)
{
    uint32_t spins = timeout_spins;
    while (!ps2_channel_received_ready(&ps2_mouse))
    {
        spins--;
        if (spins == 0)
        {
            return false;
        }
    }
    *out = ps2_channel_getchar(&ps2_mouse);
    return true;
}

/* Send a command byte and consume the device's ACK. */
bool mouse_command(uint8_t command)
{
    if (!ps2_channel_send_byte(&ps2_mouse, command))
    {
        return false;
    }
    uint8_t reply = 0;
    if (!mouse_wait_byte(&reply, MOUSE_REPLY_TIMEOUT_SPINS))
    {
        return false;
    }
    return reply == MOUSE_REPLY_ACK;
}

/* Send a command byte plus one argument byte, each ACKed. */
bool mouse_command_arg(uint8_t command, uint8_t argument)
{
    if (!mouse_command(command))
    {
        return false;
    }
    return mouse_command(argument);
}

/* Feed one received byte into the packet assembler. */
void mouse_accept_byte(uint8_t byte)
{
    if (mouse.index == 0 && (byte & MOUSE_FLAG_ALWAYS_ONE) == 0)
    {
        /* Not a packet byte 0, so a byte was lost somewhere.  Drop it and
         * keep hunting; the marker bit resynchronizes us within one packet. */
        mouse.resyncs++;
        return;
    }

    mouse.packet[mouse.index] = byte;
    mouse.index++;
    if (mouse.index < mouse.packet_bytes)
    {
        return;
    }
    mouse.index = 0;

    uint8_t flags = mouse.packet[0];

    /* 9-bit two's complement: the sign bit lives in byte 0, so subtract 256
     * rather than sign-extending the data byte on its own. */
    int32_t dx = static_cast<int32_t>(mouse.packet[1]);
    if (flags & MOUSE_FLAG_X_SIGN)
    {
        dx -= 256;
    }
    int32_t dy = static_cast<int32_t>(mouse.packet[2]);
    if (flags & MOUSE_FLAG_Y_SIGN)
    {
        dy -= 256;
    }

    mouse.x += dx;
    mouse.y += dy;
    mouse.buttons = static_cast<uint8_t>(flags & MOUSE_FLAG_BUTTONS);

    if (mouse.packet_bytes == MOUSE_PACKET_WHEEL)
    {
        /* Low nibble is a 4-bit signed detent count. */
        int32_t dz = static_cast<int32_t>(mouse.packet[3] & 0x0Fu);
        if (dz & 0x08)
        {
            dz -= 16;
        }
        mouse.wheel += dz;
    }

    mouse.packets++;
}

}  // namespace

extern "C" {

bool mouse_init(void)
{
    /* MUST run from mainline with interrupts enabled: the sends below spin
     * on a TX_DONE that only ports_isr (level 2) can deliver, so calling
     * this from crt0.s or from any ISR would deadlock until the timeout. */
    mouse.index = 0;
    mouse.packet_bytes = MOUSE_PACKET_STANDARD;
    mouse.buttons = 0;
    mouse.present = false;
    mouse.x = 0;
    mouse.y = 0;
    mouse.wheel = 0;
    mouse.packets = 0;
    mouse.resyncs = 0;

    /* Reset: ACK, then BAT result, then the device ID. */
    if (!mouse_command(MOUSE_CMD_RESET))
    {
        return false;
    }
    uint8_t reply = 0;
    if (!mouse_wait_byte(&reply, MOUSE_RESET_TIMEOUT_SPINS) || reply != MOUSE_REPLY_BAT_OK)
    {
        return false;
    }
    if (!mouse_wait_byte(&reply, MOUSE_REPLY_TIMEOUT_SPINS))
    {
        return false;
    }

    /* Try for wheel packets.  A device that does not understand the knock
     * still ACKs the sample-rate writes and reports its original ID, so a
     * failure here is not fatal — just stay with 3-byte packets. */
    if (mouse_command_arg(MOUSE_CMD_SET_SAMPLE_RATE, MOUSE_KNOCK_RATE_1)
        && mouse_command_arg(MOUSE_CMD_SET_SAMPLE_RATE, MOUSE_KNOCK_RATE_2)
        && mouse_command_arg(MOUSE_CMD_SET_SAMPLE_RATE, MOUSE_KNOCK_RATE_3)
        && mouse_command(MOUSE_CMD_GET_DEVICE_ID)
        && mouse_wait_byte(&reply, MOUSE_REPLY_TIMEOUT_SPINS)
        && reply == MOUSE_DEVICE_ID_WHEEL)
    {
        mouse.packet_bytes = MOUSE_PACKET_WHEEL;
    }

    if (!mouse_command(MOUSE_CMD_ENABLE_REPORTING))
    {
        return false;
    }

    mouse.present = true;
    return true;
}

void mouse_poll(void)
{
    while (ps2_channel_received_ready(&ps2_mouse))
    {
        mouse_accept_byte(ps2_channel_getchar(&ps2_mouse));
    }
}

void mouse_get_report(mouse_report_t *out)
{
    out->x = mouse.x;
    out->y = mouse.y;
    out->wheel = mouse.wheel;
    out->packets = mouse.packets;
    out->resyncs = mouse.resyncs;
    out->buttons = mouse.buttons;
    out->packet_bytes = mouse.packet_bytes;
    out->present = mouse.present ? 1u : 0u;
}

};
