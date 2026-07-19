#ifndef GHOSTTY_STUBS_H
#define GHOSTTY_STUBS_H

// Test-visible recorder for ghostty_terminal_mode_set calls.
//
// Implemented in ghostty_stubs.cpp; linked into tests that need to assert
// which terminal modes GhosttyVt::create() enables. The stubs (not the real
// libghostty) record the calls, so these tests verify the *contract* (the
// mode is enabled), not the underlying engine behavior.

#include <ghostty/vt/modes.h>

#ifdef __cplusplus
extern "C" {
#endif

// Clear all recorded mode-set calls. Call at the start of each test.
void ghostty_stubs_reset_modes(void);

// Returns true if ghostty_terminal_mode_set was called with this mode since
// the last reset, writing the last value to *out_value (if non-null).
// Returns false if the mode was never set.
bool ghostty_stubs_mode_set_called(GhosttyMode mode, bool *out_value);

#ifdef __cplusplus
}
#endif

#endif // GHOSTTY_STUBS_H
