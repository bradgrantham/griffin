"""vga.kicad_sch: R4G4B4 output stage -- 2x 74AC541 buffers, weighted-resistor DACs, sync series R, DE-15."""
DIP20 = 'Package_DIP:DIP-20_W7.62mm'
CAPFP = 'Capacitor_THT:C_Disc_D5.0mm_W2.5mm_P5.00mm'
RFP = 'Resistor_THT:R_Axial_DIN0207_L6.3mm_D2.5mm_P10.16mm_Horizontal'
PWR_START = 340

A = ['2', '3', '4', '5', '6', '7', '8', '9']
Y = ['18', '17', '16', '15', '14', '13', '12', '11']


def buf(ref, y, ins, outs):
    pins = {'1': ('power', 'GND'), '19': ('power', 'GND'), '20': ('power', '+5V'), '10': ('power', 'GND')}
    for i in range(8):
        pins[A[i]] = ins[i]
        pins[Y[i]] = outs[i]
    return dict(ref=ref, lib='74xx', name='74AHC541', value='74AC541', footprint=DIP20,
                at=(50.8, y), pins=pins)


def g(net):
    return ('global', net, 'input')


def loc(net):
    return ('local', net)


def res(ref, value, x, y, left, right):
    return dict(ref=ref, lib='Device', name='R', value=value, footprint=RFP, at=(x, y), rot=90,
                pins={'1': left, '2': right})


def cap(ref, x, y):
    return dict(ref=ref, lib='Device', name='C', value='100nF', footprint=CAPFP, at=(x, y),
                pins={'1': ('power', '+5V'), '2': ('power', 'GND')})


U47 = buf('U47', 50.8,
          [g(f'RGB_OUT{n}') for n in (11, 10, 9, 8, 7, 6, 5, 4)],
          [loc(n) for n in ('VR3', 'VR2', 'VR1', 'VR0', 'VG3', 'VG2', 'VG1', 'VG0')])
U48 = buf('U48', 111.76,
          [g('RGB_OUT3'), g('RGB_OUT2'), g('RGB_OUT1'), g('RGB_OUT0'), g('VGA_HSYNC'), g('VGA_VSYNC'),
           ('power', 'GND'), ('power', 'GND')],
          [loc('VB3'), loc('VB2'), loc('VB1'), loc('VB0'), loc('HS_BUF'), loc('VS_BUF'), 'nc', 'nc'])


def ladder(colour, first, x):
    """bit3 = 2x 1k parallel, bit2 = 1k, bit1 = 2x 1k series, bit0 = 3.9k, shunt 100R."""
    out = f'VGA_{colour}'
    rows = [('1k', f'V{colour}3', out), ('1k', f'V{colour}3', out), ('1k', f'V{colour}2', out),
            ('1k', f'V{colour}1', f'V{colour}1_MID'), ('1k', f'V{colour}1_MID', out),
            ('3.9k', f'V{colour}0', out), ('100R', out, None)]
    parts = []
    for i, (val, a, b) in enumerate(rows):
        right = ('power', 'GND') if b is None else loc(b)
        parts.append(res(f'R{first + i}', val, x, 38.1 + 10.16 * i, loc(a), right))
    return parts


J22 = dict(ref='J22', lib='HD15', name='ICD15S13E4GX00LF', value='DE-15 VGA',
           footprint='board:AMPHENOL_ICD15S13E4GX00LF', at=(241.3, 63.5),
           pins={'1': loc('VGA_R'), '2': loc('VGA_G'), '3': loc('VGA_B'),
                 '13': loc('HS_OUT'), '14': loc('VS_OUT'),
                 '5': ('power', 'GND'), '6': ('power', 'GND'), '7': ('power', 'GND'),
                 '8': ('power', 'GND'), '10': ('power', 'GND'),
                 'S1': ('power', 'GND'), 'S2': ('power', 'GND'),
                 '4': 'nc', '9': 'nc', '11': 'nc', '12': 'nc', '15': 'nc'})

PARTS = ([U47, U48] + ladder('R', 50, 104.14) + ladder('G', 57, 144.78) + ladder('B', 64, 185.42)
         + [res('R71', '100R', 104.14, 116.84, loc('HS_BUF'), loc('HS_OUT')),
            res('R72', '100R', 104.14, 127.0, loc('VS_BUF'), loc('VS_OUT')),
            cap('C47', 25.4, 165.1), cap('C48', 38.1, 165.1), J22])
TEXTS = [('Ladders and shunts at the DE-15, not at the buffers; 12 DAC traces short\\n'
          'and matched (griffin.yml VGA entry).  yml BOM says 21x 1k; this topology uses 15.',
          88.9, 144.78)]
