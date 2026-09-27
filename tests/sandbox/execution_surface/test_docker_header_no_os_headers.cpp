// ADR-207 (#120 S7): docker_execution_surface.hpp and sandbox/detail/process_identity.hpp include no OS header. Their
// process-spawning and process-identity bodies are compiled in src/sandbox/, which is where <windows.h> and the POSIX
// process headers now live, so a file that includes the Docker surface no longer gets <windows.h>'s macros (min, max,
// ERROR, DELETE, ...) or the POSIX process API as a side effect.
//
// The check is at compile time: this file does not build if either header pulls <windows.h> (which defines _WINDOWS_)
// or glibc's <spawn.h> (_SPAWN_H), <sys/wait.h> (_SYS_WAIT_H) or <poll.h> (_SYS_POLL_H) back in. <unistd.h> is not
// checked: libstdc++'s own <atomic> includes it (bits/atomic_wait.h), and the header needs <atomic>. Positive control
// (ADR-207 §5a C5): against main's versions of the two headers this file fails to compile on its platform's check.
#include "agentengine/sandbox/docker_execution_surface.hpp"

#include <cstdio>

#if defined(_WINDOWS_)
#error "docker_execution_surface.hpp pulls in <windows.h> again (ADR-207)"
#endif
#if defined(_SPAWN_H)
#error "docker_execution_surface.hpp pulls in <spawn.h> again (ADR-207)"
#endif
#if defined(_SYS_WAIT_H)
#error "docker_execution_surface.hpp pulls in <sys/wait.h> again (ADR-207)"
#endif
#if defined(_SYS_POLL_H)
#error "docker_execution_surface.hpp pulls in <poll.h> again (ADR-207)"
#endif

int main() {
    // Reaching main means the checks above held when this file compiled.
    std::puts("ok: docker_execution_surface.hpp includes no OS process header (ADR-207)");
    return 0;
}
