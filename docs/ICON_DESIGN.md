# LCL icon design

## Purpose

This document defines the visual contract for LCL application icons and small
launcher artwork. The direction is clear, colourful, and layered, with a
restrained foreground mark over a strong colour field.

The result should feel confident at launcher size: one recognisable idea, a
strong colour field, and a simple foreground mark. It must not depend on
third-party product imagery or reproduced system artwork.

## Presentation: continuous rounded-rectangle mask

LCL presents application icons inside a system-owned **continuous
rounded-rectangle mask**. The mask belongs to the launcher or icon-view
component, not to the asset.

- Reference display size: `60 x 60` logical units in the mobile launcher.
- Canonical corner radius: `22` logical units at the reference size.
- Canonical corner roundness: `3.2`, the exponent used by LCL's continuous
  superellipse corner model.
- Larger grids and dock views scale the same mask proportions and asset; they
  do not use a second shape or a separately cropped file.
- Source SVGs stay square and unmasked. A full-bleed background must extend to
  all four edges of the square canvas, so the launcher can clip it cleanly.
- Never draw an opaque mask edge, outer shadow, or baked corner treatment
  into the SVG. Those create a visible double edge after masking.

The same canonical radius and roundness are used by the static launcher icon
and the opening transition's initial silhouette. Do not duplicate or override
the mask geometry in individual assets.

## Asset format and geometry

Every icon lives in this directory and uses this baseline:

| Property | Rule |
| --- | --- |
| Source format | Plain SVG 1.1, no embedded bitmap and no external references |
| Generated format | `1024 x 1024` PNG under `assets/icons/png` |
| Artboard | `1024 x 1024`, `viewBox="0 0 1024 1024"` |
| Background | Full square colour or gradient; never transparent by accident |
| Visible safe zone | Keep the primary mark inside the centred `760 x 760` area |
| Preferred mark size | About `480–640` units wide or tall, adjusted optically |
| Shape count | Prefer 1–5 purposeful shapes; merge incidental detail |
| Text | No visible label; a local icon-font glyph is permitted |
| Strokes | Avoid hairlines; use filled shapes or strokes at least `56` units |

Design against the final system mask, not only the square artboard. No meaningful
detail may sit near a corner or depend on the square’s corners being visible.
At `60` logical units, the foreground mark should read at a glance; if it needs
an explanation, simplify it.

## Layer model

Use two primary visual planes, named from back to front in the SVG:

1. **Background** — a solid colour or restrained two-to-three-stop gradient.
2. **Symbol** — one filled, high-contrast motif representing the application.

The depth is graphic, not photorealistic. Do not add the same decorative circle
or generic atmosphere shape to every icon. Avoid drop shadows, bevels, texture,
specular highlights, blur filters, noise, and fake glass reflections. These
details collapse at small sizes and are renderer-dependent.

## Symbol font

Application symbols use the locally packaged `CupertinoIcons` font at
`assets/fonts/cupertino-icons/CupertinoIcons.ttf`. SVG files place one glyph
in a `text` element with `font-family="CupertinoIcons"`; this is a vector glyph,
not a visible text label. The consuming icon renderer must register that local
font family before rendering the SVGs.

Generate the PNG derivatives with:

```sh
python3 tools/convert_icons_to_png.py
```

The converter creates an isolated Fontconfig environment, resolves the local
font explicitly, and verifies the signature and dimensions of every PNG.

## Colour palette

Use these system colours as a familiar starting point, not as universal
semantic meaning. The light and dark entries are paired for icon variants when
both are provided. Preserve the same symbol and composition across variants.

| Family | Light |  | Dark |  | Typical use |
| --- | --- | --- | --- | --- | --- |
| Red | <div style="display:inline-block;width:16px;height:16px;border-radius:5px;background:#FF3B30;vertical-align:middle"></div> | `#FF3B30` | <div style="display:inline-block;width:16px;height:16px;border-radius:5px;background:#FF453A;vertical-align:middle"></div> | `#FF453A` | destructive, recording, urgent utility |
| Orange | <div style="display:inline-block;width:16px;height:16px;border-radius:5px;background:#FF9500;vertical-align:middle"></div> | `#FF9500` | <div style="display:inline-block;width:16px;height:16px;border-radius:5px;background:#FF9F0A;vertical-align:middle"></div> | `#FF9F0A` | media, warmth, active work |
| Yellow | <div style="display:inline-block;width:16px;height:16px;border-radius:5px;background:#FFCC00;vertical-align:middle"></div> | `#FFCC00` | <div style="display:inline-block;width:16px;height:16px;border-radius:5px;background:#FFD60A;vertical-align:middle"></div> | `#FFD60A` | notes, ideas, caution without error |
| Green | <div style="display:inline-block;width:16px;height:16px;border-radius:5px;background:#34C759;vertical-align:middle"></div> | `#34C759` | <div style="display:inline-block;width:16px;height:16px;border-radius:5px;background:#30D158;vertical-align:middle"></div> | `#30D158` | success, communication, growth |
| Mint | <div style="display:inline-block;width:16px;height:16px;border-radius:5px;background:#00C7BE;vertical-align:middle"></div> | `#00C7BE` | <div style="display:inline-block;width:16px;height:16px;border-radius:5px;background:#66D4CF;vertical-align:middle"></div> | `#66D4CF` | fresh content and ambient utilities |
| Teal | <div style="display:inline-block;width:16px;height:16px;border-radius:5px;background:#30B0C7;vertical-align:middle"></div> | `#30B0C7` | <div style="display:inline-block;width:16px;height:16px;border-radius:5px;background:#40C8E0;vertical-align:middle"></div> | `#40C8E0` | network, spatial, or cool utilities |
| Cyan | <div style="display:inline-block;width:16px;height:16px;border-radius:5px;background:#32ADE6;vertical-align:middle"></div> | `#32ADE6` | <div style="display:inline-block;width:16px;height:16px;border-radius:5px;background:#64D2FF;vertical-align:middle"></div> | `#64D2FF` | browser, information, sky-like surfaces |
| Blue | <div style="display:inline-block;width:16px;height:16px;border-radius:5px;background:#007AFF;vertical-align:middle"></div> | `#007AFF` | <div style="display:inline-block;width:16px;height:16px;border-radius:5px;background:#0A84FF;vertical-align:middle"></div> | `#0A84FF` | primary navigation and general-purpose apps |
| Indigo | <div style="display:inline-block;width:16px;height:16px;border-radius:5px;background:#5856D6;vertical-align:middle"></div> | `#5856D6` | <div style="display:inline-block;width:16px;height:16px;border-radius:5px;background:#5E5CE6;vertical-align:middle"></div> | `#5E5CE6` | developer tools and focused work |
| Purple | <div style="display:inline-block;width:16px;height:16px;border-radius:5px;background:#AF52DE;vertical-align:middle"></div> | `#AF52DE` | <div style="display:inline-block;width:16px;height:16px;border-radius:5px;background:#BF5AF2;vertical-align:middle"></div> | `#BF5AF2` | creative tools and personalization |
| Pink | <div style="display:inline-block;width:16px;height:16px;border-radius:5px;background:#FF2D55;vertical-align:middle"></div> | `#FF2D55` | <div style="display:inline-block;width:16px;height:16px;border-radius:5px;background:#FF375F;vertical-align:middle"></div> | `#FF375F` | personal, expressive, or health-adjacent apps |
| Brown | <div style="display:inline-block;width:16px;height:16px;border-radius:5px;background:#A2845E;vertical-align:middle"></div> | `#A2845E` | <div style="display:inline-block;width:16px;height:16px;border-radius:5px;background:#AC8E68;vertical-align:middle"></div> | `#AC8E68` | documents, archive, and tactile material |
| Gray | <div style="display:inline-block;width:16px;height:16px;border-radius:5px;background:#8E8E93;vertical-align:middle"></div> | `#8E8E93` | <div style="display:inline-block;width:16px;height:16px;border-radius:5px;background:#8E8E93;vertical-align:middle"></div> | `#8E8E93` | neutral system tools only |

- One family is dominant per icon. A neighbouring hue may appear only as a
  gradient stop or secondary plane.
- Use near-white (`#FFFFFF`) for most foreground marks; use deep navy
  (`#0B1020`) only when it creates clearly stronger contrast.
- Do not make colour alone carry status or meaning. The symbol and label must
  remain understandable in grayscale and at reduced contrast.
- Export in sRGB. Do not rely on Display P3-only colour differences until LCL
  has an end-to-end wide-gamut asset and compositor contract.

## Symbol language

- Express exactly one noun or action: terminal prompt, folder, camera, clock,
  browser globe, and similar direct metaphors.
- Use bold filled geometry with consistent visual mass. Outline-only glyphs
  tend to disappear against rich launcher backgrounds.
- Prefer circles, rounded rectangles, rounded capsules, arcs, and simple
  diagonals. Keep corner radii intentional and generous.
- Centre symbols geometrically first, then make small optical corrections for
  asymmetrical forms such as a terminal prompt or musical note.
- Do not place generic UI controls, screenshots, device silhouettes, or tiny
  file/document details inside an app icon.
- Do not reuse another platform’s distinctive app-icon compositions or brand
  marks.

## Variants

An icon may provide these sibling files when the shell supports them:

```text
name.svg          default
name-dark.svg     dark presentation
name-mono.svg     single-colour/tinted presentation
```

`name-dark.svg` keeps the same background-to-symbol relationship while using
the paired dark palette. `name-mono.svg` reduces the artwork to one solid
foreground shape on transparency; it must remain identifiable without its
colour field. Do not replace the core symbol between variants.

## Review checklist

Before adding an icon, check it at `60`, `40`, and `24` logical units:

- Does the central mark remain recognizable after the continuous rounded crop?
- Is the mark clearly distinct from other LCL applications in grayscale?
- Does the source fill the square canvas without baking in the system mask?
- Are all meaningful shapes inside the safe zone?
- Does one colour family dominate and retain sufficient foreground contrast?
- Are there no embedded raster assets, external SVG dependencies, text labels,
  third-party artwork, or renderer-specific filter effects?
- Does the default, dark, and monochrome version preserve the same identity?
