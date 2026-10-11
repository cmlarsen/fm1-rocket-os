# X0X licensing

X0X is free software under the GNU General Public License, version 3 only
(`GPL-3.0-only`, full text in `LICENSE`). It is a fork of Felucca and keeps Felucca's
licence. If you distribute X0X, or firmware derived from it, you must give your
recipients its complete corresponding source under the same licence.

fm1-rocket-os (this repository, work in progress) is a fork of X0X and keeps the same
licence. X0X's BREAK generator — ported from an unlicensed repository "with the author's
permission" — was removed in this fork, along with its tools, web page, tests and docs;
the code and assets that remain are the ones listed below. The licence texts the tree
needs are collected in `LICENSES/` (`LICENSE` itself is the GPL-3.0).

## Where the code comes from

| What | Origin | Licence |
| --- | --- | --- |
| Platform: `firmware/hal/`, `firmware/loader/`, `firmware/src/{libc,lcd,gfx,usb,storage,ota}.c`, `tools/` (build, package, install, upload), `web/` (installer) | [Felucca](https://github.com/hugelton/Felucca), Copyright (C) 2026 Leo Kuroshita (@kurogedelic), Hügelton Instruments; `usb.c` and `storage.c` changed for X0X | GPL-3.0-only |
| `firmware/src/app/{slots,editor,panel,main_fm1}.c` | adapted from Felucca's `eng_sample.c`, `editor.c`, `panel.c`, `main.c` / `audio.c` | GPL-3.0-only |
| 909 (`dsp/drum909*`, `dsp/fxbus*`) | ported from [9W9](https://github.com/athousanddetails/schwung-9W9) (athousanddetails), itself after [ER-99](https://github.com/matthewcieplak/er-99) (Matthew Cieplak) | GPL-3.0 |
| 909 hi-hat, ride and crash samples (`assets/909/`) | ER-99 via 9W9 | GPL-3.0 |
| 808 (`dsp/drum808*`) | ported from [8W8](https://github.com/athousanddetails/schwung-8W8) (athousanddetails); circuit models after the TR-808 service notes and Werner / Abel / Smith; rim shot after sc808 (Yoshinosuke Horiuchi / Sam Aaron) | GPL-3.0 |
| 303 (`dsp/bass303*`) | ported from [schwung-303](https://github.com/charlesvestal/schwung-303): Open303 by Robin Schmidt (MIT), Devilfish extensions after jc303 (midilab), RAT drive after dm-Rat (Dave Mollen) | GPL-3.0 (Open303 parts MIT) |
| TB-3PO (`seq/tb3po*`) | ported from [schwung-tb3po](https://github.com/charlesvestal/schwung-tb3po), itself a port of the Phazerville Hemisphere Suite `TB_3PO` applet (djphazer and contributors) | GPL-3.0 |
| The idea of a browser emulator for FM-1 firmware (`web/emu/`) | [Groove OS](https://www.groove-os.com/) by Peter Gombos, whose site runs its firmware in the browser; no code from it | credit only |
| Everything else in `firmware/src/{app,dsp,seq}`, `host/`, `tests/` | X0X | GPL-3.0-only |

No recorded breaks are in this tree or in the default firmware. The removed BREAK part
(generator and its tools) came from [schwung-breakbeat](https://github.com/mestela/schwung-breakbeat)
(BB Gen) by mestela, an unlicensed repository used "with the author's permission"; nothing of
it remains here.

## Felucca Assets

Felucca's non-GPL assets — the icon atlas, the panel photo and the Hügelton drum pack —
are **not** part of this fork and are not used by it. Felucca's section 7 additional
permission concerns those assets only, so it is not needed here.

## Third-party material

| What | Licence | Where |
| --- | --- | --- |
| Barlow Semi Condensed (The Barlow Project Authors), the default UI face | SIL OFL 1.1 | `assets/fonts/BarlowSemiCondensed-*.ttf`, `assets/fonts/Barlow-OFL.txt` |
| Terminus (Dimitar Toshkov Zhekov), the `terminus` font set | SIL OFL 1.1 | `assets/fonts/ter-u*.bdf`, `assets/fonts/Terminus-LICENSE.txt` |
| Fukiai icon font (Hügelton Instruments), web pages only | MIT | `web/fukiai.ttf`, `web/FUKIAI-LICENSE.txt` |
| JieLi AC79 SDK: `uboot.boot`, `cfg_tool.bin`, `eq_cfg_hw.bin` are read from your SDK checkout at build time and placed in the package; no SDK files are in this tree | Apache-2.0 | <https://gitee.com/Jieli-Tech/fw-AC79_AIoT_SDK> |

## Trademarks

"Felucca" and "Hügelton Instruments" are names of Hügelton Instruments. "M-VAVE" and
"FM-1" are trademarks of their respective owners. TR-808, TR-909 and TB-303 are
trademarks of Roland Corporation, used only to describe what the parts emulate. X0X is
independent firmware; it is not affiliated with, endorsed by or supported by any of them.

## Radio

X0X never enables the Bluetooth / Wi-Fi radio of the hardware.
