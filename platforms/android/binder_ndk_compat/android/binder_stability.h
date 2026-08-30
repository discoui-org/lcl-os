#pragma once

// The NDK AIDL compiler emits this private platform include for
// --stability=vintf, while the public NDK headers omit it. LCL only needs the
// system-side transaction flag and the runtime symbol used by generated
// Bn*::createBinder() implementations.

#include <android/binder_ibinder.h>

__BEGIN_DECLS

enum {
    FLAG_PRIVATE_LOCAL = 0,
};

void lcl_AIBinder_markVintfStability(AIBinder* binder);

__END_DECLS

// Route generated Bn*::createBinder() calls through the compatibility bridge;
// the NDK stub library does not expose this platform symbol at link time.
#define AIBinder_markVintfStability lcl_AIBinder_markVintfStability
