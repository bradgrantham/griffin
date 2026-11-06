"""Series 0R footprints on the PIXELS, VIDCMD and AUDCTL FIFO /W strobes
(griffin.yml "FIFO control strobes").  A ganged pair shares one resistor: both
chips' /W pins carry the local *_FIFO_SIDE net (relabel_pins.py), the resistor
joins it to ENGINE's global strobe.  Run with --sheet to pick the part."""
import os

RES = dict(lib='Device', name='R', value='0R',
           footprint='Resistor_THT:R_Axial_DIN0207_L6.3mm_D2.5mm_P10.16mm_Horizontal')

def series(ref, net, at):
    return dict(ref=ref, at=at, rot=90, **RES,
                pins={'1': ('global', f'~{{{net}}}', 'input'),
                      '2': ('local', f'~{{{net}_FIFO_SIDE}}')})

ALL = {
    'pixel.kicad_sch':      [series('R155', 'PIXELS_FIFO_W', (170.18, 27.94))],
    'compositor.kicad_sch': [series('R156', 'VIDCMD_FIFO_W', (170.18, 27.94))],
    'audio.kicad_sch':      [series('R157', 'AUDCTL_FIFO_W', (170.18, 27.94))],
}
PARTS = ALL[os.environ['FIFO_W_SHEET']]
PWR_START = 710
