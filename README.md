This is a version of the Quake 1 engine that runs on the
[Olimex ESP32-P4-PC](https://www.olimex.com/Products/IoT/ESP32-P4/ESP32-P4-PC/open-source-hardware),
with video output over HDMI.

It is a port of [espressif/esp32-quake](https://github.com/espressif/esp32-quake), which targets
the ESP32-P4-Function-EV-Board with a 1024x600 MIPI LCD. See
[Differences from the EV-Board version](#differences-from-the-ev-board-version) below for what
changed.

Features
========

 * Can run both the shareware as well as the registered version of Quake

 * Renders at 512x288 pixels, upscaled exactly 2.5x to 1280x720 HDMI, at around 18-20fps.

 * Audio support through the 3.5mm headphone jack. Both digital sound, as well as music if a
   .cue/.bin image of the original CDROM is provided.

 * Supports USB keyboard for input

 * Network multiplayer support over the Ethernet port


Hardware required
=================

 * An Olimex ESP32-P4-PC. Tested on hardware revision C with an ESP32-P4 chip revision v1.0.

 * An HDMI monitor or TV that accepts 1280x720 at 60Hz, and an HDMI cable.

 * A USB keyboard, plugged into any of the four USB-A ports. These sit behind the board's
   onboard USB hub; a further hub with keyboard and mouse should also work, but has not been
   tested.

 * Headphones or powered speakers for the 3.5mm jack if you want sound. HDMI is video-only on
   this board, and there is no onboard speaker amplifier.

 * A micro-SD card to put the Quake files on. The size depends on the version of Quake you're
   going to put on there, but generally anything 2GiB or larger will fit nearly any version of
   Quake out there.

 * A USB-C data cable for flashing; the USB-C port also powers the board.

Quake game data
===============

This repository does not contain any Quake game data; you should bring your own. Here
are a few options. If you have an original Quake CDROM or other source of game
data, you can also install it somewhere and copy the files from there.

Quake (The Offering) from GOG.com
---------------------------------

Buy the game at [Good Old Games](https://www.gog.com/en/game/quake_the_offering). Download 
and run the installer (under Windows or Wine) to install to a directory. Alternatively, use 
[innoextract](https://constexpr.org/innoextract/) to extract the Quake files from the 
installation executable; in this case the data you need is under the 'app' subdirectory.

Shareware Quake
---------------

Download quake106.zip from e.g. [dosarchives](https://www.dosgamesarchive.com/file/quake/quake106/)
or simply chuck 'quake106.zip' into your favourite search engine and download the resulting
file. Extracting the files gives you the DOS Quake shareware installer. You can either use 
e.g. Dosbox to run install.bat, or alternatively use 7zip to extract the 'resource.1' file
in the archive.

LibreQuake
----------

LibreQuake has been tested, but seems to be written for more modern engines. Regardless, 
it still seems to load and kind-of work. To install, download 'full.zip' from the
[releases](https://github.com/lavenderdotpet/LibreQuake/releases) page and extract.
Note you need to take the 'id1' folder (inside the 'full' folder) and copy that to your
micro-SD card.

Copying the data
================

The micro-SD card must be formatted as FAT32 (or FAT). exFAT does **not** work: ESP-IDF's FAT
driver is built without exFAT support, and the card will fail to mount. Cards larger than 32GB
usually come formatted as exFAT, so reformat them first. On Linux, for example (this erases the
card; replace `/dev/sdX1` with your card's partition):

    sudo mkfs.vfat -F 32 -n QUAKE /dev/sdX1

The data needs to be copied directly to the root of the micro-SD-card: most notably, the 'ID1'
folder should be located in the root, not in a subfolder.

If you want music (and the version of Quake you're using supports it - the shareware version
and LibreQuake does not) you'll need to place a .bin/.cue image of the CDROM in the root
of the Micro-SD card as well. If you're using the GOG.com version, no need to do anything - it 
already comes with the required image.

Compiling, flashing and running
===============================

This project needs ESP-IDF v5.5.3 or a later v5.5.x release; it has been tested with v5.5.6.
(Olimex notes that v5.5.0 through v5.5.2 lack ESP32-P4 fixes their board support relies on.)
With a proper ESP-IDF installed and exported, run:

- ``idf.py set-target esp32p4`` (only the first time)

- ``idf.py -p PORT flash monitor``

The USB-C port is the ESP32-P4's built-in USB Serial/JTAG, so no buttons are needed to flash;
it shows up as e.g. `/dev/ttyACM0` on Linux or a COM port on Windows. The serial console
(game log and fps counter) is on the same port. On Linux, your user needs to be in the
`dialout` group to open it.

Now insert the micro-SD-card with the Quake data files into the board's micro-SD slot, connect
the HDMI monitor and a keyboard, reset the board, and have fun! Connect HDMI before powering
or resetting the board so the monitor syncs reliably.

Chip revision
-------------

`sdkconfig.defaults` sets `CONFIG_ESP32P4_SELECTS_REV_LESS_V3=y`, which builds for ESP32-P4
chip revisions below v3.0, as found on current ESP32-P4-PC boards. ESP-IDF v5.5 otherwise
defaults to revision v3.0 and newer, which is not compatible with older chips. If your board
has a v3.x chip (`esptool.py chip_id` prints the revision), remove that line and run
`idf.py fullclean` before building.


Network play
============

Network play is possible by connecting the RJ-45 Ethernet port of the board to a network.
On startup, the ESP32P4 will request an IP via DHCP.

When connecting to a server, you should enter its IP directly in the 'Join game at' field,
rather than using 'Search for local games'. The latter will find running games, but will
fail to actually connect.

This version of Quake only speaks the original version of the Quake network protocol. If
you get the error ``Server returned version xxx, not 15`` when trying to connect to a server,
try to switch the server back to the old protocol. To do that, use the command 
``sv_protocol 15`` on the console (``~``-key) on the server before starting a multiplayer 
game.

Note that this version of Quake is also capable of acting as a server itself. Modern versions
of Quake should be able to connect to it. Simply select the ``New game`` option under the
``multiplayer`` menu. Note that clients also should connect to the game using the 
``Join game at`` method.

Differences from the EV-Board version
=====================================

The ESP32-P4-PC is close to the ESP32-P4-Function-EV-Board: the SD slot, Ethernet PHY, audio
codec, I2C/I2S pins and flash are wired the same way, and Olimex's own demo firmware uses
Espressif's EV-Board BSP. The differences, and how this port handles them:

| | EV-Board + MIPI LCD | ESP32-P4-PC | This port |
| --- | --- | --- | --- |
| Display | 1024x600 MIPI LCD, RGB565, PWM backlight on GPIO26 | HDMI via an onboard LT8912B DSI-to-HDMI bridge, which needs RGB888 | 1280x720@60 HDMI; the PPA scales and converts RGB565 to RGB888. No backlight control: in HDMI mode the BSP's backlight pin is GPIO23, which is the UEXT I2C_SCL2 pin here |
| Render size | 512x300 | 1280x720 is 16:9 | 512x288, an exact 2.5x scale |
| Serial console | UART via a USB-UART bridge | USB-C is the chip's USB Serial/JTAG | Console on USB Serial/JTAG |
| SD card power | Always on | Switched by GPIO45 (SD1_PWR_EN, active low) | Powered on at startup; mounted at 20MHz, the speed Olimex validated |
| USB host | One USB-A port | Four USB-A ports behind an FE1.1s hub, reset on GPIO21 (active low) | Hub reset pulsed at startup; nested hubs enabled |
| Audio output | Speaker amplifier enabled by GPIO53 | 3.5mm headphone jack only; GPIO53 is the UEXT SPI MOSI pin | Unchanged; the BSP still toggles GPIO53, which only matters if something is attached to UEXT |

The board support package in `components/esp-bsp` is the HDMI-capable snapshot from Olimex's
[p4_board_demo](https://github.com/OLIMEX/ESP32-P4-PC/tree/main/SOFTWARE/ESP-IDF/p4_board_demo),
which carries a few HDMI timing and audio fixes over Espressif's esp32_p4_function_ev_board
BSP v5.2.0. Board pin assignments come from the ESP32-P4-PC Rev.C schematic.

Credits
=======

This is based on [quake-generic](https://github.com/erysdren/quakegeneric) by erysdren, which 
in turn is based on the [Quake sources](https://github.com/id-Software/Quake) released by 
Id Software. The original ESP32-P4 port is
[espressif/esp32-quake](https://github.com/espressif/esp32-quake). The board support package
is from [OLIMEX/ESP32-P4-PC](https://github.com/OLIMEX/ESP32-P4-PC), based on Espressif's
[esp-bsp](https://github.com/espressif/esp-bsp).
