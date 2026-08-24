# LCL test-device Gestalts

These profiles are development fixtures for real-device and QEMU testing. A
production Android port installs its selected profile as
`/vendor/etc/lcl/gestalt.json`; the runtime does not identify models or query
Android framework display policy.

Use the same profile on either substrate:

```bash
./main.py qemu --mobile --gestalt test-devices/galaxy-tab-s7-sm-t870.json
./main.py android --no-build --gestalt test-devices/galaxy-tab-s7-sm-t870.json
```
