"""audio.kicad_sch: the audio pipeline's FIFO write-data buffers (griffin.yml interfaces, Stream FIFO parts)."""
DIP20 = 'Package_DIP:DIP-20_W7.62mm'
CDISC = 'Capacitor_THT:C_Disc_D5.0mm_W2.5mm_P5.00mm'
PWR_START = 400


def buf(ref, at, rot, lo):
    pins = {'1': ('power', 'GND'), '19': ('power', 'GND'), '20': ('power', '+5V'), '10': ('power', 'GND')}
    for i in range(8):
        pins[str(2 + i)] = ('global', f'D{lo + i}', 'input')
        pins[str(18 - i)] = ('global', f'D_AUDIO_FIFO{lo + i}', 'output')
    return dict(ref=ref, lib='74xx', name='74AHCT541', value='74AHCT541', footprint=DIP20, at=at, rot=rot, pins=pins)


def bypass(ref, at):
    return dict(ref=ref, lib='Device', name='C', value='100nF', footprint=CDISC, at=at,
                pins={'1': ('power', '+5V'), '2': ('power', 'GND')})


PARTS = [
    buf('U45', (185.42, 119.38), 0, 0),
    buf('U46', (40.64, 165.1), 0, 8),
    bypass('C50', (241.3, 119.38)),
    bypass('C51', (88.9, 165.1)),
]
TEXTS = [('Audio FIFO write buffers U45, U46: AHCT, not HCT;\\n'
          'write-data path only (griffin.yml Stream FIFO parts).\\n'
          'The main bus does not run to the audio section.', 101.6, 182.88)]
