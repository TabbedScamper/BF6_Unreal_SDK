# 0.8.1 release verification

Verified on Windows with Unreal Engine 5.8.1, September 10, 2026. The SDK and High Poly are released as a matching pair; the SDK also launches without the optional add-on.

| Check | Result |
| --- | --- |
| Shared native reader Release build and installed DLL hashes | Passed; source, runtime and build-output copies match |
| Unreal Development Editor build | Passed |
| Repository lint | Passed with two existing Enter-key handler warnings |
| Blocks round-trip, style, value icons and pan checks | Passed |
| Real-browser 5,088 and 10,176 block workspaces | Passed: imports, complete exports, text, themes, compact spacing, cursor-anchored zoom, artwork restoration, off-screen edits, undo/redo, real mouse pan and overview selection |
| Focused-file project snapshots | Passed |
| Native import rejection checks | 8 passed |
| Handwritten script backup and import protection | 18 passed |
| Converter round-trip cases | 6 passed; reports retain the existing site-export and stale-typing differences |
| Checked single-file Portal export | 31 passed, including stateful imports, UI output, fractional waits and rejection of broken final bundles |
| UI builder suite | 243 passed |
| Equipment-card template and image runtime exports | Passed |
| Windows test harness and strict performance verdicts | 14 passed |
| Extracted starter project without High Poly | Launch passed; both session-transform and pivot tests passed with the required SDK prop fixture |
| Extracted add-on with its matching SDK | 8 tests passed: material ownership and lifetime, palette cache, mirrored placements, streaming backing data, geometry metadata, corner sharing and asynchronous ocean replay |
| Release archives | SHA-256, byte counts, CRC, versions, required scripts, portable artwork and exclusion of local caches/debug files checked |

The first session-transform test on the empty fresh install failed because its required `FiringRange_Floor_01` SDK mesh had not been installed. Supplying that single fixture to the disposable test project's Saved directory allowed the complete three-cycle transform test to pass. This fixture is not included in the release archives.

The New Features popup now includes the installed High Poly release in its default view and tool history, and renders release subsection headings distinctly. Its unread indicator tracks each component's descriptor version.

Native embedded-browser close-up zoom averaged 16.7 ms per animation frame with both tested workspace sizes, compared with 146.7 and 253.0 ms before the off-screen artwork fix. See [the detailed block report](reports/BLOCKS-PERFORMANCE-2026-09-09.md) for conditions and limits. These measurements do not certify a minimum-spec GPU or full-detail zoom-to-fit performance.

No live Portal experience was published during these checks. Full in-game mode behavior, pixel-exact card appearance, exhaustive attachment compatibility, all map materials, and every minimum-spec graphics driver remain outside this release's automated coverage. Source imports reject unsupported native conversions rather than silently losing them.
