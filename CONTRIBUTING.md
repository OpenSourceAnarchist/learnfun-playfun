# Contributing and Re‑implementation Guide

This document is a ground‑up, diff‑level blueprint for (re)implementing the learning/playing system in this repository. It’s written for engineers starting from a clean codebase, with zero reliance on any specific patch history. It captures intent, math, pitfalls, and file layout so another team or LLM can rebuild the system independently.

The focus is the main gameplay/search loop in `tasbot/playfun.cc` and the emulator/input stack under `tasbot/fceu`. Where relevant, we also call out ancillary files.

If you prefer a short map of the terrain before diving in: build hygiene → objective scoring → nexts + subsampling → futures life‑cycle → evaluation (local + distributed) → selection + commit + motif reweighting → backtracking + TryImprove → persistence/resume → diagnostics → tests.

---

## 0) Design goals and constraints

- Deterministic, reproducible search: All randomness must be seeded and serializable.
- Time‑bounded per round: Cap the number of candidate “next” sequences evaluated.
- Optimistic but not reckless: Evaluate futures (lookahead), but penalize harmful detours by integrating intermediate step costs.
- Adaptive exploration: When outcomes are broadly bad, explore more, shorter futures; when good, deepen fewer futures.
- Recover from local minima: Periodic backtracking + local improvement over spans.
- Long‑running resilience: Checkpoint emulator and search state; resume safely.

---

## 1) Structural integrity and build hygiene

These produce zero functional change but prevent compiler garden paths.

### 1.1 Class/method scope correctness (C++)

- Ensure `struct PlayFun` encloses all member functions (e.g., `Master`, `TakeBestAmong`, `ParallelStep`, etc.) and closes before global `main`.
- A single missing `}` can cause errors like “function-definition is not allowed here” or “expected ‘}’ at end of input”. Use an editor’s fold/brace highlighting after any large edit.

Acceptance:
- All `PlayFun` members compile and are callable; `main` links with `PlayFun` symbols.

### 1.2 ODR: device structs must be defined once

- File(s): `tasbot/fceu/input/shadow.cpp` must include the canonical header `tasbot/fceu/input/zapper.h` and avoid redefining `struct ZAPPER` locally.
- Any duplicated typedefs with mismatched field types (e.g., `uint8 bogo` vs `int bogo`) violate the One Definition Rule.

Acceptance:
- No ODR warnings; only a single definition for `ZAPPER` is visible across translation units.

---

## 2) Objective scoring semantics (first principles)

You need two primitives in your objective layer (see `tasbot/weighted-objectives.{h,cc}` and callers in `playfun.cc`).

### 2.1 Per‑step magnitude

- Function: `double EvaluateMagnitude(const vector<uint8>& prev, const vector<uint8>& next)`.
- Intent: Return the signed/unsigned sum of magnitudes of objective changes between two memory snapshots.
  - For each feature i: contribution could be `w_i * (next_i - prev_i)` if “larger is better”, or `w_i * (prev_i - next_i)` if “smaller is better”. Use flags per feature to encode monotonicity.
  - If you have non‑linear objectives, document the transform and ensure it’s stable under sampling.
- Determinism: Must be pure for identical inputs.

Notation:
- Let memory be mapped to a feature vector x ∈ R^d. For a step from x_t to x_{t+1}, define
  
  $$\Delta f(x_t \to x_{t+1}) = \sum_{i=1}^d w_i \cdot s_i \cdot (x_{t+1,i} - x_{t,i})$$
  
  where s_i ∈ {+1, −1} encodes “larger is better” or “smaller is better”.

### 2.2 Positive/negative decomposition

- Function: `void DeltaMagnitude(prev, next, &positive, &negative)`.
- Intent: For diagnostics and penalties, split the total into strictly non‑negative components:
  - positive ≥ 0 captures gains (sum of positive contributions).
  - negative ≤ 0 captures losses (sum of negative contributions).
- Use this split later to penalize futures that regress critical metrics even if their integral is OK.

Acceptance:
- Unit tests on micro pairs where only one feature moves up/down produce the expected signed parts and totals.

---

## 3) Integration over time (path cost)

### 3.1 Path integral of magnitudes

- Function: `double ScoreIntegral(State start, const vector<uint8>& inputs, vector<uint8>* final_memory)`.
- Intent: Accumulate per‑step magnitudes along a proposed input sequence.
- Algorithm:
  1) Load emulator with `start` (an uncompressed savestate blob).
  2) Read initial memory M_0.
  3) For each input u_t:
     - Step emulator (use a caching step if available).
     - Read M_{t+1}; sum `EvaluateMagnitude(M_t, M_{t+1})`.
     - M_t ← M_{t+1}.
  4) Optionally return M_T.
- Deterministic for fixed `(start, inputs)`.

Acceptance:
- Harness: With contrived memory transitions, verify that the sum equals the sum of step magnitudes, independent of internal emulator caches.

---

## 4) Candidate “nexts” and subsampling

### 4.1 Generate candidate next sequences

- File: `tasbot/playfun.cc` (search loop helpers).
- Build candidates from:
  - Futures’ heads (prefix of each future’s `inputs`).
  - Weighted motifs as backfill.
- Implementation notes:
  - Keep next length small (e.g., 10 frames). Long “nexts” reduce branching but increase per‑candidate variance.
  - Deduplicate nexts via hashing (e.g., string/bytes key) to avoid redundant work.
  - Annotate each next with an explanation, e.g., `ftr-<idx>` or `backfill`.

Acceptance:
- Generated set contains unique sequences; explanations align with source.

### 4.2 Subsample nexts with futures bias

- Purpose: Bound per‑round compute by selecting only `[MIN_NEXTS, MAX_NEXTS]` candidates.
- Policy:
  - Partition next indices: those derived from futures vs backfill.
  - Shuffle both lists with a seedable RNG.
  - Prefer a healthy share from futures; fill remainder from backfill; top off from futures if still short.
- Tip: Keep the exact ratio configurable; 50/50 is a good baseline.

Acceptance:
- Given N_f futures‑derived and N_b backfill with a target K, the output is reproducible under a fixed RNG state and prioritizes futures‑derived items.

---

## 5) Futures: population, projection, adaptation

A “future” is a candidate long input plan used to evaluate the outlook from a post‑next state.

### 5.1 Populate futures to a target working‑set size

- Data per future:
  - `vector<uint8> inputs`
  - `bool weighted` (whether to sample motifs with current weights)
  - `int desired_length` (in [MINFUTURELENGTH, MAXFUTURELENGTH])
- Algorithm:
  - If fewer than `nfutures_`, create new futures until size matches target.
  - Choose random `desired_length` in range.
  - Fill `inputs` to `desired_length` by concatenating motifs (weighted if `weighted`, else uniform/random motif selection).
  - Prefer to avoid exact duplicates (optional but recommended).

Acceptance:
- After population, `futures.size() == nfutures_` and each future has `inputs.size() == desired_length`.

### 5.2 Project nexts through futures (ensemble evaluation)

- For each candidate `next`:
  1) Apply `next` to the current state to obtain `post_next_state` and `post_next_memory`.
  2) For each future:
     - Compute `(positive, negative)` between `post_next_memory` and the future’s terminal memory.
     - Compute `integral_score` = path integral along the future from `post_next_state`.
  3) Aggregate a per‑future total for pruning: 
     

  ---

  ## 18) Logic fixes and intent alignment (catalog)

  This section enumerates concrete logic fixes that align implementation with the intended algorithm. Use these as targeted patches or regression tests when porting/refactoring.

  ### 18.1 Dropping the wrong futures (comparator sign)

  - Symptom: The pruning step in the selection loop removed the best futures and kept the worst due to a reversed comparison when selecting indices to drop.
  - Root cause: Using a comparison that prefers larger totals when we meant to drop the minima.
  - Patch (pseudocode):

  ```cpp
     $$\text{futureTotal} = \text{integral} + \text{positive} + \text{negative}.$$
  4) Aggregate a per‑next total to rank nexts: 
     
     $$\text{nextScore} = \sum_{f \in \text{futures}} (\text{integral}_f + \text{positive}_f + \text{negative}_f) + \text{immediate}.$$
- Rationale:
  - Counting both positive and negative components discourages futures that regress important signals, even if they spike a few others.
  - The integral bakes in intermediate costs.

Acceptance:
- Shapes: futurescores arrays match futures size; best/worst future scores are tracked for diagnostics; next ranking stable under repeated runs with fixed RNG.

### 5.3 Adapt desired lengths and working‑set size

- Per‑future adaptation:
  - If a future’s total > 0: increase `desired_length` by ~10% (min step 1), clamp to `[MINFUTURELENGTH, MAXFUTURELENGTH]`.
  - Else: decrease `desired_length` by ~10%, same clamping.
- Working‑set adaptation:
  - Let `p = (# futures with total > 0) / nfutures_`.
  - If `p` is low (e.g., < 0.4), increase `nfutures_` by ~5% up to `MAX_FUTURES`.
  - If `p` is high (e.g., > 0.6), decrease `nfutures_` by ~5% down to `MIN_FUTURES`.
- Commentary:

  - Acceptance:
    - With synthetic totals `[3, -2, 5, 0]`, the first drop is index 1 (−2), then index 3 (0), etc.
    - After pruning, futures and totals vectors remain the same length and aligned.

  ### 18.2 Subrange reversal bug in `ReverseRange`

  - Symptom: Reversal wrote output starting at index 0, corrupting the prefix when `start > 0`.
  - Root cause: Ignored the base offset when writing reversed elements.
  - Patch (correct in‑place subrange reversal):

  ```cpp
  - These thresholds and step sizes should be config‑driven.
  - The goal is a self‑tuning breadth/depth trade‑off.

Acceptance:
- With synthetic totals, lengths and `nfutures_` evolve monotonically to bounds as expected.

### 5.4 Prune worst futures and duplicate the best via mutation

- Drop `DROPFUTURES + MUTATEFUTURES` futures with the lowest totals.

  - Acceptance:
    - For `v = [0,1,2,3,4,5], start=2, len=3` result is `[0,1,4,3,2,5]`.
    - Idempotence on len≤1; no out‑of‑bounds on start+len==v.size().

  ### 18.3 Bounds check in MARIONET future scores aggregation

  - Symptom: Out‑of‑range write when merging helper responses into `futuretotals`.
  - Root cause: Using `<= futuretotals->size()` instead of `< ...size()`.
  - Patch:

  ```cpp
- Duplicate the single best future by pushing `MUTATEFUTURES` mutated variants:
  - Randomly flip `weighted` with small probability.
  - Replace tail (e.g., halve inputs length, min clamp) to encourage exploration.
  - Occasionally dualize inputs (swap left/right, up/down, a/b, start/select) to explore mirrored policies.
- Implementation pitfall:

  - Acceptance:
    - With mismatched sizes in a test harness, the check trips before any write; with correct sizes, totals sum exactly the helper outputs.

  ### 18.4 Logging pointer vs value

  - Symptom: Logs printed the address of an integer field (e.g., `&start->movenum`) rather than its value.
  - Root cause: Passed a pointer to `fprintf` `%d`.
  - Patch:

  ```cpp
  - If you swap‑erase a future from the vector, also swap the corresponding entry in `futuretotals` to keep indices aligned.


  - Acceptance:
    - Log lines show the correct integer; no UB from printing raw addresses.

  ### 18.5 Keep metadata aligned on swap‑erase

  - Symptom: Dropping futures desynchronized indices between the `futures` vector and parallel arrays like `futuretotals`.
  - Root cause: Swap‑erase applied to one vector but not the others.
  - Patch: Always swap corresponding elements in all aligned arrays before `pop_back()` (see 18.1 snippet).
  - Acceptance: After removing any index, all aligned arrays maintain correct pairings (spot‑check a few indices).

  ### 18.6 Thread‑local emulator state in parallel loops

  - Symptom: Data races or nondeterminism when multiple next candidates reuse a single mutable emulator instance in parallel.
  - Root cause: Shared mutable state across threads.
  - Patch: Clone/load a thread‑local copy of the emulator state per iteration.

  ```cpp
Acceptance:
- Post‑prune size equals new `nfutures_`; the duplicated best is present; mutated futures differ from the parent.

---

## 6) Evaluation engine: local parallelism and distributed helpers

  - Acceptance: Multi‑threaded and single‑threaded runs produce identical choices under a fixed seed.

  ### 18.7 PRNG state serialization for deterministic resume

  - Symptom: After loading a snapshot, subsequent random draws diverge from the original run.
  - Root cause: RNG state not captured or restored.
  - Patch: Expose `GetState()`/`SetState()` on the RNG (e.g., ArcFour) to serialize its internal state (indices + S‑box) as bytes in the snapshot.
  - Acceptance: Save→Load→Continue yields identical behavior to an uninterrupted run given the same prior inputs.

  ### 18.8 Structural fixes with logic impact

  - Missing brace closing a large method (e.g., `ParallelStep`) caused dozens of cascading compile errors and mis‑scoped methods. Resolution: add the missing `}` so subsequent methods are members of the class.
  - ODR mismatch on `ZAPPER` struct (`uint8` vs `int` field type) led to ABI hazards. Resolution: include and use the canonical definition from `fceu/input/zapper.h` everywhere; remove local typedefs.

  - Acceptance: Codebase compiles cleanly; device behavior is consistent across object files.

### 6.1 Local threading over nexts

- Use OpenMP or a thread pool to parallelize evaluation of candidate nexts.
- For each next:
  - Copy the current state (`post_next_state` is thread‑local), run `InnerLoop`.
- Never share a mutable emulator state across threads.

Acceptance:
- Single‑threaded and multi‑threaded runs produce identical outputs with the same RNG seed (modulo FP noise).

### 6.2 Distributed helpers (optional, MARIONET)

- A helper accepts a serialized current state, one next, and the futures set; returns immediate score and per‑future scores.
- Master:
  - Probe helper ports, batch requests (one per next), collect responses.
  - Fallback to local execution if no helpers are reachable.

Acceptance:
- When helpers are available, wall clock improves; results remain consistent with local evaluation.

---

## 7) Choosing and committing the next

- Pick the next with the highest `immediate + futures sum`.
- Commit by stepping emulator for each input; append to `movie` (and optional subtitles/explanations).
- Create a savestate every `CHECKPOINT_EVERY` frames.

### 7.1 Immediate motif reweighting

- After committing a next originating from a motif, update its weight based on immediate normalized value change:
  - If value increased: divide its weight by `MOTIF_ALPHA` (increase influence).
  - If decreased: multiply by `MOTIF_ALPHA`.
- Caps: prevent dominance or irrelevance with `MOTIF_MAX_FRAC` and `MOTIF_MIN_FRAC` of the total weight mass.

Acceptance:
- Motif weights drift within caps; sampling routine remains well‑behaved.

---

## 8) Backtracking and local improvement (TryImprove)

### 8.1 Stuck detection

- Track consecutive rounds with “bad” best scores (e.g., < 0). If the streak exceeds a fraction of the backtrack interval, trigger a backtrack early.
- Commentary:
  - This heuristic prevents wasting time in a trough; tune threshold experimentally.

### 8.2 Improvements over a span

- When backtracking triggers:
  1) Choose a checkpoint at least `MIN_BACKTRACK_DISTANCE` frames in the past and above a `watermark` (e.g., skip pre‑game menus).
  2) Extract the “improveme” sequence from that checkpoint to the present.
  3) Generate candidate replacements using a mix of:
     - RANDOM: motif‑sampled sequences of equal length.
     - OPPOSITES: dualize + reverse the whole sequence and halves; random spans dualize/reverse with optional keepreversed.
     - ABLATION: mask out buttons probabilistically (avoid mask=255 which does nothing).
     - CHOP: delete random spans, biased towards small lengths (e.g., len proportional to U^2 where U~Uniform[0,1]). Iterate chopping until no improvement.
  4) Deduplicate candidates with a set/hash of sequences.

### 8.3 Acceptance test for a candidate

- Definitions:
  - `end_integral`: path integral from checkpoint to current end.
  - For candidate, compute `new_integral` similarly and `n_minus_e = EvaluateMagnitude(end_memory, new_memory)`.
- Accept if:
  - `new_integral ≥ end_integral` (path no worse than the existing one), and
  - `new_integral > 0` (improves over the checkpoint state), and
  - `n_minus_e > 0` (new end strictly better than current end).
- Score for ranking replacements:
  
  $$\text{score} = (\,new\_integral - end\_integral\,) + n\_minus\_e.$$

### 8.4 Replay via the normal selection machinery

- Rewind to the checkpoint.
- Treat the original span and all accepted replacements as alternative “nexts”; call your standard selection/commit routine.
- Commentary:
  - This keeps a single decision policy for all actions; no special handling of backtracked segments.

Acceptance:
- Backtrack events are logged; the chosen replacement diverges movie inputs accordingly.

---

## 9) Persistence and resume (binary snapshot)

### 9.1 Snapshot format

- File: e.g., `<game>.pfstate`, written on each checkpoint.
- Binary layout (little‑endian):
  1) Magic: 4 bytes, e.g., `P F S T`.
  2) Game string: `u32 length` + bytes.
  3) `int32 watermark`.
  4) Movie inputs: `u32 length` + bytes.
  5) Subtitles: `u32 count`, then for each: `u32 length` + bytes.
  6) Memories: `u32 count`, then for each: `u32 length` + bytes.
  7) Latest checkpoint: `int32 movenum`, then `u32 length` + savestate bytes.
  8) Motif weights snapshot: `u32 count`, then for each:
     - weight as IEEE‑754 `double` (8 raw bytes),
     - motif inputs: `u32 length` + bytes.
  9) `u32 nfutures_` (clip to bounds on load).
  10) RNG state: `u32 length` + bytes.

### 9.2 Loading

- Validate magic and game name.
- Restore watermark, movie, subtitles, memories.
- Load emulator to the checkpoint savestate.
- Re‑feed memories to the objective observation mechanism (if it accumulates statistics over time).
- Restore motif weights, `nfutures_` (clamped), RNG state.

Acceptance:
- Save → Load roundtrip yields identical in‑memory structures (minus transient caches).
- Resumed run makes identical choices as an uninterrupted run with the same seed.

---

## 10) Diagnostics and observability

- HTML log: timestamps, backtrack events, TryImprove summaries (iters, improved count/ratio, best scores).
- Futures HTML: enumerate futures with `inputs.size()/desired_length`, flags (weighted/mutant), and a colorized depiction of inputs.
- Score distribution SVG:
  - For each round, add a column of points representing immediate scores, future sums, and min/max (or worst future) for each next.
  - Mark the chosen index.
- Throttle artifact generation (e.g., quick every 10 rounds; full every 50) to minimize IO.

Acceptance:
- Artifacts appear periodically; file sizes remain bounded over long runs (by throttling; you can rotate logs if needed).

---

## 11) Quality gates and tests

- Build: compile after each phase.
- Unit tests (tiny):
  - Objective magnitude and positive/negative splits on synthetic vectors.
  - ScoreIntegral on 2–3 step toy transitions.
  - Serialization roundtrip (without emulator): write/read bytes and compare.
  - Subsampling determinism with fixed seed.
- Smoke tests:
  - Single‑threaded vs multi‑threaded runs produce the same outcomes.
  - Backtrack + TryImprove path with a contrived ROM or mock that responds predictably.

---

## 12) Configuration and tunables (suggested)

- `config.txt` keys (read at startup):
  - NEXTS: `MIN_NEXTS`, `MAX_NEXTS`.
  - FUTURES: `MIN_FUTURES`, `MAX_FUTURES`, `NFUTURES_STEP_FRAC`, `DESIRED_LENGTH_STEP_FRAC`.
  - BACKTRACK: `TRY_BACKTRACK_EVERY`, `MIN_BACKTRACK_DISTANCE`, `STUCK_THRESHOLD_FRAC`.
  - IMPROVE: iterations per approach, toggles per strategy.
  - MOTIFS: `MOTIF_ALPHA`, `MOTIF_MAX_FRAC`, `MOTIF_MIN_FRAC`.

---

## 13) Reference snippets (safe to adapt)

These are intentionally schematic; adapt to your style.

### 13.1 Thread‑local evaluation pattern

```cpp
// Pseudocode inside ParallelStep
vector<LocalRes> local(nexts.size());
#pragma omp parallel for schedule(dynamic)
for (int i = 0; i < (int)nexts.size(); i++) {
  vector<uint8> state_copy = current_state; // thread‑local
  double immediate = 0, best = 0, worst = 0, futsum = 0;
  vector<double> futscores(futures.size(), 0.0);
  InnerLoop(nexts[i], futures, &state_copy,
            &immediate, &best, &worst, &futsum, &futscores);
  local[i] = {immediate, futsum, worst, std::move(futscores)};
}
```

### 13.2 Swap‑erase with aligned arrays

```cpp
auto erase_at = [&](int idx) {
  int last = futures.size() - 1;
  if (idx != last) {
    std::swap(futures[idx], futures[last]);
    std::swap(futuretotals[idx], futuretotals[last]);
  }
  futures.pop_back();
  futuretotals.pop_back();
};
```

### 13.3 Little‑endian helpers

```cpp
static inline void AppendU32(vector<uint8>& out, uint32_t v) {
  out.push_back((uint8)(v));
  out.push_back((uint8)(v >> 8));
  out.push_back((uint8)(v >> 16));
  out.push_back((uint8)(v >> 24));
}
static inline bool ReadU32(const vector<uint8>& in, size_t* p, uint32_t* v) {
  if (*p + 4 > in.size()) return false;
  *v = (uint32_t)in[*p] | ((uint32_t)in[*p+1] << 8) |
       ((uint32_t)in[*p+2] << 16) | ((uint32_t)in[*p+3] << 24);
  *p += 4; return true;
}
```

---

## 14) Pitfalls and candid notes

- Missing brace in a large method (e.g., `ParallelStep`) will cascade into dozens of nonsensical errors. When you see “function-definition not allowed here”, stop and check scopes before anything else.
- ODR mismatches across device headers are silent footguns until link/runtime. Always include the canonical header (e.g., `zapper.h`) instead of re‑declaring structs.
- Emulator state is not thread‑safe. Each thread must load its own copy; avoid sharing mutable state or memory buffers across threads.
- If you drop futures by value without synchronizing auxiliary arrays (scores, metadata), your reads and writes desynchronize. Swap the metadata together or store futures and their scores in a single struct.
- Persistence without the RNG state, motif weights, and `nfutures_` will make resumes diverge from the original run. Serialize all of them.
- Integral scoring can “like” long, slightly negative excursions if the final end state is great. Our acceptance test for improvements compares path integrals strictly to avoid being fooled by good endpoints.
- Distributed helpers failing mid‑run should degrade gracefully to local execution. Don’t couple correctness to availability of helpers.

---

## 15) Glossary

- next: A short sequence of inputs considered for immediate commitment.
- future: A longer plan used for forecasting the value of committing a next.
- integral (path integral): Sum of per‑step magnitudes over a sequence.
- positive/negative: Decomposition of objective magnitude into non‑negative gains and non‑positive losses.
- backtrack: Rewind to a previous checkpoint to try improved alternatives for the span to current time.
- TryImprove: Heuristics to generate replacement sequences for a span (random, opposites, ablation, chop).

---

## 16) Where things live (files)

- `tasbot/playfun.cc`: Main search loop, futures/nexts machinery, evaluation, selection/commit, backtracking, TryImprove, persistence, diagnostics.
- `tasbot/weighted-objectives.{h,cc}`: Objective representation and magnitude evaluation.
- `tasbot/motifs.{h,cc}`: Motif storage, sampling, and weight manipulation.
- `tasbot/fceu/*`: Emulator core and input devices; ensure input device structs (e.g., `zapper.h`) are included rather than redefined.

---

## 17) Final remarks

Start simple: single thread, no helpers, no backtracking. Get magnitude and integral right first—they are the foundation. Only then add subsampling, futures adaptation, and finally backtracking. Leave persistence and diagnostics last (but plan their data shapes early).

If you deviate from any policy here, that’s fine—treat this as a rigorous baseline. The acceptance checks and pitfalls are the non‑negotiables that keep the system sane over long runs.

---

## 18) Build modes, performance, and determinism

### 18.1 Configure options

This project uses autotools. Typical flows:

- Portable build (default):
  - `./autogen.sh && ./configure && make -j`
- Maximum performance (host‑tuned):
  - `./autogen.sh && ./configure --enable-aggressive-opts && make -j`

The `--enable-aggressive-opts` switch injects the following flags:

- CXXFLAGS/CFLAGS: `-Ofast -march=native -flto=auto -falign-functions=32 -fomit-frame-pointer` plus legacy warning suppressions needed by this codebase.
- LDFLAGS: `-flto=auto` (ensures link‑time optimization is active).

Notes and caveats:

- `-Ofast` implies `-ffast-math` (non‑IEEE 754 strict). If you require strict FP semantics or bit‑for‑bit reproducibility across compilers/hosts, use the default build.
- `-march=native` generates binaries that may not run on older/different CPUs. Use only if you deploy on the same class of hosts.
- LTO (`-flto`) requires compatible binutils/compilers. If link errors mention plugin/LTO, either upgrade toolchain or disable aggressive mode.

### 18.2 Determinism guidelines

- With the default build, determinism is governed primarily by RNG state and emulator behavior; save RNG state to snapshots (`.pfstate`) for reproducible resumes.
- With aggressive mode, floating‑point differences and instruction reordering may cause tiny score drifts that can change tie‑breaks. If you need strict reproduction for experiments, prefer the default build.

---

## 19) Objectives feature typing and migration

Objectives now support per‑component semantics via high‑bit flags in the token integers (signed and decreasing‑is‑good). Internals are backward‑compatible, but external tools must be aware.

### 19.1 Reading/writing inside this repo

- `WeightedObjectives::LoadFromFile` accepts both legacy tokens (no flags) and new tokens (with flags). Legacy tokens are treated as unsigned/increasing by default.
- `WeightedObjectives::SaveToFile` writes flagged tokens reflecting the learned/selected semantics.

### 19.2 Migration of existing objective files

Recommended: Re‑generate objectives with `learnfun` against your source movie(s). This enables:

- Automatic per‑index flag determination via `WeightByExamples` (which may split composite objectives into groups with different flags).
- Output of flagged tokens that fully capture feature typing.

Alternative (stop‑gap): Keep using legacy `.objectives` with no rewrite. `playfun` will load them and assume unsigned/increasing semantics. You will not benefit from feature typing until you re‑generate.

### 19.3 External tools and scripts

- If you have out‑of‑tree parsers for `.objectives`, update them to:
  - Mask indices: `index = token & ((1<<29)-1)`.
  - Optionally read flags: `signed = (token & (1<<29)) != 0`, `decreasing = (token & (1<<30)) != 0`.
- If your external tools must emit updated files, preserve flags when writing tokens. For legacy inputs, either write with default flags (both false) or re‑generate via `learnfun` to infer flags.
