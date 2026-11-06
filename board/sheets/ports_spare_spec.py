"""place_parts.py spec: PORTS sheet spare-pin header J13 (U14 pins 34/41)."""
PWR_START = 690
PARTS = [
    dict(ref='J13', lib='Connector_Generic', name='Conn_01x03', value='PORTS SPARE',
         footprint='Connector_PinHeader_2.54mm:PinHeader_1x03_P2.54mm_Vertical', at=(165.1, 139.7),
         pins={'1': ('local', 'PORTS_SPARE_34'), '2': ('local', 'PORTS_SPARE_41'), '3': ('power', 'GND')}),
]
