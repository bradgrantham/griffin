"""pixel.kicad_sch additions: palette RAM (U40-U42), bypass, AUDIO_TICK test point, PIXEL_SPARE header."""
DIP16 = 'Package_DIP:DIP-16_W7.62mm'
CAPFP = 'Capacitor_THT:C_Disc_D5.0mm_W2.5mm_P5.00mm'
PWR_START = 300


def ram(ref, y, lo):
    pins = {'1': ('global', 'IDX0', 'input'), '15': ('global', 'IDX1', 'input'),
            '14': ('global', 'IDX2', 'input'), '13': ('global', 'IDX3', 'input'),
            '2': ('power', 'GND'), '3': ('global', '~{CLUT_WE}', 'input'),
            '16': ('power', '+5V'), '8': ('power', 'GND')}
    for i, (d, q) in enumerate([('4', '5'), ('6', '7'), ('10', '9'), ('12', '11')]):
        pins[d] = ('global', f'set_pix_value{lo + i}', 'input')
        pins[q] = ('global', f'RGB_IN{lo + i}', 'output')
    return dict(ref=ref, lib='Griffin', name='RAM_16x4_74x219', value='74F219 (part TBD)',
                footprint=DIP16, at=(45.72, y), pins=pins)


def cap(ref, x, y):
    return dict(ref=ref, lib='Device', name='C', value='100nF', footprint=CAPFP, at=(x, y),
                pins={'1': ('power', '+5V'), '2': ('power', 'GND')})


SPARE = [30, 34, 37, 50, 54, 55, 63, 64, 65, 73, 74, 75, 76, 77, 79, 80, 81]
jpins = {str(i + 1): ('local', f'PIXEL_SPARE_{p}') for i, p in enumerate(SPARE)}
jpins.update({'18': ('power', 'GND'), '19': ('power', 'GND'), '20': ('power', 'GND')})

PARTS = [
    ram('U40', 35.56, 0),
    ram('U41', 81.28, 4),
    ram('U42', 127.0, 8),
    cap('C42', 125.73, 165.1),
    cap('C43', 138.43, 165.1),
    cap('C44', 151.13, 165.1),
    dict(ref='TP8', lib='Connector', name='TestPoint', value='AUDIO_TICK',
         footprint='TestPoint:TestPoint_Pad_D1.5mm', at=(81.28, 177.8),
         pins={'1': ('global', 'AUDIO_TICK', 'passive')}),
    dict(ref='J23', lib='Connector_Generic', name='Conn_02x10_Odd_Even', value='PIXEL_SPARE',
         footprint='Connector_PinHeader_2.54mm:PinHeader_2x10_P2.54mm_Vertical', at=(38.1, 185.42),
         pins=jpins),
]
TEXTS = [('Palette RAM: part not chosen (74F219 pinout;\\ntAA and ICC are the selection criteria, griffin.yml issues).',
          96.52, 189.23)]
