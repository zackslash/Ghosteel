// Stub implementations of the Ghostty C API for test builds, allowing
// ghosttyvt.cpp (and terminalview.cpp) to link without the real
// libghostty-vt.a. Most are no-ops; ghostty_terminal_mode_set records
// its calls (see ghostty_stubs.h) for contract tests.

#include "ghostty_stubs.h"

#include <ghostty/vt.h>
#include <string.h>
#include <stdlib.h>
#include <map>

// Recorded mode-set calls for test inspection (see ghostty_stubs.h).
static std::map<GhosttyMode, bool> g_stubs_modesSet;

// ---- Terminal ----

GHOSTTY_API GhosttyResult ghostty_terminal_new(
    const GhosttyAllocator*, GhosttyTerminal* out, const GhosttyTerminalOptions*)
{
    if (out) *out = (GhosttyTerminal)1;
    return GHOSTTY_SUCCESS;
}

GHOSTTY_API void ghostty_terminal_free(GhosttyTerminal) {}

GHOSTTY_API void ghostty_terminal_reset(GhosttyTerminal) {}

GHOSTTY_API GhosttyResult ghostty_terminal_set(
    GhosttyTerminal, GhosttyTerminalOption, const void*)
{
    return GHOSTTY_SUCCESS;
}

GHOSTTY_API GhosttyResult ghostty_terminal_get(
    GhosttyTerminal, GhosttyTerminalData, void* out)
{
    if (out) memset(out, 0, 8);
    return GHOSTTY_SUCCESS;
}

GHOSTTY_API GhosttyResult ghostty_terminal_get_multi(
    GhosttyTerminal, size_t, const GhosttyTerminalData*, void**, size_t*)
{
    return GHOSTTY_SUCCESS;
}

GHOSTTY_API void ghostty_terminal_vt_write(
    GhosttyTerminal, const uint8_t*, size_t) {}

GHOSTTY_API void ghostty_terminal_scroll_viewport(
    GhosttyTerminal, GhosttyTerminalScrollViewport) {}

GHOSTTY_API GhosttyResult ghostty_terminal_resize(
    GhosttyTerminal, uint16_t, uint16_t, uint32_t, uint32_t)
{
    return GHOSTTY_SUCCESS;
}

GHOSTTY_API GhosttyResult ghostty_terminal_mode_get(
    GhosttyTerminal, GhosttyMode, bool* out_value)
{
    if (out_value) *out_value = false;
    return GHOSTTY_SUCCESS;
}

GHOSTTY_API GhosttyResult ghostty_terminal_mode_set(
    GhosttyTerminal, GhosttyMode mode, bool value)
{
    g_stubs_modesSet[mode] = value;
    return GHOSTTY_SUCCESS;
}

// --- Test-visible recorder (declared in ghostty_stubs.h) ---

extern "C" void ghostty_stubs_reset_modes(void)
{
    g_stubs_modesSet.clear();
}

extern "C" bool ghostty_stubs_mode_set_called(GhosttyMode mode, bool *out_value)
{
    auto it = g_stubs_modesSet.find(mode);
    if (it == g_stubs_modesSet.end())
        return false;
    if (out_value)
        *out_value = it->second;
    return true;
}

GHOSTTY_API GhosttyResult ghostty_terminal_grid_ref(
    GhosttyTerminal, GhosttyPoint, GhosttyGridRef*)
{
    return GHOSTTY_SUCCESS;
}

GHOSTTY_API GhosttyResult ghostty_terminal_grid_ref_track(
    GhosttyTerminal, GhosttyPoint, GhosttyTrackedGridRef*)
{
    return GHOSTTY_SUCCESS;
}

GHOSTTY_API GhosttyResult ghostty_terminal_point_from_grid_ref(
    GhosttyTerminal, const GhosttyGridRef*, GhosttyPointTag, GhosttyPointCoordinate*)
{
    return GHOSTTY_SUCCESS;
}

GHOSTTY_API GhosttyResult ghostty_grid_ref_cell(
    const GhosttyGridRef*, GhosttyCell*)
{
    return GHOSTTY_SUCCESS;
}

GHOSTTY_API GhosttyResult ghostty_cell_get(GhosttyCell, GhosttyCellData, void *out)
{
    if (out) *static_cast<int*>(out) = 0;
    return GHOSTTY_SUCCESS;
}

GHOSTTY_API GhosttyResult ghostty_grid_ref_row(
    const GhosttyGridRef*, GhosttyRow*)
{
    return GHOSTTY_SUCCESS;
}

GHOSTTY_API GhosttyResult ghostty_row_get(GhosttyRow, GhosttyRowData, void *out)
{
    if (out) *static_cast<bool*>(out) = false;
    return GHOSTTY_SUCCESS;
}

GHOSTTY_API GhosttyResult ghostty_grid_ref_graphemes(
    const GhosttyGridRef*, uint32_t*, size_t, size_t *out_len)
{
    if (out_len) *out_len = 0;
    return GHOSTTY_SUCCESS;
}

GHOSTTY_API GhosttyResult ghostty_grid_ref_hyperlink_uri(
    const GhosttyGridRef*, uint8_t*, size_t, size_t *out_len)
{
    if (out_len) *out_len = 0;
    return GHOSTTY_SUCCESS;
}

// ---- Render state ----

GHOSTTY_API GhosttyResult ghostty_render_state_new(
    const GhosttyAllocator*, GhosttyRenderState* out)
{
    if (out) *out = (GhosttyRenderState)1;
    return GHOSTTY_SUCCESS;
}

GHOSTTY_API void ghostty_render_state_free(GhosttyRenderState) {}

GHOSTTY_API GhosttyResult ghostty_render_state_update(
    GhosttyRenderState, GhosttyTerminal)
{
    return GHOSTTY_SUCCESS;
}

GHOSTTY_API GhosttyResult ghostty_render_state_get(
    GhosttyRenderState, GhosttyRenderStateData, void*)
{
    return GHOSTTY_SUCCESS;
}

GHOSTTY_API GhosttyResult ghostty_render_state_get_multi(
    GhosttyRenderState, size_t, const GhosttyRenderStateData*, void**, size_t*)
{
    return GHOSTTY_SUCCESS;
}

GHOSTTY_API GhosttyResult ghostty_render_state_set(
    GhosttyRenderState, GhosttyRenderStateOption, const void*)
{
    return GHOSTTY_SUCCESS;
}

GHOSTTY_API GhosttyResult ghostty_render_state_colors_get(
    GhosttyRenderState, GhosttyRenderStateColors*)
{
    return GHOSTTY_SUCCESS;
}

GHOSTTY_API GhosttyResult ghostty_render_state_row_iterator_new(
    const GhosttyAllocator*, GhosttyRenderStateRowIterator* out)
{
    if (out) *out = (GhosttyRenderStateRowIterator)1;
    return GHOSTTY_SUCCESS;
}

GHOSTTY_API bool ghostty_render_state_row_iterator_next(
    GhosttyRenderStateRowIterator)
{
    return false;
}

GHOSTTY_API void ghostty_render_state_row_iterator_free(
    GhosttyRenderStateRowIterator) {}

GHOSTTY_API GhosttyResult ghostty_render_state_row_get(
    GhosttyRenderStateRowIterator, GhosttyRenderStateRowData, void*)
{
    return GHOSTTY_SUCCESS;
}

GHOSTTY_API GhosttyResult ghostty_render_state_row_get_multi(
    GhosttyRenderStateRowIterator, size_t,
    const GhosttyRenderStateRowData*, void**, size_t*)
{
    return GHOSTTY_SUCCESS;
}

GHOSTTY_API GhosttyResult ghostty_render_state_row_set(
    GhosttyRenderStateRowIterator, GhosttyRenderStateRowOption, const void*)
{
    return GHOSTTY_SUCCESS;
}

GHOSTTY_API GhosttyResult ghostty_render_state_row_cells_new(
    const GhosttyAllocator*, GhosttyRenderStateRowCells* out)
{
    if (out) *out = (GhosttyRenderStateRowCells)1;
    return GHOSTTY_SUCCESS;
}

GHOSTTY_API bool ghostty_render_state_row_cells_next(
    GhosttyRenderStateRowCells)
{
    return false;
}

GHOSTTY_API GhosttyResult ghostty_render_state_row_cells_select(
    GhosttyRenderStateRowCells, uint16_t)
{
    return GHOSTTY_SUCCESS;
}

GHOSTTY_API GhosttyResult ghostty_render_state_row_cells_get(
    GhosttyRenderStateRowCells, GhosttyRenderStateRowCellsData, void* out)
{
    if (out) memset(out, 0, 8);
    return GHOSTTY_SUCCESS;
}

GHOSTTY_API GhosttyResult ghostty_render_state_row_cells_get_multi(
    GhosttyRenderStateRowCells, size_t,
    const GhosttyRenderStateRowCellsData*, void**, size_t*)
{
    return GHOSTTY_SUCCESS;
}

GHOSTTY_API void ghostty_render_state_row_cells_free(
    GhosttyRenderStateRowCells) {}

// ---- Key encoder ----

GHOSTTY_API GhosttyResult ghostty_key_encoder_new(
    const GhosttyAllocator*, GhosttyKeyEncoder* out)
{
    if (out) *out = (GhosttyKeyEncoder)1;
    return GHOSTTY_SUCCESS;
}

GHOSTTY_API void ghostty_key_encoder_free(GhosttyKeyEncoder) {}

GHOSTTY_API void ghostty_key_encoder_setopt_from_terminal(
    GhosttyKeyEncoder, GhosttyTerminal) {}

GHOSTTY_API void ghostty_key_encoder_setopt(
    GhosttyKeyEncoder, GhosttyKeyEncoderOption, const void*) {}

GHOSTTY_API GhosttyResult ghostty_key_encoder_encode(
    GhosttyKeyEncoder, GhosttyKeyEvent,
    char* out, size_t cap, size_t* written)
{
    if (written) *written = 0;
    (void)out; (void)cap;
    return GHOSTTY_SUCCESS;
}

// ---- Key event ----

GHOSTTY_API GhosttyResult ghostty_key_event_new(
    const GhosttyAllocator*, GhosttyKeyEvent* out)
{
    if (out) *out = (GhosttyKeyEvent)1;
    return GHOSTTY_SUCCESS;
}

GHOSTTY_API void ghostty_key_event_free(GhosttyKeyEvent) {}

GHOSTTY_API void ghostty_key_event_set_action(GhosttyKeyEvent, GhosttyKeyAction) {}

GHOSTTY_API GhosttyKeyAction ghostty_key_event_get_action(GhosttyKeyEvent)
{
    return GHOSTTY_KEY_ACTION_PRESS;
}

GHOSTTY_API void ghostty_key_event_set_key(GhosttyKeyEvent, GhosttyKey) {}

GHOSTTY_API GhosttyKey ghostty_key_event_get_key(GhosttyKeyEvent)
{
    return GHOSTTY_KEY_UNIDENTIFIED;
}

GHOSTTY_API void ghostty_key_event_set_mods(GhosttyKeyEvent, GhosttyMods) {}

GHOSTTY_API GhosttyMods ghostty_key_event_get_mods(GhosttyKeyEvent) { return 0; }

GHOSTTY_API void ghostty_key_event_set_consumed_mods(GhosttyKeyEvent, GhosttyMods) {}

GHOSTTY_API GhosttyMods ghostty_key_event_get_consumed_mods(GhosttyKeyEvent) { return 0; }

GHOSTTY_API void ghostty_key_event_set_composing(GhosttyKeyEvent, bool) {}

GHOSTTY_API bool ghostty_key_event_get_composing(GhosttyKeyEvent) { return false; }

GHOSTTY_API void ghostty_key_event_set_utf8(GhosttyKeyEvent, const char*, size_t) {}

GHOSTTY_API const char* ghostty_key_event_get_utf8(GhosttyKeyEvent, size_t* len)
{
    if (len) *len = 0;
    return nullptr;
}

GHOSTTY_API void ghostty_key_event_set_unshifted_codepoint(GhosttyKeyEvent, uint32_t) {}

GHOSTTY_API uint32_t ghostty_key_event_get_unshifted_codepoint(GhosttyKeyEvent) { return 0; }

// ---- Mouse encoder ----

GHOSTTY_API GhosttyResult ghostty_mouse_encoder_new(
    const GhosttyAllocator*, GhosttyMouseEncoder* out)
{
    if (out) *out = (GhosttyMouseEncoder)1;
    return GHOSTTY_SUCCESS;
}

GHOSTTY_API void ghostty_mouse_encoder_free(GhosttyMouseEncoder) {}

GHOSTTY_API void ghostty_mouse_encoder_setopt_from_terminal(
    GhosttyMouseEncoder, GhosttyTerminal) {}

GHOSTTY_API void ghostty_mouse_encoder_setopt(
    GhosttyMouseEncoder, GhosttyMouseEncoderOption, const void*) {}

GHOSTTY_API void ghostty_mouse_encoder_reset(GhosttyMouseEncoder) {}

GHOSTTY_API GhosttyResult ghostty_mouse_encoder_encode(
    GhosttyMouseEncoder, GhosttyMouseEvent,
    char* out, size_t cap, size_t* written)
{
    if (written) *written = 0;
    (void)out; (void)cap;
    return GHOSTTY_SUCCESS;
}

// ---- Mouse event ----

GHOSTTY_API GhosttyResult ghostty_mouse_event_new(
    const GhosttyAllocator*, GhosttyMouseEvent* out)
{
    if (out) *out = (GhosttyMouseEvent)1;
    return GHOSTTY_SUCCESS;
}

GHOSTTY_API void ghostty_mouse_event_free(GhosttyMouseEvent) {}

GHOSTTY_API void ghostty_mouse_event_set_action(GhosttyMouseEvent, GhosttyMouseAction) {}

GHOSTTY_API GhosttyMouseAction ghostty_mouse_event_get_action(GhosttyMouseEvent)
{
    return GHOSTTY_MOUSE_ACTION_PRESS;
}

GHOSTTY_API void ghostty_mouse_event_set_button(GhosttyMouseEvent, GhosttyMouseButton) {}

GHOSTTY_API void ghostty_mouse_event_clear_button(GhosttyMouseEvent) {}

GHOSTTY_API bool ghostty_mouse_event_get_button(GhosttyMouseEvent, GhosttyMouseButton*)
{
    return false;
}

GHOSTTY_API void ghostty_mouse_event_set_mods(GhosttyMouseEvent, GhosttyMods) {}

GHOSTTY_API GhosttyMods ghostty_mouse_event_get_mods(GhosttyMouseEvent) { return 0; }

GHOSTTY_API void ghostty_mouse_event_set_position(GhosttyMouseEvent, GhosttyMousePosition) {}

GHOSTTY_API GhosttyMousePosition ghostty_mouse_event_get_position(GhosttyMouseEvent)
{
    return {0.0f, 0.0f};
}

// ---- Paste ----

GHOSTTY_API bool ghostty_paste_is_safe(const char*, size_t) { return true; }

GHOSTTY_API GhosttyResult ghostty_paste_encode(
    char*, size_t, bool, char*, size_t, size_t* written)
{
    if (written) *written = 0;
    return GHOSTTY_SUCCESS;
}

// ---- Color ----

GHOSTTY_API void ghostty_color_rgb_get(
    GhosttyColorRgb color, uint8_t* r, uint8_t* g, uint8_t* b)
{
    if (r) *r = color.r;
    if (g) *g = color.g;
    if (b) *b = color.b;
}

// ---- Type JSON ----

GHOSTTY_API const char* ghostty_type_json(void) { return "{}"; }
