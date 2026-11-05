# Releasing

How a release is built, signed and checked, so that players only get builds
made from this repository's public source. Three layers, each checkable by
someone who doesn't trust the one before:

| Layer | Says | Checked by |
|-------|------|------------|
| **Reproducible builds** | these bytes come from this commit | anyone: rebuild the tag, compare hashes |
| **Build provenance attestations** | GitHub's CI built these bytes from this commit, in this workflow | `gh attestation verify` (Sigstore's public log) |
| **The release signature** (`SHA256SUMS.sig`) | the maintainer approved exactly these bytes | Tome's update check, `release-check.yml`, `tools/release_sign.py verify` |

The maintainer never builds a release: CI does, and the maintainer only
signs the hashes CI produced, after checking them. So the signature doesn't
ask anyone to trust that the maintainer didn't change the binary: the hashes
it covers are the ones CI's public log and the attestations show, and anyone
can rebuild the tag to get them too.

## Reproducible builds

Building the same commit gives the same bytes, wherever and whenever:

- `tome.mix` and the SDK's binaries are linked with `--no-insert-timestamp`
  (top-level `CMakeLists.txt`): otherwise ld writes the link time into the
  PE header and the export table, the only difference between two builds.
  No build paths end up in them (release builds are stripped).
- The zips are packed by `cmake/repro_zip.py` (from `cmake/dist.cmake` and
  `sdk/cmake/dist.cmake`): sorted, one fixed time, fixed permissions, no
  extra fields, and the import library's archive members with their times
  and owners zeroed. CMake's own zip writer stores each file's access and
  change times; it's only the fallback when Python is missing (with a
  warning).
- The WAMR archive is downloaded and checked against its SHA-256.

Checked on 2026-11-04 with the host's MinGW toolchain: two `make dist` +
`sdk-dist` builds from scratch in different folders, a minute apart and
without ccache, gave identical `tome.mix`, `tome-<version>.zip` and
`tome-sdk-<version>.zip`. Not yet compared: a local container build with
CI's.

The same bytes need the same toolchain. CI builds in the Containerfile's
`ci` stage (layers cached in the Actions cache, not stored anywhere else),
which installs Fedora's current packages, so the image isn't kept: each
release carries `toolchain.txt`, the image's exact package versions. To
check a release:

```sh
git checkout v0.1.0
make container IMAGE_TARGET=ci                          # then compare its packages with toolchain.txt:
podman run --rm tome-dev rpm -qa --qf '%{NAME}-%{VERSION}-%{RELEASE}.%{ARCH}\n' | sort | diff - toolchain.txt
make container-sdk IMAGE_READY=1 DEV=OFF                # as ci.yml does
make container-dist IMAGE_READY=1 CONTAINER_RELEASE_BUILD=build-container
sha256sum build-container/tome-0.1.0.zip build-container/tome-sdk-0.1.0.zip
# compare with the release's SHA256SUMS
```

Soon after a release the packages match. Later, Fedora may have updated
the compiler; `toolchain.txt` then says which versions to install (Fedora
keeps old builds in Koji) for the same bytes.

## Attestations

`ci.yml`'s release job attests the zips, the WAMR archive and `tome.mix`
(`actions/attest`): a record in Sigstore's public, append-only log that
this repository's workflow built each file from the tagged commit. Anyone
can check a file, including an installed `tome.mix`:

```sh
gh attestation verify tome-0.1.0.zip --repo reignoforder/tome
```

GitHub only makes attestations for public repositories on this plan, so
the step is skipped while the repository is private.

## Release keys

`SHA256SUMS.sig` is an Ed25519 signature (the same code as mods'
`tome.sig`: Monocypher in Tome, RFC 8032's reference code in Python) over

```
tome-release-v1
<version>
<SHA256SUMS, byte for byte>
```

The version is signed with the hashes, so a signature can't be moved to
another release. The keys allowed to sign are in `loader/release-keys.txt`,
built into `tome.mix`. Tome's update check only announces a release whose
`SHA256SUMS.sig` verifies with one of them and whose `SHA256SUMS` lists
`tome-<version>.zip` (`core/update_check.h`, `verify_release`); the popup
shows that zip's hash.

### Making the keys (once)

On your own computer, not a shared or CI machine:

```sh
python3 tools/release_sign.py keygen ~/tome-release.key          # the key you sign with
python3 tools/release_sign.py keygen ~/tome-release-backup.key   # kept elsewhere, offline
```

- Put both **public** keys (printed) in `loader/release-keys.txt` and commit
  it. The backup key is there so that a lost or leaked key can be replaced:
  sign with the backup, and remove the lost key from the file in that same
  release. Installs only learn the change when they update, so a key that
  leaked has to stay out of every later release.
- Keep the **secret** keys offline (an encrypted USB drive, a password
  manager's file storage), each in two places. Never in the repository, a
  GitHub secret, or a CI job: anyone who can push a workflow change could use
  a key stored there.
- The CI release job refuses a tag while `release-keys.txt` has no key
  (`release_sign.py check-keys`).

## Making a release

1. Set the version in `CMakeLists.txt`, update `CHANGELOG.md`, commit.
2. Tag and push: `git tag v0.1.0 && git push origin v0.1.0`. CI builds,
   tests, attests and makes a **draft** release with the zips, the WAMR
   archive and `SHA256SUMS`.
3. Check what CI made, then sign it:

   ```sh
   mkdir release && cd release
   gh release download v0.1.0 --pattern SHA256SUMS --pattern 'tome-*.zip' --pattern 'WAMR-*.tar.gz' \
       --pattern toolchain.txt
   sha256sum --check SHA256SUMS                         # the files match it
   gh attestation verify tome-0.1.0.zip --repo reignoforder/tome   # CI built them (public repository)
   # optionally: rebuild the tag in the container and compare (above)
   python3 ../tools/release_sign.py sign SHA256SUMS ~/tome-release.key
   python3 ../tools/release_sign.py verify SHA256SUMS
   gh release upload v0.1.0 SHA256SUMS.sig
   ```

4. Publish the draft. `release-check.yml` verifies the signature and the
   files once more; if either fails, it turns the release back into a draft.

## What this doesn't cover

- **The first download.** A player who gets a fake `tome.mix` from another
  site has no Tome to check it. The published public keys (`SECURITY.md`),
  the attestations and Windows code signing (not yet:
  `ROADMAP.md`, item 11) are the answers there.
- **Downloads by hand.** Update Now installs only the signed zip; a player
  who downloads it from the release page instead can compare its SHA-256
  with the one the popup shows.
- **A `tome.mix` too broken to start.** The rollback runs in the new
  version's startup; one that crashes before it (in DllMain, say) can't put
  the old one back. Then deleting `tome.mix` and renaming `tome.mix.old`
  back by hand does.
- **The source.** The signature and the attestations show a release was
  built from the public source; whether that source is trustworthy is what
  reading it (and reviewing changes) is for.
