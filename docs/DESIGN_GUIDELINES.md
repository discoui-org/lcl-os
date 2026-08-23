# LCL Visual Design Guidelines

## Direction

Blend clear hierarchy with glassmorphism depth.

- Typography and spacing should stay calm, neutral, and practical.
- Surfaces should feel layered, not flat.
- Contrast should prioritize readability over colorful accents.

## Core Palette

Use neutral system tones as defaults with one restrained blue accent family.

- `bg/base`: `#000000`
- `bg/surface`: `#1C1C1E`
- `bg/elevated`: `rgba(44, 44, 46, 0.95)`
- `bg/glass-tint`: `rgba(28, 28, 30, 0.72)`
- `text/primary`: `#FFFFFF`
- `text/secondary`: `rgba(235, 235, 245, 0.60)`
- `separator`: `rgba(84, 84, 88, 0.50)`
- `accent`: `#0A84FF`
- `focus`: `rgba(100, 210, 255, 0.86)`

## Window Chrome

- Corner radius baseline: `20` logical units
- Header controls: circular (`roundness = 2.0`), neutral fill, subtle border
- No hard separator line between titlebar and content
- Use inset dual-border (dark outer + light inner) for depth

## Glass Rules

- Glass is depth, not color.
- Keep tint dark-neutral for terminal and tooling surfaces.
- Prefer subtle blur/refraction with high text contrast.

## Control Rules

- Built-in controls resolve colors, borders, radii, and interaction values
  through `lcl-theme`; drawing code must not embed a second palette.
- Text fields use a restrained focus ring and an accent-colored caret.
- Toggle selection uses the accent family; inactive tracks and menu selection
  remain neutral.
- Application-specific colors remain explicit overrides and must not mutate the
  shared theme.

## Terminal Rules

- Terminal background must be dark neutral, not transparent black-only and not saturated.
- Terminal text defaults to neutral white/gray tones.
- Accent colors should be optional and sparse.

## Implementation Notes

- CSD and SSD use the same renderer-independent `WindowChromeWidget` state and
  produce the same logical chrome `DisplayList`; CSD records it through the
  client `Canvas`, while SSD replays it in the compositor.
- Keep all radius/border tokens centralized where possible.
- Layout, hit testing, actions, interaction state, motion, and visual color
  resolution come from `lcl-window-chrome`; neither host may reimplement them.
- Window motion and hit testing must share the same forward/inverse
  `WindowGroupTransform`. Device scale is applied only during raster replay.
