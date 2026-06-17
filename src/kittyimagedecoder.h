#ifndef KITTYIMAGEDECODER_H
#define KITTYIMAGEDECODER_H

// Qt defines `emit` as an empty macro, which conflicts with the
// Ghostty C API using 'emit' as a struct field name.
#ifdef emit
#undef emit
#endif

extern "C" {
#include <ghostty/vt/sys.h>
}

// Registers the PNG decoder callback for Kitty Graphics Protocol.
// Must be called ONCE at app startup, before any terminal is created.
// The callback is process-global (ghostty_sys_set is not per-terminal).
//
// Thread safety: The callback may be called from any Ghostty thread.
// Currently all VT ops run on the GUI thread, so QImage is safe.
void kittyImageDecoderRegister();

#endif // KITTYIMAGEDECODER_H
