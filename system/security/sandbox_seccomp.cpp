#include "system/security/sandbox_seccomp.hpp"

#include <linux/audit.h>
#include <linux/filter.h>
#include <linux/seccomp.h>
#include <sched.h>
#include <sys/prctl.h>
#include <sys/syscall.h>

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace lcl::security {
namespace {

constexpr std::uint32_t kDeniedSyscallResult = SECCOMP_RET_ERRNO | (EPERM & SECCOMP_RET_DATA);

#if defined(__x86_64__)
constexpr std::uint32_t kExpectedAuditArchitecture = AUDIT_ARCH_X86_64;
#elif defined(__aarch64__)
constexpr std::uint32_t kExpectedAuditArchitecture = AUDIT_ARCH_AARCH64;
#else
#error "LCL sandbox seccomp requires an explicit Linux audit architecture"
#endif

void denySyscall(std::vector<sock_filter>& filter, int syscallNumber) {
    filter.push_back(BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K,
                              static_cast<std::uint32_t>(syscallNumber), 0, 1));
    filter.push_back(BPF_STMT(BPF_RET | BPF_K, kDeniedSyscallResult));
}

void denySyscallWithResult(std::vector<sock_filter>& filter, int syscallNumber,
                           std::uint32_t result) {
    filter.push_back(BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K,
                              static_cast<std::uint32_t>(syscallNumber), 0, 1));
    filter.push_back(BPF_STMT(BPF_RET | BPF_K, result));
}

void appendEscalationDenials(std::vector<sock_filter>& filter) {
#ifdef __NR_bpf
    denySyscall(filter, __NR_bpf);
#endif
#ifdef __NR_ptrace
    denySyscall(filter, __NR_ptrace);
#endif
#ifdef __NR_process_vm_readv
    denySyscall(filter, __NR_process_vm_readv);
#endif
#ifdef __NR_process_vm_writev
    denySyscall(filter, __NR_process_vm_writev);
#endif
#ifdef __NR_mount
    denySyscall(filter, __NR_mount);
#endif
#ifdef __NR_umount2
    denySyscall(filter, __NR_umount2);
#endif
#ifdef __NR_pivot_root
    denySyscall(filter, __NR_pivot_root);
#endif
#ifdef __NR_setns
    denySyscall(filter, __NR_setns);
#endif
#ifdef __NR_unshare
    denySyscall(filter, __NR_unshare);
#endif
#ifdef __NR_kexec_load
    denySyscall(filter, __NR_kexec_load);
#endif
#ifdef __NR_kexec_file_load
    denySyscall(filter, __NR_kexec_file_load);
#endif
#ifdef __NR_init_module
    denySyscall(filter, __NR_init_module);
#endif
#ifdef __NR_finit_module
    denySyscall(filter, __NR_finit_module);
#endif
#ifdef __NR_delete_module
    denySyscall(filter, __NR_delete_module);
#endif
#ifdef __NR_reboot
    denySyscall(filter, __NR_reboot);
#endif
#ifdef __NR_swapon
    denySyscall(filter, __NR_swapon);
#endif
#ifdef __NR_swapoff
    denySyscall(filter, __NR_swapoff);
#endif
#ifdef __NR_open_by_handle_at
    denySyscall(filter, __NR_open_by_handle_at);
#endif
#ifdef __NR_name_to_handle_at
    denySyscall(filter, __NR_name_to_handle_at);
#endif
#ifdef __NR_iopl
    denySyscall(filter, __NR_iopl);
#endif
#ifdef __NR_ioperm
    denySyscall(filter, __NR_ioperm);
#endif
#ifdef __NR_syslog
    denySyscall(filter, __NR_syslog);
#endif
#ifdef __NR_perf_event_open
    denySyscall(filter, __NR_perf_event_open);
#endif
#ifdef __NR_userfaultfd
    denySyscall(filter, __NR_userfaultfd);
#endif
#ifdef __NR_keyctl
    denySyscall(filter, __NR_keyctl);
#endif
#ifdef __NR_add_key
    denySyscall(filter, __NR_add_key);
#endif
#ifdef __NR_request_key
    denySyscall(filter, __NR_request_key);
#endif
#ifdef __NR_capset
    denySyscall(filter, __NR_capset);
#endif
#ifdef __NR_setuid
    denySyscall(filter, __NR_setuid);
#endif
#ifdef __NR_setgid
    denySyscall(filter, __NR_setgid);
#endif
#ifdef __NR_setreuid
    denySyscall(filter, __NR_setreuid);
#endif
#ifdef __NR_setregid
    denySyscall(filter, __NR_setregid);
#endif
#ifdef __NR_setresuid
    denySyscall(filter, __NR_setresuid);
#endif
#ifdef __NR_setresgid
    denySyscall(filter, __NR_setresgid);
#endif
}

void appendNamespaceCloneDenials(std::vector<sock_filter>& filter) {
#ifdef __NR_clone
    unsigned int namespaceFlags = CLONE_NEWNS | CLONE_NEWUTS | CLONE_NEWIPC | CLONE_NEWUSER |
                                  CLONE_NEWPID | CLONE_NEWNET;
#ifdef CLONE_NEWCGROUP
    namespaceFlags |= CLONE_NEWCGROUP;
#endif
    // If this is not clone(), skip the argument inspection and reload the
    // syscall number consumed by the following deny rules.  A clone that asks
    // for any namespace is rejected; ordinary thread/process creation keeps
    // working.
    filter.push_back(BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K,
                              static_cast<std::uint32_t>(__NR_clone), 0, 4));
    filter.push_back(BPF_STMT(BPF_LD | BPF_W | BPF_ABS,
                              static_cast<std::uint32_t>(offsetof(seccomp_data, args[0]))));
    filter.push_back(BPF_STMT(BPF_ALU | BPF_AND | BPF_K, namespaceFlags));
    filter.push_back(BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, 0, 1, 0));
    filter.push_back(BPF_STMT(BPF_RET | BPF_K, kDeniedSyscallResult));
    filter.push_back(BPF_STMT(BPF_LD | BPF_W | BPF_ABS,
                              static_cast<std::uint32_t>(offsetof(seccomp_data, nr))));
#endif
#ifdef __NR_clone3
    // libc implementations can fall back to clone() when clone3 reports
    // ENOSYS.  Returning that value preserves ordinary threading while never
    // accepting an opaque clone3 flag structure that could request a namespace.
    denySyscallWithResult(filter, __NR_clone3, SECCOMP_RET_ERRNO | (ENOSYS & SECCOMP_RET_DATA));
#endif
}

std::vector<sock_filter> buildBaselineFilter(SandboxRuntime runtime) {
    std::vector<sock_filter> filter;
    filter.reserve(72);
    filter.push_back(BPF_STMT(BPF_LD | BPF_W | BPF_ABS,
                              static_cast<std::uint32_t>(offsetof(seccomp_data, arch))));
    filter.push_back(BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, kExpectedAuditArchitecture, 1, 0));
    filter.push_back(BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_KILL_PROCESS));
    filter.push_back(BPF_STMT(BPF_LD | BPF_W | BPF_ABS,
                              static_cast<std::uint32_t>(offsetof(seccomp_data, nr))));
    appendEscalationDenials(filter);
    appendNamespaceCloneDenials(filter);

    // Keep the runtime switch explicit.  Both variants receive the same
    // minimal privilege boundary today; their independently traced allowlists
    // will replace this shared baseline rather than accidentally inheriting a
    // broad policy from the other runtime.
    switch (runtime) {
        case SandboxRuntime::Native:
        case SandboxRuntime::JavaScript:
            break;
    }
    filter.push_back(BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW));
    return filter;
}

} // namespace

bool installSandboxBaselineSeccomp(SandboxRuntime runtime, std::string& error) {
    error.clear();
    if (runtime != SandboxRuntime::Native && runtime != SandboxRuntime::JavaScript) {
        error = "sandbox seccomp received an unknown runtime";
        return false;
    }
    if (prctl(PR_GET_NO_NEW_PRIVS, 0, 0, 0, 0) != 1) {
        error = "seccomp requires PR_SET_NO_NEW_PRIVS before filter installation";
        return false;
    }
    std::vector<sock_filter> filter = buildBaselineFilter(runtime);
    sock_fprog program{};
    program.len = static_cast<unsigned short>(filter.size());
    program.filter = filter.data();
    if (prctl(PR_SET_SECCOMP, SECCOMP_MODE_FILTER, &program) != 0) {
        error = std::string("could not install sandbox seccomp filter: ") + std::strerror(errno);
        return false;
    }
    return true;
}

} // namespace lcl::security
