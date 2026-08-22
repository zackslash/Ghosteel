// Stub implementations of the Ghostty C API for test builds, allowing
// ghosttyvt.cpp (and terminalview.cpp) to link without the real
// libghostty-vt.a. Most are no-ops; ghostty_terminal_set records mode
// calls (see ghostty_stubs.h) for contract tests.

#include "ghostty_stubs.h"

#include <ghostty/vt.h>
#include <string.h>
#include <stdlib.h>
#include <map>
#include <string>
#include <vector>

// Recorded mode-set calls for test inspection (see ghostty_stubs.h).
static std::map<GhosttyMode, bool> g_stubs_modesSet;

// Outstanding-handle counters for lifecycle tests (see ghostty_stubs.h).
static int g_stubs_outstandingTerminals = 0;
static int g_stubs_outstandingRenderStates = 0;

// Opt-in canned grid fixture for exportScrollback tests (see ghostty_stubs.h).
// Disarmed by default so the zero-output stubs (COLS=0) keep every other test
// unchanged. When armed, ghostty_terminal_get / grid_ref / row_get serve the
// fixture's rows x cols of ASCII text plus per-row WRAP_CONTINUATION flags.
static bool g_stubs_gridArmed = false;
static uint16_t g_stubs_gridCols = 0;
static uint16_t g_stubs_gridRows = 0;
static std::vector<std::string> g_stubs_gridText; // per-row ASCII text
static std::vector<bool> g_stubs_gridCont;        // per-row WRAP_CONTINUATION
static uint16_t g_stubs_cursorX = 0;              // served as CURSOR_X
static uint16_t g_stubs_cursorY = 0;              // served as CURSOR_Y

// ---- Terminal ----

GHOSTTY_API GhosttyResult ghostty_terminal_new(
    const GhosttyAllocator*, GhosttyTerminal* out, uint16_t, uint16_t)
{
    if (out) {
        *out = (GhosttyTerminal)1;
        g_stubs_outstandingTerminals++;
    }
    return GHOSTTY_SUCCESS;
}

GHOSTTY_API void ghostty_terminal_free(GhosttyTerminal)
{
    g_stubs_outstandingTerminals--;
}

GHOSTTY_API void ghostty_terminal_reset(GhosttyTerminal) {}

GHOSTTY_API GhosttyResult ghostty_terminal_set(
    GhosttyTerminal, GhosttyTerminalOption option, const void* value)
{
    if ((option == GHOSTTY_TERMINAL_OPT_MODE ||
         option == GHOSTTY_TERMINAL_OPT_MODE_DEFAULT) && value) {
        auto* cfg = static_cast<const GhosttyTerminalModeConfig*>(value);
        g_stubs_modesSet[cfg->mode] = cfg->value;
    }
    return GHOSTTY_SUCCESS;
}

GHOSTTY_API GhosttyResult ghostty_terminal_get(
    GhosttyTerminal, GhosttyTerminalData data, void* out)
{
    if (!out)
        return GHOSTTY_SUCCESS;
    if (g_stubs_gridArmed) {
        switch (data) {
        case GHOSTTY_TERMINAL_DATA_ACTIVE_SCREEN:
            *static_cast<GhosttyTerminalScreen*>(out) = GHOSTTY_TERMINAL_SCREEN_PRIMARY;
            return GHOSTTY_SUCCESS;
        case GHOSTTY_TERMINAL_DATA_COLS:
            *static_cast<uint16_t*>(out) = g_stubs_gridCols;
            return GHOSTTY_SUCCESS;
        case GHOSTTY_TERMINAL_DATA_ROWS:
            *static_cast<uint16_t*>(out) = g_stubs_gridRows;
            return GHOSTTY_SUCCESS;
        case GHOSTTY_TERMINAL_DATA_TOTAL_ROWS:
            *static_cast<size_t*>(out) = g_stubs_gridRows;
            return GHOSTTY_SUCCESS;
        case GHOSTTY_TERMINAL_DATA_CURSOR_X:
            *static_cast<uint16_t*>(out) = g_stubs_cursorX;
            return GHOSTTY_SUCCESS;
        case GHOSTTY_TERMINAL_DATA_CURSOR_Y:
            *static_cast<uint16_t*>(out) = g_stubs_cursorY;
            return GHOSTTY_SUCCESS;
        default:
            break;
        }
    }
    // Covers the largest struct any caller reads (scrollbar, 24B) with headroom.
    memset(out, 0, 32);
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

extern "C" void ghostty_stubs_reset_handles(void)
{
    g_stubs_outstandingTerminals = 0;
    g_stubs_outstandingRenderStates = 0;
}

extern "C" int ghostty_stubs_outstanding_terminals(void)
{
    return g_stubs_outstandingTerminals;
}

extern "C" int ghostty_stubs_outstanding_render_states(void)
{
    return g_stubs_outstandingRenderStates;
}

// Arm the canned grid fixture. Copies the per-row ASCII text and
// WRAP_CONTINUATION flags so the caller's buffers need not outlive the call.
// `cont` is a per-row 0/1 char array (std::vector<bool> has no data()).
extern "C" void ghostty_stubs_set_grid(
    uint16_t cols, uint16_t rows, const char* const* text, const char* cont)
{
    g_stubs_gridCols = cols;
    g_stubs_gridRows = rows;
    g_stubs_gridText.clear();
    g_stubs_gridCont.clear();
    g_stubs_gridText.reserve(rows);
    g_stubs_gridCont.reserve(rows);
    for (uint16_t r = 0; r < rows; r++) {
        g_stubs_gridText.push_back(text ? (text[r] ? text[r] : "") : "");
        g_stubs_gridCont.push_back(cont ? cont[r] != 0 : false);
    }
    g_stubs_gridArmed = true;
}

// Disarm the fixture, restoring the default zero-output stub behavior.
extern "C" void ghostty_stubs_clear_grid(void)
{
    g_stubs_gridArmed = false;
    g_stubs_gridCols = 0;
    g_stubs_gridRows = 0;
    g_stubs_gridText.clear();
    g_stubs_gridCont.clear();
    g_stubs_cursorX = 0;
    g_stubs_cursorY = 0;
}

// Set the cursor position served as CURSOR_X/CURSOR_Y while the grid fixture
// is armed.
extern "C" void ghostty_stubs_set_cursor(uint16_t x, uint16_t y)
{
    g_stubs_cursorX = x;
    g_stubs_cursorY = y;
}

GHOSTTY_API GhosttyResult ghostty_terminal_grid_ref(
    GhosttyTerminal, GhosttyPoint point, GhosttyGridRef* ref)
{
    if (!ref)
        return GHOSTTY_SUCCESS;
    if (g_stubs_gridArmed) {
        uint16_t x = point.value.coordinate.x;
        uint32_t y = point.value.coordinate.y;
        if (x >= g_stubs_gridCols || y >= g_stubs_gridRows)
            return GHOSTTY_INVALID_VALUE;
        ref->size = sizeof(GhosttyGridRef);
        ref->node = nullptr;
        ref->x = x;
        ref->y = static_cast<uint16_t>(y);
        return GHOSTTY_SUCCESS;
    }
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
    const GhosttyGridRef*, GhosttyCell* out_cell)
{
    // Serve the default (0) cell handle: isWideSpacerCell(0) is false, so
    // the export loop treats every cell as a normal narrow cell.
    if (out_cell)
        *out_cell = 0;
    return GHOSTTY_SUCCESS;
}

GHOSTTY_API GhosttyResult ghostty_cell_get(GhosttyCell, GhosttyCellData, void *out)
{
    if (out) *static_cast<int*>(out) = 0;
    return GHOSTTY_SUCCESS;
}

GHOSTTY_API GhosttyResult ghostty_grid_ref_row(
    const GhosttyGridRef* ref, GhosttyRow* out_row)
{
    if (!ref || !out_row)
        return GHOSTTY_SUCCESS;
    if (g_stubs_gridArmed) {
        // Encode the row index (ref->y) into a nonzero handle so
        // ghostty_row_get can map it back to the fixture's flag.
        *out_row = static_cast<GhosttyRow>(ref->y) + 1;
        return GHOSTTY_SUCCESS;
    }
    return GHOSTTY_SUCCESS;
}

GHOSTTY_API GhosttyResult ghostty_row_get(GhosttyRow row, GhosttyRowData data, void *out)
{
    if (!out)
        return GHOSTTY_SUCCESS;
    if (g_stubs_gridArmed && data == GHOSTTY_ROW_DATA_WRAP_CONTINUATION) {
        size_t idx = static_cast<size_t>(row) - 1;
        bool cont = (idx < g_stubs_gridCont.size()) && g_stubs_gridCont[idx];
        *static_cast<bool*>(out) = cont;
        return GHOSTTY_SUCCESS;
    }
    *static_cast<bool*>(out) = false;
    return GHOSTTY_SUCCESS;
}

GHOSTTY_API GhosttyResult ghostty_grid_ref_graphemes(
    const GhosttyGridRef* ref, uint32_t* buf, size_t buf_len, size_t *out_len)
{
    if (out_len)
        *out_len = 0;
    if (!ref || !buf || buf_len == 0)
        return GHOSTTY_SUCCESS;
    if (g_stubs_gridArmed) {
        size_t y = ref->y;
        size_t x = ref->x;
        if (y < g_stubs_gridText.size() && x < g_stubs_gridText[y].size()) {
            unsigned char c = static_cast<unsigned char>(g_stubs_gridText[y][x]);
            if (c != 0) {
                buf[0] = c;
                if (out_len)
                    *out_len = 1;
            }
        }
    }
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
    if (out) {
        *out = (GhosttyRenderState)1;
        g_stubs_outstandingRenderStates++;
    }
    return GHOSTTY_SUCCESS;
}

GHOSTTY_API void ghostty_render_state_free(GhosttyRenderState)
{
    g_stubs_outstandingRenderStates--;
}

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

GHOSTTY_API GhosttyResult ghostty_render_state_clean(GhosttyRenderState)
{
    return GHOSTTY_SUCCESS;
}

GHOSTTY_API GhosttyResult ghostty_render_state_set(
    GhosttyRenderState, GhosttyRenderStateOption, const void*)
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

GHOSTTY_API bool ghostty_render_state_row_iterator_next_dirty(
    GhosttyRenderStateRowIterator, uint16_t*)
{
    // No render-state row model in the stubs; report no dirty rows,
    // consistent with ghostty_render_state_row_iterator_next().
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
    // Covers the largest struct any caller reads (scrollbar, 24B) with headroom.
    if (out) memset(out, 0, 32);
    return GHOSTTY_SUCCESS;
}

GHOSTTY_API GhosttyResult ghostty_render_state_row_cells_get_multi(
    GhosttyRenderStateRowCells, size_t count,
    const GhosttyRenderStateRowCellsData*, void**, size_t* out_written)
{
    // No render-state row model in the stubs, so there is nothing to write;
    // just report all keys as written. No test reads these outputs (the row
    // iterator reports no cells), so leaving them untouched is safe.
    if (out_written) *out_written = count;
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
