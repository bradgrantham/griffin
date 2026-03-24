#!/usr/bin/env python3
"""
at89s52_isp.py — FT232H-based ISP programmer for AT89S52

Uses pyftdi to bit-bang SPI via the FT232H MPSSE engine.
Pin mapping (Adafruit FT232H breakout):
    D0 = SCK  → AT89S52 P1.7 (SCK)
    D1 = MOSI → AT89S52 P1.5 (MOSI)
    D2 = MISO → AT89S52 P1.6 (MISO)
    D3 = RST  → AT89S52 RST  (active high for ISP)

RST handling:
    pyftdi claims D3 as CS0 (active-low). We use raw MPSSE commands
    to set D3 HIGH (RST asserted) and clock SPI data on D0-D2 without
    ever toggling CS, keeping RST stable throughout the session.

Requirements:
    pip install pyftdi

Usage:
    python at89s52_isp.py flash firmware.bin
    python at89s52_isp.py verify firmware.bin
    python at89s52_isp.py erase
    python at89s52_isp.py read output.bin [--size N]
    python at89s52_isp.py signature
    python at89s52_isp.py --id-only

Any command can be combined with --id-only to stop after signature check.
"""

import argparse
import sys
import time
from pathlib import Path

from pyftdi.ftdi import Ftdi


# AT89S52 constants
FLASH_SIZE = 8192  # 8K bytes
SIGNATURE = (0x1E, 0x52, 0x06)  # Manufacturer, Device ID1, Device ID2
CHIP_NAME = "AT89S52"

# ISP command opcodes (per datasheet Table 21-2)
CMD_PROGRAMMING_ENABLE = [0xAC, 0x53, 0x00, 0x00]
CMD_CHIP_ERASE         = [0xAC, 0x80, 0x00, 0x00]
CMD_READ_FLASH         = 0x20
CMD_WRITE_FLASH        = 0x40
CMD_READ_SIGNATURE     = 0x28

# Timing
ERASE_WAIT_S     = 0.5    # 400ms typical per datasheet, 500ms for margin
WRITE_BYTE_WAIT  = 0.0012 # ~1.2ms per byte write (conservative)
PROGRAMMING_ENABLE_RETRIES = 5

# SPI clock: must be < crystal_freq / 16
# 11.0592 MHz / 16 = 691.2 kHz, use 500 kHz for margin
SPI_FREQ = 500_000

# FT232H URL for pyftdi (first available device)
FTDI_URL = "ftdi://ftdi:232h/1"

# MPSSE pin assignments (active-low bus, directly controlled)
#   D0 = SCK  (output)
#   D1 = MOSI (output)
#   D2 = MISO (input)
#   D3 = RST  (output, directly controlled)
PIN_SCK  = 0x01  # D0
PIN_MOSI = 0x02  # D1
PIN_MISO = 0x04  # D2
PIN_RST  = 0x08  # D3
DIR_MASK = PIN_SCK | PIN_MOSI | PIN_RST  # outputs: D0, D1, D3; D2 = input


class AT89S52Programmer:
    """ISP programmer for AT89S52 via FT232H raw MPSSE.

    Bypasses pyftdi's SpiController entirely to avoid CS conflicts.
    Uses the FTDI MPSSE engine directly for SPI clocking while
    independently controlling D3 as RST.
    """

    def __init__(self, url=FTDI_URL, freq=SPI_FREQ, verbose=False):
        self.verbose = verbose
        self.freq = freq
        self.url = url
        self.ftdi = None
        self._rst_is_high = False

    def log(self, msg):
        if self.verbose:
            print(f"  [{CHIP_NAME}] {msg}")

    def open(self):
        """Open FT232H and configure MPSSE for SPI + GPIO on D3."""
        self.ftdi = Ftdi()
        self.ftdi.open_from_url(self.url)

        # Configure MPSSE mode
        self.ftdi.set_bitmode(0, Ftdi.BitMode.RESET)
        time.sleep(0.010)
        self.ftdi.set_bitmode(0, Ftdi.BitMode.MPSSE)
        time.sleep(0.010)

        # Purge buffers
        self.ftdi.purge_buffers()

        # Configure MPSSE clock divisor for desired SPI frequency.
        # FT232H base clock = 60 MHz.
        # With divide-by-5 disabled: freq = 60MHz / ((1 + divisor) * 2)
        # With divide-by-5 enabled:  freq = 12MHz / ((1 + divisor) * 2)
        #
        # Disable divide-by-5 (command 0x8A) for higher resolution
        self.ftdi.write_data(bytes([0x8A]))  # disable /5 divisor

        # Calculate divisor: freq = 30MHz / (1 + divisor)
        # (MPSSE clocks at 30MHz base for data, each clock = 2 edges)
        # Actually: with /5 disabled, clock = 60MHz / ((1+div)*2)
        divisor = max(0, int((30_000_000 / self.freq) - 1))
        actual_freq = 30_000_000 / (1 + divisor)
        self.log(f"Requested {self.freq}Hz, divisor={divisor}, "
                 f"actual={actual_freq:.0f}Hz")

        # Set clock divisor: command 0x86, value_low, value_high
        self.ftdi.write_data(bytes([
            0x86,
            divisor & 0xFF,
            (divisor >> 8) & 0xFF
        ]))

        # Set initial pin states: SCK=LOW, MOSI=LOW, RST=LOW
        # Command 0x80 = set data bits low byte
        # [0x80, value, direction]
        self.ftdi.write_data(bytes([0x80, 0x00, DIR_MASK]))

        # Disable adaptive clocking, disable 3-phase clocking
        self.ftdi.write_data(bytes([0x97]))  # disable adaptive clocking
        self.ftdi.write_data(bytes([0x8D]))  # disable 3-phase clocking

        # Flush any stale read data
        self.ftdi.purge_buffers()

        self.log(f"Opened FT232H at {self.url}, MPSSE SPI at "
                 f"~{actual_freq:.0f}Hz")

    def close(self):
        """Release FT232H."""
        if self.ftdi:
            self._rst_low()
            self.ftdi.close()
            self.ftdi = None
        self.log("Closed FT232H")

    def _set_pins(self, value):
        """Set ADBUS low byte pins. value is the pin states."""
        self.ftdi.write_data(bytes([0x80, value & 0xFF, DIR_MASK]))

    def _rst_high(self):
        """Assert RST high to enter ISP mode."""
        # Drive D3 HIGH, keep SCK and MOSI LOW
        self._set_pins(PIN_RST)
        self._rst_is_high = True
        self.log("RST asserted HIGH")

    def _rst_low(self):
        """Deassert RST to let target run."""
        if self._rst_is_high:
            self._set_pins(0x00)  # all low
            self._rst_is_high = False
            self.log("RST deasserted LOW, target running")

    def _spi_exchange(self, data):
        """Send and receive bytes over SPI (D0=SCK, D1=MOSI, D2=MISO).

        Uses MPSSE opcode 0x31: clock data bytes out on -ve CLK edge,
        in on +ve CLK edge, MSB first. This is SPI mode 0.

        D3 (RST) is maintained at its current state since we only
        change D3 via _set_pins and never via the SPI clock commands.
        """
        n = len(data)
        length = n - 1  # MPSSE uses length-1

        # Build MPSSE command: simultaneous read+write, MSB first
        # 0x31 = write on -ve, read on +ve, MSB first
        cmd = bytearray([0x31, length & 0xFF, (length >> 8) & 0xFF])
        cmd.extend(data)

        # Send SEND_IMMEDIATE (0x87) to flush the read buffer
        cmd.append(0x87)

        self.ftdi.write_data(bytes(cmd))

        # Read back n bytes
        response = bytearray()
        retries = 200
        while len(response) < n and retries > 0:
            chunk = self.ftdi.read_data(n - len(response))
            if chunk:
                response.extend(chunk)
            else:
                time.sleep(0.001)
                retries -= 1

        if len(response) < n:
            self.log(f"WARNING: Short read, expected {n} got {len(response)}")
            response.extend(bytes(n - len(response)))

        return list(response)

    def enter_isp_mode(self):
        """Enter ISP programming mode: assert RST, send programming enable."""
        print("Entering ISP mode...")

        # Drive RST HIGH
        self._rst_high()
        time.sleep(0.020)  # 20ms for oscillator startup

        for attempt in range(1, PROGRAMMING_ENABLE_RETRIES + 1):
            resp = self._spi_exchange(CMD_PROGRAMMING_ENABLE)
            self.log(f"Programming Enable attempt {attempt}: "
                     f"sent={CMD_PROGRAMMING_ENABLE} resp={resp}")
            # Datasheet: byte 2 of response should echo 0x53
            if len(resp) >= 4 and resp[2] == 0x53:
                print("  Programming enabled successfully.")
                return True
            time.sleep(0.050)

        print("ERROR: Failed to enter programming mode after "
              f"{PROGRAMMING_ENABLE_RETRIES} attempts.")
        print("  Check wiring: SCK→P1.7, MOSI→P1.5, MISO→P1.6, RST→pin 9")
        print("  Check crystal is installed and target is powered.")
        return False

    def read_signature(self):
        """Read the 3-byte device signature."""
        sig = []
        for addr in range(3):
            resp = self._spi_exchange([CMD_READ_SIGNATURE, addr, 0x00, 0x00])
            sig.append(resp[3] if len(resp) >= 4 else 0xFF)
        return tuple(sig)

    def check_signature(self):
        """Read and verify device signature. Returns True if matched."""
        print("Reading device signature...")
        sig = self.read_signature()
        mfr, dev1, dev2 = sig

        print(f"  Manufacturer: 0x{mfr:02X} "
              f"({'Atmel/Microchip' if mfr == 0x1E else 'UNKNOWN'})")
        print(f"  Device ID 1:  0x{dev1:02X}")
        print(f"  Device ID 2:  0x{dev2:02X}")

        if sig == SIGNATURE:
            print(f"  → {CHIP_NAME} identified correctly.")
            return True
        else:
            expected = ', '.join(f'0x{b:02X}' for b in SIGNATURE)
            got = ', '.join(f'0x{b:02X}' for b in sig)
            print(f"  → Signature mismatch! Expected ({expected}), got ({got})")
            return False

    def chip_erase(self):
        """Erase entire flash (sets all bytes to 0xFF)."""
        print("Erasing chip...")
        self._spi_exchange(CMD_CHIP_ERASE)
        time.sleep(ERASE_WAIT_S)
        print(f"  Erase complete (waited {ERASE_WAIT_S}s).")

        # Must re-enter programming mode after erase: cycle RST
        print("  Re-entering programming mode after erase...")
        self._rst_low()
        time.sleep(0.010)
        self._rst_high()
        time.sleep(0.020)

        for attempt in range(1, PROGRAMMING_ENABLE_RETRIES + 1):
            resp = self._spi_exchange(CMD_PROGRAMMING_ENABLE)
            if len(resp) >= 4 and resp[2] == 0x53:
                print("  Programming re-enabled after erase.")
                return True
            time.sleep(0.050)

        print("ERROR: Failed to re-enter programming mode after erase.")
        return False

    def write_flash(self, data, verify=True):
        """Program flash with the given byte data.

        Args:
            data: bytes to write (max FLASH_SIZE)
            verify: if True, verify after writing

        Returns:
            True on success
        """
        if len(data) > FLASH_SIZE:
            print(f"ERROR: Data size ({len(data)}) exceeds flash size "
                  f"({FLASH_SIZE}).")
            return False

        total = len(data)
        print(f"Programming {total} bytes...")

        start = time.time()

        for addr in range(total):
            addr_hi = (addr >> 8) & 0xFF
            addr_lo = addr & 0xFF
            self._spi_exchange(
                [CMD_WRITE_FLASH, addr_hi, addr_lo, data[addr]])
            time.sleep(WRITE_BYTE_WAIT)

            if (addr + 1) % 512 == 0 or addr == total - 1:
                pct = (addr + 1) * 100 // total
                elapsed = time.time() - start
                print(f"\r  Writing: {addr+1}/{total} bytes ({pct}%) "
                      f"[{elapsed:.1f}s]", end="", flush=True)

        elapsed = time.time() - start
        print(f"\n  Write complete in {elapsed:.1f}s "
              f"({total/elapsed:.0f} bytes/s).")

        if verify:
            return self.verify_flash(data)
        return True

    def read_flash(self, size=FLASH_SIZE):
        """Read flash contents.

        Args:
            size: number of bytes to read (default: full 8K)

        Returns:
            bytes object with flash contents
        """
        print(f"Reading {size} bytes...")
        data = bytearray(size)
        start = time.time()

        for addr in range(size):
            addr_hi = (addr >> 8) & 0xFF
            addr_lo = addr & 0xFF
            resp = self._spi_exchange(
                [CMD_READ_FLASH, addr_hi, addr_lo, 0x00])
            data[addr] = resp[3] if len(resp) >= 4 else 0xFF

            if (addr + 1) % 512 == 0 or addr == size - 1:
                pct = (addr + 1) * 100 // size
                elapsed = time.time() - start
                print(f"\r  Reading: {addr+1}/{size} bytes ({pct}%) "
                      f"[{elapsed:.1f}s]", end="", flush=True)

        elapsed = time.time() - start
        print(f"\n  Read complete in {elapsed:.1f}s.")
        return bytes(data)

    def verify_flash(self, expected_data):
        """Verify flash contents against expected data.

        Args:
            expected_data: bytes to compare against

        Returns:
            True if flash matches
        """
        size = len(expected_data)
        print(f"Verifying {size} bytes...")
        start = time.time()
        mismatches = 0
        first_mismatch = None

        for addr in range(size):
            addr_hi = (addr >> 8) & 0xFF
            addr_lo = addr & 0xFF
            resp = self._spi_exchange(
                [CMD_READ_FLASH, addr_hi, addr_lo, 0x00])
            got = resp[3] if len(resp) >= 4 else 0xFF

            if got != expected_data[addr]:
                mismatches += 1
                if first_mismatch is None:
                    first_mismatch = addr
                if mismatches <= 10:
                    print(f"\n  MISMATCH @ 0x{addr:04X}: "
                          f"expected 0x{expected_data[addr]:02X}, "
                          f"got 0x{got:02X}", end="")

            if (addr + 1) % 512 == 0 or addr == size - 1:
                pct = (addr + 1) * 100 // size
                elapsed = time.time() - start
                print(f"\r  Verifying: {addr+1}/{size} bytes ({pct}%) "
                      f"[{elapsed:.1f}s]", end="", flush=True)

        elapsed = time.time() - start
        print()

        if mismatches == 0:
            print(f"  Verify OK — {size} bytes match ({elapsed:.1f}s).")
            return True
        else:
            print(f"  VERIFY FAILED — {mismatches} mismatches "
                  f"(first at 0x{first_mismatch:04X}).")
            return False


def load_bin_file(path):
    """Load a binary firmware file."""
    p = Path(path)
    if not p.exists():
        print(f"ERROR: File not found: {path}")
        sys.exit(1)
    data = p.read_bytes()
    if len(data) > FLASH_SIZE:
        print(f"WARNING: File is {len(data)} bytes, "
              f"truncating to {FLASH_SIZE}.")
        data = data[:FLASH_SIZE]
    print(f"Loaded {len(data)} bytes from {path}")
    return data


def main():
    parser = argparse.ArgumentParser(
        description=f"{CHIP_NAME} ISP Programmer via FT232H",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""\
examples:
  %(prog)s signature              Read and display device signature
  %(prog)s flash firmware.bin     Erase, program, and verify
  %(prog)s verify firmware.bin    Verify flash against file
  %(prog)s erase                  Erase entire flash
  %(prog)s read output.bin        Read flash to file
  %(prog)s --id-only              Just check device signature and exit
""")

    parser.add_argument(
        "command", nargs="?",
        choices=["flash", "verify", "erase", "read", "signature"],
        help="Programming command")
    parser.add_argument(
        "file", nargs="?",
        help="Binary firmware file (for flash/verify/read)")
    parser.add_argument(
        "--id-only", action="store_true",
        help="Enter ISP, read signature, and exit (no programming)")
    parser.add_argument(
        "--size", type=int, default=FLASH_SIZE,
        help=f"Bytes to read (default: {FLASH_SIZE})")
    parser.add_argument(
        "--no-verify", action="store_true",
        help="Skip verification after flash")
    parser.add_argument(
        "--freq", type=int, default=SPI_FREQ,
        help=f"SPI clock frequency in Hz (default: {SPI_FREQ})")
    parser.add_argument(
        "--url", default=FTDI_URL,
        help=f"FTDI device URL (default: {FTDI_URL})")
    parser.add_argument(
        "-v", "--verbose", action="store_true",
        help="Verbose debug output")

    args = parser.parse_args()

    # Validate arguments
    if not args.id_only and args.command is None:
        parser.error("Either --id-only or a command is required.")

    if args.command in ("flash", "verify") and args.file is None:
        parser.error(f"'{args.command}' requires a firmware file argument.")

    if args.command == "read" and args.file is None:
        parser.error("'read' requires an output file argument.")

    # Go
    prog = AT89S52Programmer(
        url=args.url, freq=args.freq, verbose=args.verbose)

    try:
        prog.open()

        if not prog.enter_isp_mode():
            sys.exit(1)

        if not prog.check_signature():
            print("Aborting due to signature mismatch.")
            sys.exit(1)

        # --id-only: stop here
        if args.id_only:
            print("--id-only: done.")
            return

        if args.command == "signature":
            # Already printed above
            pass

        elif args.command == "erase":
            if not prog.chip_erase():
                sys.exit(1)
            print("Done.")

        elif args.command == "flash":
            data = load_bin_file(args.file)
            if not prog.chip_erase():
                sys.exit(1)
            if not prog.write_flash(data, verify=not args.no_verify):
                sys.exit(1)
            print("Done.")

        elif args.command == "verify":
            data = load_bin_file(args.file)
            if not prog.verify_flash(data):
                sys.exit(1)
            print("Done.")

        elif args.command == "read":
            data = prog.read_flash(size=args.size)
            p = Path(args.file)
            p.write_bytes(data)
            print(f"Saved {len(data)} bytes to {args.file}")
            print("Done.")

    except KeyboardInterrupt:
        print("\nInterrupted.")
        sys.exit(130)
    except Exception as e:
        print(f"ERROR: {e}")
        if args.verbose:
            import traceback
            traceback.print_exc()
        sys.exit(1)
    finally:
        prog._rst_low()
        prog.close()


if __name__ == "__main__":
    main()
