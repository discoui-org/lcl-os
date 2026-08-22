#pragma once

#include <chrono>

#include "apps/terminal/terminal_app.hpp"
#include "lcl-ui/widgets/widget.hpp"

namespace lcl::apps {

/**
 * Terminal presentation widget.  The PTY/model stays in TerminalApp while
 * WindowApp owns IPC, shared buffers, resize and frame scheduling.
 */
class TerminalView final : public lcl::ui::Widget {
public:
    explicit TerminalView(TerminalApp& terminal);

    /** Returns true only when the visible cursor state changed. */
    bool updateCursorBlink();
    void draw(lcl::graphics::Canvas& canvas, const lcl::graphics::RectF& damageRect) override;

private:
    bool isCursorVisible() const;
    std::string clippedLine(const std::string& line, float availableWidth,
                            float cellWidth) const;

    TerminalApp& m_terminal;
    bool m_cursorStateInitialized{false};
    bool m_cursorVisible{true};
};

} // namespace lcl::apps
