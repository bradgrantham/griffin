"""serial.kicad_sch: the RS-232 level stages (griffin.yml interfaces, "RS-232 level stage").

Channel A console: MAX232 U36, DB-25 female J18 wired DCE.  Channel B modem:
MAX232 U37, DB-25 male J19 wired DTE, DCD and RI through SN75189 U38.
Local nets between connector and level shifter are named for the RS-232
signal on that DB-25 pin (DTE-referenced: pin 2 TXD, 3 RXD, 4 RTS, 5 CTS,
8 DCD, 22 RI), so RS232_A_TXD is the terminal's transmit data arriving at the
console port.  Charge-pump and VCC caps are 1 uF aluminium electrolytics
(datasheet values), polarised as in the TI MAX232 typical circuit.
"""
DIP14 = 'Package_DIP:DIP-14_W7.62mm'
DIP16 = 'Package_DIP:DIP-16_W7.62mm'
CP = 'Capacitor_THT:CP_Radial_D5.0mm_P2.00mm'
CDISC = 'Capacitor_THT:C_Disc_D5.0mm_W2.5mm_P5.00mm'
RES = 'Resistor_THT:R_Axial_DIN0207_L6.3mm_D2.5mm_P10.16mm_Horizontal'
DB25F = 'Connector_Dsub:DSUB-25_Socket_Horizontal_P2.77x2.84mm_EdgePinOffset7.70mm_Housed_MountingHolesOffset9.12mm'
DB25M = 'Connector_Dsub:DSUB-25_Pins_Horizontal_P2.77x2.84mm_EdgePinOffset7.70mm_Housed_MountingHolesOffset9.12mm'
PWR_START = 520


def max232(ref, ch, at, t1in, r1out, t2in, r2out, t1out, r1in, t2out, r2in):
    n = f'MAX{ch}'
    return dict(ref=ref, lib='Interface_UART', name='MAX232', value='MAX232', footprint=DIP16, at=at,
                datasheet='https://www.ti.com/lit/ds/symlink/max232.pdf',
                description='Dual RS-232 driver/receiver, 5 V, charge pump',
                pins={'1': ('local', f'{n}_C1+'), '3': ('local', f'{n}_C1-'),
                      '4': ('local', f'{n}_C2+'), '5': ('local', f'{n}_C2-'),
                      '2': ('local', f'{n}_VS+'), '6': ('local', f'{n}_VS-'),
                      '16': ('power', '+5V'), '15': ('power', 'GND'),
                      '11': ('global', t1in, 'input'), '12': ('global', r1out, 'output'),
                      '10': ('global', t2in, 'input'), '9': ('global', r2out, 'output'),
                      '14': ('local', t1out), '13': ('local', r1in),
                      '7': ('local', t2out), '8': ('local', r2in)})


def cp(ref, at, plus, minus, rot=0):
    return dict(ref=ref, lib='Device', name='C_Polarized', value='1uF', footprint=CP, at=at, rot=rot,
                description='Polarized capacitor', pins={'1': plus, '2': minus})


def charge_pump_caps(first, ch, x0, y):
    n = f'MAX{ch}'
    return [cp(f'C{first}', (x0, y), ('local', f'{n}_C1+'), ('local', f'{n}_C1-')),
            cp(f'C{first + 1}', (x0 + 15.24, y), ('local', f'{n}_C2+'), ('local', f'{n}_C2-')),
            cp(f'C{first + 2}', (x0 + 30.48, y), ('local', f'{n}_VS+'), ('power', 'GND')),
            cp(f'C{first + 3}', (x0 + 45.72, y), ('power', 'GND'), ('local', f'{n}_VS-'), rot=180),
            cp(f'C{first + 4}', (x0 + 60.96, y), ('power', '+5V'), ('power', 'GND'))]


def db25(ref, name, value, footprint, at, wired):
    pins = {str(n): 'nc' for n in range(1, 26)}
    pins.update(wired)
    return dict(ref=ref, lib='Connector', name=name, value=value, footprint=footprint, at=at,
                description='DB-25 connector', pins=pins)


def pullup(ref, x, y, net):
    return dict(ref=ref, lib='Device', name='R', value='10k', footprint=RES, at=(x, y),
                description='Resistor', pins={'1': ('power', '+5V'), '2': ('global', net, 'passive')})


PARTS = [
    max232('U36', 'A', (60.96, 55.88), t1in='UART_TX_A', r1out='UART_RX_A', t2in='~{RTS_A}', r2out='~{CTS_A}',
           t1out='RS232_A_RXD', r1in='RS232_A_TXD', t2out='RS232_A_CTS', r2in='RS232_A_RTS'),
    db25('J18', 'DB25_Socket', 'CONSOLE DB-25F DCE', DB25F, (147.32, 55.88),
         {'2': ('local', 'RS232_A_TXD'), '3': ('local', 'RS232_A_RXD'), '4': ('local', 'RS232_A_RTS'),
          '5': ('local', 'RS232_A_CTS'), '6': ('local', 'CONSOLE_DTR'), '7': ('power', 'GND'),
          '8': ('local', 'CONSOLE_DTR'), '20': ('local', 'CONSOLE_DTR')}),
    max232('U37', 'B', (60.96, 146.05), t1in='UART_TX_B', r1out='UART_RX_B', t2in='~{RTS_B}', r2out='~{CTS_B}',
           t1out='RS232_B_TXD', r1in='RS232_B_RXD', t2out='RS232_B_RTS', r2in='RS232_B_CTS'),
    db25('J19', 'DB25_Pins', 'MODEM DB-25M DTE', DB25M, (147.32, 146.05),
         {'2': ('local', 'RS232_B_TXD'), '3': ('local', 'RS232_B_RXD'), '4': ('local', 'RS232_B_RTS'),
          '5': ('local', 'RS232_B_CTS'), '6': ('local', 'MODEM_DTR'), '7': ('power', 'GND'),
          '8': ('local', 'RS232_B_DCD'), '20': ('local', 'MODEM_DTR'), '22': ('local', 'RS232_B_RI')}),
    dict(ref='U38', lib='Griffin', name='SN75189', value='SN75189', footprint=DIP14, at=(215.9, 119.38),
         datasheet='https://www.ti.com/lit/ds/symlink/sn75189.pdf', description='Quad RS-232 line receiver',
         pins={'1': ('local', 'RS232_B_DCD'), '3': ('global', 'DCD_B', 'output'),
               '4': ('local', 'RS232_B_RI'), '6': ('global', 'RI_B', 'output'),
               '10': ('power', 'GND'), '13': ('power', 'GND'), '8': 'nc', '11': 'nc',
               '2': 'nc', '5': 'nc', '9': 'nc', '12': 'nc',
               '14': ('power', '+5V'), '7': ('power', 'GND')}),
    dict(ref='C83', lib='Device', name='C', value='100nF', footprint=CDISC, at=(254.0, 119.38),
         description='Unpolarized capacitor', pins={'1': ('power', '+5V'), '2': ('power', 'GND')}),
] + charge_pump_caps(70, 'A', 180.34, 33.02) + charge_pump_caps(75, 'B', 180.34, 73.66) + [
    pullup(f'R{104 + i}', 172.72 + 12.7 * i, 149.86, net)
    for i, net in enumerate(['UART_RX_A', 'UART_RX_B', '~{CTS_A}', '~{CTS_B}', 'DCD_B', 'RI_B'])]

TEXTS = [('RS-232 level stages (griffin.yml interfaces, RS-232 level stage).\\n'
          'Ch A console: DB-25 female wired DCE; terminal TXD (2) and RTS (4) are received, TXDA and ~RTS_A\\n'
          'drive pins 3 (RXD) and 5 (CTS); DSR (6) and DCD (8) strapped to the terminal DTR (20).\\n'
          'Ch B modem: DB-25 male wired DTE; TXDB -> 2, 3 -> RXDB, ~RTS_B -> 4, 5 -> ~CTS_B,\\n'
          'DCD (8) and RI (22) through the 75189 to IP4/IP5 (unused 75189 inputs grounded); DTR (20) strapped to DSR (6).\\n'
          'Local net names follow the DB-25 pin signal (DTE-referenced).  Pin 7 signal ground.\\n'
          'The TTL headers share these nets (io sheet, 1k series); 10k pull-ups hold RXD, ~CTS, DCD_B and RI_B\\n'
          'idle when the level stages are unstuffed.  No isolation jumpers.', 20.32, 203.2)]
