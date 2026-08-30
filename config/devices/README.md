# LCL device Gestalts

Profile filenames exactly match Android's `ro.product.model` value so real
device deployment can select the correct Gestalt automatically. A production
Android port installs its selected profile as
`/vendor/etc/lcl/gestalt.json`; the runtime does not identify models or query
Android framework display policy.

The root-level `shell` field is the session presentation profile. Phone and
tablet profiles use `"mobile"`; the canonical desktop profile uses
`"desktop"`. Both shell executables remain in the same rootfs and the session
launcher starts exactly one of them from this field.

`main.py android` reads the connected device model through ADB and uses the
matching profile when present. `--gestalt` remains available as an explicit
override. Profiles can also be passed to QEMU:

```bash
./main.py android --no-build
./main.py android --no-build --gestalt config/devices/SM-T870.json
./main.py qemu --mobile --gestalt config/devices/SM-T870.json
```
