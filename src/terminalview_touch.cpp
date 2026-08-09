#include "terminalview.h"
#include "settings.h"
#include "keymapping.h"

#include <QLineF>
#include <QDateTime>
#include <cmath>

namespace {
// Qt wheel delta is in 1/8° units; 120 units = 15° = 3 lines -> 40 units/line.
constexpr qreal kWheelUnitsPerLine = 40.0;
}   // namespace

void TerminalView::resetSessionSwipe()
{
    if (!m_sessionSwiping)
        return;
    m_sessionSwiping = false;
    setKeepMouseGrab(false);
    setKeepTouchGrab(false);
    Q_EMIT sessionSwipeCancelled();
}

void TerminalView::resetTouchInteractionState()
{
    if (m_longPressTimerId) {
        killTimer(m_longPressTimerId);
        m_longPressTimerId = 0;
    }
    if (m_selecting)
        clearSelection();
    m_pendingLinkTap = false;
    m_draggingHandle = 0;
}

void TerminalView::mousePressEvent(QMouseEvent *event)
{
    // A new press always supersedes a stale swipe (e.g. after a TouchCancel
    // that never delivered a release).
    resetSessionSwipe();

    if (m_shellExited) {
        restartShell();
        event->accept();
        return;
    }

    if (event->button() == Qt::LeftButton) {
        m_cursorBlinkVisible = true;
        m_lastInputTime.start();

        m_mouseTrackingActive = m_vt->isMouseTracking();

        if (m_mouseTrackingActive) {
            // Safety net: reject touches in the pull-down zone. In practice,
            // touchEvent accepts TUI touches before synthesis, so this is
            // rarely reached.
            if (event->pos().y() < m_pullDownZoneHeight) {
                QQuickItem::mousePressEvent(event);
                return;
            }

            sendMouseEvent(GHOSTTY_MOUSE_ACTION_PRESS, GHOSTTY_MOUSE_BUTTON_LEFT,
                           event->pos(), KeyMapping::mapQtModifiers(event->modifiers()));

            // Enables motion-event delivery on subsequent drag
            m_mouseButtonPressed = true;
            m_vt->setMouseButtonPressed(true);

            // Prevent SilicaFlickable from stealing drag gestures
            setKeepMouseGrab(true);
            event->accept();
            return;
        }

        // Order matters: handle-tap must precede tap-count reset and selection clearing
        int handle = handleHitTest(event->pos());
        if (handle != 0) {
            m_draggingHandle = handle;
            m_handlesVisible = false;
            m_magnifierVisible = true;
            m_tapCount = 0; // Prevent phantom triple-tap after handle drag
            setKeepMouseGrab(true);
            event->accept();
            return;
        }

        // Defer link open to release so a drag abandons it cleanly
        {
            QPointF cell = cellFromPixel(event->pos());
            if (cell.x() >= 0 && cell.y() >= 0) {
                QString uri = findLinkAt(static_cast<int>(cell.x()),
                                         static_cast<int>(cell.y()));
                if (!uri.isEmpty()) {
                    m_pendingLinkTap = true;
                    m_tappedLinkUri = uri;
                    m_linkTapStartPos = event->pos();
                    setKeepMouseGrab(true);
                    event->accept();
                    return;
                }
            }
        }

        qint64 now = QDateTime::currentMSecsSinceEpoch();
        qreal dist = QLineF(event->pos(), m_lastTapPos).length();
        bool withinWindow = (m_tapCount > 0)
            && (now - m_lastTapTime) <= TapTimeoutMs
            && dist <= TapDistancePx;

        if (withinWindow) {
            m_tapCount = qMin(m_tapCount + 1, 3);
        } else {
            m_tapCount = 1;
        }
        m_lastTapTime = now;
        m_lastTapPos = event->pos();

        if (m_tapCount == 2) {
            clearSelection();
            selectWordAt(event->pos());
            event->accept();
            return;
        }
        if (m_tapCount == 3) {
            clearSelection();
            selectLineAt(event->pos());
            event->accept();
            return;
        }

        clearSelection();
        m_selStart = event->pos();
        m_selEnd = event->pos();
        m_longPressTimerId = startTimer(LongPressTimeout);
        event->accept();
        return;
    }
    QQuickItem::mousePressEvent(event);
}

void TerminalView::mouseMoveEvent(QMouseEvent *event)
{
    if (m_draggingHandle != 0) {
        if (m_draggingHandle == 1)
            m_selStart = event->pos();
        else
            m_selEnd = event->pos();

        // Magnifier stays visible during handle drags — no velocity-based hiding.
        // It was set visible on mousePress and should remain so until release.

        m_lastInputTime.start();

        update();
        event->accept();
        return;
    }

    if (m_selecting) {
        // Only track movement for long-press drags (handles not yet visible).
        // Word/line selections have finalized endpoints — use handles to adjust.
        if (!m_handlesVisible) {
            m_selEnd = event->pos();
        }

        // Magnifier stays visible for the whole drag — no velocity-based hiding.
        // Visibility is bracketed by timerEvent (show on long-press fire) and
        // mouseReleaseEvent (hide on release). Velocity gating caused flicker
        // (hysteresis band sat inside typical drag velocity) and stuck-invisible
        // when the finger stopped mid-drag (no move events to revive it).

        // Keep cursor blink paused during active selection to prevent
        // full redraws that cause magnifier flicker
        m_lastInputTime.start();

        update();
        event->accept();
        return;
    }

    if (m_mouseTrackingActive) {
        GhosttyMouseButton btn = m_mouseButtonPressed
            ? GHOSTTY_MOUSE_BUTTON_LEFT : GHOSTTY_MOUSE_BUTTON_UNKNOWN;
        sendMouseEvent(GHOSTTY_MOUSE_ACTION_MOTION, btn,
                       event->pos(), KeyMapping::mapQtModifiers(event->modifiers()));
        event->accept();
        return;
    }

    // Session-swipe classifier — runs only after every grabbing branch above
    // has returned (NORMAL single-finger, pre-selection window).
    if (!m_pendingLinkTap && !m_multiTouchActive && m_sessionSwipeEnabled
        && m_gestureMode == GestureMode::Undecided && !m_sessionSwiping) {
        QPointF delta = event->pos() - m_lastTapPos;   // m_lastTapPos set in press
        if (qAbs(delta.x()) > SwipeMinHorizontalPx
            && qAbs(delta.x()) > qAbs(delta.y()) * SwipeDominanceRatio) {
            if (m_longPressTimerId) { killTimer(m_longPressTimerId); m_longPressTimerId = 0; }
            m_sessionSwiping = true;
            m_swipeStartX = event->pos().x();
            // Mirror the multitouch pattern — lock BOTH mouse and touch grab so
            // terminalFlickable can't steal the sequence mid-swipe (on Qt 5.6 /
            // Sailfish, mouse grab alone does NOT stop touch stealing).
            setKeepMouseGrab(true);
            setKeepTouchGrab(true);
            Q_EMIT sessionSwipeStarted();
        }
    }
    if (m_sessionSwiping) {
        Q_EMIT sessionSwipeProgress(event->pos().x() - m_swipeStartX);
        event->accept();
        return;
    }

    QQuickItem::mouseMoveEvent(event);
}

void TerminalView::mouseReleaseEvent(QMouseEvent *event)
{
    if (m_longPressTimerId) {
        killTimer(m_longPressTimerId);
        m_longPressTimerId = 0;
    }

    if (m_sessionSwiping) {
        m_sessionSwiping = false;
        setKeepMouseGrab(false);
        setKeepTouchGrab(false);
        qreal dx = event->pos().x() - m_swipeStartX;
        if (qAbs(dx) > width() * SwipeCommitFraction) {
            Q_EMIT sessionSwipeCommitted(dx < 0 ? 1 : -1);   // leftward -> next session
        } else {
            Q_EMIT sessionSwipeCancelled();
        }
        event->accept();
        return;
    }

    if (m_mouseTrackingActive) {
        // Re-check live tracking state — the app may have disabled mouse
        // tracking between press and release (e.g. htop exiting to shell).
        if (m_vt->isMouseTracking()) {
            sendMouseEvent(GHOSTTY_MOUSE_ACTION_RELEASE, GHOSTTY_MOUSE_BUTTON_LEFT,
                           event->pos(), KeyMapping::mapQtModifiers(event->modifiers()));
        }

        m_mouseButtonPressed = false;
        m_vt->setMouseButtonPressed(false);
        m_mouseTrackingActive = false;
        setKeepMouseGrab(false);

        event->accept();
        return;
    }

    // Only on clean tap — significant drag abandons the link
    if (m_pendingLinkTap) {
        qreal dragDist = QLineF(m_linkTapStartPos, event->pos()).length();
        m_pendingLinkTap = false;
        setKeepMouseGrab(false);
        QString uri = m_tappedLinkUri;
        m_tappedLinkUri.clear();
        if (dragDist < TapDistancePx && !uri.isEmpty()) {
            Q_EMIT linkActivated(uri);
            event->accept();
            return;
        }
        // Fall through to normal release handling if finger moved
    }

    if (m_draggingHandle != 0) {
        if (m_draggingHandle == 1)
            m_selStart = event->pos();
        else
            m_selEnd = event->pos();
        m_draggingHandle = 0;
        m_magnifierVisible = false;
        m_handlesVisible = true;
        setKeepMouseGrab(false);
        copySelection();
        update();
        event->accept();
        return;
    }

    if (m_selecting) {
        // Only update endpoint for long-press drags (handles not yet visible).
        // Word/line selections already finalized their endpoints.
        if (!m_handlesVisible) {
            m_selEnd = event->pos();
            // Cancel if finger didn't move enough — long-press without drag
            // would create a phantom single-character selection
            if (QLineF(m_selStart, m_selEnd).length() < TapDistancePx) {
                clearSelection();
                event->accept();
                return;
            }
            copySelection();
        }
        m_magnifierVisible = false;
        m_handlesVisible = true;
        update();
        event->accept();
        return;
    }
    // Release mouse grab acquired by touchEvent multi-touch/TUI path.
    setKeepMouseGrab(false);
    QQuickItem::mouseReleaseEvent(event);
}

void TerminalView::wheelEvent(QWheelEvent *event)
{
    if (!m_vt || !m_vt->terminal()) {
        QQuickItem::wheelEvent(event);
        return;
    }

    // When mouse tracking is active, forward scroll as mouse buttons 4/5
    if (m_vt->isMouseTracking()) {
        int delta = event->angleDelta().y();
        GhosttyMods mods = KeyMapping::mapQtModifiers(event->modifiers());
        GhosttyMouseButton button = (delta > 0) ? GHOSTTY_MOUSE_BUTTON_FOUR
                                                 : GHOSTTY_MOUSE_BUTTON_FIVE;
        sendMouseEvent(GHOSTTY_MOUSE_ACTION_PRESS, button, event->pos(), mods);
        sendMouseEvent(GHOSTTY_MOUSE_ACTION_RELEASE, button, event->pos(), mods);
        event->accept();
        return;
    }

    int delta = event->angleDelta().y(); // positive = up, negative = down

    // Accumulate fractional scroll lines so sub-line deltas aren't lost
    qreal newDelta = -static_cast<qreal>(delta) / kWheelUnitsPerLine;
    auto scrollResult = TextUtil::accumulateScroll(m_scrollAccumulator, newDelta);
    m_scrollAccumulator = scrollResult.accumulator;
    int lines = scrollResult.lines;

    if (lines != 0) {
        // Ghostty scroll: negative delta = scroll up (toward scrollback)
        GhosttyTerminalScrollViewport scroll = {};
        scroll.tag = GHOSTTY_SCROLL_VIEWPORT_DELTA;
        scroll.value.delta = -lines;
        ghostty_terminal_scroll_viewport(m_vt->terminal(), scroll);
        m_linkScanDirty = true;
        update();
    }

    event->accept();
}

void TerminalView::touchEvent(QTouchEvent *event)
{
    if (!m_vt || !m_vt->terminal()) {
        QQuickItem::touchEvent(event);
        return;
    }

    const auto points = event->touchPoints();

    if (points.size() >= 2) {
        setKeepMouseGrab(true);
        // Qt 5.6: the touch grab is a SEPARATE mechanism from the mouse grab.
        // SilicaFlickable (a filtering parent) steals the touch grab via its
        // childMouseEventFilter — setKeepMouseGrab alone does NOT stop this.
        // setKeepTouchGrab(true) denies the steal so two-finger scroll/pinch
        // stays with the terminal instead of triggering the PullDownMenu.
        setKeepTouchGrab(true);

        // ACTIVE grab — passive flags above only prevent future steals.
        // SilicaFlickable ignores them once its drag recogniser has armed.
        // grabTouchPoints()/grabMouse() wrest the grab back immediately.
        {
            QVector<int> ids;
            ids.reserve(points.size());
            for (const auto &p : points)
                ids.append(p.id());
            grabTouchPoints(ids);
            grabMouse();
        }

        switch (event->type()) {
        case QEvent::TouchEnd:
        case QEvent::TouchCancel:
            handleMultiTouchEnd();
            break;
        default:
            // Start the gesture on the FIRST ≥2-point event of any type.
            // When the second finger lands after the first, Qt delivers a
            // TouchUpdate (not TouchBegin), so keying off the event type alone
            // would skip handleMultiTouchBegin — and the Flickable would never
            // be disabled, re-opening the PullDownMenu bug for staggered taps.
            if (!m_multiTouchActive)
                handleMultiTouchBegin(points);
            else
                handleMultiTouchUpdate(points);
            break;
        }
        event->accept();
        return;
    }

    // Check m_multiTouchActive too: a brief two-finger tap may never
    // leave Undecided, but must still end (or the Flickable stays disabled).
    if (m_multiTouchActive || m_gestureMode != GestureMode::Undecided) {
        handleMultiTouchEnd();
    }

    // TUI mode (mouse tracking): accept + grab + forward as synthetic
    // mouse/wheel events.  Normal mode: fall through to QQuickItem —
    // accepting would break the Flickable's press-delay disambiguation
    // (instant drag -> pull-down, press-hold -> selection).

    if (m_mouseTrackingActive) {
        if (event->type() == QEvent::TouchBegin && points.size() == 1) {
            handleTuiTouchBegin(event, points.first());
            return;
        }
        if (event->type() == QEvent::TouchUpdate && points.size() == 1) {
            handleTuiTouchUpdate(event, points.first());
            return;
        }
        if (event->type() == QEvent::TouchEnd
            || event->type() == QEvent::TouchCancel) {
            handleTuiTouchEnd(event, points);
            return;
        }
    }

    // Normal mode: native fall-through — let Qt synthesise mouse events and
    // the Flickable handle pull-down disambiguation.
    if (event->type() == QEvent::TouchCancel) {
        resetSessionSwipe();   // no release follows a cancel; keep the flag honest
        resetTouchInteractionState();
    }
    QQuickItem::touchEvent(event);
}

void TerminalView::handleTuiTouchBegin(QTouchEvent *event,
                                       const QTouchEvent::TouchPoint &pt)
{
    resetSessionSwipe();   // TUI path grabs everything; keep the swipe flag honest
    event->accept();
    setKeepMouseGrab(true);
    setKeepTouchGrab(true);
    Q_EMIT requestParentInteractive(false);
    grabTouchPoints(QVector<int>{ pt.id() });
    grabMouse();
    m_tuiDragLastY = pt.pos().y();
    m_tuiScrollAccumulator = 0;
    QMouseEvent synthPress(QEvent::MouseButtonPress,
                           pt.pos(), pt.screenPos(),
                           Qt::LeftButton, Qt::LeftButton,
                           event->modifiers());
    mousePressEvent(&synthPress);
}

void TerminalView::handleTuiTouchUpdate(QTouchEvent *event,
                                        const QTouchEvent::TouchPoint &pt)
{
    event->accept();

    qreal deltaY = pt.pos().y() - m_tuiDragLastY;
    m_tuiDragLastY = pt.pos().y();
    qreal newDelta = deltaY / m_cellHeight; // positive: down-drag = scroll up (natural scrolling)
    auto scrollResult = TextUtil::accumulateScroll(
        m_tuiScrollAccumulator, newDelta);
    m_tuiScrollAccumulator = scrollResult.accumulator;
    if (scrollResult.lines != 0) {
        GhosttyMods mods = KeyMapping::mapQtModifiers(event->modifiers());
        GhosttyMouseButton btn = (scrollResult.lines > 0)
            ? GHOSTTY_MOUSE_BUTTON_FOUR : GHOSTTY_MOUSE_BUTTON_FIVE;
        for (int i = 0; i < qAbs(scrollResult.lines); ++i) {
            sendMouseEvent(GHOSTTY_MOUSE_ACTION_PRESS, btn, pt.pos(), mods);
            sendMouseEvent(GHOSTTY_MOUSE_ACTION_RELEASE, btn, pt.pos(), mods);
        }
    }

    // Also forward mouse motion for TUI click/drag/selection.
    QMouseEvent synthMove(QEvent::MouseMove,
                          pt.pos(), pt.screenPos(),
                          Qt::LeftButton, Qt::LeftButton,
                          event->modifiers());
    mouseMoveEvent(&synthMove);
}

void TerminalView::handleTuiTouchEnd(QTouchEvent *event,
                                     const QList<QTouchEvent::TouchPoint> &points)
{
    if (points.size() == 1) {
        const auto &pt = points.first();
        QMouseEvent synthRel(QEvent::MouseButtonRelease,
                             pt.pos(), pt.screenPos(),
                             Qt::LeftButton, Qt::NoButton,
                             event->modifiers());
        mouseReleaseEvent(&synthRel);
    }
    Q_EMIT requestParentInteractive(true);
    m_tuiScrollAccumulator = 0;
    setKeepMouseGrab(false);
    setKeepTouchGrab(false);
    event->accept();
}

void TerminalView::handleMultiTouchBegin(const QList<QTouchEvent::TouchPoint> &points)
{
    resetSessionSwipe();   // a second finger lands -> abandon any in-progress swipe

    // Shell exited — ignore multi-touch, let it fall through to parent
    if (m_shellExited)
        return;

    // Clean up any previous gesture that wasn't properly ended. This can
    // happen if TouchEnd was missed (e.g., window deactivated, touch stolen
    // by another item) and a new gesture starts while the overlay is still
    // visible. Without this, pinchingChanged(false) is never emitted.
    if (m_multiTouchActive || m_gestureMode != GestureMode::Undecided) {
        handleMultiTouchEnd();
    }

    // Disable the parent Flickable immediately — passive grabs aren't
    // enough; SilicaFlickable steals the gesture before scroll-commit.
    Q_EMIT requestParentInteractive(false);
    m_multiTouchActive = true;

    if (m_longPressTimerId) {
        killTimer(m_longPressTimerId);
        m_longPressTimerId = 0;
    }
    if (m_draggingHandle != 0) {
        m_draggingHandle = 0;
        m_magnifierVisible = false;
        m_handlesVisible = true;
        setKeepMouseGrab(false);
    }
    if (m_selecting)
        clearSelection();

    qreal avgY = (points[0].pos().y() + points[1].pos().y()) / 2.0;
    m_twoFingerLastY = avgY;

    // TUI apps or pinch-to-zoom disabled: force scroll mode
    if (m_vt->isMouseTracking() || !Settings::instance()->pinchToZoom()) {
        m_gestureMode = GestureMode::Scrolling;
        return;
    }

    // Undecided — defer classification to Update
    QPointF p0 = points[0].pos();
    QPointF p1 = points[1].pos();
    m_pinchInitialDistance = QLineF(p0, p1).length();
    m_gestureInitialCentroid = (p0 + p1) / 2.0;
    m_gestureMode = GestureMode::Undecided;
    m_pinchCandidateFrames = 0;
}

void TerminalView::handleMultiTouchUpdate(const QList<QTouchEvent::TouchPoint> &points)
{
    // If any touch point was released, end the gesture immediately. Qt may
    // deliver a TouchUpdate with a released point before TouchEnd arrives.
    // Without this, the gesture continues processing a stale finger position
    // and the overlay may not hide if the final TouchEnd is also missed.
    for (const auto &p : points) {
        if (p.state() & Qt::TouchPointReleased) {
            handleMultiTouchEnd();
            return;
        }
    }

    QPointF p0 = points[0].pos();
    QPointF p1 = points[1].pos();
    qreal currentDistance = QLineF(p0, p1).length();
    QPointF currentCentroid = (p0 + p1) / 2.0;

    const qreal pinchScale = (m_pinchInitialDistance > 0)
        ? currentDistance / m_pinchInitialDistance : 1.0;

    switch (m_gestureMode) {
    case GestureMode::Undecided: {
        bool ratioExceeded = (pinchScale > PinchRatioThreshold)
                          || (pinchScale < 1.0 / PinchRatioThreshold);

        if (ratioExceeded) {
            m_pinchCandidateFrames++;
            if (m_pinchCandidateFrames >= PinchRatioFrames) {
                m_gestureMode = GestureMode::Pinching;
                m_pinchBaseFontSize = m_fontSize;
                m_lastAppliedFontSize = m_fontSize;
                // Reset baseline so scale starts at 1.0 at commitment
                m_pinchInitialDistance = currentDistance;
                m_pinchAtDefault = false;
                Q_EMIT pinchingChanged(true);
                return;
            }
        } else {
            m_pinchCandidateFrames = 0;
        }

        QPointF centroidDelta = currentCentroid - m_gestureInitialCentroid;
        if (qAbs(centroidDelta.y()) > ScrollMinDistancePx
            && qAbs(centroidDelta.y()) > qAbs(centroidDelta.x())) {
            m_gestureMode = GestureMode::Scrolling;
            // Reset lastY to current centroid so first scroll delta is clean
            m_twoFingerLastY = currentCentroid.y();
            return;
        }

        return;
    }

    case GestureMode::Pinching: {
        // Power-curve dampening: requires more finger travel for the same font
        // delta. Exponent < 1 softens the response around scale=1.0 so small
        // finger movements no longer produce large font jumps.
        qreal dampedScale = std::pow(pinchScale, PinchScaleExponent);
        int rawTarget = qRound(m_pinchBaseFontSize * dampedScale);

        if (rawTarget <= Settings::kMinFontSize) {
            // Snap to global default — QML shows "Default", pinch-end stores 0
            int defaultSize = Settings::instance()->fontSize();
            if (defaultSize != m_lastAppliedFontSize) {
                setFontSize(defaultSize);
                m_lastAppliedFontSize = defaultSize;
            }
            if (!m_pinchAtDefault) {
                m_pinchAtDefault = true;
                Q_EMIT pinchAtDefaultChanged(true);
            }
        } else {
            int targetSize = qBound(Settings::kMinFontSize, rawTarget, Settings::kMaxFontSize);
            if (targetSize != m_lastAppliedFontSize) {
                setFontSize(targetSize);
                m_lastAppliedFontSize = targetSize;
            }
            if (m_pinchAtDefault) {
                m_pinchAtDefault = false;
                Q_EMIT pinchAtDefaultChanged(false);
            }
        }
        return;
    }

    case GestureMode::Scrolling: {
        qreal avgY = (p0.y() + p1.y()) / 2.0;
        qreal deltaY = avgY - m_twoFingerLastY;
        m_twoFingerLastY = avgY;

        qreal newDelta = -deltaY / m_cellHeight;
        auto touchScrollResult = TextUtil::accumulateScroll(m_touchScrollAccumulator, newDelta);
        m_touchScrollAccumulator = touchScrollResult.accumulator;
        int lines = touchScrollResult.lines;

        if (lines != 0) {
            GhosttyTerminalScrollViewport scroll = {};
            scroll.tag = GHOSTTY_SCROLL_VIEWPORT_DELTA;
            scroll.value.delta = lines;
            ghostty_terminal_scroll_viewport(m_vt->terminal(), scroll);
            m_linkScanDirty = true;
            update();
        }
        return;
    }
    }
}

void TerminalView::handleMultiTouchEnd()
{
    if (m_gestureMode == GestureMode::Pinching) {
        Q_EMIT pinchingChanged(false);
        if (m_pinchAtDefault) {
            m_pinchAtDefault = false;
            Q_EMIT pinchAtDefaultChanged(false);
        }
    }

    // Restore the parent SilicaFlickable so single-finger pull-down works again.
    Q_EMIT requestParentInteractive(true);

    m_gestureMode = GestureMode::Undecided;
    m_pinchCandidateFrames = 0;
    m_multiTouchActive = false;
    m_twoFingerLastY = 0;
    m_touchScrollAccumulator = 0;
    setKeepMouseGrab(false);
    setKeepTouchGrab(false);
}

void TerminalView::timerEvent(QTimerEvent *event)
{
    if (event->timerId() == m_longPressTimerId) {
        killTimer(m_longPressTimerId);
        m_longPressTimerId = 0;
        m_selecting = true;
        m_magnifierVisible = true;
        // Prevent parent SilicaFlickable from stealing the drag
        setKeepMouseGrab(true);
        update();
        return;
    }
    if (event->timerId() == m_blinkTimerId) {
        if (m_lastInputTime.isValid() &&
            m_lastInputTime.elapsed() < BlinkPauseMs) {
            m_cursorBlinkVisible = true;
            update();
            return;
        }

        // Blink cursor by default. Only stop when terminal explicitly requests
        // a steady cursor (DECSCUSR mode 2, 4, or 6).
        GhosttyRenderState state = m_vt ? m_vt->renderState() : nullptr;
        bool cursorBlinking = true;
        if (state) {
            ghostty_render_state_get(state,
                                     GHOSTTY_RENDER_STATE_DATA_CURSOR_BLINKING,
                                     &cursorBlinking);
        }
        if (cursorBlinking) {
            m_cursorBlinkVisible = !m_cursorBlinkVisible;
            update();
        }
        return;
    }
    QQuickItem::timerEvent(event);
}
