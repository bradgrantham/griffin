Programming an ATF1504 with WinCUPL
* Install wine-stable in MacPorts
* export WINEPREFIX=$HOME/.wine-wincupl
* Run WinCUPL 5.30.4 installer
* wine $WINEPREFIX/drive_c/Wincupl/Shared/cupl.exe runs cupl

Run against the Atmel device library, which knows about f1504ispplcc84:
    WINEDEBUG="-all" MVK_CONFIG_LOG_LEVEL=0 LIBCUPL=C:\\Wincupl\\Shared\\Atmel.dl wine $WINEPREFIX/drive_c/Wincupl/Shared/cupl.exe -j -a -l -e -x -f -b blinky.pld

Run against the CUPL device library, which knows about f1508ispplcc84:
    WINEDEBUG="-all" MVK_CONFIG_LOG_LEVEL=0 LIBCUPL=C:\\Wincupl\\Shared\\cupl.dl wine $WINEPREFIX/drive_c/Wincupl/Shared/cupl.exe -j -a -l -e -x -f -b blinky.pld

Put this in MoltenVKConfig.plist both in ./ and in glue/ :
<?xml version="1.0" encoding="UTF-8"?>
<plist version="1.0">
<dict>
    <key>MVKConfiguration</key>
    <dict>
        <key>LogLevels</key>
        <integer>0</integer>
    </dict>
</dict>
</plist>

---------------

Programming an ATF1504 with Verilog:

Getting the 15xx fitter from Pro Chip Designer 5.0.1:
wget http://ww1.microchip.com/downloads/en/DeviceDoc/ProChip5.0.1.zip
unzip ProChip5.0.1.zip  -d ProChip5.0.1
cd ProChip5.0.1
sudo winetricks --self-update
WINEPREFIX=$HOME/.wine32 WINEARCH=win64 winetricks vcrun6sp6
WINEPREFIX=$HOME/.wine32 WINEARCH=win64 wine ProChip5_setup.exe

---------------

Unpack oss_cad_suite and checkout atf15xx_yosys into ~/trees/ (or somewhere)
cp /Users/grantham/.wine32/drive_c/ATMEL_PLS_Tools/Prochip/pldfit/aprim.lib /Users/grantham/.wine-wincupl/drive_c/Wincupl/WinCupl/Fitters/{FIT,fit}*.{EXE,exe} /Users/grantham/.wine-wincupl/drive_c/Wincupl/WinCupl/Fitters/atmel.std ~/trees/atf15xx_yosys/vendor/

---------------

  - cpld/Makefile — accepts OSS_CAD_SUITE and ATF15XX_YOSYS as variables, prepends $(OSS_CAD_SUITE)/bin
  to PATH via export, calls the upstream wrapper scripts from the design directory
  - cpld/.config.mk (gitignored, you create it once) — put your local paths there so you don't pass them
  on every make invocation:
  OSS_CAD_SUITE := /Users/grantham/tools/oss-cad-suite
  ATF15XX_YOSYS := /Users/grantham/tools/atf15xx_yosys
  - The $(error ...) in ?= is lazy — make clean won't require the tools to be configured

  The run_fitter.sh reads //PIN: <pin_def> comments from the .v file for pin constraints, so you'll need
  those in glue/glue.v — same idea as the current .pld pin declarations.

---------------

MacBookPro:cpld grantham$ cat .config.mk
OSS_CAD_SUITE := /Users/grantham/trees/oss-cad-suite
ATF15XX_YOSYS := /Users/grantham/trees/atf15xx_yosys
