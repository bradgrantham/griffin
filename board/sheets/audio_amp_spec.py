"""audio-amp.kicad_sch: volume pot header, headphone LM386s, speaker LM386 (griffin.yml interfaces, Audio output stage)."""
DIP8 = 'Package_DIP:DIP-8_W7.62mm'
RFP = 'Resistor_THT:R_Axial_DIN0207_L6.3mm_D2.5mm_P10.16mm_Horizontal'
FILM = 'Capacitor_THT:C_Rect_L7.2mm_W2.5mm_P5.00mm'
CDISC = 'Capacitor_THT:C_Disc_D5.0mm_W2.5mm_P5.00mm'
CP8 = 'Capacitor_THT:CP_Radial_D8.0mm_P3.50mm'
PWR_START = 450
G, P5 = ('power', 'GND'), ('power', '+5V')


def two(ref, lib, name, value, fp, at, p1, p2):
    return dict(ref=ref, lib=lib, name=name, value=value, footprint=fp, at=at, pins={'1': p1, '2': p2})


def res(ref, value, at, p1, p2):
    return two(ref, 'Device', 'R', value, RFP, at, p1, p2)


def lm386(ref, at, inp, out):
    return dict(ref=ref, lib='Amplifier_Audio', name='LM386', value='LM386', footprint=DIP8, at=at,
                pins={'3': ('local', inp), '2': G, '6': P5, '4': G, '7': 'nc', '1': 'nc', '8': 'nc',
                      '5': ('local', out)})


def headphone(side, y, rpot, rpad, ramp, rzob, czob, cout):
    pot, hin, amp, zob = f'POT_{side}', f'HP_IN_{side}', f'HP_AMP_{side}', f'ZOB_{side}'
    return [
        res(rpot, '10k', (76.2, y), ('local', pot), ('local', hin)),
        res(rpad, '1k', (88.9, y), ('local', hin), G),
        lm386(ramp, (119.38, y), hin, amp),
        res(rzob, '10R', (144.78, y), ('local', amp), ('local', zob)),
        two(czob, 'Device', 'C', '47nF', FILM, (157.48, y), ('local', zob), G),
        two(cout, 'Device', 'C_Polarized', '250uF', CP8, (170.18, y), ('local', amp), ('global', f'HP_{side}', 'passive')),
    ]


PARTS = [
    dict(ref='J16', lib='Connector_Generic', name='Conn_01x06', value='VOLUME POT (dual 10kA, panel)',
         footprint='Connector_PinHeader_2.54mm:PinHeader_1x06_P2.54mm_Vertical', at=(40.64, 63.5), rot=180,
         pins={'1': ('global', 'LINE_L', 'passive'), '2': ('local', 'POT_L'), '3': G,
               '4': ('global', 'LINE_R', 'passive'), '5': ('local', 'POT_R'), '6': G}),
]
PARTS += headphone('L', 38.1, 'R91', 'R92', 'U33', 'R93', 'C57', 'C58')
PARTS += headphone('R', 91.44, 'R94', 'R95', 'U34', 'R96', 'C59', 'C60')
PARTS += [
    dict(ref='J6', lib='Connector_Audio', name='AudioJack3_SwitchTR',
         value='SJ1-3525NG',   # Same Sky 3.5 mm stereo, tip and ring NC switches (pins 4, 5)
         footprint='Connector_Audio:Jack_3.5mm_CUI_SJ1-3525N_Horizontal', at=(40.64, 177.8),
         pins={'S': G, 'T': ('global', 'HP_L', 'passive'), 'R': ('global', 'HP_R', 'passive'),
               'TN': ('local', 'SPK_IN_L'), 'RN': ('local', 'SPK_IN_R')}),
    res('R97', '47k', (76.2, 144.78), ('local', 'SPK_IN_L'), ('local', 'SPK_SUM')),
    res('R98', '47k', (88.9, 144.78), ('local', 'SPK_IN_R'), ('local', 'SPK_SUM')),
    res('R99', '1k', (101.6, 144.78), ('local', 'SPK_SUM'), G),
    lm386('U35', (129.54, 144.78), 'SPK_SUM', 'SPK_AMP'),
    res('R154', '10R', (152.4, 144.78), ('local', 'SPK_AMP'), ('local', 'ZOB_SPK')),
    two('C61', 'Device', 'C', '47nF', FILM, (165.1, 144.78), ('local', 'ZOB_SPK'), G),
    two('C62', 'Device', 'C_Polarized', '250uF', CP8, (177.8, 144.78), ('local', 'SPK_AMP'), ('local', 'SPK_PLUS')),
    dict(ref='J17', lib='Connector_Generic', name='Conn_01x02', value='SPEAKER 8 ohm (case)',
         footprint='Connector_PinHeader_2.54mm:PinHeader_1x02_P2.54mm_Vertical', at=(205.74, 144.78),
         pins={'1': ('local', 'SPK_PLUS'), '2': G}),
    two('C65', 'Device', 'C', '100nF', CDISC, (200.66, 38.1), P5, G),
    two('C66', 'Device', 'C', '100nF', CDISC, (213.36, 38.1), P5, G),
    two('C67', 'Device', 'C', '100nF', CDISC, (226.06, 38.1), P5, G),
]
TEXTS = [
    ('J16: dual-gang 10k audio taper, element top = LINE,\\nbottom = GND, wiper out; panel part on a header.', 190.5, 76.2),
    ('Inserting a plug opens TN/RN and mutes the speaker;\\nsoftware mute = VOLUME 0 on all four channels (griffin.yml).', 190.5, 101.6),
    ('C65, C66, C67 bypass U33, U34, U35.\\nR? (speaker Zobel 10R): no free reference in R80-R99.', 190.5, 121.92),
]
