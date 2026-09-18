# What a Docker image ID actually digests — and why the `config` kind is unreachable here

**Date:** 2026-09-18
**Why:** ADR-176 §13 rests on a claim about what `docker image inspect`'s image ID *is* under two
different image stores. ADR-176 has already had to retract one claim of exactly this shape (§3b asserted
flatly that the value is a *config* digest, and that sentence had to be corrected in three separate
places). CLAUDE.md's rule — "Do not assert what a protocol does from memory" — is why this file exists
rather than another confident sentence in the ADR.

## The two claims, and what backs each

### 1. Under the classic graph driver, an image ID is the digest of the image's configuration JSON

**Cited, primary.** The Docker/moby image specification v1.2, under *Terminology*, states verbatim:

> Each image's ID is given by the SHA256 hash of its configuration JSON.

Source: [moby/moby, `image/spec/v1.2.md`](https://github.com/moby/moby/blob/749d90e10f989802638ae542daf54257f3bf71f2/image/spec/v1.2.md)
(permalinked at commit `749d90e`, read 2026-09-18).

This is the whole reason `ImageDigestKind::config` exists in the enum: on such a daemon the ID genuinely
*is* a config digest, so `config` is a real thing to be, not a hypothetical.

### 2. Under the containerd image store, an image ID is the digest of the target descriptor

**Measured in this repo, not taken from a web page.** `resolve_image_digest()` reads the *container's*
`.Image`, and `resolve_image_digest_kind()` then asks the daemon for that ID's
`{{.Descriptor.mediaType}}` — which only resolves at all because the ID names a descriptor in the content
store. What that descriptor is was measured directly (`N11`, `N14`, and the survey below): an **index**
for a multi-platform reference, a **manifest** for a `docker commit`. Development daemon: Docker 29.7.2,
storage driver `overlayfs`.

Secondary, uncited-but-consistent: community write-ups describe the same split (image ID from the config
JSON classically, from the manifest/target descriptor under containerd). They agree with the measurement
and are **not** what the ADR relies on.

## The survey behind "no backend in this tree can emit `config`"

Every image in the development daemon's local store, asked for its descriptor media type
(2026-09-18, 22 images):

| count | `{{.Descriptor.mediaType}}` | kind |
| --- | --- | --- |
| 18 | `application/vnd.oci.image.index.v1+json` | `index` |
| 2 | `application/vnd.docker.distribution.manifest.list.v2+json` | `index` |
| 1 | `application/vnd.oci.image.manifest.v1+json` | `manifest` |
| 1 | `application/vnd.docker.distribution.manifest.v2+json` | `manifest` |

Both index spellings and both modern manifest spellings occur on one ordinary machine. **Neither config
spelling occurs at all**, which is what would be expected if `.Descriptor` is by construction the
descriptor a *reference resolves to* — an index or a manifest, never a config, since a config is what a
manifest points *at*.

This is a sample, not a proof, and it is written down so the next reader can re-take it in one command
rather than trust it.

## The gap this leaves, stated plainly

On a classic-graph-driver daemon the ID **is** a config digest (claim 1) and the daemon exposes no
`.Descriptor` to say so — so AgentEngine reports `unknown` there. That is a deliberate refusal to infer a
kind from a missing field, not an oversight; ADR-176 §13(c) names the experiment that would let it be
measured (toggling Docker Desktop's containerd image store off) and why this session did not run it on
the project owner's daemon.

**Unverified and deliberately not asserted anywhere in the ADR:** whether Docker 29 on a classic graph
driver omits the `.Descriptor` field entirely or merely leaves its `mediaType` empty. CI's Linux daemon
produced an empty result for the template, which is consistent with both, and this repo has no
classic-store daemon to tell them apart.
