# BedlamClothSetup — UE 5.7 Editor Plugin

C++ editor plugin that bridges the gap the UE Python API cannot cross for the
**Chaos Cloth** migration in BEDLAM2: authoring `ChaosClothAsset` Dataflow graphs,
configuring cloth simulation, and recording a deterministic **Chaos Cache** of a
garment simulating on an animated body.

It exposes its functionality as **editor console commands** so the rest of the
pipeline can stay Python-driven (call them with
`unreal.SystemLibrary.execute_console_command`).

> **Future Python-orchestration session: start here.** This document is the
> contract for the 4 commands you will call. Pay special attention to
> [§ Calling from Python](#calling-from-python) — `RecordChaosCache` is
> **asynchronous** (it launches PIE and returns before the recording finishes).

---

## Status (2026-06-01)

| Command | Status |
|---|---|
| `BedlamCloth.CreateClothAsset` | ✅ Working |
| `BedlamCloth.SetWeightMap` | ⚠️ `all_dynamic` works but is now a footgun (see notes); `auto`/`file` stubbed |
| `BedlamCloth.ConfigureSimulation` | ✅ Working (see MaxDistance caveat) |
| `BedlamCloth.RecordChaosCache` | ✅ Complete — PIE-based, warmup/settle frames, auto-save, auto-cleanup |

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

### 1. `BedlamCloth.CreateClothAsset <GarmentMeshPath> <BodySkeletalMeshPath> <OutputAssetPath>`
Builds a `UChaosClothAsset` (+ a sibling `UDataflow` asset at `<OutputAssetPath>_DF`)
from a garment **StaticMesh** and a body **SkeletalMesh**, evaluates the Dataflow
graph, and saves both. Sets the cloth asset's skeleton and physics asset from the body.

The Dataflow graph it builds (Collection passthrough chain):
```
Import(StaticMesh) → TransferSkinWeights(from body) → SetPhysicsAsset
  → MaxDistance(4cm) → Stretch(PBD) → Bending(PBD) → Gravity → Collision → Damping → Solver → Terminal
```
Key points:
- **Stretch + Bending** config nodes give the cloth structural integrity. Without
  them the mesh stretches without limit and flies apart.
- **MaxDistance Low=High=4cm** anchors the garment to the body (hugs within 4 cm).
  For these weighted-value configs *without a weight map, only `.Low` is used* —
  so Low and High are both set.
- **Self-Collision is intentionally NOT added** (it was the dominant cost on the
  dense ~36k-particle sim mesh, >1 min/frame). Re-add later once validated if
  self-intersection matters.

Example:
```
BedlamCloth.CreateClothAsset /Game/ClothSimulation/Clothing/garment /Game/ClothSimulation/Body/body /Game/ClothSimulation/ClothAssets/CA_garment
```

### 2. `BedlamCloth.SetWeightMap <ClothAssetPath> <Mode> [FilePath]`
Modifies the MaxDistance on an existing cloth asset.
- `all_dynamic` — sets MaxDistance Low=0, **High=1000**. ⚠️ **Footgun:** this
  contradicts the 4 cm default from `CreateClothAsset` and will make the garment
  fly off the body. Do **not** run this on assets built by the current
  `CreateClothAsset`. (Left intact for legacy/experimental use.)
- `auto` — **stubbed** (needs per-vertex body-proximity computation).
- `file` — **stubbed** (needs a WeightMapNode + per-vertex values from a file).

### 3. `BedlamCloth.ConfigureSimulation <ClothAssetPath> [Key=Value ...]`
Updates config-node properties on an existing cloth asset, then re-evaluates+saves.
Recognized keys:
| Key | Effect |
|---|---|
| `Gravity` | Gravity **scale** (Low=High) |
| `Damping` | Damping coefficient 0–1 (Low=High) |
| `Substeps` | Solver substeps (int) |
| `Iterations` | Solver iterations (int) |
| `MaxDistance` | Sets MaxDistance Low=**0**, High=value. ⚠️ With no weight map only `.Low` is used → value of 0 makes the cloth kinematic. Inconsistent with CreateClothAsset (which sets Low=High). Prefer not to use until fixed; see Future Work. |
| `CollisionThickness` | Collision thickness (float) |
| `SelfCollision` | No-op in practice — the SelfCollision node is no longer in the default graph. |

Example:
```
BedlamCloth.ConfigureSimulation /Game/ClothSimulation/ClothAssets/CA_garment Gravity=1.0 Damping=0.1 Iterations=30
```

### 4. `BedlamCloth.RecordChaosCache <ClothAssetPath> <BodySKMPath> <AnimPath> <OutputCachePath> [NumFrames]`
Records a `UChaosCacheCollection` of the cloth simulating on the animated body.
**Asynchronous** — see [§ Calling from Python](#calling-from-python).

`NumFrames` (optional) = number of **animation** frames to record; `0`/omitted
derives it from the animation length at 30 fps. Warmup frames are added on top.

Example:
```
BedlamCloth.RecordChaosCache /Game/ClothSimulation/ClothAssets/CA_garment /Game/ClothSimulation/Body/body /Game/ClothSimulation/Body/body_Anim /Game/ClothSimulation/Cache/CC_garment
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
   finalize that verifies `NumRecordedFrames`, saves the collection asset, and
   destroys the temporary actors.

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

### ⚠️ RecordChaosCache is asynchronous
`execute_console_command("BedlamCloth.RecordChaosCache ...")` **returns immediately** —
it only *requests* PIE. The actual recording runs across subsequent editor frames
and finishes when PIE ends (the deferred finalize then saves the cache). A Python
orchestrator that calls it and immediately proceeds will race the recording.

To sequence multiple recordings or post-steps, **wait for the cache to be saved /
PIE to end** before continuing, e.g.:
- Subscribe to end-of-PIE from Python: `unreal.register_python_shutdown_callback`
  is not it — use `unreal.EditorLevelLibrary`/editor tick polling, or poll
  `unreal.EditorAssetLibrary.does_asset_exist(<cache_path>)` **and** that PIE is no
  longer running, then proceed; or
- Drive one record per editor "session"/tick loop and gate the next call on the
  `SUCCESS (Step 3): recording finished ...` log / cache asset modification.

A clean future enhancement (see below) is to expose a queryable "record complete"
flag or a follow-up console command so Python can poll a single boolean.

All `RecordChaosCache` progress is logged under the `LogBedlamCloth` category; the
final lines are `Record: cache '<name>' recorded <N> frames ... Saved=OK` and
`SUCCESS (Step 3): recording finished after <N> frames`.

---

## Known limitations / footguns
- `SetWeightMap all_dynamic` and `ConfigureSimulation MaxDistance=…` both write a
  MaxDistance range that is **inconsistent** with the `CreateClothAsset` default
  (which sets Low=High=4 cm). Using them can re-break the cloth (fly-away or fully
  kinematic). Until fixed, configure MaxDistance only via `CreateClothAsset`.
- `SetWeightMap auto` / `file` are stubs (log a warning, do nothing).
- Self-collision is disabled by default (performance on the dense sim mesh).
- The garment is imported at full render resolution as the sim mesh (~36k
  particles) — no decimation yet; simulation cost scales with this.
- One recording at a time (guarded). A stale session (PIE that failed to start) is
  auto-cleared on the next `RecordChaosCache` call.

## Future work
- Make `SetWeightMap`/`ConfigureSimulation` MaxDistance set `Low == High` (consistent
  with CreateClothAsset); implement `auto`/`file` weight-map modes.
- Optional self-collision re-enable (arg or config key) + sim-mesh decimation.
- Expose record FPS and a Python-pollable "record complete" signal (today: poll the
  log/cache asset + PIE state).
- Integrate into the main BEDLAM2 pipeline (replace CLO3D GeometryCache clothing).
