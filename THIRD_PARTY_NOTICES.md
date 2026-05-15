# Third-Party Notices

This repository is primarily distributed under the MIT License in `LICENSE`.
That license applies to Temporal-authored source code, documentation, website
content, firmware glue, and hardware files unless a file-level notice or this
document says otherwise.

Some firmware builds and bundled assets include third-party components with
different license terms. Redistributors are responsible for preserving the
applicable notices and source availability obligations for those components.

## DoomGeneric

The badge firmware includes DoomGeneric-derived Doom engine source code.
DoomGeneric files carry GPL-2.0-or-later license notices.

Firmware binaries that include DoomGeneric are not MIT-only binaries. They
include GPL Doom engine code and must be distributed under GPL-compatible terms
with the corresponding GPL-covered source code made available.

The GPL-2.0 license text is included at `licenses/GPL-2.0.txt`.

## doom1.wad

The file `firmware/initial_filesystem/doom1.wad`, when present, is Doom
shareware game data from id Software. It is game data, not source code, and it
is not covered by this repository's MIT License.

The Doom shareware data is freely redistributable, but it remains proprietary
and non-open-source game data with terms separate from the source code in this
repository. Credit for Doom and the shareware game data belongs to id Software.

The badge firmware also credits id Software before launching Doom.

A Doom WAD-specific notice is included at `licenses/DOOM1-WAD.txt`.

## Other Firmware Dependencies

The public firmware tree may include or depend on additional third-party
components, including MicroPython, Arduino/ESP32/PlatformIO libraries, QR code
utilities, U8g2 fonts and display code, ArduinoJson, Adafruit libraries, and
SparkFun libraries. Preserve the license notices included with those files and
packages when redistributing source or binaries.

This notice should be expanded as the firmware import is completed and the
exact vendored dependency set is finalized.
