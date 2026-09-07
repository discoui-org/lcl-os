#include "lcl-ui/core/window_app.hpp"
#include "lcl-ui/widgets/button.hpp"
#include "lcl-ui/widgets/container.hpp"
#include "lcl-ui/widgets/text.hpp"
#include "system/render/raster_canvas.hpp"

#include <fcntl.h>
#include <grp.h>
#include <linux/capability.h>
#include <linux/seccomp.h>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/un.h>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

namespace {

namespace layout = lcl::ui::layout;
using Color = lcl::graphics::Color;

constexpr Color kBackground{242, 242, 247, 255};
constexpr Color kCard{255, 255, 255, 255};
constexpr Color kText{25, 25, 29, 255};
constexpr Color kMuted{105, 107, 115, 255};
constexpr Color kGood{34, 155, 85, 255};
constexpr Color kBad{207, 54, 54, 255};

bool hasZeroCapabilities() {
    __user_cap_header_struct header{};
    header.version = _LINUX_CAPABILITY_VERSION_3;
    std::array<__user_cap_data_struct, 2> data{};
    if (syscall(SYS_capget, &header, data.data()) != 0) return false;
    for (const auto& set : data) {
        if (set.effective != 0 || set.permitted != 0 || set.inheritable != 0) return false;
    }
    return true;
}

bool serviceIsHidden(const char* path) {
    return access(path, F_OK) != 0 && errno == ENOENT;
}

bool directoryIsWritable(const char* path, const char* marker) {
    const std::string target = std::string(path) + "/" + marker;
    const int fd = open(target.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    if (fd < 0) return false;
    constexpr char payload[] = "sandbox-ok\n";
    const bool ok = write(fd, payload, sizeof(payload) - 1) ==
                    static_cast<ssize_t>(sizeof(payload) - 1);
    close(fd);
    return ok;
}

bool systemIsReadOnly() {
    const int fd = open("/System/.sandbox-write-test", O_WRONLY | O_CREAT | O_CLOEXEC, 0600);
    if (fd >= 0) {
        close(fd);
        unlink("/System/.sandbox-write-test");
        return false;
    }
    return errno == EROFS || errno == EACCES;
}

struct Check { std::string label; bool passed; };

std::vector<Check> runChecks() {
    struct rlimit processes {};
    const bool limited = getrlimit(RLIMIT_NPROC, &processes) == 0 &&
                         processes.rlim_cur != RLIM_INFINITY;
    return {
        {"Unique unprivileged app UID", geteuid() >= 61000 && geteuid() <= 61999},
        {"No Linux capabilities", hasZeroCapabilities()},
        {"No new privileges", prctl(PR_GET_NO_NEW_PRIVS, 0, 0, 0, 0) == 1},
        {"Seccomp filter active", prctl(PR_GET_SECCOMP, 0, 0, 0, 0) == SECCOMP_MODE_FILTER},
        {"Private /Data is writable", directoryIsWritable("/Data", "touch-test.txt")},
        {"Private /Temporary is writable", directoryIsWritable("/Temporary", "touch-test.txt")},
        {"System image is read-only", systemIsReadOnly()},
        {"Input devices are hidden", access("/dev/input", F_OK) != 0},
        {"sessiond is hidden", serviceIsHidden("/Runtime/lcl-sessiond.sock")},
        {"securityd is hidden", serviceIsHidden("/Runtime/lcl-securityd.sock")},
        {"sandboxd is hidden", serviceIsHidden("/Runtime/lcl-sandboxd.sock")},
        {"Process limit is installed", limited},
    };
}

class SandboxTestApp final {
public:
    explicit SandboxTestApp(lcl::ui::WindowApp& window) : window_(window) { rebuild(); }

    void rebuild() {
        auto root = std::make_unique<lcl::ui::Container>();
        root_ = root.get();
        root->setDirection(layout::Direction::Column);
        root->setPadding(22.0f);
        root->setGap(14.0f);
        root->setBackgroundColor(kBackground);

        auto title = std::make_unique<lcl::ui::Text>("Sandbox Test");
        title->setFontSize(26.0f);
        title->setTextColor(kText);
        root->addChild(std::move(title));

        auto subtitle = std::make_unique<lcl::ui::Text>(
            "This ordinary app checks its own enforced isolation. No keyboard needed.");
        subtitle->setFontSize(14.0f);
        subtitle->setTextColor(kMuted);
        root->addChild(std::move(subtitle));

        auto button = std::make_unique<lcl::ui::Button>("Run isolation tests");
        button->setHeight(46.0f);
        button->setOnClick([this] { showResults(); });
        root->addChild(std::move(button));

        auto card = std::make_unique<lcl::ui::Container>();
        results_ = card.get();
        card->setDirection(layout::Direction::Column);
        card->setPadding(16.0f);
        card->setGap(7.0f);
        card->setBackgroundColor(kCard);
        card->setBorderRadius(16.0f);
        auto ready = std::make_unique<lcl::ui::Text>("Ready — tap the button above.");
        ready->setFontSize(15.0f);
        ready->setTextColor(kMuted);
        card->addChild(std::move(ready));
        root->addChild(std::move(card));
        window_.setRootWidget(std::move(root));
    }

    void applySafeArea(const lcl::ui::LayoutEnvironment& environment) {
        if (!root_) return;
        root_->setPadding(layout::Edge::Top, environment.safeArea.top + 22.0f);
        root_->setPadding(layout::Edge::Bottom, environment.safeArea.bottom + 22.0f);
        root_->setPadding(layout::Edge::Left, environment.safeArea.left + 22.0f);
        root_->setPadding(layout::Edge::Right, environment.safeArea.right + 22.0f);
    }

private:
    void showResults() {
        const auto checks = runChecks();
        bool allPassed = true;
        while (!results_->getChildren().empty()) {
            results_->removeChild(results_->getChildren().back().get());
        }
        for (const auto& check : checks) {
            allPassed = allPassed && check.passed;
            auto line = std::make_unique<lcl::ui::Text>(
                std::string(check.passed ? "PASS  " : "FAIL  ") + check.label);
            line->setFontSize(14.0f);
            line->setTextColor(check.passed ? kGood : kBad);
            results_->addChild(std::move(line));
        }
        auto summary = std::make_unique<lcl::ui::Text>(
            allPassed ? "All checks passed" : "One or more checks failed");
        summary->setFontSize(17.0f);
        summary->setTextColor(allPassed ? kGood : kBad);
        results_->addChild(std::move(summary));
    }

    lcl::ui::WindowApp& window_;
    lcl::ui::Container* root_{nullptr};
    lcl::ui::Container* results_{nullptr};
};

} // namespace

int main() {
    lcl::ui::WindowApp window(lcl::render::makeDisplayListCanvas(),
                              720.0f, 650.0f, "Sandbox Test");
    window.setAppId("org.lcl.sandbox-test");
    window.setInitialBounds(120.0f, 70.0f, 720.0f, 650.0f);
    window.setDecorationMode(lcl::protocol::LCLDecorationMode::SSD);
    window.setWindowCornerStyle(18.0f, 2.0f);
    SandboxTestApp app(window);
    window.setOnLayoutEnvironmentChanged(
        [&app](const lcl::ui::LayoutEnvironment& environment) { app.applySafeArea(environment); });
    if (!window.connectCompositor()) return 1;
    window.runEventLoop();
    return 0;
}
