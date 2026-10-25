# Provenance and accreditation

GrumpyVNC is a **self-contained fork**. It carries vendored copies of its
upstream project and of every dependency, so it builds from a single clone with
no network access and **no git submodules** — nothing here depends on an
external repository continuing to exist. Each vendored component keeps its own
licence file; refer to that file for its terms.

**Upstream fixes are applied by hand** from the sources below. There is no live
tie to any of them; the URLs are recorded for credit and so a maintainer knows
where to pull a fix from.

## This fork's upstream

- **grumpyscreen** — <https://github.com/pellcorp/grumpyscreen> (GPL-3.0), itself
  a fork of **GuppyScreen** (<https://github.com/ballaswag/guppyscreen>).

GrumpyVNC adds, on top of grumpyscreen, a built-in RFB/VNC server (`vnc/`, with a
DES engine for VNC authentication), an ARMv6 cross-build (`crossbuild/`), and
Centauri / C2 integration. Those additions are Copyright (c) 2026 B. C. Services,
GPL-3.0.

## Vendored dependencies (upstream used git submodules; here they are vendored in full)

| Component | Upstream | Branch | Licence |
|---|---|---|---|
| `lvgl/` | <https://github.com/pellcorp/lvgl.git> | `g8.3.11` | MIT (`lvgl/LICENCE.txt`) |
| `lv_drivers/` | <https://github.com/pellcorp/lv_drivers.git> | `g8.3.0` | MIT (`lv_drivers/LICENSE`) |
| `libhv/` | <https://github.com/ithewei/libhv.git> | — | BSD-3-Clause (`libhv/LICENSE`) |
| `fmt/` | <https://github.com/fmtlib/fmt.git> | — | MIT (`fmt/LICENSE`) |
| `wpa_supplicant/` | hostap — <https://w1.fi/hostap.git> | — | BSD (`wpa_supplicant/COPYING`) |

Each dependency is vendored **trimmed to the source this project actually
builds**, plus its licence file; the upstream projects' own demos, examples,
tests and documentation are not reproduced here. All five licences are
compatible with this project's GPL-3.0.

The `material*/` icon assets derive from the Material Design Icons webfont
(Pictogrammers / Templarian); see the credits in `README.md`.
