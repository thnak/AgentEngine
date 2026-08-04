# MicroVM isolation: is a Windows-hosted path viable?

Compiled: 2026-08-04. Grounds 008 §1's "No `microvm` profile" decision independent of the
now-dropped macOS claim (021 §7 OQ-1) — the question this record answers is narrower than "does
microVM isolation exist" (it does, and is production-grade on Linux): **could AgentEngine host
genuine Linux-kernel microVM isolation (Firecracker-class) on a Windows development/deployment
machine**, since Windows and Linux are now the only two target platforms.

## Firecracker: officially Linux-host-only, not a portability gap waiting to be closed

- Firecracker's own FAQ states plainly: *"Firecracker supports Linux host and guest operating
  systems."* This is a stated support boundary, not an unimplemented feature.
- Firecracker's `design.md` documents the VMM core built directly against KVM's `KVM_RUN` ioctl
  loop throughout, with no hypervisor-abstraction layer between the VMM and KVM. Porting to
  Windows Hypervisor Platform (WHP) would mean rewriting the entire hypervisor-interface layer, not
  retargeting a portable build — it would produce a new, unaudited VMM that happens to share
  Firecracker's name, losing the production track record that is the actual reason to want
  Firecracker in the first place.
- An unofficial `firecracker-win` fork exists on GitHub but has ~4 stars, 0 forks, and no tagged
  releases as of this compile date — not a maintained dependency by any reasonable bar.

**Conclusion:** no production-grade path exists to run real Firecracker on a Windows host today,
and the porting cost is a new hypervisor integration, not a build flag.

## Cloud Hypervisor's "Windows support" runs the opposite direction

Cloud Hypervisor's `docs/windows.md` documents Windows as a **guest** OS running on a **Linux +
KVM host** — not Cloud Hypervisor running natively on a Windows host. Search summaries describing
this as "Windows support" without qualification are misleading; the primary source disambiguates
it immediately. This does not help a Windows-hosted deployment.

## Hyperlight: real WHP support, but a materially smaller sandbox

- Hyperlight uses WHP (Windows) and KVM/mshv (Linux) as its hypervisor backends and is explicitly
  described by its own maintainers as **experimental, not production-grade**.
- Hyperlight's guest has no kernel, no OS, and no filesystem — it runs a single function, not a
  general-purpose interpreter. Hyperlight *Wasm* (the variant that runs WASM guests) inherits the
  `wasm` profile's existing "no native extensions" ceiling (008 §3), so it does not open a new
  capability class the `wasm` profile doesn't already offer.
- A newer Hyperlight+Nanvix integration (blog post, 2026-01-28) adds a POSIX compatibility layer,
  but the source does not confirm dynamic-loading or native-extension support, and reads as
  early-stage. Not evidence of a native-library-capable microVM today.

## What this means for 008 §1

The `microvm` profile was dropped in an earlier revision citing "no macOS backend" as the reason.
That framing is now superseded twice over: macOS is out of scope entirely (021 §7 OQ-1), and
independent of macOS, **there is also no production-grade path to run genuine microVM isolation on
Windows** — the remaining target platform besides Linux. The decision to not build/maintain a
`microvm` profile stands on firmer ground than the original rationale stated: it is not a gap that
narrows as platform support changes, it is that Firecracker-class isolation is architecturally
Linux+KVM-only, and a Windows-hosted reimplementation would be new, unaudited VMM code — exactly
the kind of second local isolation technology 008 §1 already declines to build. A workload that
needs that strength of boundary uses the `remote` profile against Linux/KVM infrastructure that
already has it, which is that section's existing fallback.

Sources: Firecracker `FAQ.md` and `docs/design.md` (github.com/firecracker-microvm/firecracker,
fetched 2026-08-04); `firecracker-win` fork repository (fetched 2026-08-04); Cloud Hypervisor
`docs/windows.md` (github.com/cloud-hypervisor/cloud-hypervisor, fetched 2026-08-04); Hyperlight
project docs and the Hyperlight+Nanvix blog post (fetched 2026-08-04).
