# lcl-qt

`lcl-qt` hosts a Qt 6.4+ QML scene without creating a native Qt platform
window. `QQuickRenderControl` is integrated with LCL's non-blocking surface
client, and compositor/raster socket readiness is driven by Qt's event loop.

The first implementation renders Qt Quick into an offscreen image and uploads
that image into the existing DMA-BUF/AHardwareBuffer pool. From rasterd onward
the path is zero-copy and DisplayList-free. A future QRhi/native render-target
backend can remove this one staging upload without changing the public API or
the LCL protocol.

Keyboard, text input, mouse, wheel, focus, and close events are translated to
public Qt events. Touch currently follows Qt's primary-pointer mouse path;
multi-touch delivery belongs in the later QPA/input-device integration.

Canonical Linux builds install Qt in the pinned QEMU Docker builder and compile
`lcl-qt`; the host machine does not need Qt. The current Android compositor
build intentionally does not require a Qt SDK; Qt's Android runtime integration
is a later migration stage. For a standalone Linux host build, provide Qt
through `CMAKE_PREFIX_PATH`.

```sh
cmake -S . -B out/qt -DCMAKE_PREFIX_PATH=/path/to/Qt/6.4/gcc_64
cmake --build out/qt --target lcl-qt
```
