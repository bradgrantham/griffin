"""audio-dac.kicad_sch: R-2R ladders, 74HC4066 gates, LM358 summers, line out (griffin.yml interfaces, Audio output stage)."""
DIP8 = 'Package_DIP:DIP-8_W7.62mm'
DIP14 = 'Package_DIP:DIP-14_W7.62mm'
RFP = 'Resistor_THT:R_Axial_DIN0207_L6.3mm_D2.5mm_P10.16mm_Horizontal'
FILM = 'Capacitor_THT:C_Rect_L7.2mm_W2.5mm_P5.00mm'
CDISC = 'Capacitor_THT:C_Disc_D5.0mm_W2.5mm_P5.00mm'
CP5 = 'Capacitor_THT:CP_Radial_D5.0mm_P2.00mm'
CP63 = 'Capacitor_THT:CP_Radial_D6.3mm_P2.50mm'
PWR_START = 420


def two(ref, lib, name, value, fp, at, p1, p2):
    return dict(ref=ref, lib=lib, name=name, value=value, footprint=fp, at=at, pins={'1': p1, '2': p2})


def res(ref, value, at, p1, p2):
    return two(ref, 'Device', 'R', value, RFP, at, p1, p2)


def ladder(ref, n, at):
    pins = {'1': ('local', f'LADDER_{n}'), '10': ('power', 'GND')}
    for b in range(8):
        pins[str(9 - b)] = ('global', f'SAMPLE{n}_Q{b}', 'input')     # pin 2 = Q7 (MSB) ... pin 9 = Q0
    return dict(ref=ref, lib='Griffin', name='4610X-R2R-103LF', value='4610X-R2R-103LF',
                footprint='board:BO_4610X', at=at, pins=pins)


# 4066 unit -> (side A, side B, control)
SW = {1: ('1', '2', '13'), 2: ('4', '3', '5'), 3: ('8', '9', '6'), 4: ('11', '10', '12')}


def switch(n, at):
    a, b, c = SW[n + 1]
    return dict(ref='U32', lib='4xxx', name='4066', value='74HC4066', footprint=DIP14, unit=n + 1, at=at,
                pins={a: ('local', f'LADDER_{n}'), b: ('local', 'SUM_L' if n < 2 else 'SUM_R'),
                      c: ('global', f'GATE{n}', 'input')})


ROWS = [35.56, 76.2, 116.84, 157.48]
G, P5 = ('power', 'GND'), ('power', '+5V')
B = 12.7      # passive column pitch
PARTS = [ladder(r, n, (20.32, ROWS[n])) for n, r in enumerate(['R22', 'R80', 'R81', 'R82'])]
PARTS += [switch(n, (121.92, ROWS[n] + 5.08)) for n in range(4)]
PARTS += [
    dict(ref='U32', lib='4xxx', name='4066', value='74HC4066', footprint=DIP14, unit=5, at=(180.34, 144.78),
         pins={'14': P5, '7': G}),
    dict(ref='U17', lib='Amplifier_Operational', name='LM358', value='LM358', footprint=DIP8, unit=1,
         at=(175.26, 33.02), pins={'3': ('local', 'VBIAS'), '2': ('local', 'SUM_L'), '1': ('local', 'AMP_L')}),
    dict(ref='U17', lib='Amplifier_Operational', name='LM358', value='LM358', footprint=DIP8, unit=2,
         at=(233.68, 33.02), pins={'5': ('local', 'VBIAS'), '6': ('local', 'SUM_R'), '7': ('local', 'AMP_R')}),
    dict(ref='U17', lib='Amplifier_Operational', name='LM358', value='LM358', footprint=DIP8, unit=3,
         at=(165.1, 144.78), pins={'8': P5, '4': G}),
    # row B: bias divider, feedback
    res('R83', '10k', (160.02, 68.58), P5, ('local', 'VBIAS')),
    res('R84', '10k', (160.02 + B, 68.58), ('local', 'VBIAS'), G),
    two('C52', 'Device', 'C_Polarized', '10uF', CP5, (160.02 + 2 * B, 68.58), ('local', 'VBIAS'), G),
    res('R85', '2k', (160.02 + 3 * B, 68.58), ('local', 'SUM_L'), ('local', 'AMP_L')),
    two('C53', 'Device', 'C', '8.2nF', FILM, (160.02 + 4 * B, 68.58), ('local', 'SUM_L'), ('local', 'AMP_L')),
    res('R86', '2k', (160.02 + 5 * B, 68.58), ('local', 'SUM_R'), ('local', 'AMP_R')),
    two('C54', 'Device', 'C', '8.2nF', FILM, (160.02 + 6 * B, 68.58), ('local', 'SUM_R'), ('local', 'AMP_R')),
    # row C: class-A bias, coupling, series, bypass
    res('R87', '4.7k', (160.02, 106.68), ('local', 'AMP_L'), G),
    res('R88', '4.7k', (160.02 + B, 106.68), ('local', 'AMP_R'), G),
    two('C55', 'Device', 'C_Polarized', '100uF', CP63, (160.02 + 2 * B, 106.68),
        ('local', 'AMP_L'), ('global', 'LINE_L', 'passive')),
    two('C56', 'Device', 'C_Polarized', '100uF', CP63, (160.02 + 3 * B, 106.68),
        ('local', 'AMP_R'), ('global', 'LINE_R', 'passive')),
    res('R89', '100R', (160.02 + 4 * B, 106.68), ('global', 'LINE_L', 'passive'), ('local', 'RCA_L')),
    res('R90', '100R', (160.02 + 5 * B, 106.68), ('global', 'LINE_R', 'passive'), ('local', 'RCA_R')),
    two('C63', 'Device', 'C', '100nF', CDISC, (160.02 + 6 * B, 106.68), P5, G),
    two('C64', 'Device', 'C', '100nF', CDISC, (160.02 + 7 * B, 106.68), P5, G),
    # row D: RCA jacks
    dict(ref='J14', lib='Connector', name='Conn_Coaxial', value='RCA LINE OUT L', footprint='board:CUI_RCJ-044',
         at=(215.9, 142.24), pins={'1': ('local', 'RCA_L'), '2': G}),
    dict(ref='J15', lib='Connector', name='Conn_Coaxial', value='RCA LINE OUT R', footprint='board:CUI_RCJ-044',
         at=(248.92, 142.24), pins={'1': ('local', 'RCA_R'), '2': G}),
]
TEXTS = [('Offset-binary samples: 0x80 = 2.5 V = silence; no inverter, no pulls on Q.\\n'
          'Summer biased at VBIAS 2.5 V, 2k || 8.2 nF (pole ~9.7 kHz), 4.7k class-A bias, 100 uF to LINE.\\n'
          'Line out fixed level ~0.88 Vrms.  Not yet breadboarded (griffin.yml issues).\\n'
          'C63 bypasses U32, C64 bypasses U17.', 20.32, 190.5)]
