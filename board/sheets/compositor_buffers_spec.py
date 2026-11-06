"""compositor.kicad_sch additions: video FIFO write-data buffers U43/U44 and bypass C45/C46."""
DIP20 = 'Package_DIP:DIP-20_W7.62mm'
CAPFP = 'Capacitor_THT:C_Disc_D5.0mm_W2.5mm_P5.00mm'
PWR_START = 320

A = ['2', '3', '4', '5', '6', '7', '8', '9']
Y = ['18', '17', '16', '15', '14', '13', '12', '11']


def buf(ref, y, lo):
    pins = {'1': ('power', 'GND'), '19': ('power', 'GND'), '20': ('power', '+5V'), '10': ('power', 'GND')}
    for i in range(8):
        pins[A[i]] = ('global', f'D{lo + i}', 'input')
        pins[Y[i]] = ('global', f'D_VIDEO_FIFO{lo + i}', 'output')
    return dict(ref=ref, lib='74xx', name='74AHCT541', value='74AHCT541', footprint=DIP20,
                at=(38.1, y), pins=pins)


def cap(ref, x, y):
    return dict(ref=ref, lib='Device', name='C', value='100nF', footprint=CAPFP, at=(x, y),
                pins={'1': ('power', '+5V'), '2': ('power', 'GND')})


PARTS = [
    buf('U43', 43.18, 0),
    buf('U44', 99.06, 8),
    cap('C45', 25.4, 160.02),
    cap('C46', 38.1, 160.02),
]
TEXTS = [('Video FIFO write buffers: AHCT, not HCT (the buffer delay comes out of\\n'
          'data setup at the 7200 /W edge); write-data path only, /W /RS /R /EF Q\\n'
          'unbuffered (griffin.yml Stream FIFO parts).', 20.32, 185.42)]
