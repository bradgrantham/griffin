"""glue.kicad_sch: the 1x6 JTAG programming header (chain starts at GLUE)."""
PWR_START = 510
PARTS = [dict(ref='J2', lib='Connector', name='Conn_01x06_Pin', value='JTAG',
              footprint='Connector_PinHeader_2.54mm:PinHeader_1x06_P2.54mm_Vertical', at=(78.74, 41.91), rot=180,
              description='JTAG programming header: TCK TMS TDI TDO 5V GND',
              pins={'1': ('global', 'JTAG_TCK', 'output'),
                    '2': ('global', 'JTAG_TMS', 'output'),
                    '3': ('global', 'JTAG_TDI', 'output'),
                    '4': ('global', 'JTAG_TDO', 'input'),
                    '5': ('power', '+5V'),
                    '6': ('power', 'GND')})]
