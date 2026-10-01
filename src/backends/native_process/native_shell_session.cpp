// Implements native_shell_session.hpp -- decisions/ADR-209-persistent-shell-sessions.md §9 (build step 7). See the
// header for the transport, the Job Object and what is and is not contained.

#include "backends/native_process/native_shell_session.hpp"

#include <algorithm>
#include <charconv>
#include <cstddef>
#include <fstream>
#include <random>
#include <thread>

#include "agentengine/core/base64.hpp"

namespace agentengine::native_process {

namespace {

namespace ps = agentengine::persistent_shell;

[[nodiscard]] std::wstring widen(std::string const& utf8) {
    if (utf8.empty()) return {};
    int const n = MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
    std::wstring out(static_cast<std::size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), out.data(), n);
    return out;
}

[[nodiscard]] std::string narrow(std::wstring const& w) {
    if (w.empty()) return {};
    int const n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<std::size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), out.data(), n, nullptr, nullptr);
    return out;
}

[[nodiscard]] agentengine::error err(failure_class k, std::string msg, char const* code) {
    return agentengine::error{k, std::move(msg), code};
}

[[nodiscard]] std::string random_hex(std::size_t bytes) {
    std::random_device rd;  // MSVC: rand_s, a CSPRNG
    static constexpr char kHex[] = "0123456789abcdef";
    std::string out;
    out.reserve(bytes * 2);
    for (std::size_t i = 0; i < bytes; ++i) {
        auto const b = static_cast<unsigned>(rd() & 0xFFu);
        out.push_back(kHex[b >> 4]);
        out.push_back(kHex[b & 0xF]);
    }
    return out;
}

// The same minimal block the one-shot native providers spawn with (native_process_spawn.cpp): no ambient
// authority from this host process's own environment.
[[nodiscard]] std::wstring minimal_environment_block() {
    auto get = [](wchar_t const* name) -> std::wstring {
        wchar_t buf[MAX_PATH]{};
        DWORD const len = GetEnvironmentVariableW(name, buf, MAX_PATH);
        return (len > 0 && len < MAX_PATH) ? std::wstring(buf, len) : std::wstring{};
    };
    std::wstring root = get(L"SystemRoot");
    if (root.empty()) root = L"C:\\Windows";
    std::wstring block;
    auto add = [&](std::wstring const& kv) {
        block += kv;
        block.push_back(L'\0');
    };
    add(L"SystemRoot=" + root);
    add(L"Path=" + root + L"\\System32;" + root);
    // A fixed lookup rule, not inherited: without it pwsh cannot resolve `cmd` or `ping` by bare name.
    add(L"PATHEXT=.COM;.EXE;.BAT;.CMD");
    if (auto v = get(L"LOCALAPPDATA"); !v.empty()) add(L"LOCALAPPDATA=" + v);
    if (auto v = get(L"USERPROFILE"); !v.empty()) add(L"USERPROFILE=" + v);
    block.push_back(L'\0');
    return block;
}

[[nodiscard]] std::wstring quote_arg(std::wstring const& arg) {
    if (!arg.empty() && arg.find_first_of(L" \t\n\v\"") == std::wstring::npos) return arg;
    std::wstring out = L"\"";
    std::size_t bs = 0;
    for (wchar_t c : arg) {
        if (c == L'\\') {
            ++bs;
            continue;
        }
        if (c == L'"') {
            out.append(bs * 2 + 1, L'\\');
            out.push_back(L'"');
            bs = 0;
            continue;
        }
        out.append(bs, L'\\');
        bs = 0;
        out.push_back(c);
    }
    out.append(bs * 2, L'\\');
    out.push_back(L'"');
    return out;
}

// -EncodedCommand takes base64 of UTF-16LE.
[[nodiscard]] std::string encode_utf16le_base64(std::string const& utf8) {
    std::wstring const w = widen(utf8);
    std::vector<std::byte> bytes;
    bytes.reserve(w.size() * 2);
    for (wchar_t c : w) {
        bytes.push_back(static_cast<std::byte>(static_cast<unsigned>(c) & 0xFFu));
        bytes.push_back(static_cast<std::byte>((static_cast<unsigned>(c) >> 8) & 0xFFu));
    }
    return agentengine::base64::encode(std::span<std::byte const>(bytes));
}

[[nodiscard]] std::optional<std::string> b64_text(std::string_view s) {
    auto bytes = agentengine::base64::decode_strict(s);
    if (!bytes.has_value()) return std::nullopt;
    return std::string(reinterpret_cast<char const*>(bytes->data()), bytes->size());
}

// Reads at most `cap` bytes of `p`; `total` gets the file's size. nullopt if it cannot be opened.
[[nodiscard]] std::optional<std::string> read_bounded(std::filesystem::path const& p, std::size_t cap,
                                                      std::uint64_t* total = nullptr) {
    std::error_code ec;
    auto const size = std::filesystem::file_size(p, ec);
    if (ec) return std::nullopt;
    if (total != nullptr) *total = size;
    std::ifstream in(p, std::ios::binary);
    if (!in) return std::nullopt;
    std::string out(static_cast<std::size_t>(std::min<std::uintmax_t>(size, cap)), '\0');
    in.read(out.data(), static_cast<std::streamsize>(out.size()));
    out.resize(static_cast<std::size_t>(in.gcount()));
    return out;
}

// The server loop. `{DIR}` is replaced by the base64 (UTF-8) of the private directory, decoded inside pwsh, so no
// host path is ever spliced into the script as a literal. `__ae_*` names are the loop's own; a command that
// redefines them only breaks its own replies (ADR-209 §7: the framing is not a security boundary).
constexpr std::string_view kServerScript = R"PS($ErrorActionPreference = 'Continue'
$ProgressPreference = 'SilentlyContinue'
$PSDefaultParameterValues['Out-File:Encoding'] = 'utf8'
$PSDefaultParameterValues['Out-File:Width'] = 8192
$PSDefaultParameterValues['Set-Content:Encoding'] = 'utf8'
$PSDefaultParameterValues['Add-Content:Encoding'] = 'utf8'
$__ae_D = [Text.Encoding]::UTF8.GetString([Convert]::FromBase64String('{DIR}'))
function __ae_env {
  $sb = [Text.StringBuilder]::new()
  foreach ($e in [Environment]::GetEnvironmentVariables().GetEnumerator()) {
    $n = [string]$e.Key
    if ($n.StartsWith('=')) { continue }
    [void]$sb.Append([Convert]::ToBase64String([Text.Encoding]::UTF8.GetBytes($n))).Append(' ').Append([Convert]::ToBase64String([Text.Encoding]::UTF8.GetBytes([string]$e.Value))).Append("`n")
  }
  $sb.ToString()
}
[IO.File]::WriteAllText("$__ae_D\base", (__ae_env))
# The containment probe: a child the host checks with IsProcessInJob before declaring the shell ready.
$__ae_probe = 0
try {
  $__ae_psi = New-Object Diagnostics.ProcessStartInfo ("$env:SystemRoot\System32\PING.EXE"), '-n 30 127.0.0.1'
  $__ae_psi.UseShellExecute = $false
  $__ae_psi.CreateNoWindow = $true
  $__ae_probe = ([Diagnostics.Process]::Start($__ae_psi)).Id
} catch { }
[IO.File]::WriteAllText("$__ae_D\ready.t", [string]$PID + ' ' + [string]$__ae_probe)
[IO.File]::Move("$__ae_D\ready.t", "$__ae_D\ready")
while ($true) {
  $__ae_r = [IO.Directory]::GetFiles($__ae_D, 'r.*')
  if ($__ae_r.Count -eq 0) { Start-Sleep -Milliseconds 10; continue }
  $__ae_n = [IO.Path]::GetExtension($__ae_r[0]).TrimStart('.')
  [IO.File]::Delete($__ae_r[0])
  if ($__ae_n -notmatch '^[0-9a-f]{32}$') { continue }
  $__ae_rc = 0
  $global:LASTEXITCODE = $null
  try {
    $__ae_sb = [ScriptBlock]::Create([IO.File]::ReadAllText("$__ae_D\c.$__ae_n"))
    try {
      . $__ae_sb *> "$__ae_D\o.$__ae_n"
      $__ae_ok = $?
      if ($null -ne $global:LASTEXITCODE) { $__ae_rc = [int]$global:LASTEXITCODE } elseif (-not $__ae_ok) { $__ae_rc = 1 }
    } catch {
      ($_ | Out-String) | Add-Content -LiteralPath "$__ae_D\o.$__ae_n"
      $__ae_rc = 1
    }
  } catch [System.Management.Automation.ParseException] {
    ($_ | Out-String) | Set-Content -LiteralPath "$__ae_D\o.$__ae_n"
    $__ae_rc = 2
  } catch {
    ($_ | Out-String) | Set-Content -LiteralPath "$__ae_D\o.$__ae_n"
    $__ae_rc = 1
  }
  $__ae_loc = ''
  try { $__ae_loc = [string](Get-Location).Path } catch { }
  $__ae_x = [string]$__ae_rc + "`n" + [Convert]::ToBase64String([Text.Encoding]::UTF8.GetBytes($__ae_loc)) + "`n" + (__ae_env)
  [IO.File]::WriteAllText("$__ae_D\x.$__ae_n.t", $__ae_x)
  [IO.File]::Move("$__ae_D\x.$__ae_n.t", "$__ae_D\x.$__ae_n")
}
)PS";

}  // namespace

namespace native_shell_detail {

std::optional<std::vector<std::pair<std::string, std::string>>> parse_env_lines(std::string_view raw) {
    std::vector<std::pair<std::string, std::string>> env;
    std::size_t pos = 0;
    while (pos < raw.size()) {
        std::size_t const nl = raw.find('\n', pos);
        std::string_view line = raw.substr(pos, nl == std::string_view::npos ? std::string_view::npos : nl - pos);
        pos = nl == std::string_view::npos ? raw.size() : nl + 1;
        if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
        if (line.empty()) continue;
        std::size_t const sp = line.find(' ');
        if (sp == std::string_view::npos) return std::nullopt;
        auto name = b64_text(line.substr(0, sp));
        auto value = b64_text(line.substr(sp + 1));
        if (!name.has_value() || !value.has_value()) return std::nullopt;
        env.emplace_back(std::move(*name), std::move(*value));
    }
    return env;
}

std::optional<StatusRecord> parse_status_record(std::string_view raw) {
    std::size_t const nl1 = raw.find('\n');
    if (nl1 == std::string_view::npos) return std::nullopt;
    std::size_t const nl2 = raw.find('\n', nl1 + 1);
    if (nl2 == std::string_view::npos) return std::nullopt;
    std::string_view rc_text = raw.substr(0, nl1);
    if (!rc_text.empty() && rc_text.back() == '\r') rc_text.remove_suffix(1);
    StatusRecord r;
    long long rc = 0;
    auto [ptr, ec] = std::from_chars(rc_text.data(), rc_text.data() + rc_text.size(), rc);
    if (ec != std::errc{} || ptr != rc_text.data() + rc_text.size()) return std::nullopt;
    if (rc < INT32_MIN || rc > INT32_MAX) return std::nullopt;
    r.exit_code = static_cast<int>(rc);
    std::string_view cwd_b64 = raw.substr(nl1 + 1, nl2 - nl1 - 1);
    if (!cwd_b64.empty() && cwd_b64.back() == '\r') cwd_b64.remove_suffix(1);
    auto cwd = b64_text(cwd_b64);
    if (!cwd.has_value()) return std::nullopt;
    r.cwd = std::move(*cwd);
    auto env = parse_env_lines(raw.substr(nl2 + 1));
    if (!env.has_value()) return std::nullopt;
    r.env = std::move(*env);
    return r;
}

}  // namespace native_shell_detail

bool NativePwshSession::is_live() const noexcept {
    return live_ && process_ != nullptr && WaitForSingleObject(process_, 0) == WAIT_TIMEOUT;
}

std::optional<std::uint32_t> NativePwshSession::active_processes() const noexcept {
    if (!job_) return std::nullopt;
    JOBOBJECT_BASIC_ACCOUNTING_INFORMATION info{};
    if (!QueryInformationJobObject(job_->native_handle(), JobObjectBasicAccountingInformation, &info, sizeof(info),
                                   nullptr)) {
        return std::nullopt;
    }
    return static_cast<std::uint32_t>(info.ActiveProcesses);
}

std::optional<std::uint64_t> NativePwshSession::cpu_ms_used() const noexcept {
    if (!job_) return std::nullopt;
    JOBOBJECT_BASIC_ACCOUNTING_INFORMATION info{};
    if (!QueryInformationJobObject(job_->native_handle(), JobObjectBasicAccountingInformation, &info, sizeof(info),
                                   nullptr)) {
        return std::nullopt;
    }
    // 100 ns units; includes processes that already exited.
    auto const total = static_cast<std::uint64_t>(info.TotalUserTime.QuadPart) +
                       static_cast<std::uint64_t>(info.TotalKernelTime.QuadPart);
    return total / 10'000u;
}

void NativePwshSession::kill_job() noexcept {
    if (job_) {
        (void)TerminateJobObject(job_->native_handle(), 1);
        if (process_ != nullptr) (void)WaitForSingleObject(process_, 5000);
    }
    live_ = false;
}

void NativePwshSession::terminate() noexcept {
    kill_job();
    job_.reset();  // closing the job: KILL_ON_JOB_CLOSE is the second net
    if (process_ != nullptr) {
        CloseHandle(process_);
        process_ = nullptr;
    }
    pid_ = 0;
    if (!dir_.empty()) {
        std::error_code ec;
        std::filesystem::remove_all(dir_, ec);
        dir_.clear();
    }
    base_env_.clear();
}

agentengine::result<void> NativePwshSession::open(NativeShellSessionConfig const& config, ShellSnapshot const* snapshot) {
    terminate();
    output_cap_ = config.output_cap_bytes;
    if (config.pwsh_path.empty() || config.cwd.empty()) {
        return std::unexpected(err(failure_class::contract, "a native shell needs a resolved pwsh and a worktree root",
                                   "native_shell_session.bad_config"));
    }
    std::error_code ec;
    dir_ = std::filesystem::temp_directory_path(ec) / ("ae_pwsh_" + random_hex(16));
    if (ec || !std::filesystem::create_directory(dir_, ec) || ec) {
        dir_.clear();
        return std::unexpected(err(failure_class::fatal, "could not create the shell's private directory",
                                   "native_shell_session.open_failed"));
    }

    std::string script(kServerScript);
    std::string const dir_b64 = agentengine::base64::encode(narrow(dir_.wstring()));
    script.replace(script.find("{DIR}"), 5, dir_b64);
    std::wstring cmdline = quote_arg(widen(config.pwsh_path)) + L" -NoLogo -NoProfile -NonInteractive -EncodedCommand " +
                           widen(encode_utf16le_base64(script));
    std::vector<wchar_t> mutable_cmd(cmdline.begin(), cmdline.end());
    mutable_cmd.push_back(L'\0');

    SECURITY_ATTRIBUTES sa{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
    HANDLE nul_in = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_EXISTING, 0, nullptr);
    HANDLE nul_out = CreateFileW(L"NUL", GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_EXISTING, 0, nullptr);
    // ADR-209 §15.5 M3: `bInheritHandles=TRUE` with no handle list would duplicate EVERY inheritable handle this
    // host process holds (another session's `docker exec` pipes, sockets) into a shell that lives for hours --
    // the exact defect native_jail_backend.cpp records as reproduced and fixed. Only the two NUL handles go.
    if (nul_in == INVALID_HANDLE_VALUE || nul_out == INVALID_HANDLE_VALUE) {
        if (nul_in != INVALID_HANDLE_VALUE) CloseHandle(nul_in);
        if (nul_out != INVALID_HANDLE_VALUE) CloseHandle(nul_out);
        terminate();
        return std::unexpected(err(failure_class::fatal, "could not open NUL for the shell's standard handles",
                                   "native_shell_session.open_failed"));
    }
    HANDLE inherit_list[2] = {nul_in, nul_out};
    SIZE_T attr_size = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &attr_size);
    std::vector<std::byte> attr_buf(attr_size);
    auto* attr_list = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attr_buf.data());
    bool const attr_ok = InitializeProcThreadAttributeList(attr_list, 1, 0, &attr_size) != 0;
    bool const list_ok = attr_ok && UpdateProcThreadAttribute(attr_list, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
                                                              inherit_list, sizeof(inherit_list), nullptr,
                                                              nullptr) != 0;
    BOOL created = FALSE;
    DWORD create_error = 0;
    PROCESS_INFORMATION pi{};
    if (list_ok) {
        STARTUPINFOEXW si{};
        si.StartupInfo.cb = sizeof(si);
        si.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
        si.StartupInfo.hStdInput = nul_in;
        si.StartupInfo.hStdOutput = nul_out;
        si.StartupInfo.hStdError = nul_out;
        si.lpAttributeList = attr_list;
        std::wstring env = minimal_environment_block();
        created = CreateProcessW(nullptr, mutable_cmd.data(), nullptr, nullptr, TRUE,
                                 EXTENDED_STARTUPINFO_PRESENT | CREATE_SUSPENDED | CREATE_UNICODE_ENVIRONMENT |
                                     CREATE_NO_WINDOW,
                                 env.data(), config.cwd.c_str(), reinterpret_cast<LPSTARTUPINFOW>(&si), &pi);
        create_error = created ? 0 : GetLastError();
    } else {
        create_error = GetLastError();
    }
    if (attr_ok) DeleteProcThreadAttributeList(attr_list);
    CloseHandle(nul_in);
    CloseHandle(nul_out);
    if (!created) {
        terminate();
        return std::unexpected(err(failure_class::fatal, "CreateProcessW(pwsh) failed: Win32 error " +
                                                             std::to_string(create_error),
                                   "native_shell_session.open_failed"));
    }
    process_ = pi.hProcess;
    pid_ = pi.dwProcessId;

    // The job BEFORE the first instruction runs (CREATE_SUSPENDED).
    ResourceLimits limits;
    limits.memory_bytes = config.memory_bytes;
    limits.pids = config.max_processes;
    limits.cpu_ms = config.cpu_ms.value_or(0);  // best-effort in the kernel; the provider checks it too
    job_.emplace();
    auto made = job_->create(limits);
    auto assigned = made.has_value() ? job_->assign_process(pi.hProcess) : agentengine::result<void>{};
    if (!made.has_value() || !assigned.has_value()) {
        TerminateProcess(pi.hProcess, 1);
        CloseHandle(pi.hThread);
        auto e = !made.has_value() ? made.error() : assigned.error();
        terminate();
        return std::unexpected(e);
    }
    ResumeThread(pi.hThread);
    CloseHandle(pi.hThread);

    auto const until = std::chrono::steady_clock::now() + config.startup_deadline;
    while (!std::filesystem::exists(dir_ / "ready", ec)) {
        if (WaitForSingleObject(process_, 0) != WAIT_TIMEOUT || std::chrono::steady_clock::now() >= until) {
            terminate();
            return std::unexpected(err(failure_class::fatal, "pwsh did not start its shell loop",
                                       "native_shell_session.shell_start_failed"));
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    // The containment probe (ADR-209 §15): a process the shell started must be inside this job. Measured: the
    // Store (MSIX) pwsh 7.6's children are NOT -- every one of them escaped the job -- so a shell that fails this
    // is refused, whichever build it is.
    {
        auto ready = read_bounded(dir_ / "ready", 256);
        DWORD probe_pid = 0;
        if (ready.has_value()) {
            auto const sp = ready->find(' ');
            if (sp != std::string::npos) {
                unsigned long v = 0;
                auto [p, e] = std::from_chars(ready->data() + sp + 1, ready->data() + ready->size(), v);
                (void)p;
                if (e == std::errc{}) probe_pid = static_cast<DWORD>(v);
            }
        }
        BOOL in_job = FALSE;
        bool checked = false;
        if (probe_pid != 0) {
            HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_TERMINATE, FALSE, probe_pid);
            if (h != nullptr) {
                checked = IsProcessInJob(h, job_->native_handle(), &in_job) != FALSE;
                TerminateProcess(h, 0);
                CloseHandle(h);
            }
        }
        if (!checked || !in_job) {
            terminate();
            return std::unexpected(err(failure_class::policy,
                                       checked ? "this shell's child processes escape its Job Object (e.g. the Store/"
                                                 "MSIX pwsh build), so it cannot be contained; use a non-packaged pwsh "
                                                 "or Windows PowerShell"
                                               : "the shell's containment probe could not run (max_processes must be "
                                                 "at least 2: the shell itself plus one child)",
                                       "native_shell_session.job_not_enforced"));
        }
    }
    auto base = read_bounded(dir_ / "base", kMaxStatusBytes);
    auto base_env = base.has_value() ? native_shell_detail::parse_env_lines(*base) : std::nullopt;
    if (!base_env.has_value()) {
        terminate();
        return std::unexpected(err(failure_class::fatal, "pwsh's open-time environment could not be read",
                                   "native_shell_session.shell_start_failed"));
    }
    base_env_ = std::move(*base_env);
    live_ = true;

    if (snapshot != nullptr) {
        auto replayed = send(ps::pwsh_replay_script(*snapshot), std::chrono::milliseconds(30000), false);
        if (!replayed.has_value() || replayed->shell_lost) {
            terminate();
            return std::unexpected(err(failure_class::fatal, "the snapshot replay did not complete",
                                       "native_shell_session.replay_failed"));
        }
    }
    return {};
}

agentengine::result<LiveExecOutcome> NativePwshSession::exec(std::string const& command, std::chrono::milliseconds deadline) {
    if (!is_live()) {
        return std::unexpected(err(failure_class::contract, "no live native shell", "native_shell_session.not_live"));
    }
    if (command.find('\0') != std::string::npos) {
        return std::unexpected(err(failure_class::policy, "a command may not contain a NUL byte",
                                   "native_shell_session.nul_in_command"));
    }
    if (command.size() > kMaxCommandBytes) {
        return std::unexpected(err(failure_class::resource, "the command is larger than 256 KiB",
                                   "native_shell_session.command_too_large"));
    }
    return send(command, deadline, true);
}

agentengine::result<LiveExecOutcome> NativePwshSession::send(std::string const& command, std::chrono::milliseconds deadline,
                                                             bool with_snapshot) {
    std::string const nonce = random_hex(16);
    auto const c = dir_ / ("c." + nonce);
    auto const r = dir_ / ("r." + nonce);
    auto const o = dir_ / ("o." + nonce);
    auto const x = dir_ / ("x." + nonce);
    {
        std::ofstream out(c, std::ios::binary);
        out << command;
        if (!out) {
            return std::unexpected(err(failure_class::fatal, "could not stage the command",
                                       "native_shell_session.stage_failed"));
        }
    }
    { std::ofstream(r, std::ios::binary).flush(); }

    LiveExecOutcome out;
    auto const until = std::chrono::steady_clock::now() + deadline;
    std::error_code ec;
    bool done = false;
    for (;;) {
        if (std::filesystem::exists(x, ec)) {
            done = true;
            break;
        }
        if (WaitForSingleObject(process_, 0) != WAIT_TIMEOUT) break;  // `exit`, or the job hit a limit
        if (std::chrono::steady_clock::now() >= until) {
            out.timed_out = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    if (!done) {
        // §4.3 / §7: a timeout ends every process in the shell; so does the shell's own death. Either way the
        // whole job goes, so nothing the command started outlives it.
        kill_job();
        out.shell_lost = true;
    }
    std::uint64_t total = 0;
    if (auto text = read_bounded(o, output_cap_, &total); text.has_value()) {
        out.output = std::move(*text);
        if (out.output.starts_with("\xEF\xBB\xBF")) out.output.erase(0, 3);  // Windows PowerShell writes a BOM
        out.output_bytes = total;
        out.output_truncated = total > out.output.size();
    }
    if (done) {
        auto raw = read_bounded(x, kMaxStatusBytes);
        auto status = raw.has_value() ? native_shell_detail::parse_status_record(*raw) : std::nullopt;
        if (!status.has_value()) {
            kill_job();
            out.shell_lost = true;
        } else {
            out.exit_code = status->exit_code;
            if (with_snapshot) out.snapshot = ps::make_snapshot(std::move(status->cwd), status->env, base_env_);
        }
    }
    std::filesystem::remove(c, ec);
    std::filesystem::remove(r, ec);
    std::filesystem::remove(o, ec);
    std::filesystem::remove(x, ec);
    return out;
}

}  // namespace agentengine::native_process
