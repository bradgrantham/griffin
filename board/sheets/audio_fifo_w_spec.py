"""Series 0R footprints on the four sample-FIFO /W strobes (griffin.yml "FIFO
control strobes": every 7200 /W and /RE line has a series axial footprint,
stuffed 0R).  The FIFO pin itself is relabelled to the local *_FIFO_SIDE net
first (relabel_pins.py); this spec adds the resistor between that local net and
ENGINE's global strobe."""

RES = dict(lib='Device', name='R', value='0R',
           footprint='Resistor_THT:R_Axial_DIN0207_L6.3mm_D2.5mm_P10.16mm_Horizontal')

def series(ref, n, at):
    return dict(ref=ref, at=at, rot=90, **RES,
                pins={'1': ('global', f'~{{SAMPLES{n}_FIFO_W}}', 'input'),
                      '2': ('local', f'~{{SAMPLES{n}_FIFO_W_FIFO_SIDE}}')})

PARTS = [
    series('R150', 0, (71.12, 27.94)),
    series('R151', 1, (71.12, 99.06)),
    series('R152', 2, (185.42, 27.94)),
    series('R153', 3, (185.42, 99.06)),
]
PWR_START = 700
