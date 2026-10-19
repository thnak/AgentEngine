#pragma once
// Shared harness for the Kata egress-policy acceptance tests (ADR-239; GitHub #194). Linux, root, a real
// containerd + Kata + CNI `ae-kata-net` deployment (same precondition as test_kata_backend_linux.cpp), gated
// behind AGENTENGINE_KATA_SANDBOX_TESTS like every other Kata test.
//
// The lab (no internet needed):
//   * a "world" network namespace `aepv-w` behind veth `aepvw0` (host side 198.51.100.1/24), holding the
//     sink/source servers on 198.51.100.2 (:9000 and :9001) and 198.51.100.3 (:9000). 198.51.100.0/24 is
//     TEST-NET-2: not in is_blocked_address()'s table and not a host address, so a real create() accepts
//     `198.51.100.2:9000:http` as an allowlist entry.
//   * the operator precondition for traffic between the Kata bridge and the world is a FORWARD accept; the
//     backend never opens it and this harness never edits iptables either: the runbook wrapper inserts two
//     `ae-prove-temp` rules and removes them in the same command. Without it the positive controls fail and
//     the test says so.
// Every negative check in these tests has a positive control: the same probe run against a hand-wired
// ControlSandbox (netns + cnitool + ctr run, NO policy, the pre-change behaviour) or against the same sandbox
// after one barrier is removed, proving the probe can detect the opposite.

#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "backends/kata/egress_policy.hpp"

namespace aepv {

namespace egress = agentengine::kata::egress;

inline std::atomic<int> g_failures{0};
inline std::mutex g_out_mutex;

inline void check(bool cond, std::string const& what) {
    std::lock_guard<std::mutex> const lock(g_out_mutex);
    if (!cond) {
        ++g_failures;
        std::fprintf(stderr, "FAIL: %s\n", what.c_str());
    } else {
        std::fprintf(stderr, "ok: %s\n", what.c_str());
    }
}

// One line per claim, grep-able: EVIDENCE|<claim>|<positive control>|<result>
inline void evidence(std::string const& claim, std::string const& control, std::string const& result) {
    std::lock_guard<std::mutex> const lock(g_out_mutex);
    std::fprintf(stderr, "EVIDENCE|%s|%s|%s\n", claim.c_str(), control.c_str(), result.c_str());
}

inline std::string shq(std::string const& s) {
    std::string out = "'";
    for (char c : s) {
        if (c == '\'') out += "'\\''";
        else out += c;
    }
    return out + "'";
}

struct Sh {
    int rc = -1;
    std::string out;
    [[nodiscard]] bool ok() const { return rc == 0; }
    [[nodiscard]] bool has(std::string const& needle) const { return out.find(needle) != std::string::npos; }
};

// stdout+stderr merged, bounded by coreutils `timeout`.
inline Sh sh(std::string const& cmd, int timeout_s = 120) {
    std::string const full = "timeout " + std::to_string(timeout_s) + " /bin/sh -c " + shq(cmd) + " 2>&1";
    Sh r;
    FILE* p = ::popen(full.c_str(), "r");
    if (p == nullptr) return r;
    std::array<char, 4096> buf{};
    std::size_t n = 0;
    while ((n = std::fread(buf.data(), 1, buf.size(), p)) > 0) r.out.append(buf.data(), n);
    int const status = ::pclose(p);
    r.rc = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    return r;
}

inline std::string trim(std::string s) {
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' ')) s.pop_back();
    std::size_t i = 0;
    while (i < s.size() && (s[i] == ' ' || s[i] == '\n')) ++i;
    return s.substr(i);
}

inline long long first_int_after(std::string const& text, std::string const& key) {
    std::size_t at = text.find(key);
    if (at == std::string::npos) return -1;
    at += key.size();
    while (at < text.size() && (text[at] < '0' || text[at] > '9')) ++at;
    if (at >= text.size()) return -1;
    return std::atoll(text.c_str() + at);
}

// argv runner for egress::HostEgress (real processes, stderr kept separate from stdout)
inline egress::CommandRunner real_runner() {
    return [](std::vector<std::string> const& argv) -> agentengine::result<egress::CommandOutcome> {
        std::string cmd;
        for (auto const& a : argv) cmd += (cmd.empty() ? "" : " ") + shq(a);
        static std::atomic<int> seq{0};
        std::string const errf = "/tmp/aepv-err." + std::to_string(::getpid()) + "." + std::to_string(++seq);
        FILE* p = ::popen(("timeout 60 " + cmd + " 2>" + shq(errf)).c_str(), "r");
        if (p == nullptr) {
            return std::unexpected(agentengine::error{agentengine::failure_class::fatal, "popen failed", "aepv.popen"});
        }
        egress::CommandOutcome o;
        std::array<char, 4096> buf{};
        std::size_t n = 0;
        while ((n = std::fread(buf.data(), 1, buf.size(), p)) > 0) o.stdout_text.append(buf.data(), n);
        int const status = ::pclose(p);
        o.exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
        std::ifstream ef(errf);
        std::stringstream ss;
        ss << ef.rdbuf();
        o.stderr_text = ss.str();
        ::unlink(errf.c_str());
        return o;
    };
}

// ---- guest-side python helpers ------------------------------------------------------------------------------

// Raw frame builders for a hostile guest (the backend grants CAP_NET_RAW, which is all of this needs).
inline constexpr char kGuestLib[] = R"PY(
import socket, struct, sys, time
def cs(b):
    if len(b) % 2: b += b'\0'
    s = sum(struct.unpack('!%dH' % (len(b) // 2), b))
    while s >> 16: s = (s & 0xffff) + (s >> 16)
    return (~s) & 0xffff
def mb(m): return bytes(int(x, 16) for x in m.split(':'))
def ipb(a): return socket.inet_aton(a)
def eth(dst, src, et, pl): return mb(dst) + mb(src) + struct.pack('!H', et) + pl
def ip4(src, dst, proto, pl, ident=1, mf=0, off=0, ttl=64):
    fl = (mf << 13) | off
    h = struct.pack('!BBHHHBBH4s4s', 0x45, 0, 20 + len(pl), ident, fl, ttl, proto, 0, ipb(src), ipb(dst))
    h = h[:10] + struct.pack('!H', cs(h)) + h[12:]
    return h + pl
def tcp(src, dst, sp, dp, data=b'', flags=0x02, seq=1):
    th = struct.pack('!HHIIBBHHH', sp, dp, seq, 0, 5 << 4, flags, 65535, 0, 0)
    ph = ipb(src) + ipb(dst) + struct.pack('!BBH', 0, 6, len(th) + len(data))
    th = th[:16] + struct.pack('!H', cs(ph + th + data)) + th[18:]
    return th + data
def arp(op, sha, sip, tha, tip):
    return struct.pack('!HHBBH', 1, 0x0800, 6, 4, op) + mb(sha) + ipb(sip) + mb(tha) + ipb(tip)
RAW = socket.socket(socket.AF_PACKET, socket.SOCK_RAW)
RAW.bind(('eth0', 0))
def snd(f, n=1):
    for _ in range(n): RAW.send(f)
)PY";

// Concurrent TCP connect probes: argument list of "label ip port"; prints "label CONNECTED <reply>" or
// "label FAILED <exception>". 4 s timeout per probe, all probes in parallel threads.
inline std::string probe_script(std::vector<std::array<std::string, 3>> const& probes, std::string const& send = "HELLO\\n") {
    std::ostringstream o;
    o << "import socket, threading\nP = [";
    for (auto const& p : probes) o << "('" << p[0] << "','" << p[1] << "'," << p[2] << "),";
    o << "]\nR = {}\n"
         "def go(l, ip, port):\n"
         "    af = socket.AF_INET6 if ':' in ip else socket.AF_INET\n"
         "    s = socket.socket(af, socket.SOCK_STREAM); s.settimeout(4)\n"
         "    try:\n"
         "        s.connect((ip, port) if af == socket.AF_INET else (ip, port, 0, int(open('/sys/class/net/eth0/ifindex').read())))\n"
         "        s.sendall(b'" << send << "'); s.settimeout(3)\n"
         "        try: d = s.recv(64)\n"
         "        except Exception: d = b''\n"
         "        R[l] = 'CONNECTED ' + d.decode('latin1').strip()\n"
         "    except Exception as e:\n"
         "        R[l] = 'FAILED ' + type(e).__name__\n"
         "T = [threading.Thread(target=go, args=p) for p in P]\n"
         "[t.start() for t in T]; [t.join() for t in T]\n"
         "for l, _, _ in P: print(l, R.get(l))\n";
    return o.str();
}

inline std::string parse_probe(std::string const& output, std::string const& label) {
    std::istringstream in(output);
    std::string line;
    std::string const prefix = label + " ";
    while (std::getline(in, line)) {
        if (line.rfind(prefix, 0) == 0) return line.substr(prefix.size());
    }
    return "NO-OUTPUT";
}

// ---- hand-wired control sandbox: the pre-change world (no policy, nothing on the port) ---------------------

inline constexpr char kRuntime[] = "io.containerd.kata-clh.v2";
inline constexpr char kPythonImage[] = "docker.io/library/python:3.12-alpine";
inline constexpr char kCniEnv[] = "CNI_PATH=/opt/cni/bin NETCONFPATH=/etc/cni/net.d";

inline std::atomic<unsigned> g_exec_seq{0};

// `ctr tasks exec` into any container by id (also used for backend-created sandboxes, bypassing the backend's
// own exec-time verification so a test can probe a guest whose policy it just broke on purpose).
inline Sh guest_exec(std::string const& id, std::string const& script, int timeout_s = 90) {
    return sh("ctr tasks exec --exec-id x" + std::to_string(++g_exec_seq) + " " + id + " /bin/sh -c " + shq(script), timeout_s);
}
inline Sh guest_py(std::string const& id, std::string const& script, int timeout_s = 90) {
    return guest_exec(id, "python3 -c " + shq(script), timeout_s);
}

struct GuestFacts {
    std::string mac;
    std::string ip;
};
inline GuestFacts guest_facts(std::string const& id) {
    GuestFacts f;
    f.mac = trim(guest_exec(id, "cat /sys/class/net/eth0/address").out);
    Sh a = guest_exec(id, "ip -4 -o addr show dev eth0");
    std::size_t at = a.out.find("inet ");
    if (at != std::string::npos) {
        std::size_t end = a.out.find('/', at);
        f.ip = a.out.substr(at + 5, end - at - 5);
    }
    return f;
}

struct ControlSandbox {
    std::string id;
    std::string veth;
    std::string mac;
    std::string ip;
    bool up = false;

    bool create(std::string const& name, std::string const& image = kPythonImage) {
        id = name;
        if (!sh("ip netns add " + id).ok()) return false;
        Sh cni = sh(std::string(kCniEnv) + " cnitool add ae-kata-net /var/run/netns/" + id);
        if (!cni.ok()) {
            std::fprintf(stderr, "control: cnitool add failed: %s\n", cni.out.c_str());
            destroy();
            return false;
        }
        auto att = egress::parse_cni_result(cni.out);
        if (!att) {
            destroy();
            return false;
        }
        mac = att->guest_mac;
        ip = egress::ipv4_dotted(att->guest_ip);
        egress::HostEgress host(real_runner());
        auto port = host.locate_port(att->host_interface_candidates);
        if (!port) {
            destroy();
            return false;
        }
        veth = port->first;
        up = true;
        Sh run = sh("ctr run -d --runtime " + std::string(kRuntime) + " --with-ns network:/var/run/netns/" + id + " " + image + " " + id + " sleep 3600", 120);
        if (!run.ok()) {
            std::fprintf(stderr, "control: ctr run failed: %s\n", run.out.c_str());
            destroy();
            return false;
        }
        return true;
    }

    void destroy() {
        (void)sh("ctr task kill --signal SIGKILL " + id);
        (void)sh("ctr task rm -f " + id);
        (void)sh("ctr container rm " + id);
        (void)sh(std::string(kCniEnv) + " cnitool del ae-kata-net /var/run/netns/" + id);
        (void)sh("ip netns delete " + id);
        up = false;
    }
};

// ---- the world lab --------------------------------------------------------------------------------------------

inline constexpr char kSinkPy[] = R"PY(
import socket, threading, time
LOG = '/tmp/aepv-sink.log'
L = threading.Lock()
def log(s):
    with L:
        open(LOG, 'a').write(s + '\n')
def handle(c, addr, lport):
    try:
        c.settimeout(8)
        buf = b''
        while b'\n' not in buf:
            d = c.recv(1024)
            if not d: return
            buf += d
        line, rest = buf.split(b'\n', 1)
        cmd = line.split()
        if cmd[0] == b'HELLO':
            c.sendall(('WORLD-OK %s:%d\n' % (c.getsockname()[0], lport)).encode())
        elif cmd[0] == b'DOWN':
            n = int(cmd[1]); sent = 0; chunk = b'x' * 65536
            while sent < n:
                m = min(len(chunk), n - sent); c.sendall(chunk[:m]); sent += m
            c.shutdown(socket.SHUT_WR); time.sleep(0.3)
        elif cmd[0] == b'UP':
            tag = cmd[1].decode() if len(cmd) > 1 else 'x'
            total = len(rest)
            while True:
                try: d = c.recv(65536)
                except socket.timeout: break
                if not d: break
                total += len(d)
            log('UP %s %d' % (tag, total))
            try: c.sendall(('GOT %d\n' % total).encode())
            except Exception: pass
    except Exception:
        pass
    finally:
        try: c.close()
        except Exception: pass
def serve(port):
    s = socket.socket(); s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1); s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEPORT, 1)
    s.bind(('0.0.0.0', port)); s.listen(256)
    while True:
        c, a = s.accept()
        threading.Thread(target=handle, args=(c, a, port), daemon=True).start()
for p in (9000, 9001):
    threading.Thread(target=serve, args=(p,), daemon=True).start()
while True: time.sleep(3600)
)PY";

inline constexpr char kSniffPy[] = R"PY(
import socket, sys, time
# usage: sniff.py <iface> <seconds> <marker>... ; prints "<marker> <count of frames containing it>" per marker
s = socket.socket(socket.AF_PACKET, socket.SOCK_RAW, socket.htons(3))
s.bind((sys.argv[1], 0)); s.settimeout(0.5)
end = time.time() + float(sys.argv[2]); ms = [m.encode() for m in sys.argv[3:]]; n = {m: 0 for m in ms}
while time.time() < end:
    try: d = s.recv(65535)
    except socket.timeout: continue
    for m in ms:
        if m in d: n[m] += 1
for m in ms: print(m.decode(), n[m])
print('DONE')
)PY";

struct Lab {
    static constexpr char kWorldNs[] = "aepv-w";
    static constexpr char kWorldVeth[] = "aepvw0";
    static constexpr char kWorldIp[] = "198.51.100.2";
    static constexpr char kWorldIp2[] = "198.51.100.3";
    static constexpr char kBridge[] = "aekata0";
    pid_t sink_pid = -1;

    static void write_file(std::string const& path, char const* content) {
        std::ofstream f(path, std::ios::trunc);
        f << content;
    }

    bool up() {
        down();
        write_file("/tmp/aepv-sink.py", kSinkPy);
        write_file("/tmp/aepv-sniff.py", kSniffPy);
        (void)sh("rm -f /tmp/aepv-sink.log");
        char const* cmds[] = {
            "ip netns add aepv-w",
            "ip link add aepvw0 type veth peer name aepvw1",
            "ip link set aepvw1 netns aepv-w",
            "ip addr add 198.51.100.1/24 dev aepvw0",
            "ip link set aepvw0 up",
            "ip netns exec aepv-w ip link set lo up",
            "ip netns exec aepv-w ip addr add 198.51.100.2/24 dev aepvw1",
            "ip netns exec aepv-w ip addr add 198.51.100.3/24 dev aepvw1",
            "ip netns exec aepv-w ip link set aepvw1 up",
            "ip netns exec aepv-w ip route add 10.88.77.0/24 via 198.51.100.1",
            "ip netns exec aepv-w ip route add 10.99.0.0/16 via 198.51.100.1",
        };
        for (char const* c : cmds) {
            Sh r = sh(c);
            if (!r.ok()) {
                std::fprintf(stderr, "lab: '%s' failed: %s\n", c, r.out.c_str());
                return false;
            }
        }
        pid_t pid = ::fork();
        if (pid == 0) {
            ::setsid();
            ::execlp("ip", "ip", "netns", "exec", "aepv-w", "python3", "/tmp/aepv-sink.py", static_cast<char*>(nullptr));
            ::_exit(127);
        }
        sink_pid = pid;
        for (int i = 0; i < 50; ++i) {
            if (sh("ip netns exec aepv-w python3 -c \"import socket; s=socket.create_connection(('198.51.100.2',9000),2); s.sendall(b'HELLO\\n'); print(s.recv(64))\"").has("WORLD-OK")) {
                return true;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
        }
        return false;
    }

    void down() {
        if (sink_pid > 0) {
            ::kill(-sink_pid, SIGKILL);
            ::kill(sink_pid, SIGKILL);
            int st = 0;
            ::waitpid(sink_pid, &st, 0);
            sink_pid = -1;
        }
        (void)sh("ip netns delete aepv-w");
        (void)sh("ip link delete aepvw0");
    }

    // The runbook's temporary FORWARD accepts (comment ae-prove-temp) must exist for any traffic between the Kata
    // bridge and the world; this reports whether they do, without touching iptables.
    static bool forward_precondition_met() { return sh("iptables -S FORWARD | grep -c ae-prove-temp").out.find('0') != 0; }

    // Starts a host-side sniffer on `iface` for `markers`; collect() waits for it and returns marker -> count.
    struct Sniffer {
        std::string out_file;
        std::string iface;
    };
    static Sniffer sniff_start(std::string const& iface, int seconds, std::vector<std::string> const& markers) {
        static std::atomic<int> n{0};
        Sniffer s{"/tmp/aepv-sniff." + std::to_string(::getpid()) + "." + std::to_string(++n), iface};
        std::string m;
        for (auto const& x : markers) m += " " + x;
        (void)sh("(python3 /tmp/aepv-sniff.py " + iface + " " + std::to_string(seconds) + m + " > " + s.out_file + " 2>&1 &) ; sleep 1");
        return s;
    }
    static std::map<std::string, int> sniff_collect(Sniffer const& s) {
        std::map<std::string, int> out;
        for (int i = 0; i < 120; ++i) {
            std::ifstream f(s.out_file);
            std::stringstream ss;
            ss << f.rdbuf();
            std::string text = ss.str();
            if (text.find("DONE") != std::string::npos) {
                std::istringstream in(text);
                std::string k;
                int v = 0;
                while (in >> k >> v) out[k] = v;
                ::unlink(s.out_file.c_str());
                return out;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
        }
        return out;
    }
};

inline std::string host_mac(std::string const& dev) { return trim(sh("cat /sys/class/net/" + dev + "/address").out); }

inline std::string fdb_dev_of(std::string const& mac, std::string const& bridge = "aekata0") {
    Sh r = sh("bridge fdb get " + mac + " br " + bridge);
    std::size_t at = r.out.find(" dev ");
    if (at == std::string::npos) return "NONE";
    std::size_t end = r.out.find(' ', at + 5);
    return r.out.substr(at + 5, end - at - 5);
}

// Names of ports of the Kata bridge, space separated, for before/after leak comparisons.
inline std::string bridge_ports() { return trim(sh("ls /sys/class/net/aekata0/brif 2>/dev/null | sort | tr '\\n' ' '").out); }
inline std::string netns_list() { return trim(sh("ls /var/run/netns 2>/dev/null | grep -v '^cnitest-' | sort | tr '\\n' ' '").out); }
inline std::string ae_tables() { return trim(sh("nft list tables | grep -c ' ae_k$'").out); }
inline std::string ctr_containers() { return trim(sh("ctr container ls -q | sort | tr '\\n' ' '").out); }

}  // namespace aepv
