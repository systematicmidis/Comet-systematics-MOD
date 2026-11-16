<h1 align="center">Comet Systematics MOD</h1>

<p align="center">
	A fast, lightweight MIDI player/renderer built on <a href="https://github.com/MrCHB1/Comet">Comet</a>.
	<br />
	This fork focuses on very large black-MIDI files, compressed archives, and the tools used to inspect and farm them.
</p>

## Current release

**v1.11.9b** — [download the latest release](https://github.com/systematicmidis/Comet-systematics-MOD/releases/tag/v1.11.9b)

| Asset | Use it for |
| --- | --- |
| `Comet-Systematics-MOD-windows-x64.zip` | Recommended: portable package with `Comet.exe`, assets, runtime DLLs, and documentation. |
| `Comet-Release.exe` | Normal use and large MIDI files if you already have a Comet folder. |
| `Comet-Debug.exe` | Debugging and diagnostics. It is much slower on large files. |

The standalone EXEs are **not complete portable packages**. Keep them beside your existing Comet files: `assets/`, `config.yml`, `bass.dll`, `bassmidi.dll`, the Visual C++ runtime DLLs, and `OmniMIDI.dll` if you use MIDI out. Do not replace a whole Comet folder with only the EXE. The portable ZIP includes the required application files; extract it to a new folder.

## Requirements

- 64-bit Windows (the published builds are x64).
- An OpenGL-capable graphics card.
- The Visual C++ runtime files that ship with Comet.
- **OmniMIDI v2 or newer** only for real-time MIDI output. Prerendered and Maestro audio can be used without it.
- A soundfont for the internal synthesizers. The repository's `assets/soundfonts/` contains GeneralUser-GS by default.

## Quick start

1. Put `Comet-Release.exe` in the same folder as your existing Comet installation.
2. Start it and open a `.mid`, `.midi`, or a supported compressed archive.
3. For large files, use MIDITrail and leave the per-key renderer data to build lazily.
4. Settings are saved to `config.yml`; the previous good copy is kept as `config.yml.bak`, and a malformed file is moved aside as `config.yml.broken` instead of blocking every launch.
5. If a bad setting prevents startup, run `Comet.exe --safe-mode` once. Safe mode ignores `config.yml` and starts with defaults so you can repair the file or click **Apply & Close** to rewrite it.

Supported archive wrappers include `7z`, `xz`, `lzma`, `zip`, `gz`, `bz2`, `zst`, `lz4`, `rar`, `tar`, and compressed tars. Layers can be nested, for example a `.mid.xz` inside a `.7z`. A plain `.mid.xz` can be streamed without being fully decompressed first.

## What is different from normal Comet?

This table compares this MOD with the upstream [MrCHB1/Comet](https://github.com/MrCHB1/Comet) 1.10.0b baseline. Upstream may add features independently; the MOD is not a drop-in replacement for every upstream change.

| Area | Normal Comet (upstream) | Comet Systematics MOD |
| --- | --- | --- |
| **Very large MIDI loading** | Builds the per-key render store while loading. Large files can be slow and memory-heavy. | Files above roughly 4 million unique notes use a packed per-track store. MIDITrail and audio walk that store directly; legacy per-key data is built lazily only when another renderer needs it. |
| **Note totals** | Large files can be limited by the loader's storage and count path. | Keeps the complete non-deduplicated note count, including repeated tracks and repeated note-ons. Tested with a 100,000,000-note file and a 35-trillion-note archive. |
| **Compressed files and archives** | Plain MIDI files are the normal input path. | Nested archive support, streaming `.xz` chains, and repeated-cycle proof for archives that contain the same compressed cycle many times. |
| **Repeated layers/tracks** | No Mod-tab dedupe controls. | **Layered DeDupe** can detect repeated tracks and layered archive repeats, while still counting every represented note. Optional fast modes trade certainty for speed. |
| **Note counter** | Shows the loaded note values and rates. | Adds a display-only **note multiplier**, a target-count solver, melody-note protection, per-track exclusions, NPS/polyphony thresholds, and an optional multiplied graph. |
| **MIDITrail auras** | Basic held-note aura sizing. | Auras follow the configured `auraSize` instead of being multiplied to twice the requested size. Repeated/stacked notes increase the aura weight; compressed archives carry the same stack weight as their raw equivalent. |
| **MIDI output** | KDMAPI/OmniMIDI path. | Targets OmniMIDI v2+, guards the version probe and every per-note send, and shows an in-app warning if the driver faults instead of crashing. |
| **Updates** | Checks the official project release. | **Help → Check for Updates** can check either the official `MrCHB1/Comet` branch or this MOD's branch, with a source-build update path. |
| **Diagnostics** | Console-oriented error reporting. | Adds crash reporting and diagnostic logging so driver/load failures can be investigated after the app closes. |
| **MIDI tab / Mod tab** | Standard settings tabs. | Adds a **Mod** tab for dedupe and note-farming display controls. |

## Large MIDI and archive notes

- **Use the portable package or `Comet-Release.exe`.** The Debug build is intentionally much slower and uses more memory.
- **MIDITrail is the optimized renderer for packed files.** Other renderers may need to materialize the per-key data, which can take time on a huge file.
- **Compressed does not automatically mean small in memory.** The loader avoids keeping a full decompressed copy for streamable `.xz` chains, but an archive that must be extracted still needs memory for its data.
- **Repeated-cycle proof is conservative.** If the archive's repeated region cannot be proven, Comet falls back to the complete decode instead of guessing.
- **Do not enable "Risky fast dedup" unless you know the file.** It can stop early when different tiles happen to divide evenly; it is off at startup and is not saved.
- **A 4-million-note threshold is deliberate.** Below it, small files keep the established merged render path; above it, the packed track store avoids building 128 per-key arrays up front.

Representative development-machine results, not guarantees:

- 100,000,000-note test MIDI: **516 ms** to load in the current Release build, with the full 100,000,000 count. The same 763 MB file takes Ziggy **918.563 ms** to parse on the same machine, so Comet's packed load is faster on this workload.
- 4,101,001-note packed test: about 134 ms raw and 168 ms from `.xz`.
- Four-layer 35-trillion-note archive: about 11.4–11.9 seconds to prove and load, with the full repeated count preserved.

The Ziggy comparison is a parse-vs-load comparison on the 100M file. On an 8,000-track/320,000-note file Ziggy parses in **26.463 ms**, while Comet's loader benchmark takes about **126 ms**; that Comet number includes track hashing, merge/dedupe, and render-store preparation, so it is not parse-only. Comet's small-file path therefore does more work than Ziggy's parser, while the 100M packed path is faster overall.

## Settings worth knowing

- **Mod tab → note multiplier:** purely cosmetic. It changes the counter and optionally the graph, never the actual notes, rendering, or audio.
- **Mod tab → Layered DeDupe:** skips decoding repeated tracks/layers while preserving the represented note count.
- **Visual → MIDITrail → Aura size:** the value is the visual scale; stacked notes add a small logarithmic boost rather than drawing one aura per note.
- **Audio → MIDI out:** choose OmniMIDI for real-time MIDI out, or an internal Prerendered/Maestro mode.

## Crash reports and private configuration

- Crash reporting is **off by default**. When enabled, reports are queued under the user's local app data and uploaded to the endpoint configured for the build.
- The endpoint is a deployment secret, not a source constant. Set `COMET_CRASH_WEBHOOK` with `-D` at configure time or in the environment at runtime. A build without it still captures reports locally but disables the **Send test report** button and leaves queued reports on disk.
- Never commit a webhook URL, token, or `.env` file containing one. Treat a leaked endpoint as compromised and rotate it in the service that owns it.
- `config.yml` contains no crash endpoint, so it cannot redirect reports.

## Portable package

The release ZIP contains `Comet.exe`, `assets/` (including the soundfont), `bass.dll`, `bassmidi.dll`, the Visual C++ runtime DLLs, `README.md`, and `LICENSE.txt`. `SHA256SUMS.txt` is published beside the ZIP. Extract it to a new folder; do not copy only the EXE over an existing installation.

## Build from source

Requirements: CMake 3.16+, Visual Studio 2022 with the C++ desktop workload, Git, and the project's submodules/dependencies.

```powershell
cmake --preset x64-release
cmake --build --preset x64-release --parallel 2 --target Comet
```

The Release executable is written to `out/build/x64-release/Comet.exe`. A Debug build is available with `--preset x64-debug`.

Build and run the repository tests with:

```powershell
cmake --build --preset x64-release --parallel 2 --target CometMergeEquivalence CometArchiveRepeatRegression
ctest --test-dir out/build/x64-release -R "merge_equivalence|archive_repeat_regression" --output-on-failure
```

The archive regression test builds its own synthetic archive and a small raw MIDI with repeated tracks, so it does not need the large test files from the development folder.

## Troubleshooting

- **OmniMIDI warning:** install OmniMIDI v2 or newer, or switch the MIDI output mode to an internal renderer.
- **A huge file is slow in a non-MIDITrail renderer:** switch to MIDITrail or wait for the lazy per-key build to finish.
- **App does not start:** verify that `assets/` and the Visual C++ runtime DLLs are beside the executable, and check the diagnostic log/crash report.
- **Archive load fails:** try extracting the MIDI first. Nested or malformed archives can fall back to a full decode or be rejected.
- **Settings look wrong after a crash:** edit or restore `config.yml`, or start once with `--safe-mode`; the **Apply & Close** button writes settings immediately. The previous good file is `config.yml.bak`.
- **Crash test button is disabled:** this build has no `COMET_CRASH_WEBHOOK` configured, or the environment variable is empty. Reports are still written locally.

## Credits and license

Comet is created by [MrCHB1](https://github.com/MrCHB1) and the upstream contributors. This MOD is maintained by [systematicmidis](https://github.com/systematicmidis) and is not affiliated with the upstream project.

The project follows the upstream **DON'T BE A DICK PUBLIC LICENSE** in [LICENSE.txt](LICENSE.txt). If you redistribute a modified build, keep the upstream credit and make your changes available.
