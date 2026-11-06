"""io.kicad_sch: the two FTDI TTL-232R bench headers (DUART ch A and ch B).

TTL-232R 6-pin: 1 GND, 2 CTS (cable input), 3 VCC (not connected), 4 TXD (cable
output), 5 RXD (cable input), 6 RTS (cable output).  The cable's outputs reach
the DUART through 1k so a stuffed MAX232 receiver wins any collision
(griffin.yml interfaces, "RS-232 level stage").  The header-side nets carry
global labels (HDR_TXD_x, HDR_RTS_x): a local label's text, drawn above the
wire, would collide with the global label on the next pin at 2.54 mm pitch.
"""
HDR = 'Connector_PinHeader_2.54mm:PinHeader_1x06_P2.54mm_Vertical'
PWR_START = 500


RES = 'Resistor_THT:R_Axial_DIN0207_L6.3mm_D2.5mm_P10.16mm_Horizontal'


def header(ref, value, at, ch, r_txd, r_rts):
    x, y = at
    return [dict(ref=ref, lib='Connector', name='Conn_01x06_Pin', value=value, footprint=HDR, at=at,
                 description='FTDI TTL-232R 6-pin header',
                 pins={'1': ('power', 'GND'),
                       '2': ('global', f'~{{RTS_{ch}}}', 'input'),
                       '3': 'nc',
                       '4': ('global', f'HDR_TXD_{ch}', 'passive'),
                       '5': ('global', f'UART_TX_{ch}', 'input'),
                       '6': ('global', f'HDR_RTS_{ch}', 'passive')}),
            dict(ref=r_txd, lib='Device', name='R', value='1k', footprint=RES, at=(x + 45.72, y - 2.54), rot=90,
                 description='Resistor',
                 pins={'1': ('global', f'HDR_TXD_{ch}', 'passive'), '2': ('global', f'UART_RX_{ch}', 'output')}),
            dict(ref=r_rts, lib='Device', name='R', value='1k', footprint=RES, at=(x + 45.72, y + 7.62), rot=90,
                 description='Resistor',
                 pins={'1': ('global', f'HDR_RTS_{ch}', 'passive'), '2': ('global', f'~{{CTS_{ch}}}', 'output')})]


PARTS = header('J3', 'FTDI serial A', (38.1, 33.02), 'A', 'R100', 'R102') + \
    header('J12', 'FTDI serial B', (127.0, 33.02), 'B', 'R101', 'R103')
TEXTS = [('TTL headers: FTDI TTL-232R pinout.  Header TXD and RTS reach the DUART\\n'
          'through 1k so a stuffed MAX232 wins any collision (griffin.yml RS-232 entry).', 38.1, 58.42)]
