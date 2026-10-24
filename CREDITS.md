# Credits

This repository is a **PS Vita port** of an existing project. None of the
recompilation work is mine; all rights belong to the original authors below.
All I did was port it to the Vita.

## Original project and framework

- **Matthew Stanley ([mstan](https://github.com/mstan))**: author of
  [SuperMetroidRecomp](https://github.com/mstan/SuperMetroidRecomp), the
  [snesrecomp](https://github.com/mstan/snesrecomp) framework and
  [recomp-ui](https://github.com/mstan/recomp-ui). Licensed under the
  PolyForm Noncommercial License 1.0.0 (see [LICENSE](LICENSE)), Copyright (c) 2026 Matthew Stanley.
- **Alex Vanderveen** and the other snesrecomp contributors (Cellenseres,
  Nicktendonick, navjack, FS and others in the upstream history).

## Work this builds on (credited upstream)

- **[snesrev](https://github.com/snesrev)**: the reverse-engineered
  [Super Metroid decomp](https://github.com/snesrev/sm) used as the symbol and
  oracle reference, and the zelda3/smw ports the C runner was based on.
- **[LakeSnes](https://github.com/angelo-wf/lakesnes)** by angelo_wf and
  contributors (MIT): the 65816 interpreter, PPU, APU and DMA cores, with
  individual algorithms credited inline to **snes9x**.
- **ares** team (Near et al., ISC): the coprocessor cores.
- **[SMWDisX](https://github.com/IsoFrieze/SMWDisX)** and mikeyk: disassembly references.
- Full third-party notices: [snesrecomp/THIRD_PARTY_ATTRIBUTION.md](snesrecomp/THIRD_PARTY_ATTRIBUTION.md).

## Game

*Super Metroid* is (c) 1994 Nintendo. This repository contains no ROM or game
data; you need your own legally obtained copy (*Super Metroid (Japan, USA) (En,Ja).sfc*).

## Vita port

PS Vita port by DarthSully, built with [VitaSDK](https://vitasdk.org) and
AI-assisted (Claude). Vita-specific changes: the Vita host (threads, audio,
touch gestures, LiveArea), 16:9 widescreen drawing across the Vita's cores,
performance work for 444 MHz, save persistence, and a set of bug fixes.
