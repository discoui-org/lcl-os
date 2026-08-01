# LCL Visual Design Guidelines

## Direction

Blend libadwaita clarity with macOS glassmorphism depth.

- Typography and spacing should stay calm, neutral, and practical.
- Surfaces should feel layered, not flat.
- Contrast should prioritize readability over colorful accents.

## Core Palette

Use neutral tones as defaults. Avoid catppuccin-like saturated accents.

- `bg/base`: `#111317`
- `bg/elevated`: `#1A1E24`
- `bg/glass-tint`: `rgba(17, 19, 23, 0.72)`
- `text/primary`: `#ECEFF4`
- `text/secondary`: `#C4CAD3`
- `text/muted`: `#9AA3AF`
- `border/outer`: `rgba(10, 12, 16, 0.47)`
- `border/inner`: `rgba(245, 248, 252, 0.34)`

## Window Chrome

- Corner radius baseline: `20px`
- Header controls: circular (`roundness = 2.0`), neutral fill, subtle border
- No hard separator line between titlebar and content
- Use inset dual-border (dark outer + light inner) for depth

## Glass Rules

- Glass is depth, not color.
- Keep tint dark-neutral for terminal and tooling surfaces.
- Prefer subtle blur/refraction with high text contrast.

## Terminal Rules

- Terminal background must be dark neutral, not transparent black-only and not saturated.
- Terminal text defaults to neutral white/gray tones.
- Accent colors should be optional and sparse.

## Implementation Notes

- Reuse shared chrome widgets between compositor and CSD overlays.
- Keep all radius/border tokens centralized where possible.
- If visual behavior diverges between CSD and SSD, compositor-side rendering is source of truth.
