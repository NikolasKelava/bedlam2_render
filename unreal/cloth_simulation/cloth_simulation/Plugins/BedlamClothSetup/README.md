# BedlamClothSetup — UE 5.7 Editor Plugin

C++ editor plugin that bridges the gap the UE Python API cannot cross for the
**Chaos Cloth** migration in BEDLAM2: authoring `ChaosClothAsset` Dataflow graphs,
configuring cloth simulation, and recording a deterministic **Chaos Cache** of a
garment simulating on an animated body.

It exposes its functionality as **editor console commands** so the rest of the
pipeline can stay Python-driven (call them with
`unreal.SystemLibrary.execute_console_command`).

> **Future Python-orchestration session: start here.** This document is the
> contract for the commands you will call. Pay special attention to
> [§ Calling from Python](#calling-from-python) — `RecordChaosCache` is
> **asynchronous** (it launches PIE and returns before the recording finishes).

---

## Status (2026-06-03)

| Command | Status |
|---|---|
| `BedlamCloth.CreateClothAsset` | ✅ Working — optional 4th arg `[PhysicsAssetPath]`; if omitted, uses the body SKM's physics asset, else **auto-discovers** one in the body's folder; loud error only if none found anywhere. Natural-fabric defaults: `MaxDistance` 5, weight-map skirt `High` 20, `Bending` 0.8, `Damping` 0.1, `Gravity` 0.5 (decouples drape from leg-collision), `Solver` 3 substeps / 10 iterations (performant; sets NumIterations **and** MaxNumIterations — the latter caps at 6 by default), `CollisionThickness` 1.0. All tunable in `BedlamClothDefaults`. |
| `BedlamCloth.SetWeightMap` | ✅ Working — `all_dynamic` (fixed), `auto` (per-vertex proximity), `file`. Dynamic `High` 20 cm (a modest, fabric-like skirt freedom; collision keeps it off the legs). |
| `BedlamCloth.ConfigureSimulation` | ✅ Working — MaxDistance weight-map-aware; `Bending`/`Stretch`/`Friction`/`CCD` keys added |
| `BedlamCloth.RecordChaosCache` | ✅ Complete — PIE-based, warmup/settle frames, auto-save, auto-cleanup |
| `BedlamCloth.ShowGraph` | ✅ New — rebuilds the visual Dataflow graph so a script-created asset's nodes are visible/editable in the editor (CreateClothAsset now does this automatically). |

---

## Location, modules & build

- **Plugin:** `unreal/cloth_simulation/cloth_simulation/Plugins/BedlamClothSetup/`
- **Module:** `BedlamClothSetup` (Editor, `LoadingPhase: PostEngineInit`)
- **Sources:** `Source/BedlamClothSetup/{Public,Private}/BedlamClothSetup*.{h,cpp}`
- **UE project:** `unreal/cloth_simulation/cloth_simulation/cloth_simulation.uproject` (UE 5.7)

**Build.cs dependencies** (verified at runtime):
`Core, CoreUObject, Engine, UnrealEd` (public);
`ChaosClothAsset, ChaosClothAssetEngine, ChaosClothAssetDataflowNodes,
ChaosClothAssetTools, DataflowCore, DataflowEngine, DataflowNodes, ChaosCaching,
EditorScriptingUtilities, PhysicsCore, AssetRegistry` (private).

**Build command** (PowerShell; the editor must be **closed** — Live Coding locks
the DLL, and struct/global changes are not Live-Coding-safe):

```powershell
& "C:\Program Files\Epic Games\UE_5.7\Engine\Build\BatchFiles\Build.bat" `
  cloth_simulationEditor Win64 Development `
  -Project="<repo>\unreal\cloth_simulation\cloth_simulation\cloth_simulation.uproject" -WaitMutex
```

Two `LNK4206` PCH warnings on the module's generated `.cpp` files are benign.
After adding new source files, regenerate VS project files so they appear in the
solution (UBT compiles them either way).

---

## Console commands (the API)

### 1. `BedlamCloth.CreateClothAsset <GarmentMeshPath> <BodySkeletalMeshPath> <OutputAssetPath> [PhysicsAssetPath]`
Builds a `UChaosClothAsset` (+ a sibling `UDataflow` asset at `<OutputAssetPath>_DF`)
from a garment **StaticMesh** and a body **SkeletalMesh**, evaluates the Dataflow
graph, and saves both. Sets the cloth asset's skeleton and physics asset from the body.

> ⚠️ **The body MUST have a Physics Asset — it is the body's collision.** Resolution
> order: (1) the 4th arg if given; (2) the body SKM's own Physics Asset; (3)
> **auto-discovery** of a `UPhysicsAsset` in the body's content folder (preferring a
> name-match). **Only if none exists anywhere** does the cloth get NO body collision
> (limbs pass straight through) — the command logs a loud error then. This was the #1
> real footgun: a body imported without `create_physics_asset` had no asset
> *assigned*, leaving the `SetPhysAsset` node empty. After creating, you can still
> open the Dataflow and confirm that node has a Physics Asset. See
> [§ footguns](#known-limitations--footguns).

The Dataflow graph it builds (Collection passthrough chain):
```
Import(StaticMesh) → TransferSkinWeights(from body) → SetPhysicsAsset
  → MaxDistance → Stretch(PBD) → Bending(PBD) → Gravity → Collision → Damping → Solver → Terminal
```

**All the tunable numbers live in one place — the `BedlamClothDefaults` namespace
at the top of `BedlamClothSetupCommands.cpp`.** Edit those constants and rebuild
to change the baked-in behaviour; most can also be overridden per-asset afterwards
with `ConfigureSimulation` / `SetWeightMap`. Current defaults:

| Constant | Default | Meaning |
|---|---|---|
| `MaxDistanceCm` | 5 | How far a particle may drift from its skinned body position. Main knob for **how much the garment drapes**: smaller = tighter hug, less drape/waving, less chance of drifting into the body; larger = more flow. Stay ~4–12. (Does *not* cause cloth to follow the wrong limb — that was a skin-weight bug, not this.) |
| `StretchStiffness` | 1.0 | PBD stretch [0,1]; keep high so the garment doesn't visibly stretch/look rubbery. |
| `BendingStiffness` | 0.8 | PBD bending [0,1]; higher = firmer folds, less floppy waving; lower = floppier. |
| `DampingCoefficient` | 0.1 | Global point damping [0,1]. **Knob for "behaves like rubber / waves too much":** higher bleeds off oscillation so it settles. |
| `GravityScale` | 0.5 | World-gravity multiplier. **The knob that decouples drape from leg-collision:** keep `MaxDistance` big enough for the leg, then lower gravity so the skirt only spends a fraction of that headroom sagging. Lower = less drape (collision headroom unchanged). ~0.4–0.7. |
| `SolverSubsteps` | 3 | Solver substeps/frame; re-collides against the limb's interpolated position within the frame (anti-tunnelling). **Performance dial** — cost ≈ `Substeps × Iterations`. 3 is enough once a real physics asset exists; raise to 5–8 only if a fast limb still tunnels. |
| `SolverIterations` | 10 | Constraint iterations; higher = stiffer. **Performance dial** (cost ≈ `Substeps × Iterations`). 10 is the top of the node's normal range and plenty for a non-stretchy look. **Sets BOTH `NumIterations` and `MaxNumIterations`** — the latter is a hard cap defaulting to **6**, so setting `NumIterations` alone silently leaves the sim at 6. |
| `CollisionThicknessCm` | 1.0 | Added thickness of body collision shapes; larger = caught further out but a puffier/offset drape. 1.0 reads most natural (the earlier 2.0 was chasing poke-through that was really a missing physics asset). |
| `FrictionCoefficient` | 0.8 | Cloth↔body grip (slides off less when higher). |
| `bUseContinuousCollision` | true | CCD — stops fast cloth tunnelling through the thin capsule collision. |

Key points:
- **Stretch + Bending** config nodes give the cloth structural integrity. Without
  them the mesh stretches without limit and flies apart.
- **MaxDistance** anchors the garment to the body. For these weighted-value
  configs *without a weight map only `.Low` is used*, so CreateClothAsset sets
  `Low == High` (uniform). `SetWeightMap auto/file` later paints a per-vertex map
  and switches to `Low=0` (kinematic) … `High` (dynamic) — see below.
- **Collision** against the body physics asset is enabled here (thickness +
  friction + CCD). This prevents the cloth penetrating/tunnelling the body, but it
  does **not** hold an un-anchored garment up (see `all_dynamic` note).
- **Gravity + Solver** are now set explicitly (previously rode on node defaults).
  `GravityScale=0.5` controls downward drape (and decouples it from the leg-collision
  headroom that `MaxDistance` must keep large — see footguns); `SolverSubsteps=3` +
  `SolverIterations=10` give stable resolution at a performant cost (cost ≈
  substeps × iterations; the real anti-penetration fix is the physics asset, not
  brute force here). Tune in `BedlamClothDefaults`.
- **Self-Collision is intentionally NOT added** (it was the dominant cost on the
  dense ~36k-particle sim mesh, >1 min/frame). Re-add later once validated if
  self-intersection matters.

Example:
```
BedlamCloth.CreateClothAsset /Game/ClothSimulation/Clothing/garment /Game/ClothSimulation/Body/body /Game/ClothSimulation/ClothAssets/CA_garment
```

#### How skin weights vs. the MaxDistance map work (mental model)
Two *different* per-vertex datasets drive the cloth; people often conflate them:

1. **Skin weights** (built by the `TransferSkinWeights` node, copied from the body).
   Every cloth vertex gets bone weights → a **skinned target position** each frame:
   *where the vertex would be if the garment just rode the skeleton like rigid
   clothing, no simulation.* This is the "animated reference" the sim is measured
   against. For a loose skirt these weights are a torso/pelvis blend, so its skinned
   target barely follows a single swinging leg.

2. **The `MaxDistance` map** (a per-vertex float, group `SimVertices3D`, name
   `"MaxDistance"`). Per vertex, **how far the simulated cloth may move from its
   skinned target**:
   - `0` → pinned exactly to the skinned target = **kinematic** (rides the bone,
     ignores gravity/collision). This is the "stays in first-frame shape, follows the
     wrong bone" look.
   - large → free to be driven by gravity + collision, up to that many cm.

   So the sim each step = skin to the target, then let it deviate within its
   MaxDistance sphere under forces/collision, then clamp back into the sphere.

The garment is held on the body by the **kinematic (0) region**; it drapes/flows in
the **high region**; collision only matters where MaxDistance is big enough to let
the cloth move. `MaxDistanceCm` (CreateClothAsset) is the *uniform* value; a
`SetWeightMap auto/file` paints the per-vertex version (waist 0, skirt high).

**Where to see/paint the MaxDistance map in the editor:** it only EXISTS once
`SetWeightMap auto/file` has created the `BedlamWeightMap` node — until then there is
no map, just the uniform `MaxDist` node value (which is why you may only see the
skin-weights data). With the map present, open the cloth asset's `*_DF` Dataflow
(`ShowGraph` makes the nodes visible), select the `BedlamWeightMap` (or `MaxDist`)
node, and use the cloth editor's weight-map paint tool / the weight-map view
dropdown and choose `MaxDistance` to see the gradient (0 = pinned, 1/high = free) on
the sim mesh. Skin weights are viewed per-bone from the `TransferSkinWeights` node.

### 2. `BedlamCloth.SetWeightMap <ClothAssetPath> <Mode> [args]`
Controls the MaxDistance weight map on an existing cloth asset (per-vertex how far
the garment may drift from the body). Re-evaluates + saves. Idempotent /
re-runnable — `auto`/`file` create one `BedlamWeightMap` PaintWeightMap node and
reuse it on later calls.

| Mode | Args | Effect |
|---|---|---|
| `all_dynamic` | — | Uniform `Low=High=1000` (effectively unconstrained). **Removes all kinematic anchoring → the garment is free and will sag/fall off the body.** Mainly a debug state; re-pin with `auto`. (Footgun fixed: the old `Low=0/High=1000` made everything *kinematic*, the opposite.) |
| `auto` | `<BodySKMPath> [ThresholdCm] [BandCm]` | Per-vertex proximity to the body. Sim vertices within `Threshold` (default 2 cm) of the nearest body vertex are kinematic (weight 0 → `Low=0`); past `Threshold+Band` (default +8 cm) fully dynamic (weight 1 → `High=20`); linear ramp between. Logs kinematic % and mean distance. |
| `file` | `<FilePath>` | Per-vertex weights `0..1`, one per sim vertex (comma/whitespace/newline separated; clamped; padded/truncated with a warning). `Low=0 … High=20`. |

How it works: a generic `FDataflowCollectionAddScalarVertexPropertyNode`
("PaintWeightMap") is spliced into the chain just before MaxDistance and writes a
float attribute named `"MaxDistance"` into the `SimVertices3D` group — the same
name the MaxDistance config reads, so the cloth facade picks it up as a weight map.

`auto` needs the garment and body to share a coordinate space (the co-authored
test FBXs do). The diagnostic log line is the tell — a sane `meanDist` of a few cm
means they line up; a huge value (or `Kinematic≈100%`) means misalignment or a
`Threshold` that's too large.

Examples:
```
BedlamCloth.SetWeightMap /Game/ClothSimulation/ClothAssets/CA_garment auto /Game/ClothSimulation/Body/body
BedlamCloth.SetWeightMap /Game/ClothSimulation/ClothAssets/CA_garment auto /Game/ClothSimulation/Body/body 3 10
BedlamCloth.SetWeightMap /Game/ClothSimulation/ClothAssets/CA_garment file C:/weights/garment.txt
```

### 3. `BedlamCloth.ConfigureSimulation <ClothAssetPath> [Key=Value ...]`
Updates config-node properties on an existing cloth asset, then re-evaluates+saves.
Recognized keys:
| Key | Effect |
|---|---|
| `Gravity` | Gravity **scale** (Low=High) |
| `Damping` | Damping coefficient 0–1 (Low=High); raise to reduce rubbery waving |
| `Bending` | PBD bending stiffness 0–1 (Low=High); raise to reduce floppy waving |
| `Stretch` | PBD stretch stiffness 0–1 (Low=High); raise to reduce rubbery stretch |
| `Substeps` | Solver substeps (int); raise to stop limbs tunnelling through |
| `Iterations` | Solver iterations (int). Sets **both** `NumIterations` and `MaxNumIterations` — the latter (a hard cap, node default 6) clamps the former, so both must move together or the effective count stays at 6. |
| `MaxDistance` | **Weight-map-aware.** If a `SetWeightMap auto/file` map is active, sets the dynamic `High`=value and keeps the kinematic `Low=0` (preserves the painted anchoring; also recovers an asset a prior `all_dynamic` had loosened). Otherwise sets a uniform `Low=High`=value. |
| `CollisionThickness` | Added collision thickness, cm |
| `Friction` | Cloth↔body friction (Low=High); higher = grips the body more |
| `CCD` | `true`/`false` — continuous collision detection (anti-tunnelling) |
| `SelfCollision` | No-op in practice — the SelfCollision node is no longer in the default graph. |

Example:
```
BedlamCloth.ConfigureSimulation /Game/ClothSimulation/ClothAssets/CA_garment Gravity=1.0 Damping=0.1 Iterations=30
BedlamCloth.ConfigureSimulation /Game/ClothSimulation/ClothAssets/CA_garment MaxDistance=10 Friction=0.9 CCD=true
```

**Apply ALL current `BedlamClothDefaults` to an existing asset in-place** (every
knob `ConfigureSimulation` can reach). `MaxDistance=20` assumes a painted weight map
is active (the normal case for a dress) — it then sets the dynamic `High=20` and
keeps the kinematic `Low=0`. With **no** weight map, `MaxDistance` instead sets a
uniform `Low=High`, so use `MaxDistance=5` (the uniform default) for a plain,
weight-map-less asset:
```
BedlamCloth.ConfigureSimulation /Game/ClothSimulation/ClothAssets/CA_garment MaxDistance=20 Bending=0.8 Stretch=1.0 Damping=0.1 Gravity=0.5 Substeps=3 Iterations=10 CollisionThickness=1.0 Friction=0.8 CCD=true
```

### 4. `BedlamCloth.RecordChaosCache <ClothAssetPath> <BodySKMPath> <AnimPath> <OutputCachePath> [NumFrames]`
Records a `UChaosCacheCollection` of the cloth simulating on the animated body.
**Asynchronous** — it returns before the recording finishes. Poll the
`<cache>.recordstatus` marker file for completion; see
[§ Calling from Python](#calling-from-python).

`NumFrames` (optional) = number of **animation** frames to record; `0`/omitted
derives it from the animation length at 30 fps. Warmup frames are added on top.

Example:
```
BedlamCloth.RecordChaosCache /Game/ClothSimulation/ClothAssets/CA_garment /Game/ClothSimulation/Body/body /Game/ClothSimulation/Body/body_Anim /Game/ClothSimulation/Cache/CC_garment
```

### 5. `BedlamCloth.ShowGraph <ClothAssetPath>`
Makes a **script-created** cloth asset's Dataflow graph **visible/editable in the
editor**. Factory-built nodes only populate the *runtime* graph; the Dataflow asset
editor draws `UDataflowEdNode` visual counterparts, which scripts never created — so
such assets open as an **empty graph** (and log a cosmetic "no `UEdGraphNode`"
ensure on load). This command spawns the missing visual nodes (bound to the runtime
nodes by GUID), lays them out in a row, rebuilds the wiring, and saves.

`CreateClothAsset` now does this automatically, and `SetWeightMap` /
`ConfigureSimulation` keep it in sync (so the `BedlamWeightMap` PaintWeightMap node
shows up too). Use `ShowGraph` for **assets created before this existed**, or any
time a graph opens empty. It changes **no simulation data** — purely cosmetic/visual.

To inspect/paint by hand afterwards: open the cloth asset → its `*_DF` Dataflow
asset, double-click the `MaxDist` node (or the `BedlamWeightMap` node) and use the
weight-map paint tools. This is the way to hand-author a `MaxDistance` map and test
the leg-clipping fix below without the `auto` heuristic.

Example:
```
BedlamCloth.ShowGraph /Game/ClothSimulation/ClothAssets/CA_garment
```

---

## RecordChaosCache: recording lifecycle

Recording is **passive**: `AChaosCacheManager` in Record mode opens a record cache
and registers a cloth-solver post-advance callback that captures one frame per
solver advance. So the world must actually **tick and simulate the cloth** during
the window → this is driven inside **PIE** (editor-world ticking does not advance
the record-mode cloth proxy).

Sequence:
1. **Assemble scene in the editor world:** body `ASkeletalMeshActor` (mesh + anim),
   cloth actor with `UChaosClothComponent` (`SetAsset`, attached + `SetLeaderPoseComponent`
   to the body, `AddCollisionSource(body, physicsAsset)`), and an
   `AChaosCacheManager` (Record mode, `bStartOnBeginPlay`, observing the cloth).
   Create/reuse the output `UChaosCacheCollection` asset.
2. **`RequestPlaySession`** (PlayInEditor, in-process). State is held in a static
   `FBedlamRecordSession`; `PostPIEStarted`/`EndPIE` delegates drive the rest.
3. **On `PostPIEStarted`:** force a **fixed 1/30 s timestep** (`FApp::SetUseFixedTimeStep`),
   resolve the PIE-world body via `EditorUtilities::GetSimWorldCounterpartActor`,
   set up its animation **held at frame 0** (stopped), and start a frame-counting
   `FTSTicker`.
4. **Ticker (1 call = 1 frame):** hold for `BedlamWarmupFrames` (= **30**, cloth
   settles onto the body), then `Play()` the body animation; once
   `warmup + anim` frames are reached → `RequestEndPlayMap()`.
5. **On `EndPIE`:** restore the timestep and schedule a **one-tick-deferred**
   finalize that verifies `NumRecordedFrames`, saves the collection asset, destroys
   the temporary actors, and writes the `DONE`/`FAILED` line to the
   `<cache>.recordstatus` marker (the `RECORDING` line was written just before PIE
   launched; the marker was cleared at the start of the command).

Resulting cache = `30 warmup/settle frames` + `anim frames` (e.g. 30 + 130 = 160).
The warmup frames can be discarded by the post/MRQ warmup step.

### Why the design is the way it is (hard-won; do not regress)
- **Cache collection is a content asset → NOT duplicated into PIE.** Editor and
  PIE managers share it, so recorded frames survive PIE teardown. No "retain after
  EndPlay" hack is needed.
- **Fixed timestep + frame counting** (not wall-clock): PIE startup hitches
  (e.g. Python stub generation) produce multi-second frame deltas that would blow a
  time budget in one tick and also destabilize the sim. Counting frames at a fixed
  1/30 s step makes it deterministic and hitch-proof.
- **`VisibilityBasedAnimTickOption = AlwaysTickPoseAndRefreshBones`** on body and
  cloth: actors are off the default PIE camera; the default
  `OnlyTickPoseWhenRendered` freezes the pose (and the animation) when off-screen.
- **Re-issue `PlayAnimation` on the PIE-world body** in `PostPIEStarted`: the
  "playing" state set on the editor-world actor does not survive editor→PIE
  duplication (the pose ticks but stays at frame 0).
- **Defer save+cleanup one tick after `EndPIE`:** saving inside the `EndPIE`
  callback races teardown/cache-finalization and leaves the asset dirty (asterisk).
- A cosmetic `[Callstack]` ensure logged at the `LoadObject<UChaosClothAsset>` call
  site is the known "no `UEdGraphNode`" ensure from loading a script-created
  Dataflow asset — harmless; the asset loads fine.

---

## Calling from Python

Synchronous commands — safe to call back-to-back:
```python
import unreal
world = unreal.EditorLevelLibrary.get_editor_world()  # or any UWorld

def run(cmd):
    unreal.SystemLibrary.execute_console_command(world, cmd)

run("BedlamCloth.CreateClothAsset /Game/ClothSimulation/Clothing/garment "
    "/Game/ClothSimulation/Body/body /Game/ClothSimulation/ClothAssets/CA_garment")
run("BedlamCloth.ConfigureSimulation /Game/ClothSimulation/ClothAssets/CA_garment Iterations=30")
```

### ⚠️ RecordChaosCache is asynchronous — poll the status marker
`execute_console_command("BedlamCloth.RecordChaosCache ...")` **returns immediately** —
it only *requests* PIE. The actual recording runs across subsequent editor frames
and finishes when PIE ends (the deferred finalize then saves the cache). A Python
orchestrator that calls it and immediately proceeds will race the recording.

**Completion signal — a status marker file next to the cache.** The command writes
a one-line text file `<cache>.recordstatus` on disk (same folder as the cache
`.uasset`), whose content is one of:
```
RECORDING
DONE   frames=<N> duration=<sec> cache=<LongPackageName>
FAILED reason=<text>
```
Lifecycle: the marker is **deleted** at the start of every `RecordChaosCache` call,
set to `RECORDING` right before PIE launches, then flipped to `DONE`/`FAILED` by the
deferred finalize when recording ends. So:
- marker **absent** after the call returned → the command failed *synchronously*
  (bad asset path, etc.) — check the log;
- marker `RECORDING` → in progress;
- marker starts with `DONE` / `FAILED` → finished (success / failure).

Map the cache package path to the marker file: for `/Game/ClothSimulation/Cache/CC_garment`
it is `<ProjectContentDir>/ClothSimulation/Cache/CC_garment.recordstatus`
(`unreal.Paths.project_content_dir()` + the sub-path under `/Game`). The absolute
path is also logged on launch (`... Poll completion via status marker: <path>`).

> **Do NOT busy-wait from editor Python.** Python in the editor runs on the **game
> thread**; a blocking `while: sleep` loop would stop PIE from ticking and the
> recording would never progress (deadlock). Poll from a non-blocking context:

```python
import os, unreal

def marker_path(cache_pkg):  # "/Game/Foo/CC" -> "<Content>/Foo/CC.recordstatus"
    rel = cache_pkg[len("/Game/"):]
    return os.path.join(unreal.Paths.project_content_dir(), rel + ".recordstatus")

CACHE = "/Game/ClothSimulation/Cache/CC_garment"
MARKER = marker_path(CACHE)

unreal.SystemLibrary.execute_console_command(
    unreal.EditorLevelLibrary.get_editor_world(),
    f"BedlamCloth.RecordChaosCache /Game/ClothSimulation/ClothAssets/CA_garment "
    f"/Game/ClothSimulation/Body/body /Game/ClothSimulation/Body/body_Anim {CACHE}")

# Non-blocking poll via a slate post-tick callback (does not stall PIE):
_handle = None
def _poll(dt):
    global _handle
    try:
        status = open(MARKER).read().strip() if os.path.exists(MARKER) else ""
    except OSError:
        return  # mid-write; try again next tick
    if status.startswith(("DONE", "FAILED")):
        unreal.unregister_slate_post_tick_callback(_handle)
        unreal.log(f"record finished: {status}")
        # ...kick off the next record / post-step here...
_handle = unreal.register_slate_post_tick_callback(_poll)
```
An external process (outside the editor) can just `os.path.exists` / read the marker
in a normal sleep-loop — it isn't on the game thread.

All `RecordChaosCache` progress is also logged under the `LogBedlamCloth` category;
the final lines are `Record: cache '<name>' recorded <N> frames ... Saved=OK` and
`SUCCESS (Step 3): recording finished after <N> frames`.

---

## Known limitations / footguns
- **No Physics Asset → NO body collision → limbs pass straight through the cloth.**
  *(The #1 cause of "the leg passes through the dress" — found in practice.)* The
  body's collision shapes (capsules/spheres) live in its `UPhysicsAsset`. The
  `SetPhysAsset` Dataflow node must reference one. A body imported without
  `create_physics_asset` has none, so the node is empty and the simulation has
  nothing to collide against — no amount of substeps/CCD/thickness helps because
  there are no colliders. **Check:** open the cloth asset's `*_DF` Dataflow and
  confirm the `SetPhysAsset` node has a Physics Asset. **Fix:** `CreateClothAsset` /
  `RecordChaosCache` now resolve it as 4th-arg → body SKM → **auto-discovery in the
  body's folder**, and log a loud error only if none is found anywhere. So normally
  it "just works"; if auto-discovery picks the wrong one, pass the 4th arg.
- **Solver `NumIterations` is capped by `MaxNumIterations` (default 6).** Setting
  `NumIterations` (via `ConfigureSimulation Iterations=` or in the node) appears to do
  nothing — the count stays at 6 — because the node clamps it to `MaxNumIterations`,
  whose default is only 6. **Always set both** (the plugin now does). Running at 6
  iterations leaves constraints/collision under-resolved and lets limbs push through.
- **MaxDistance OVERRIDES collision.** Once collision *exists* (above), this is the
  next thing that keeps a skirt stuck. MaxDistance is a hard sphere constraint
  re-applied every solver
  substep: each particle is yanked back to within `MaxDistance` cm of its *skinned
  target* (the position from the transferred skin weights). When a fast limb pushes
  the cloth out via collision, a **small** MaxDistance instantly pulls it back —
  often onto the wrong side of the limb, since a loose garment's skinned target is
  bound (via skin-weight transfer) to the torso/pelvis, not the swinging leg. **More
  substeps/CCD/collision-thickness do NOT fix this** — the tether wins each substep.
  The fix is to give the loose region (e.g. a skirt) enough MaxDistance freedom to be
  pushed aside and drape *around* the limb:
    1. `SetWeightMap auto` so only near-body verts (the waist) stay kinematic and the
       skirt becomes dynamic;
    2. enough dynamic `High` (`BedlamClothDefaults::WeightMapDynamicHighCm`, 20;
       or `ConfigureSimulation MaxDistance=<cm>` with the weight map active).
  A large *uniform* MaxDistance would instead make the whole garment floppy — that is
  why the spatially-varying weight map matters. Stiffness/wave is then controlled
  separately by `Bending`/`Damping`, not by tightening MaxDistance. **Caveat:** once
  body collision actually works (physics asset present), `High` does NOT need to be
  large — collision keeps the skirt off the legs. Too large (e.g. 50) makes the skirt
  drift far from the body and read as stretchy/heavy and "fly away"; keep it modest
  (~15–25).
- **Collision holds nothing up.** Collision against the body physics asset is
  always on (thickness/friction/CCD), but it only prevents penetration. The thing
  that *keeps the garment on the body* is MaxDistance anchoring. `SetWeightMap
  all_dynamic` removes that anchoring, so the garment sags/falls off (collision
  can't catch a fully un-anchored shirt on coarse capsules). This is expected — for
  a wearable result keep some anchoring (`auto`/`file`, or a finite uniform
  `MaxDistance`). CCD/thickness reduce "falling through" but don't change this.
- `SetWeightMap auto` assumes the garment and body imported into the **same
  coordinate space**. Check the `meanDist` diagnostic; a huge value means
  misalignment and the weights will be meaningless.
- Self-collision is disabled by default (performance on the dense sim mesh).
- The garment is imported at full render resolution as the sim mesh (~36k
  particles) — no decimation yet; simulation cost scales with this.
- One recording at a time (guarded). A stale session (PIE that failed to start) is
  auto-cleared on the next `RecordChaosCache` call.

## Future work (ranked backlog)
Several items target the *silent-footgun* pattern that dominated tuning (empty physics
asset, iteration cap, kinematic skirt, invisible nodes).
1. **`BedlamCloth.Validate <cloth>`** — one command that checks + reports the common
   silent problems: `SetPhysAsset` populated, `MaxNumIterations ≥ NumIterations`, a
   MaxDistance weight map present, sim-mesh particle count (perf), skeleton match.
2. **Sim-mesh decimation** (biggest perf lever) — the garment is the full render mesh
   (~36k particles) as the *sim* mesh. Insert a `FChaosClothAssetRemeshNode_v2`
   (sim-only, `DensityMapSim.Low` < 100) after `TransferSkinWeights`. Note: its
   UPROPERTYs are `private` → set via FProperty reflection.
3. **Backstop weight map** — robust "keep cloth outside the body" constraint; lets
   `MaxDistance` stay small (less drape) without legs clipping.
4. **Per-garment-type presets** (dress/shirt/pants) from JSON instead of one hardcoded
   `BedlamClothDefaults`.
5. **Skin-weight transfer controls** — expose `InpaintWeights`
   `RadiusPercentage`/`NormalThreshold` to fix the hand artifact & torso-bound skirt
   at the source.
- Also: optional self-collision re-enable, expose record FPS, and integrate into the
  main BEDLAM2 pipeline (replace CLO3D GeometryCache clothing).
