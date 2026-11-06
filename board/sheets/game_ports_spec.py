"""place_parts.py spec: PANEL sheet game ports -- 2x Atari DE-9 (J20/J21), ESD, pull-ups, port-1 paddles.

Each switch line: DE-9 pin -> local JSp_n -> 100R -> global JOYSTICK_p_NAME (to PORTS).
Clamps (1N4148, JSp_n to +5V and GND) on the connector side; pull-ups on the PORTS side.
Global labels to PORTS are shape 'output' (this sheet sources them; U14's are inputs);
PADDLE_DUMP is 'input' (driven by PORTS).
"""
DE9 = 'Connector_Dsub:DSUB-9_Pins_Horizontal_P2.77x2.84mm_EdgePinOffset4.94mm_Housed_MountingHolesOffset4.94mm'
POLYFUSE = 'Fuse:Fuse_Bourns_MF-RHT050'
DO35 = 'Diode_THT:D_DO-35_SOD27_P7.62mm_Horizontal'
RES = 'Resistor_THT:R_Axial_DIN0207_L6.3mm_D2.5mm_P10.16mm_Horizontal'
FILM = 'Capacitor_THT:C_Rect_L7.2mm_W2.5mm_P5.00mm'
SIP7 = 'Resistor_THT:R_Array_SIP7'
TO92 = 'Package_TO_SOT_THT:TO-92_Inline'
PWR_START = 600

# (DE-9 pin, PORTS name) in column order
LINES = [('1', 'UP'), ('2', 'DOWN'), ('3', 'LEFT'), ('4', 'RIGHT'), ('6', 'FIRE'), ('5', 'PIN5'), ('9', 'PIN9')]

PARTS = []


def port(p, jref, fref, x0, y0, rnum, dnum):
    js = {n: ('local', f'JS{p}_{n}') for n, _ in LINES}
    js['7'] = ('local', f'JS{p}_7')
    js['8'] = ('power', 'GND')
    PARTS.append(dict(ref=jref, lib='Connector', name='DE9_Pins', value=f'JOYSTICK {p}', footprint=DE9,
                      at=(x0 + 25.4, y0 + 38.1), pins=js))
    PARTS.append(dict(ref=fref, lib='Device', name='Polyfuse', value='0.5A polyfuse', footprint=POLYFUSE,
                      at=(x0 + 25.4, y0 + 76.2), pins={'1': ('power', '+5V'), '2': ('local', f'JS{p}_7')}))
    for i, (n, name) in enumerate(LINES):
        cx = x0 + 50.8 + 12.7 * i
        local = ('local', f'JS{p}_{n}')
        PARTS.append(dict(ref=f'D{dnum}', lib='Device', name='D', value='1N4148', footprint=DO35, rot=270,
                          at=(cx, y0 + 12.7), pins={'1': ('power', '+5V'), '2': local}))
        PARTS.append(dict(ref=f'D{dnum + 1}', lib='Device', name='D', value='1N4148', footprint=DO35, rot=270,
                          at=(cx, y0 + 45.72), pins={'1': local, '2': ('power', 'GND')}))
        PARTS.append(dict(ref=f'R{rnum}', lib='Device', name='R', value='100R', footprint=RES,
                          at=(cx, y0 + 78.74),
                          pins={'1': local, '2': ('global', f'JOYSTICK_{p}_{name}', 'output')}))
        dnum += 2
        rnum += 1


port(1, 'J20', 'F1', 25.4, 25.4, 120, 4)
port(2, 'J21', 'F2', 25.4, 152.4, 127, 18)


def network(ref, x, y, p, r6):
    pins = {'1': ('power', '+5V')}
    for k, name in enumerate(['UP', 'DOWN', 'LEFT', 'RIGHT', 'FIRE']):
        pins[str(k + 2)] = ('global', f'JOYSTICK_{p}_{name}', 'output')
    pins['7'] = r6
    PARTS.append(dict(ref=ref, lib='Device', name='R_Network06', value='10k bussed', footprint=SIP7,
                      rot=90, at=(x, y), pins=pins))


network('RN1', 284.48, 35.56, 1, 'nc')
network('RN2', 284.48, 172.72, 2, ('global', 'JOYSTICK_2_PIN9', 'output'))


def paddle(rref, cref, qref, net, x, y):
    PARTS.append(dict(ref=rref, lib='Device', name='R', value='1M', footprint=RES, at=(x, y),
                      pins={'1': ('power', '+5V'), '2': ('global', net, 'output')}))
    PARTS.append(dict(ref=cref, lib='Device', name='C', value='10nF film', footprint=FILM, at=(x + 15.24, y + 12.7),
                      pins={'1': ('global', net, 'output'), '2': ('power', 'GND')}))
    PARTS.append(dict(ref=qref, lib='Transistor_FET', name='2N7000', value='2N7000', footprint=TO92,
                      at=(x + 55.88, y + 12.7),
                      pins={'3': ('global', net, 'output'), '1': ('power', 'GND'),
                            '2': ('global', 'PADDLE_DUMP', 'input')}))


paddle('R134', 'C90', 'Q3', 'JOYSTICK_1_PIN9', 276.86, 68.58)
paddle('R135', 'C91', 'Q4', 'JOYSTICK_1_PIN5', 276.86, 119.38)

PARTS.append(dict(ref='R136', lib='Device', name='R', value='10k', footprint=RES, at=(330.2, 172.72),
                  pins={'1': ('power', '+5V'), '2': ('global', 'JOYSTICK_2_PIN5', 'output')}))

TEXTS = [
    ('Atari DE-9: 1 up, 2 down, 3 left, 4 right, 5/9 paddle pots (SMS button 2 on 9), 6 fire,\\n'
     '7 +5V (polyfuse), 8 GND.  Port 1 hosts the paddles: 1M pull-ups on 5/9 (the pot parallels\\n'
     'the pull-up), 10 nF ramp caps, 2N7000 dump FETs on PADDLE_DUMP; DNP the caps/FETs for a\\n'
     'sticks-only build.', 177.8, 142.24),
    ('ESD: 100R series + 1N4148 clamps to +5V/GND on every switch line, connector side.\\n'
     'Values 100R, 1N4148, 10k, 1M, 10 nF and 0.5 A are assumptions;\\n'
     'griffin.md gives only the topology.', 177.8, 160.02),
]
