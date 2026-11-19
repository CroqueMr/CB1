# CB1 source inventory

The initial engine source is the public LibreELEC × CB1 1.0.1 RC3 `CB1/` directory at [CroqueMr/libreelec-CB1@9e562e61](https://github.com/CroqueMr/libreelec-CB1/tree/9e562e613c5f3c378d2143f771392b601112d49d/CB1). The engine import is [41630bd](https://github.com/CroqueMr/CB1/commit/41630bd6b8399ef574705a18c973190ef40bc79b). The ownership manifests identify retained upstream material, historical extraction paths, model inputs and original CB1 sources.

| Category | Change | PR |
| --- | --- | --- |
| DV Patch | Check shared random-access timestamps in their original clocks | [#1](https://github.com/CroqueMr/CB1/pull/1) |
| DV Patch | Keep each queued EL packet with its ordering and access type | [#2](https://github.com/CroqueMr/CB1/pull/2) |
| DV Patch | Keep shared seek anchors across uneven track interleaving | [#3](https://github.com/CroqueMr/CB1/pull/3) |
| Fix du dépôt | Verify the FFmpeg 9.0.2 source and producer/consumer contract | [#4](https://github.com/CroqueMr/CB1/pull/4) |
| Augmentation perf dépôt | No independent performance patch in this BL/EL lot | |

Kodi owns media commands, the A/V clock and display completion. The engine owns source-identity pairing, enhancement frame lifetime and the Dolby Vision processing contract. FFmpeg owns source extraction and codec/filter primitives. libbluray owns the disc and Java event inventory. libplacebo owns the shared GPU primitives.

| Repository | Role and inventory |
| --- | --- |
| [LibreELEC × CB1](https://github.com/CroqueMr/libreelec-CB1) | Images, release notes and source pins |
| [Kodi](https://github.com/CroqueMr/xbmc/pull/2) | Reader and presentation inventory |
| [FFmpeg](https://github.com/CroqueMr/FFmpeg/pull/1) | Demux and decoder inventory |
| [libplacebo](https://github.com/CroqueMr/libplacebo/pull/1) | GPU and shader inventory |
| [libbluray](https://github.com/CroqueMr/libbluray-cb1/pull/1) | Disc and Java inventory |

