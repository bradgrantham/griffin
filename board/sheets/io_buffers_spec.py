"""glue.kicad_sch additions: the IO data buffers U49/U50 (74AHCT245 pair), their
D_IO pull-up networks R158/R159 and bypass C92/C93.

The pair isolates the IO segment's data pins (GLUE, CF, DUART, PORTS; nets
D_IO0..15) from the main bus D0..15.  A side = D_IO, B side = D, so DIR =
R/~W gives A->B (peripheral to bus) on a read.  /OE = ~IO_BUFFER_OE from GLUE
pin 64, low for every bus cycle in 0xF00000-0xFFFFFF (glue.v).  The D_IO
branch floats whenever the buffers are disabled, which is most of the time,
so each line has a 10k pull-up (griffin.yml "IO data buffers" interfaces
entry; griffin.log 2026-11-07)."""
DIP20 = 'Package_DIP:DIP-20_W7.62mm'
CAPFP = 'Capacitor_THT:C_Disc_D5.0mm_W2.5mm_P5.00mm'
SIP9 = 'Resistor_THT:R_Array_SIP9'
PWR_START = 700

A = ['2', '3', '4', '5', '6', '7', '8', '9']          # A1..A8: D_IO side
B = ['18', '17', '16', '15', '14', '13', '12', '11']  # B1..B8: main bus side


def buf(ref, y, lo):
    pins = {'1': ('global', 'R{slash}~{W}', 'input'),        # DIR: high = A->B = read
            '19': ('global', '~{IO_BUFFER_OE}', 'input'),
            '20': ('power', '+5V'), '10': ('power', 'GND')}
    for i in range(8):
        pins[A[i]] = ('global', f'D_IO{lo + i}', 'bidirectional')
        pins[B[i]] = ('global', f'D{lo + i}', 'bidirectional')
    return dict(ref=ref, lib='74xx', name='74HC245', value='74AHCT245', footprint=DIP20,
                at=(226.06, y), pins=pins)


def pullups(ref, y, lo):
    pins = {'1': ('power', '+5V')}
    for i in range(8):
        pins[str(2 + i)] = ('global', f'D_IO{lo + i}', 'passive')
    return dict(ref=ref, lib='Device', name='R_Network08', value='10k', footprint=SIP9,
                at=(190.5, y), pins=pins)


def cap(ref, x, y):
    return dict(ref=ref, lib='Device', name='C', value='100nF', footprint=CAPFP, at=(x, y),
                pins={'1': ('power', '+5V'), '2': ('power', 'GND')})


PARTS = [
    buf('U49', 50.8, 0),
    buf('U50', 111.76, 8),
    pullups('R158', 50.8, 0),
    pullups('R159', 111.76, 8),
    cap('C92', 215.9, 170.18),
    cap('C93', 228.6, 170.18),
]
TEXTS = [('IO data buffers: 74AHCT245 pair, A = D_IO (GLUE, CF, DUART, PORTS),\\n'
          'B = main bus D; DIR = R/~W, /OE = ~IO_BUFFER_OE from GLUE (low for\\n'
          'any cycle in 0xF00000-0xFFFFFF).  10k pull-ups hold D_IO when the\\n'
          'buffers are disabled (griffin.yml "IO data buffers").', 180.34, 185.42)]
