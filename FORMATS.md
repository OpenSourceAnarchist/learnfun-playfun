# On-disk Formats and Compatibility

This document describes the file formats produced/consumed by `learnfun` and `playfun`, with explicit compatibility notes and the recent extensions that enable feature typing (signed/descending semantics) and deterministic resume.

## 1) Objectives file: `<game>.objectives`

- Type: Text (one objective per line)
- Producer: `learnfun`
- Consumer: `playfun` (via `WeightedObjectives::LoadFromFile`)

### 1.1 Line structure

Each non-comment line encodes a weighted objective:

```
<weight> <tok1> <tok2> ... <tokN>
```

- `<weight>`: `double` in decimal.
- `<tokK>`: `int` encoding one feature component with flags in the high bits and a memory index in the low bits:
  - Bits: `[31..30] unused`, `bit 30 = OBJ_DEC_FLAG`, `bit 29 = OBJ_SIGNED_FLAG`, `bits 0..28 = OBJ_INDEX_MASK`.
  - Index extraction: `index = tok & ((1<<29) - 1)`.
  - Signed flag: `(tok & (1<<29)) != 0`.
  - Decreasing-is-good flag: `(tok & (1<<30)) != 0`.

This representation allows a single objective to be lexicographic over multiple memory bytes with per-component semantics.

### 1.2 Extension and compatibility

- Extension: Older versions emitted raw indices (no flags). Newer `learnfun` computes and emits flags per component (feature typing) and thus writes flagged tokens.
- Backward compatibility (within this repo): `WeightedObjectives::LoadFromFile` masks flags and will treat absent flags as `false` (unsigned, increasing). Old files remain readable and will simply have neutral semantics.
- Forward compatibility for external tools: Any ad hoc parser expecting only raw indices will now see large integers (due to flag bits). Such tools must be updated to mask with `OBJ_INDEX_MASK` to extract indices or, better, to honor flags.
- Migration: To adopt feature typing semantics, regenerate objectives by running `learnfun` on your movies (recommended). To maintain legacy behavior in an external pipeline, mask indices as above.

### 1.3 Semantics in `playfun`

- Magnitude scoring and ordering both respect flags by transforming bytes into rank keys (`MapKey`) prior to comparison.
- Lua overlay (`SaveLua`) prints raw indices only; it intentionally ignores flags for visualization.

## 2) Motifs file: `<game>.motifs`

- Type: Text/binary (project-internal; format has not been changed for the feature typing work).
- Producer: `learnfun` (`Motifs::SaveToFile`), Consumer: `playfun` (`Motifs::LoadFromFile`).
- Compatibility: Unchanged for this set of changes.

## 3) Snapshot state: `<game>.pfstate`

- Type: Binary (little-endian)
- Producer/Consumer: `playfun` (save on checkpoint; auto-load at startup if present)

### 3.1 Layout (v1)

1. Magic: 4 bytes: `P F S T`
2. Game string: `u32 length` + bytes
3. `int32 watermark`
4. Movie inputs: `u32 length` + bytes
5. Subtitles: `u32 count`, then for each: `u32 length` + bytes
6. Memories: `u32 count`, then for each: `u32 length` + bytes
7. Latest checkpoint: `int32 movenum`, then `u32 length` + savestate bytes
8. Motif weights snapshot: `u32 count`, then for each: `double weight` (8 bytes) + motif inputs `u32 length` + bytes
9. `u32 nfutures_`
10. RNG state: `u32 length` + bytes (e.g., ArcFour S-box + indices)

### 3.2 Compatibility

- New file type; it does not replace any existing format.
- Load path validates magic and game. Versioning can be extended by adding a `u32 version` after magic in future revisions.
- If absent or invalid, `playfun` falls back to the classic warm-up path; no external tools depend on this.

## 4) Diagnostics

- `<game>.svg`: Objective trajectories; unchanged semantics.
- `<game>.lua`: Memory overlay script; uses raw indices and ignores flags intentionally.
- `<game>-futures.html`, `<game>-log.html`: Human-readable; subject to change without notice.

## 5) Summary of breaking/behavioral changes

- Objectives line tokens now include high-bit flags for per-component semantics (signed/decreasing). This is an extension:
  - Internal readers handle both old and new.
  - External scripts must mask indices or update to honor flags.
- `pfstate` is new; it augments the workflow (deterministic resume) but doesn’t break old flows.

### 5.1 Migration checklist for external tools

- When parsing `.objectives`:
  - Extract index with `index = token & ((1<<29)-1)`.
  - Optional: read flags with `(token & (1<<29)) != 0` (signed) and `(token & (1<<30)) != 0` (decreasing-is-good).
- When writing `.objectives` lines:
  - Preserve existing flags if transforming or filtering.
  - For legacy inputs without flags, you may write default flags (both false) or re-generate via `learnfun` to infer better semantics.
- No changes required for `.motifs`, `.svg`, or `.lua` consumers.

### 5.2 Build/performance mode (non-file)

- The `--enable-aggressive-opts` configure switch alters compiler/linker flags for performance but does not change any on-disk formats.
- It may impact floating-point reproducibility across machines/compilers; formats remain unchanged.

If you author external tools, the only concrete compatibility work is updating `.objectives` parsers to apply `OBJ_INDEX_MASK` and optionally read flags. Everything else is internal to this repository.
