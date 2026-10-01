// Implements decisions/ADR-209-persistent-shell-sessions.md §4.2, §4.3, §7 (revision 5 §15): the bodies of
// include/agentengine/sandbox/docker_persistent_shell_surface.hpp. The design and the rules each body
// implements are documented at the declarations there.

#include "agentengine/sandbox/docker_persistent_shell_surface.hpp"

#include <algorithm>
#include <charconv>
#include <cstdio>
#include <fstream>
#include <random>
#include <string_view>
#include <thread>

#ifndef _WIN32
#include <sys/stat.h>
#endif

namespace agentengine {

namespace docker_persistent_shell_detail {

namespace {

[[nodiscard]] bool parse_u64(std::string_view s, std::uint64_t& out) {
    if (s.empty()) return false;
    auto const* first = s.data();
    auto const* last = s.data() + s.size();
    auto [ptr, ec] = std::from_chars(first, last, out);
    return ec == std::errc{} && ptr == last;
}

}  // namespace

ClientReply parse_client_reply(std::string_view raw, bool with_base) {
    ClientReply reply;
    std::size_t const nl = raw.find('\n');
    std::string_view const header = nl == std::string_view::npos ? raw : raw.substr(0, nl);
    if (header.rfind("AE1 ", 0) != 0 || nl == std::string_view::npos) {
        std::string_view marker = header;
        while (!marker.empty() && (marker.back() == '\r' || marker.back() == ' ')) marker.remove_suffix(1);
        reply.marker = std::string(marker.substr(0, 128));
        return reply;
    }
    // "AE1 <xlen> <olen> <blen>" -- busybox `wc -c` may pad with spaces, so split on runs of spaces.
    std::vector<std::string_view> fields;
    std::string_view rest = header.substr(4);
    while (!rest.empty()) {
        std::size_t const start = rest.find_first_not_of(' ');
        if (start == std::string_view::npos) break;
        rest.remove_prefix(start);
        std::size_t const end = rest.find(' ');
        fields.push_back(rest.substr(0, end));
        if (end == std::string_view::npos) break;
        rest.remove_prefix(end);
    }
    std::uint64_t xlen = 0, olen = 0, blen = 0;
    if (fields.size() != 3 || !parse_u64(fields[0], xlen) || !parse_u64(fields[1], olen) ||
        !parse_u64(fields[2], blen)) {
        reply.marker = "malformed header";
        return reply;
    }
    std::string_view body = raw.substr(nl + 1);
    if (xlen > body.size() || (with_base && blen > body.size() - xlen)) {
        reply.marker = "truncated reply";
        return reply;
    }
    std::string_view const record = body.substr(0, static_cast<std::size_t>(xlen));
    body.remove_prefix(static_cast<std::size_t>(xlen));
    std::string_view base;
    if (with_base) {
        base = body.substr(0, static_cast<std::size_t>(blen));
        body.remove_prefix(static_cast<std::size_t>(blen));
    }
    // record: <rc>\0<pwd output>\0<environ block>
    std::size_t const z1 = record.find('\0');
    if (z1 == std::string_view::npos) {
        reply.marker = "malformed status record";
        return reply;
    }
    std::uint64_t rc = 0;
    if (!parse_u64(record.substr(0, z1), rc) || rc > 255) {
        reply.marker = "malformed exit status";
        return reply;
    }
    std::size_t const z2 = record.find('\0', z1 + 1);
    if (z2 == std::string_view::npos) {
        reply.marker = "malformed status record";
        return reply;
    }
    std::string_view cwd = record.substr(z1 + 1, z2 - z1 - 1);
    if (!cwd.empty() && cwd.back() == '\n') cwd.remove_suffix(1);
    reply.ok = true;
    reply.exit_code = static_cast<int>(rc);
    reply.cwd = std::string(cwd);
    reply.env = persistent_shell::parse_environ_block(record.substr(z2 + 1));
    if (with_base) reply.base_env = persistent_shell::parse_environ_block(base);
    reply.output = std::string(body);
    reply.output_bytes = olen;
    return reply;
}

}  // namespace docker_persistent_shell_detail

namespace {

namespace dps = docker_persistent_shell_detail;

[[nodiscard]] std::string random_hex(std::size_t chars) {
    std::random_device rd;
    std::string out;
    while (out.size() < chars) {
        char chunk[9];
        std::snprintf(chunk, sizeof(chunk), "%08x", static_cast<unsigned>(rd()));
        out += chunk;
    }
    out.resize(chars);
    return out;
}

// The command goes to the container through `docker exec -i`'s stdin, from a file in a PRIVATE directory
// (ADR-174 §3's seed-archive pattern: an unpredictable name inside a directory created 0700 in one step).
// Removed on every path by the guard.
struct StagedCommand {
    std::filesystem::path dir;
    std::filesystem::path file;
    StagedCommand() = default;
    StagedCommand(StagedCommand const&) = delete;
    StagedCommand& operator=(StagedCommand const&) = delete;
    ~StagedCommand() {
        if (dir.empty()) return;
        std::error_code ignored;
        std::filesystem::remove_all(dir, ignored);
    }
};

[[nodiscard]] agentengine::result<void> stage_command(std::string const& command, StagedCommand& staged) {
    std::error_code ec;
    std::filesystem::path const temp_root = std::filesystem::temp_directory_path(ec);
    if (ec) {
        return std::unexpected(agentengine::error{agentengine::failure_class::fatal, "no temp directory",
                                                  "persistent_shell.stage_failed"});
    }
    std::filesystem::path const dir = temp_root / ("ae_live_" + random_hex(32));
#ifdef _WIN32
    if (!std::filesystem::create_directory(dir, ec) || ec) {
#else
    if (::mkdir(dir.c_str(), 0700) != 0) {
#endif
        return std::unexpected(agentengine::error{agentengine::failure_class::fatal,
                                                  "cannot create a private directory to stage the command",
                                                  "persistent_shell.stage_failed"});
    }
    staged.dir = dir;
    staged.file = dir / "cmd";
    std::ofstream out(staged.file, std::ios::binary | std::ios::trunc);
    if (!out) {
        return std::unexpected(agentengine::error{agentengine::failure_class::fatal, "cannot open the staged command",
                                                  "persistent_shell.stage_failed"});
    }
    out.write(command.data(), static_cast<std::streamsize>(command.size()));
    out.flush();
    if (!out) {
        return std::unexpected(agentengine::error{agentengine::failure_class::fatal, "cannot write the staged command",
                                                  "persistent_shell.stage_failed"});
    }
    return {};
}

[[nodiscard]] int deadline_seconds(std::chrono::milliseconds deadline) {
    auto const ms = std::max<std::int64_t>(deadline.count(), 1);
    auto const s = (ms + 999) / 1000;
    return static_cast<int>(std::min<std::int64_t>(s, 24 * 3600));
}

}  // namespace

DockerPersistentShellSurface::DockerPersistentShellSurface(DockerPersistentShellSurface&& other) noexcept
    : image_(std::move(other.image_)), isolation_(other.isolation_), output_cap_(other.output_cap_),
      docker_(std::move(other.docker_)), instance_(std::move(other.instance_)), live_(other.live_),
      base_env_(std::move(other.base_env_)), resolved_digest_(std::move(other.resolved_digest_)),
      resolved_kind_(other.resolved_kind_), argv_log_(std::move(other.argv_log_)) {
    other.instance_.reset();
    other.live_ = false;
    other.resolved_digest_.clear();
    other.resolved_kind_ = agentengine::ImageDigestKind::unknown;
}

DockerPersistentShellSurface& DockerPersistentShellSurface::operator=(DockerPersistentShellSurface&& other) noexcept {
    // Swap, like DockerExecutionSurface: `other`'s destructor then releases what `this` held.
    if (this != &other) {
        image_.swap(other.image_);
        std::swap(isolation_, other.isolation_);
        std::swap(output_cap_, other.output_cap_);
        std::swap(docker_, other.docker_);
        instance_.swap(other.instance_);
        std::swap(live_, other.live_);
        base_env_.swap(other.base_env_);
        resolved_digest_.swap(other.resolved_digest_);
        std::swap(resolved_kind_, other.resolved_kind_);
        argv_log_.swap(other.argv_log_);
    }
    return *this;
}

SurfaceRunOutcome DockerPersistentShellSurface::spawn(std::vector<std::string> argv, int timeout_seconds,
                                                      std::size_t cap, std::filesystem::path const* stdin_file,
                                                      bool* timed_out) {
    argv_log_.push_back(argv);
    while (argv_log_.size() > 64) argv_log_.pop_front();
    return docker_cli_detail::run_argv(argv, timeout_seconds, cap, stdin_file, timed_out);
}

bool DockerPersistentShellSurface::only_init_alive() {
    // `docker top <id> -o pid,stat` runs `ps` on the Docker host against the container's processes: nothing in
    // the container is executed. A `SIGKILL`ed process is a zombie (`Z`) until the keeper reaps it on its next
    // tick, and a zombie runs nothing, so only non-zombies count. A few tries cover the kill still landing.
    for (int attempt = 0; attempt < 5; ++attempt) {
        auto top = spawn({"docker", "top", instance_->container_id, "-o", "pid,stat"},
                         docker_cli_detail::kProcessTimeoutSeconds, 1u << 20);
        if (top.exit_code != 0) return false;
        int alive = 0;
        bool header = true;
        std::size_t pos = 0;
        std::string const& text = top.stdout_text;
        while (pos < text.size()) {
            std::size_t const eol = text.find('\n', pos);
            std::string_view line(text.data() + pos, (eol == std::string::npos ? text.size() : eol) - pos);
            pos = eol == std::string::npos ? text.size() : eol + 1;
            if (line.find_first_not_of(" \t\r") == std::string_view::npos) continue;
            if (header) {
                header = false;
                continue;
            }
            std::size_t const sp = line.find_first_of(" \t", line.find_first_not_of(" \t"));
            std::string_view const stat =
                sp == std::string_view::npos ? std::string_view{} : line.substr(line.find_first_not_of(" \t", sp));
            if (stat.empty() || stat.front() != 'Z') ++alive;
        }
        if (header) return false;  // no header: not output we understand
        if (alive <= 1) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }
    return false;
}

void DockerPersistentShellSurface::forget_container() noexcept {
    instance_.reset();
    live_ = false;
    base_env_.clear();
}

agentengine::result<dps::ClientReply> DockerPersistentShellSurface::send(std::string const& command,
                                                                         std::chrono::milliseconds deadline,
                                                                         bool with_base, bool* timed_out,
                                                                         std::string* nonce_out) {
    StagedCommand staged;
    if (auto ok = stage_command(command, staged); !ok.has_value()) return std::unexpected(ok.error());
    std::string const nonce = random_hex(32);
    if (nonce_out != nullptr) *nonce_out = nonce;
    // The host-side cap only has to hold the header, the status record (bounded by the environment the
    // kernel lets a process carry) and the already-capped output.
    std::size_t const host_cap = output_cap_ + (4u << 20);
    auto r = spawn({"docker", "exec", "-i", instance_->container_id, "sh", "-c", std::string(dps::kClientScript), "ae",
                    nonce, std::to_string(output_cap_), with_base ? "1" : "0"},
                   deadline_seconds(deadline), host_cap, &staged.file, timed_out);
    return dps::parse_client_reply(r.stdout_text, with_base);
}

agentengine::result<void> DockerPersistentShellSurface::open(std::filesystem::path const& host_dir,
                                                            ShellSnapshot const* snapshot) {
    live_ = false;
    // §4.2: always a FRESH container. A failed destroy keeps the handle (a retry targets the same one) and
    // fails the open rather than starting a second container beside a leaked first.
    if (instance_) {
        auto destroyed = docker_.destroy(*instance_);
        if (!destroyed.has_value()) return std::unexpected(destroyed.error());
        forget_container();
    }
    auto inst = docker_.create(image_, isolation_, std::string(dps::kKeeperScript));
    if (!inst.has_value()) return std::unexpected(inst.error());
    instance_ = *inst;

    resolved_digest_ = docker_.resolve_image_digest(*instance_);
    resolved_kind_ = resolved_digest_.empty() ? agentengine::ImageDigestKind::unknown
                                              : docker_.resolve_image_digest_kind(resolved_digest_);

    auto const fail = [this](agentengine::error e) -> agentengine::result<void> {
        (void)close();
        return std::unexpected(std::move(e));
    };

    std::error_code exists_ec;
    bool const seed_present = std::filesystem::exists(host_dir, exists_ec);
    if (exists_ec) {
        return fail(agentengine::error{agentengine::failure_class::fatal,
                                       "cannot determine whether there is anything to seed: " + exists_ec.message(),
                                       "persistent_shell.seed_stat_failed", exists_ec.value()});
    }
    if (seed_present) {
        auto seeded = docker_.seed_tree_as_root(*instance_, host_dir, "/workspace");
        if (!seeded.has_value()) return fail(seeded.error());
    }

    // `-w /workspace` is a constant: the snapshot's cwd is replayed INSIDE the shell, never here (C14).
    auto started = spawn({"docker", "exec", "-d", "-w", "/workspace", instance_->container_id, "sh", "-c",
                          std::string(dps::kServerScript)},
                         docker_cli_detail::kProcessTimeoutSeconds, docker_cli_detail::kOutputSafetyCapBytes);
    if (started.exit_code != 0) {
        return fail(agentengine::error{agentengine::failure_class::fatal,
                                       "could not start the live shell: " + started.stdout_text,
                                       "persistent_shell.shell_start_failed"});
    }

    live_ = true;
    std::string const replay = snapshot != nullptr ? persistent_shell::sh_replay_script(*snapshot) : std::string(":\n");
    bool timed_out = false;
    auto reply = send(replay, std::chrono::seconds(docker_cli_detail::kProcessTimeoutSeconds), true, &timed_out,
                      nullptr);
    if (!reply.has_value()) return fail(reply.error());
    if (timed_out || !reply->ok) {
        return fail(agentengine::error{
            agentengine::failure_class::fatal,
            "the live shell did not come up (" + (timed_out ? std::string("timed out") : reply->marker) +
                "); the image needs a POSIX sh with `read -t`, mkfifo, cat, mv, head, wc and sleep",
            "persistent_shell.open_failed"});
    }
    base_env_ = std::move(reply->base_env);
    return {};
}

agentengine::result<LiveExecOutcome> DockerPersistentShellSurface::exec(std::string const& command,
                                                                       std::chrono::milliseconds deadline) {
    if (!is_live()) {
        return std::unexpected(agentengine::error{agentengine::failure_class::contract, "no live shell",
                                                  "persistent_shell.not_live"});
    }
    if (command.find('\0') != std::string::npos) {
        return std::unexpected(agentengine::error{agentengine::failure_class::policy,
                                                  "a command must not contain a NUL byte",
                                                  "persistent_shell.nul_in_command"});
    }
    if (command.size() > kMaxCommandBytes) {
        return std::unexpected(agentengine::error{
            agentengine::failure_class::policy,
            "a command must not exceed " + std::to_string(kMaxCommandBytes) + " bytes",
            "persistent_shell.command_too_large"});
    }

    bool timed_out = false;
    std::string nonce;
    auto reply = send(command, deadline, false, &timed_out, &nonce);
    if (!reply.has_value()) return std::unexpected(reply.error());  // staging failed: nothing ran

    LiveExecOutcome out;
    if (timed_out) {
        // §7: stop everything but PID 1 from inside. The shell is gone either way; the workspace stays for
        // the caller to drain unless even this exec cannot start, in which case the container goes.
        out.timed_out = true;
        out.shell_lost = true;
        live_ = false;
        auto killed = spawn({"docker", "exec", instance_->container_id, "sh", "-c", "kill -KILL -1; exit 0"},
                            docker_cli_detail::kProcessTimeoutSeconds, 4096);
        // ADR-209 §15.5 M2: that kill ran the CONTAINER's `sh`, which the command (root in its own container)
        // can have replaced, so its exit status proves nothing. The host checks instead: `docker top` lists the
        // container's processes from outside it, and anything alive besides PID 1 means the kill did not take,
        // so the container goes.
        if (killed.exit_code == 0 && !only_init_alive()) killed.exit_code = -1;
        if (killed.exit_code == 0) {
            auto partial = spawn({"docker", "exec", instance_->container_id, "sh", "-c",
                                  std::string(dps::kPartialOutputScript), "ae", nonce, std::to_string(output_cap_)},
                                 docker_cli_detail::kProcessTimeoutSeconds, output_cap_ + 4096);
            if (partial.exit_code == 0) {
                out.output = std::move(partial.stdout_text);
                if (out.output.size() > output_cap_) out.output.resize(output_cap_);
                out.output_bytes = out.output.size();
            }
        } else {
            out.container_lost = true;
            auto removed = docker_.destroy(*instance_);
            if (removed.has_value()) forget_container();
        }
        return out;
    }
    if (!reply->ok) {
        // §4.3: `exit`, the command's own `kill -1`, or a corrupted reply -- the shell can no longer be
        // trusted to be where the last record said, so it is lost and the next command re-opens.
        out.shell_lost = true;
        live_ = false;
        out.output = "[live shell lost: " + reply->marker + "]";
        return out;
    }
    out.exit_code = reply->exit_code;
    out.output_bytes = reply->output_bytes;
    out.output = std::move(reply->output);
    if (out.output.size() > output_cap_) out.output.resize(output_cap_);
    out.output_truncated = out.output_bytes > out.output.size();
    out.snapshot = persistent_shell::make_snapshot(std::move(reply->cwd), reply->env, base_env_);
    return out;
}

agentengine::result<void> DockerPersistentShellSurface::drain_to(std::filesystem::path const& host_dir) {
    if (!instance_) {
        return std::unexpected(agentengine::error{agentengine::failure_class::contract,
                                                  "no container to drain", "persistent_shell.no_container"});
    }
    std::error_code mkdir_ec;
    std::filesystem::create_directories(host_dir, mkdir_ec);
    if (mkdir_ec) {
        return std::unexpected(agentengine::error{
            agentengine::failure_class::fatal, "cannot create the host directory to drain into: " + mkdir_ec.message(),
            "persistent_shell.host_dir_create_failed", mkdir_ec.value()});
    }
    // `docker cp` only adds: empty host_dir first (issue #143).
    auto cleared = clear_directory_contents(host_dir, "persistent_shell.drain_clear_failed");
    if (!cleared.has_value()) return std::unexpected(cleared.error());
    auto copied = docker_.copy_from_container(*instance_, "/workspace/.", host_dir);
    if (!copied.has_value()) return std::unexpected(copied.error());
    return {};
}

agentengine::result<void> DockerPersistentShellSurface::close() {
    live_ = false;
    if (!instance_) return {};
    auto destroyed = docker_.destroy(*instance_);
    if (!destroyed.has_value()) return std::unexpected(destroyed.error());  // handle kept for a retry
    forget_container();
    return {};
}

}  // namespace agentengine
