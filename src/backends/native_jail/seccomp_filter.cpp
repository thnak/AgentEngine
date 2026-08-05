// Implements seccomp_filter.hpp. See that header for the spec citations this satisfies.

#include "backends/native_jail/seccomp_filter.hpp"

#include <linux/audit.h>
#include <linux/filter.h>
#include <linux/seccomp.h>
#include <sys/prctl.h>
#include <sys/syscall.h>

#include <array>
#include <cerrno>
#include <cstddef>
#include <cstring>
#include <string>
#include <vector>

namespace agentengine::native_jail {

namespace {

std::unexpected<ae::error> errno_error(char const* what, failure_class klass, char const* code) {
    int e = errno;
    return std::unexpected(
        ae::error{klass, std::string(what) + " failed: " + std::strerror(e), code});
}

// The curated denylist -- see seccomp_filter.hpp's header comment for why a denylist is the right
// shape at this specific (kernel-backstop) layer, unlike 010's interpreter-level allowlist.
constexpr std::array<long, 20> kDeniedSyscalls = {
    __NR_ptrace,       __NR_mount,          __NR_umount2,        __NR_pivot_root,
    __NR_unshare,      __NR_setns,          __NR_reboot,         __NR_kexec_load,
    __NR_kexec_file_load, __NR_init_module, __NR_finit_module,   __NR_delete_module,
    __NR_acct,         __NR_swapon,         __NR_swapoff,        __NR_quotactl,
    __NR_bpf,          __NR_perf_event_open, __NR_process_vm_readv, __NR_process_vm_writev,
};

}  // namespace

result<void> install_seccomp_filter() {
    if (prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) != 0) {
        return errno_error("prctl(PR_SET_NO_NEW_PRIVS)", failure_class::fatal,
                            "seccomp.no_new_privs_failed");
    }

    // Program layout:
    //   [0]              load arch
    //   [1]              arch != X86_64 -> kill_process
    //   [2]              load syscall nr
    //   [3 .. 3+N-1]     one JEQ-per-denied-syscall, each jumping to its own ERRNO return
    //   [3+N]            default: ALLOW
    //   [3+N+1 .. ]      one ERRNO return per denied syscall (in the same order as the JEQs)
    std::size_t const n = kDeniedSyscalls.size();
    std::vector<sock_filter> program;
    program.reserve(3 + n + 1 + n);

    program.push_back(BPF_STMT(BPF_LD | BPF_W | BPF_ABS,
                                offsetof(struct seccomp_data, arch)));
    // Placeholder jump distance patched below once every offset is known.
    program.push_back(BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, AUDIT_ARCH_X86_64, 1, 0));
    program.push_back(BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_KILL_PROCESS));
    program.push_back(BPF_STMT(BPF_LD | BPF_W | BPF_ABS, offsetof(struct seccomp_data, nr)));

    std::size_t const nr_load_index = program.size() - 1;
    for (std::size_t i = 0; i < n; ++i) {
        // Jump to this syscall's own ERRNO return, computed once every instruction before it is
        // fixed: (allow-fallthrough slot) + (remaining JEQ checks after this one) + (this
        // syscall's position among the trailing RET block).
        program.push_back(BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K,
                                    static_cast<__u32>(kDeniedSyscalls[i]), 0, 0));  // jt patched below
    }
    // Every JEQ's `jf` (0) already falls through correctly: the next sequential instruction after
    // the last JEQ is this ALLOW, and after any earlier JEQ is simply the next JEQ.
    program.push_back(BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW));

    for (std::size_t i = 0; i < n; ++i) {
        std::size_t jeq_index = nr_load_index + 1 + i;
        std::size_t ret_index = program.size();
        // Distance from the instruction AFTER the jump to this syscall's ERRNO return.
        std::size_t jt = ret_index - (jeq_index + 1);
        program[jeq_index].jt = static_cast<__u8>(jt);
        program.push_back(BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ERRNO | (EPERM & SECCOMP_RET_DATA)));
    }

    struct sock_fprog prog {};
    prog.len = static_cast<unsigned short>(program.size());
    prog.filter = program.data();

    if (prctl(PR_SET_SECCOMP, SECCOMP_MODE_FILTER, &prog) != 0) {
        return errno_error("prctl(PR_SET_SECCOMP)", failure_class::fatal,
                            "seccomp.install_filter_failed");
    }
    return {};
}

}  // namespace agentengine::native_jail
