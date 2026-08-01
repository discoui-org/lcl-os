#include <iostream>
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <cstring>
#include <cerrno>
#include <vector>
#include <string>
#include <filesystem>
#include "core/app/app_bundle_parser.hpp"

namespace {

std::string sanitizeNameForLog(const std::string& in) {
    std::string out;
    out.reserve(in.size());
    for (char c : in) {
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || c == '-' || c == '_') {
            out.push_back(c);
        } else {
            out.push_back('_');
        }
    }
    if (out.empty()) out = "app";
    return out;
}

std::string resolveExecutablePath(const lcl::core::AppBundleMetadata& meta) {
    namespace fs = std::filesystem;
    if (fs::exists(meta.executablePath)) return meta.executablePath;

    fs::path execPath(meta.executablePath);
    fs::path fallback = fs::path(meta.bundlePath) / "bin" / execPath.filename();
    if (fs::exists(fallback)) return fallback.string();

    return meta.executablePath;
}

bool endsWith(const std::string& value, const std::string& suffix) {
    if (suffix.size() > value.size()) return false;
    return std::equal(suffix.rbegin(), suffix.rend(), value.rbegin());
}

} // namespace

int main(int argc, char* argv[]) {
    if (argc < 2) {
        std::cout << "Usage: open [-w|--wait] <app_name.app | path_to_app>\n";
        return 1;
    }

    bool waitMode = false;
    std::string target;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "-w" || arg == "--wait") {
            waitMode = true;
        } else if (target.empty()) {
            target = arg;
        }
    }

    if (target.empty()) {
        std::cout << "Usage: open [-w|--wait] <app_name.app | path_to_app>\n";
        return 1;
    }

    if (target.rfind(".app") == std::string::npos && target.find('/') == std::string::npos) {
        target += ".app";
    }

    if (target[0] != '/' && target.find("/home/user/Applications/") == std::string::npos && target.find("/Applications/") == std::string::npos) {
        std::string candidate = "/home/user/Applications/" + target;
        auto metaCand = lcl::core::AppBundleParser::parseBundle(candidate);
        if (metaCand && metaCand->valid) {
            target = candidate;
        } else {
            std::string sysCand = "/Applications/" + target;
            auto metaSys = lcl::core::AppBundleParser::parseBundle(sysCand);
            if (metaSys && metaSys->valid) {
                target = sysCand;
            }
        }
    }

    auto meta = lcl::core::AppBundleParser::parseBundle(target);
    if (!meta || !meta->valid) {
        std::cerr << "[LCL Open ERROR] Invalid .app bundle: " << target << " (missing or invalid metadata.json)\n";
        return 1;
    }

    std::cout << "[LCL Open] Launching " << meta->name << " v" << meta->version
              << (waitMode ? " (attached/blocking mode)" : " (detached/background mode)") << "...\n";

    std::string executablePath = resolveExecutablePath(*meta);
    std::vector<std::string> execArgs;

    if (endsWith(executablePath, ".js")) {
        std::vector<std::string> runtimeCandidates = {
            "/usr/bin/lcl-js",
            "/bin/lcl-js"
        };
        std::string runtimePath;
        for (const auto& cand : runtimeCandidates) {
            if (access(cand.c_str(), X_OK) == 0) {
                runtimePath = cand;
                break;
            }
        }

        if (runtimePath.empty()) {
            std::cerr << "[LCL Open ERROR] JS app requested, but lcl-js runtime not found."
                      << " Tried /usr/bin/lcl-js and /bin/lcl-js\n";
            return 1;
        }
        execArgs = {runtimePath, executablePath};
    } else {
        execArgs = {executablePath};
    }

    int errPipe[2]{-1, -1};
    if (pipe(errPipe) != 0) {
        std::cerr << "[LCL Open ERROR] Failed to create error pipe: "
                  << std::strerror(errno) << "\n";
        return 1;
    }

    // Ensure the write end closes on successful exec.
    int pipeFlags = fcntl(errPipe[1], F_GETFD);
    if (pipeFlags >= 0) {
        fcntl(errPipe[1], F_SETFD, pipeFlags | FD_CLOEXEC);
    }

    std::string detachedLogPath = "/tmp/lcl-open-" + sanitizeNameForLog(meta->name) + ".log";

    pid_t pid = fork();
    if (pid < 0) {
        std::cerr << "[LCL Open ERROR] Failed to fork process.\n";
        close(errPipe[0]);
        close(errPipe[1]);
        return 1;
    }

    if (pid == 0) {
        // Child process
        close(errPipe[0]);
        if (!waitMode) {
            setsid();

            int logFd = open(detachedLogPath.c_str(), O_CREAT | O_WRONLY | O_APPEND, 0644);
            if (logFd >= 0) {
                dup2(logFd, STDOUT_FILENO);
                dup2(logFd, STDERR_FILENO);
                close(logFd);
            }
        }

        std::vector<char*> argvExec;
        argvExec.reserve(execArgs.size() + 1);
        for (auto& arg : execArgs) {
            argvExec.push_back(const_cast<char*>(arg.c_str()));
        }
        argvExec.push_back(nullptr);

        execv(execArgs[0].c_str(), argvExec.data());

        int execErrno = errno;
        (void)write(errPipe[1], &execErrno, sizeof(execErrno));
        close(errPipe[1]);
        _exit(127);
    }

    close(errPipe[1]);
    int execErrno = 0;
    ssize_t readBytes = read(errPipe[0], &execErrno, sizeof(execErrno));
    close(errPipe[0]);

    if (readBytes > 0) {
        if (waitMode) {
            waitpid(pid, nullptr, 0);
        }
        std::cerr << "[LCL Open ERROR] Failed to launch executable: " << execArgs[0]
                  << " (" << std::strerror(execErrno) << ")\n";
        if (execArgs.size() > 1) {
            std::cerr << "[LCL Open ERROR] Script argument: " << execArgs[1] << "\n";
        }
        return 1;
    }

    if (waitMode) {
        int status = 0;
        waitpid(pid, &status, 0);
        if (WIFEXITED(status) && WEXITSTATUS(status) != 0) {
            std::cerr << "[LCL Open ERROR] App exited with code " << WEXITSTATUS(status) << "\n";
            return WEXITSTATUS(status);
        }
        if (WIFSIGNALED(status)) {
            std::cerr << "[LCL Open ERROR] App terminated by signal " << WTERMSIG(status) << "\n";
            return 128 + WTERMSIG(status);
        }
    } else {
        std::cout << "[LCL Open] App launched in background with PID " << pid << ".\n";
        std::cout << "[LCL Open] Detached logs: " << detachedLogPath << "\n";
    }

    return 0;
}
