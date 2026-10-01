// ADR-209 build step 7 (GitHub issue #146): `NativePwshSession` and `NativeShellSessionProvider` against a REAL pwsh,
// inside a real Job Object. Windows only (AGENTENGINE_WITH_NATIVE_PROCESS). The shell under test is Windows PowerShell
// (`powershell`, present on every supported Windows); exits 77 (ctest SKIP) if it is not on PATH -- never a silent pass.
//
// Machine safety (CLAUDE.md): this process caps its own memory; every shell runs in a job with a 512 MiB memory cap
// and a small active-process limit; an in-process watchdog _Exit()s past 300 s.
//
// Session (the process half):
//   W0  pwsh, when present, is contained or refused (`job_not_enforced`) -- MEASURED: the Store/MSIX pwsh 7.6 build
//       starts every child OUTSIDE the job, so it is refused; never open with children escaping.
//   W1  a command's output and exit code come back; a native program's exit code is the command's.
//   W2  cwd, $env: and shell variables persist across exec(); the snapshot reports the cwd and the env change.
//   W3  a parse error is exit 2 and the shell survives; `exit` loses the shell.
//   W4  a timeout ends the shell AND a background process it started (the whole job), and returns promptly.
//   W5  the job's active-process limit holds: a command trying to start 12 processes leaves at most the limit
//       running (C15, native half).
//   W6  replay is injection-safe in pwsh (C9): values with ', U+2019, newline, $(...) and ; come back byte-for-byte
//       and nothing in them runs.
//   W7  the snapshot cwd never reaches the host spawn (C14): after replaying cwd C:\Windows, the process was still
//       spawned at the worktree root and pwsh is at C:\Windows only by its own Set-Location.
// Provider:
//   N1  C13: no tool without a grant, with a one-shot grant, or with a live grant missing a cap; a tool with
//       live_session + both caps.
//   N2  the first call opens (an audit run event), state persists across calls.
//   N3  revocation: a call with the grant gone is refused AND the job is killed (its background process with it).
//   N4  a narrowed grant (smaller session_wall_ms_cap than the running session) is treated as revoked.
//   N5  another principal is refused and the opener's shell is untouched.
//   N6  LiveShellOpen bounds re-opens: once spent, a restart is refused.
//   N7  lifetime::PerRun ends the shell at run end; the next call re-opens with the last snapshot's cwd.
//   N8  session_wall_ms_cap is a ceiling: past it, the next check point ends the shell.
// ADR-209 §15.5 (red-team fixes):
//   W8  M3: an inheritable pipe handle the host holds is not reachable from the shell (handle list).
//   N9  M4: a revocation ends an IDLE shell (no further command) at the next on_context / on_run_end.
//   N10 a grantless other identity is refused as the wrong principal and cannot end the opener's shell.
//   N11 a revoked shell's snapshot is not replayed into the next one.
//   N12 M5: the grant's cpu_ms_cap ends a shell that spends it.
//
// Positive controls (planted by hand, recorded in ADR-209 §15): dropping the per-command re-check fails N3/N4;
// using `live_session` alone as the tool gate (no cap check) fails N1; not setting the job's process limit fails W5;
// skipping the containment probe fails W0 on a host whose pwsh is the Store build.

#include "backends/native_process/native_shell_session_provider.hpp"

#include "../../support/memory_cap.hpp"
#include "agentengine/core/base64.hpp"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <thread>

using namespace agentengine;
using namespace agentengine::native_process;
namespace fs = std::filesystem;
using namespace std::chrono_literals;

namespace {

int g_checks = 0;
int g_failed = 0;
void check(bool cond, std::string const& what) {
    ++g_checks;
    if (cond) {
        std::printf("[ok]   %s\n", what.c_str());
    } else {
        ++g_failed;
        std::printf("[FAIL] %s\n", what.c_str());
    }
    std::fflush(stdout);
}

[[nodiscard]] std::string show(std::string s) {
    for (auto& c : s) {
        if (c == '\n') c = '|';
        if (c == '\r') c = '~';
    }
    return s.size() > 300 ? s.substr(0, 300) + "..." : s;
}

template <class T>
[[nodiscard]] T drive(agentengine::rt::task<T> t) {
    while (!t.done()) t.resume();
    return t.take_value();
}

// Windows PowerShell 5.1 ships with every supported Windows; the Store pwsh fails the containment probe (W0).
constexpr std::string_view kShell = "powershell";

[[nodiscard]] bool process_alive(DWORD pid) {
    HANDLE h = OpenProcess(SYNCHRONIZE, FALSE, pid);
    if (h == nullptr) return false;
    bool const alive = WaitForSingleObject(h, 0) == WAIT_TIMEOUT;
    CloseHandle(h);
    return alive;
}

[[nodiscard]] cap::NativeExec grant(bool live, std::optional<std::uint64_t> session_ms, std::optional<std::uint32_t> procs,
                                    std::optional<std::uint64_t> cpu_ms = std::nullopt) {
    cap::NativeExec g{std::string(kShell), "workdir", std::nullopt, std::nullopt, 512ull << 20, false, std::nullopt, std::nullopt};
    g.live_session = live;
    g.session_wall_ms_cap = session_ms;
    g.max_processes = procs;
    g.cpu_ms_cap = cpu_ms;
    return g;
}

[[nodiscard]] DWORD pid_of(result<NativeShellSessionReply> const& r) {
    if (!r.has_value()) return 0;
    try {
        return static_cast<DWORD>(std::stoul(r->output));
    } catch (...) {
        return 0;
    }
}

struct Ctx {
    CapabilitySet caps;
    EffectContext ctx;
    std::vector<run_event_payload::SandboxExec> events;
    explicit Ctx(std::vector<Capability> held, std::string who = "native-owner")
        : caps(CapabilitySet::grant_root(std::move(held))) {
        ctx.principal.id = std::move(who);
        ctx.capabilities = borrow_capabilities(caps);
        ctx.sandbox_exec_sink = [this](run_event_kind, run_event_payload::SandboxExec p) { events.push_back(std::move(p)); };
    }
    Ctx(Ctx const&) = delete;
};

template <class P>
[[nodiscard]] std::optional<ToolDescriptor> tool_of(P& p, Ctx& c) {
    static std::vector<Message> const none;
    SessionContext sc{"native-live", c.ctx.principal, none};
    auto contribution = drive(p.on_context(sc, c.ctx));
    if (!contribution.has_value()) return std::nullopt;
    for (auto& d : contribution->tools) {
        if (d.name == "native_shell_session_exec") return d;
    }
    return std::nullopt;
}

template <class P>
[[nodiscard]] result<NativeShellSessionReply> run(P& p, Ctx& c, std::string const& command, bool restart = false) {
    auto tool = tool_of(p, c);
    if (!tool.has_value()) return std::unexpected(error{failure_class::contract, "no tool", "test.no_tool"});
    std::vector<std::pair<std::string, json::Value>> m{{"command", json::Value::make_string(command)}};
    if (restart) m.emplace_back("restart", json::Value::make_bool(true));
    auto out = tool->invoke(json::Value::make_object(std::move(m)), c.ctx);
    if (!out.has_value()) return std::unexpected(out.error());
    return schema::from_json<NativeShellSessionReply>(*out);
}

}  // namespace

int main() {
    (void)agentengine::test_support::cap_process_memory(512ull << 20, 2048ull << 20);
    std::thread([] {
        std::this_thread::sleep_for(300s);
        std::printf("[FAIL] WATCHDOG: test exceeded its 300 s budget\n");
        std::fflush(stdout);
        std::_Exit(3);
    }).detach();

    auto found = scan_path({std::string(kShell)});
    auto it = std::ranges::find_if(found, [](DiscoveredExecutable const& d) { return d.short_name == kShell; });
    if (it == found.end()) {
        std::printf("SKIP: no %s on PATH\n", std::string(kShell).c_str());
        return 77;
    }
    std::string const pwsh = it->resolved_path;

    fs::path const root = fs::temp_directory_path() / "ae_test_native_live_root";
    std::error_code ec;
    fs::remove_all(root, ec);
    fs::create_directories(root / "sub", ec);

    NativeShellSessionConfig config;
    config.pwsh_path = pwsh;
    config.cwd = root.wstring();
    config.memory_bytes = 512ull << 20;
    config.max_processes = 6;

    // ---- W0: pwsh, when present, is either contained or refused -- never open with children outside the job.
    if (auto pw = scan_path({"pwsh"}); !pw.empty()) {
        NativeShellSessionConfig pc = config;
        pc.pwsh_path = pw.front().resolved_path;
        NativePwshSession s;
        auto o = s.open(pc, nullptr);
        if (!o.has_value()) {
            check(o.error().code == "native_shell_session.job_not_enforced",
                  "W0: " + pc.pwsh_path + " is refused because its children escape the job (" + o.error().code + ")");
        } else {
            auto r = s.exec("([Diagnostics.Process]::Start('ping.exe','-n 30 127.0.0.1')).Id", 30s);
            DWORD pid = 0;
            try { pid = r ? static_cast<DWORD>(std::stoul(r->output)) : 0; } catch (...) {}
            BOOL in_job = FALSE;
            if (HANDLE h = pid ? OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid) : nullptr) {
                IsProcessInJob(h, s.job_handle(), &in_job);
                CloseHandle(h);
            }
            check(in_job != FALSE, "W0: " + pc.pwsh_path + " opened, and its children are inside the job");
        }
    }

    // ---- W1-W3
    {
        NativePwshSession s;
        auto opened = s.open(config, nullptr);
        check(opened.has_value(), "W1: pwsh opens inside a job" + (opened ? std::string() : ": " + opened.error().message));
        if (!opened.has_value()) return EXIT_FAILURE;
        auto r = s.exec("Write-Output 'hi'; cmd /c exit 3", 30s);
        check(r.has_value() && r->exit_code == 3 && r->output.find("hi") != std::string::npos,
              "W1: output and a native program's exit code come back (" + (r ? show(r->output) + " rc=" + std::to_string(r->exit_code) : r.error().message) + ")");
        (void)s.exec("Set-Location sub; $env:GREETING = 'hello there'; $kept = 5", 30s);
        auto p = s.exec("Write-Output \"$((Get-Location).Path)|$env:GREETING|$kept\"", 30s);
        bool const persisted = p.has_value() && p->output.find("sub|hello there|5") != std::string::npos;
        check(persisted, "W2: cwd, $env: and variables persist (" + (p ? show(p->output) : p.error().message) + ")");
        bool snap_ok = p.has_value() && p->snapshot.has_value() && p->snapshot->cwd.ends_with("sub");
        bool env_ok = false;
        if (snap_ok) {
            for (auto const& [k, v] : p->snapshot->env_set) env_ok = env_ok || (k == "GREETING" && v == "hello there");
        }
        check(snap_ok && env_ok, "W2: the snapshot reports the cwd and the env change");
        auto bad = s.exec("Write-Output \"unbalanced", 30s);
        auto after = s.exec("Write-Output 'still-here'", 30s);
        check(bad.has_value() && bad->exit_code == 2 && !bad->shell_lost && after.has_value() &&
                  after->output.find("still-here") != std::string::npos,
              "W3: a parse error is exit 2 and the shell survives");
        auto ex = s.exec("exit 4", 30s);
        check(ex.has_value() && ex->shell_lost && !s.is_live(), "W3: `exit` loses the shell");
    }

    // ---- W4
    {
        NativePwshSession s;
        (void)s.open(config, nullptr);
        auto bg = s.exec("(Start-Process -FilePath ping.exe -ArgumentList '-n','200','127.0.0.1' -NoNewWindow -PassThru).Id", 30s);
        DWORD bg_pid = 0;
        if (bg.has_value()) {
            try {
                bg_pid = static_cast<DWORD>(std::stoul(bg->output));
            } catch (...) {
            }
        }
        check(bg_pid != 0 && process_alive(bg_pid), "W4 setup: a background process is running in the shell (" +
                                                        (bg ? show(bg->output) : bg.error().message) + ")");
        auto t0 = std::chrono::steady_clock::now();
        auto r = s.exec("Write-Output started; Start-Sleep -Seconds 60", 2s);
        auto const took = std::chrono::steady_clock::now() - t0;
        check(r.has_value() && r->timed_out && r->shell_lost && took < 15s, "W4: a timeout ends the shell promptly");
        check(bg_pid != 0 && !process_alive(bg_pid), "W4: ... and the background process with it (the whole job)");
    }

    // ---- W5
    {
        NativeShellSessionConfig small = config;
        small.max_processes = 4;
        NativePwshSession s;
        (void)s.open(small, nullptr);
        auto r = s.exec("$n = 0; 1..12 | ForEach-Object { try { Start-Process -FilePath ping.exe -ArgumentList '-n','30','127.0.0.1' -NoNewWindow -ErrorAction Stop | Out-Null; $n++ } catch { } }; \"started=$n\"", 60s);
        // The limit counts the shell itself, so at most 3 of the 12 starts can succeed. `ActiveProcesses` also
        // counts the shell's console host (conhost.exe), which the system starts in the job: limit + 1.
        auto const active = s.active_processes();
        int started = -1;
        if (r.has_value()) {
            auto const at = r->output.find("started=");
            if (at != std::string::npos) started = std::atoi(r->output.c_str() + at + 8);
        }
        check(started >= 0 && started <= 3 && active.has_value() && *active <= 5,
              "W5: the job's process limit holds (started=" + std::to_string(started) + " of 12, active=" +
                  (active ? std::to_string(*active) : std::string("?")) + ")");
        s.terminate();
    }

    // ---- W6, W7
    {
        ShellSnapshot snap;
        snap.cwd = "C:\\Windows";
        snap.env_set = {{"V1", "it's"},
                        {"V2", "\xE2\x80\x99$(New-Item -ItemType File -Path pwned1.txt)\xE2\x80\x99"},
                        {"V3", "a\nb"},
                        {"V4", "x'; New-Item -ItemType File -Path pwned2.txt; '"}};
        NativePwshSession s;
        auto o = s.open(config, &snap);
        auto r = s.exec("Write-Output ([Convert]::ToBase64String([Text.Encoding]::UTF8.GetBytes(\"[$env:V1][$env:V2][$env:V3][$env:V4]\")))", 30s);
        if (r.has_value()) {
            std::string b64 = r->output;
            while (!b64.empty() && (b64.back() == '\n' || b64.back() == '\r')) b64.pop_back();
            auto bytes = agentengine::base64::decode_strict(b64);
            r->output = bytes ? std::string(reinterpret_cast<char const*>(bytes->data()), bytes->size()) : std::string("<not base64>");
        }
        std::string const expected = std::string("[it's][\xE2\x80\x99$(New-Item -ItemType File -Path pwned1.txt)\xE2\x80\x99][a\nb]") +
                                     "[x'; New-Item -ItemType File -Path pwned2.txt; ']";
        bool const pwned = fs::exists(root / "pwned1.txt") || fs::exists(root / "pwned2.txt") ||
                           fs::exists("C:\\Windows\\pwned1.txt") || fs::exists("C:\\Windows\\pwned2.txt");
        check(o.has_value() && r.has_value() && r->output == expected && !pwned,
              "W6: hostile values come back byte-for-byte and nothing in them ran (" + (r ? show(r->output) : std::string("error")) + ")");
        auto where = s.exec("Write-Output \"$((Get-Location).Path)|$([Environment]::CurrentDirectory)\"", 30s);
        std::string const root_text = root.string();
        check(where.has_value() && where->output.find("C:\\Windows|") != std::string::npos &&
                  where->output.find(root_text) != std::string::npos,
              "W7: the snapshot cwd moved pwsh's location only; the process was spawned at the worktree root (" +
                  (where ? show(where->output) : std::string("error")) + ")");
    }

    // ---- W8 (ADR-209 §15.5 M3): an inheritable handle the HOST holds is not handed to the shell.
    {
        SECURITY_ATTRIBUTES sa{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
        HANDLE rd = nullptr;
        HANDLE wr = nullptr;
        bool const piped = CreatePipe(&rd, &wr, &sa, 0) != 0;
        if (piped) {
            SetHandleInformation(wr, HANDLE_FLAG_INHERIT, 0);
            char const secret[] = "AE-SECRET-4711\n";
            DWORD written = 0;
            WriteFile(wr, secret, sizeof(secret) - 1, &written, nullptr);
            CloseHandle(wr);
        }
        NativePwshSession s;
        auto o = s.open(config, nullptr);
        std::string const handle_text = std::to_string(reinterpret_cast<std::uintptr_t>(rd));
        auto r = s.exec("try { $p = New-Object System.IO.Pipes.AnonymousPipeClientStream([System.IO.Pipes.PipeDirection]::In, '" +
                            handle_text + "'); $rd = New-Object System.IO.StreamReader($p); Write-Output ('got:' + $rd.ReadLine()) } "
                            "catch { Write-Output 'no-handle' }",
                        15s);
        check(piped && o.has_value() && r.has_value() && r->output.find("AE-SECRET-4711") == std::string::npos,
              "W8: a host-side inheritable pipe handle (" + handle_text + ") is not reachable from the shell (" +
                  (r ? show(r->output) : r.error().message) + ")");
        s.terminate();
        if (piped) CloseHandle(rd);
    }

    // ---- provider
    IdentityAuthority& authority = IdentityAuthority::bootstrap();
    IdentityHandle owner = authority.adopt(Principal{.id = "native-owner"});
    auto open_q = *agentengine::rt::AsyncQuota<LiveShellOpen>::mint_root(authority, owner, 100);
    auto small_q = *agentengine::rt::AsyncQuota<LiveShellOpen>::mint_root(authority, owner, 2);
    LiveShellLimits const limits{10min, 30min, 100};
    auto clock_now = std::make_shared<std::chrono::steady_clock::time_point>(std::chrono::steady_clock::now());
    LiveShellClock manual = [clock_now] { return *clock_now; };

    // ---- N1
    {
        NativeShellSessionProvider<> p({std::string(kShell)}, root.wstring(), "workdir", open_q, limits, 30s,
                                       approval_mode::always_require, lifetime::PerRun{}, default_live_shell_clock(),
                                       std::string(kShell));
        Ctx none({});
        Ctx oneshot({Capability{grant(false, std::nullopt, std::nullopt)}});
        Ctx no_procs({Capability{grant(true, 600000, std::nullopt)}});
        Ctx no_wall({Capability{grant(true, std::nullopt, 8)}});
        Ctx full({Capability{grant(true, 600000, 8)}});
        check(!tool_of(p, none).has_value() && !tool_of(p, oneshot).has_value() && !tool_of(p, no_procs).has_value() &&
                  !tool_of(p, no_wall).has_value(),
              "N1: no tool without a live grant carrying both caps (C13)");
        check(tool_of(p, full).has_value(), "N1 (positive control): live_session + both caps contributes the tool");
    }

    // ---- N2-N5
    {
        NativeShellSessionProvider<lifetime::PerSession> p({std::string(kShell)}, root.wstring(), "workdir", open_q, limits,
                                                         30s, approval_mode::always_require, lifetime::PerSession{},
                                                         default_live_shell_clock(), std::string(kShell));
        Ctx c({Capability{grant(true, 600000, 8)}});
        auto a = run(p, c, "Set-Location sub; $env:K = 'v1'");
        auto b = run(p, c, "Write-Output \"$((Get-Location).Path)|$env:K\"");
        check(a.has_value() && a->reopened && b.has_value() && !b->reopened && b->output.find("sub|v1") != std::string::npos,
              "N2: the first call opens, state persists (" + (b ? show(b->output) : b.error().message) + ")");
        bool const audited = std::ranges::any_of(c.events, [](auto const& e) {
            return e.backend == "native-live-shell" && e.stage == "create" && e.ok;
        });
        check(audited, "N2: the open is an audit run event");

        Ctx other({Capability{grant(true, 600000, 8)}}, "someone-else");
        auto intruder = run(p, other, "Write-Output intruder");
        check(!intruder.has_value() && intruder.error().code == "native_shell_session.principal_mismatch" &&
                  p.session()->is_live(),
              "N5: another principal is refused and the opener's shell is untouched");

        auto bg = run(p, c, "(Start-Process -FilePath ping.exe -ArgumentList '-n','200','127.0.0.1' -NoNewWindow -PassThru).Id");
        DWORD bg_pid = 0;
        if (bg.has_value()) {
            try {
                bg_pid = static_cast<DWORD>(std::stoul(bg->output));
            } catch (...) {
            }
        }
        // Since §15.5 (M4) the narrowing is caught at the narrowed context's on_context, before any command: the job
        // (its background process too) is killed there, and the command then runs in a FRESH shell bounded by the
        // narrower grant -- never in the one opened under the wider grant.
        Ctx narrowed({Capability{grant(true, 1000, 8)}});
        auto n4 = run(p, narrowed, "Write-Output narrowed");
        check(n4.has_value() && n4->reopened && bg_pid != 0 && !process_alive(bg_pid),
              "N4: a narrowed grant is a revocation: the job (its background process too) is killed, and the next "
              "command runs in a fresh shell under the narrower grant");

        p.terminate();
        auto again = run(p, c, "Write-Output back");
        check(again.has_value() && again->reopened, "N3 setup: the full grant re-opens");
        Ctx revoked({Capability{grant(false, std::nullopt, std::nullopt)}});
        // The tool is not even offered under the one-shot grant; drive the still-held descriptor from the full context.
        auto tool = tool_of(p, c);
        std::optional<result<json::Value>> n3;
        if (tool.has_value()) n3 = tool->invoke(json::Value::make_object({{"command", json::Value::make_string("Write-Output x")}}), revoked.ctx);
        check(n3.has_value() && !n3->has_value() && (*n3).error().code == "native_shell_session.grant_revoked" &&
                  !p.session()->is_live(),
              "N3: a call made after the grant is gone is refused and the shell is killed");
    }

    // ---- N6
    {
        NativeShellSessionProvider<lifetime::PerSession> p({std::string(kShell)}, root.wstring(), "workdir", small_q, limits,
                                                         30s, approval_mode::always_require, lifetime::PerSession{},
                                                         default_live_shell_clock(), std::string(kShell));
        Ctx c({Capability{grant(true, 600000, 8)}});
        auto a = run(p, c, "Write-Output one");
        auto b = run(p, c, "Write-Output two", true);
        auto refused = run(p, c, "Write-Output three", true);
        check(a.has_value() && b.has_value() && b->reopened && !refused.has_value() &&
                  refused.error().code == "async_quota.exhausted",
              "N6: LiveShellOpen bounds re-opens");
    }

    // ---- N7, N8
    {
        NativeShellSessionProvider<> p({std::string(kShell)}, root.wstring(), "workdir", open_q, limits, 30s,
                                       approval_mode::always_require, lifetime::PerRun{}, manual, std::string(kShell));
        Ctx c({Capability{grant(true, 600000, 8)}});
        (void)run(p, c, "Set-Location sub");
        DWORD const first_pid = p.session()->shell_pid();
        (void)drive(p.on_run_end(RunEndView{run_end_reason::final_answer}, c.ctx));
        check(!p.holds_shell() && !process_alive(first_pid), "N7: PerRun ends the shell at run end");
        auto next = run(p, c, "Write-Output (Get-Location).Path");
        check(next.has_value() && next->reopened && next->output.find("sub") != std::string::npos,
              "N7: the next call re-opens with the last snapshot's cwd (" + (next ? show(next->output) : next.error().message) + ")");
        DWORD const pid = p.session()->shell_pid();
        *clock_now += 11min;  // past session_wall_ms_cap (600000 ms)
        (void)tool_of(p, c);  // on_context
        check(!p.holds_shell() && !process_alive(pid), "N8: past session_wall_ms_cap the next check point ends the shell");
    }

    // ---- N9-N11 (ADR-209 §15.5 M4 and the ordering/snapshot minors)
    {
        NativeShellSessionProvider<lifetime::PerSession> p({std::string(kShell)}, root.wstring(), "workdir", open_q, limits,
                                                         30s, approval_mode::always_require, lifetime::PerSession{},
                                                         default_live_shell_clock(), std::string(kShell));
        Ctx c({Capability{grant(true, 600000, 8)}});
        (void)run(p, c, "Set-Location sub");
        DWORD const bg = pid_of(run(p, c, "(Start-Process -FilePath ping.exe -ArgumentList '-n','200','127.0.0.1' -NoNewWindow -PassThru).Id"));

        // N10: another identity holding NO grant must not be able to end the opener's shell.
        Ctx stranger({}, "someone-else");
        std::vector<std::pair<std::string, json::Value>> m{{"command", json::Value::make_string("Write-Output x")}};
        auto tool = tool_of(p, c);
        auto st = tool ? tool->invoke(json::Value::make_object(std::move(m)), stranger.ctx)
                       : result<json::Value>(std::unexpected(error{failure_class::contract, "no tool", "test.no_tool"}));
        check(!st.has_value() && st.error().code == "native_shell_session.principal_mismatch" && p.session()->is_live() &&
                  bg != 0 && process_alive(bg),
              "N10: a grantless other identity is refused as the wrong principal; the opener's shell keeps running");

        // N9: the grant is revoked and NO further command comes: the next check point (on_context) ends the idle
        // shell and its background process.
        Ctx revoked({Capability{grant(false, std::nullopt, std::nullopt)}});
        (void)tool_of(p, revoked);
        check(!p.holds_shell() && bg != 0 && !process_alive(bg),
              "N9: a revocation ends an IDLE shell at the next on_context (its background process too)");

        // N11: the snapshot it left is not replayed into the next shell.
        auto next = run(p, c, "Write-Output (Get-Location).Path");
        check(next.has_value() && next->reopened && next->output.find("\\sub") == std::string::npos,
              "N11: after a revocation the next shell does not replay the revoked one's cwd (" +
                  (next ? show(next->output) : next.error().message) + ")");

        // N9b: the same at run end.
        DWORD const bg2 = pid_of(run(p, c, "(Start-Process -FilePath ping.exe -ArgumentList '-n','200','127.0.0.1' -NoNewWindow -PassThru).Id"));
        (void)drive(p.on_run_end(RunEndView{run_end_reason::final_answer}, revoked.ctx));
        check(!p.holds_shell() && bg2 != 0 && !process_alive(bg2), "N9: ... and at on_run_end (PerSession would keep it)");
    }

    // ---- N12 (ADR-209 §15.5 M5): the grant's cpu_ms_cap is enforced on the held shell.
    {
        NativeShellSessionProvider<lifetime::PerSession> p({std::string(kShell)}, root.wstring(), "workdir", open_q, limits,
                                                         30s, approval_mode::always_require, lifetime::PerSession{},
                                                         default_live_shell_clock(), std::string(kShell));
        Ctx c({Capability{grant(true, 600000, 8, 3000)}});
        auto r = run(p, c, "$t = [Diagnostics.Stopwatch]::StartNew(); while ($t.ElapsedMilliseconds -lt 8000) { $null = 1 + 1 }; 'spun'");
        bool const ended = (r.has_value() && r->shell_lost) || !p.holds_shell();
        check(ended, "N12: a shell that spends its 3000 ms cpu_ms_cap is ended (" +
                         (r ? show(r->output) + (r->shell_lost ? " [lost]" : "") : r.error().message) + ")");
    }

    fs::remove_all(root, ec);
    std::printf("\n%d checks, %d failed\n", g_checks, g_failed);
    if (g_failed == 0) std::printf("ALL PASS\n");
    return g_failed == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
