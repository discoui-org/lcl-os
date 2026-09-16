#pragma once

// gfxstream's Android-native-buffer extension historically includes the AOSP
// platform header <cutils/native_handle.h>.  The public NDK used for this
// Android-only host adapter deliberately does not expose it.  gfxstream only
// needs the ABI layout for opaque pointer fields here; LCL never allocates or
// dereferences a native_handle through this shim.
typedef struct native_handle {
    int version;
    int numFds;
    int numInts;
    int data[0];
} native_handle_t;
