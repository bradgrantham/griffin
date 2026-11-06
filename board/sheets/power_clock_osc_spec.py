"""power-clock.kicad_sch additions: pixel oscillator Y3 (PIXEL_CLK) and optional Y4 (CLK2), EN pull-ups, bypass."""
OSCFP = 'Oscillator:Oscillator_DIP-8'
RFP = 'Resistor_THT:R_Axial_DIN0207_L6.3mm_D2.5mm_P10.16mm_Horizontal'
CAPFP = 'Capacitor_THT:C_Disc_D5.0mm_W2.5mm_P5.00mm'
PWR_START = 370


def osc(ref, value, y, net, rref, cref):
    en = f'{ref}_EN'
    return [
        dict(ref=ref, lib='Oscillator', name='CXO_DIP8', value=value, footprint=OSCFP, at=(45.72, y),
             pins={'1': ('local', en), '5': ('global', net, 'output'),
                   '8': ('power', '+5V'), '4': ('power', 'GND')}),
        dict(ref=rref, lib='Device', name='R', value='100K', footprint=RFP, at=(20.32, y - 5.08),
             pins={'1': ('power', '+5V'), '2': ('local', en)}),
        dict(ref=cref, lib='Device', name='C', value='100nF', footprint=CAPFP, at=(78.74, y),
             pins={'1': ('power', '+5V'), '2': ('power', 'GND')}),
    ]


PARTS = osc('Y3', '25.175MHz', 83.82, 'PIXEL_CLK', 'R20', 'C19') + \
    osc('Y4', 'OSC2 (optional, DNP)', 116.84, 'CLK2', 'R21', 'C20')
TEXTS = [('Datasheet oscillators are NC on pin 1 but some of my oscillators in hand are\\n'
          'ENB on pin 1, so pull-up as a hedge.  Y4 is the optional second oscillator on\\n'
          'PIXEL GCLK2 (pin 2), unstuffed by default.', 63.5, 142.24)]
