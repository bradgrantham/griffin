"""PWR_FLAG on the RTC battery net so ERC sees VBAT as driven."""
PARTS = [dict(ref='#FLG01', lib='power', name='PWR_FLAG', value='PWR_FLAG', footprint='', at=(68.58, 116.84), rot=0,
              pins={'1': ('local', 'VBAT')})]
PWR_START = 590
