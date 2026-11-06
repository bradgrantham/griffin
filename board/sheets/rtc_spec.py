"""rtc.kicad_sch: MCP7940N real-time clock on DUART port pins (griffin.yml DUART entry).

SCL = OP2 push-pull; SDA pulled low by Q2 when OP3's pin is low-asserted
(RTC_SDA_DRIVE), read back on IP2; MFP -> IP3.  Pull-up values and the crystal
load caps are assumptions, not griffin.yml facts.
"""
DIP8 = 'Package_DIP:DIP-8_W7.62mm'
RES = 'Resistor_THT:R_Axial_DIN0207_L6.3mm_D2.5mm_P10.16mm_Horizontal'
CDISC = 'Capacitor_THT:C_Disc_D5.0mm_W2.5mm_P5.00mm'
PWR_START = 570


def r(ref, value, x, y, net, shape):
    return dict(ref=ref, lib='Device', name='R', value=value, footprint=RES, at=(x, y), description='Resistor',
                pins={'1': ('power', '+5V'), '2': ('global', net, shape)})


def c(ref, value, x, y, top):
    return dict(ref=ref, lib='Device', name='C', value=value, footprint=CDISC, at=(x, y),
                description='Unpolarized capacitor', pins={'1': top, '2': ('power', 'GND')})


PARTS = [
    dict(ref='U39', lib='Timer_RTC', name='MCP7940N-xP', value='MCP7940N-I/P', footprint=DIP8, at=(101.6, 96.52),
         datasheet='https://ww1.microchip.com/downloads/en/DeviceDoc/20005010F.pdf',
         description='I2C real-time clock/calendar with SRAM and battery switchover',
         pins={'6': ('global', 'RTC_SCL', 'input'), '5': ('global', 'RTC_SDA', 'bidirectional'),
               '7': ('global', 'RTC_MFP', 'output'), '8': ('power', '+5V'), '4': ('power', 'GND'),
               '3': ('local', 'VBAT'), '1': ('local', 'XTAL1'), '2': ('local', 'XTAL2')}),
    dict(ref='Y6', lib='Device', name='Crystal_Small', value='32.768kHz CL 6-9pF',
         footprint='Crystal:Crystal_C26-LF_D2.1mm_L6.5mm_Vertical', at=(144.78, 86.36),
         description='32.768 kHz tuning-fork crystal, load capacitance 6-9 pF',
         pins={'1': ('local', 'XTAL1'), '2': ('local', 'XTAL2')}),
    c('C80', '2*(CL-Cs) TBD', 134.62, 109.22, ('local', 'XTAL1')),
    c('C81', '2*(CL-Cs) TBD', 160.02, 109.22, ('local', 'XTAL2')),
    dict(ref='BT1', lib='Device', name='Battery_Cell', value='CR2032',
         footprint='Battery:BatteryHolder_Keystone_3034_1x20mm', at=(68.58, 132.08),
         description='Coin cell, RTC backup', pins={'1': ('local', 'VBAT'), '2': ('power', 'GND')}),
    dict(ref='Q2', lib='Transistor_FET', name='2N7000', value='2N7000', footprint='Package_TO_SOT_THT:TO-92_Inline',
         at=(55.88, 60.96), description='N-channel MOSFET: pulls RTC_SDA low when RTC_SDA_DRIVE is high',
         pins={'2': ('global', 'RTC_SDA_DRIVE', 'input'), '3': ('global', 'RTC_SDA', 'bidirectional'),
               '1': ('power', 'GND')}),
    r('R110', '4.7k', 144.78, 50.8, 'RTC_SDA', 'bidirectional'),
    r('R111', '4.7k', 160.02, 50.8, 'RTC_SCL', 'input'),
    r('R112', '10k', 175.26, 50.8, 'RTC_MFP', 'input'),
    c('C82', '100nF', 195.58, 50.8, ('power', '+5V')),
]

TEXTS = [('SCL = DUART OP2 (push-pull: the MCP7940N is the only I2C device and does not clock-stretch);\\n'
          'SDA driven low through Q2 by OP3 (OPR_SET drives the pin LOW, FET inverts again), read back on IP2;\\n'
          'MFP -> IP3, firmware programs the 1 Hz square wave.  Pull-up values are assumptions, not in griffin.yml.',
          25.4, 165.1)]
