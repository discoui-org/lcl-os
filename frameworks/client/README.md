# lcl-client

`lcl-client` is the toolkit-independent C++20 client boundary for LCL. It
links only the public compositor and private raster protocols; it has no Qt,
Skia, Yoga, QuickJS, or `lcl-ui` dependency.

`lcl::client::SurfaceClient` owns one compositor surface and its rasterd
producer connection. It does not run an event loop. Poll `compositorFd()` and
`rasterFd()`, then call the non-blocking `dispatch()` method to receive typed
configure, focus, input, presentation, discard, close, and buffer-release
events.

Native frames use move-only descriptor ownership:

```cpp
lcl::client::DmaBufFrame frame;
frame.bufferId = id;
frame.contentRevision = revision;
frame.width = width;
frame.height = height;
frame.stride = strideBytes;
frame.format = lcl::platform::kDmaBufFormatArgb8888;
frame.damage = {0, 0, logicalWidth, logicalHeight};
frame.buffer = lcl::client::OwnedFd(dmaBufFd);
frame.acquireFence = lcl::client::OwnedFd(acquireFenceFd);
surface.submitFrame(std::move(frame));
```

Android uses `PlatformNativeFrame::writeHandle` to synchronously queue one
AHardwareBuffer handle on the registered sideband socket before raster
metadata is sent. A `BufferReleasedEvent` returns an optional release fence.

`CompositorConnection` and `RasterConnection` are lower-level compatibility
transports used by the retained `lcl-ui` producer during migration.
