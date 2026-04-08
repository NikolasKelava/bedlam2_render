# BEDLAM2 Render Pipeline

BEDLAM2 is a synthetic 3D human motion video dataset (NeurIPS 2025). This repo (`bedlam2_render`) automates rendering in **Unreal Engine** -- from data prep through import, scene composition, rendering, and post-processing.

**Target platforms:**
- **Main pipeline** (`bedlam2_render`): Windows 11, UE 5.3.2, Blender 4.0.2+, Python 3.10.6+
- **Cloth simulation** (`unreal/cloth_simulation/`): Windows 11, UE 5.7, C++ (MSVC v143), Python 3.10.6+

---

## Directory Map

```
blender/                    Blender scripts (SMPL-X .npz -> .abc / .fbx)
  smplx_anim_to_alembic/    Export GeometryCache (baked pose correctives)
  smplx_anim_to_fbx/        Export SkeletalMesh (rigged joints, for camera tracking)

config/                     Data-driven configuration
  locations/                Stage definitions (camera zones, bounds)
  shoes/                    Shoe asset configs
  vcam/                     Real-world captured camera motion data
  gender_training.csv       Body shape -> gender
  motion2outfit_training.json  Animation -> clothing outfit mapping + textures
  whitelist_animations_*.json  Per-subject approved animation lists
  whitelist_hair.json       Hair groom variants
  textures_body.txt         Available body texture list

tools/
  sequence_generation/      Generate be_seq.csv + be_camera_animations.json
  post_render_pipeline/     Post-processing (GT extraction, masks, video creation)
  animations/               Animation filtering and stats utilities

unreal/
  import/                   Import scripts (ABC, FBX, textures, HDRI, grooms, shoes)
  render/
    Core/Python/            Core UE Python scripts (LevelSequence + MRQ creation)
    remote_execution/       Batch rendering orchestration
    level_sequence_batch/   Batch LevelSequence generation
  cloth_simulation/         ** Chaos Cloth independent program (see below) **
    cloth_body_test-files/  Test FBX assets (body.fbx, garment.fbx)
    cloth_simulation/       UE 5.7 project with C++ plugin

stats/                      Motion statistics (frame counts)
```

---

## End-to-End Pipeline (Current -- CLO3D)

```
1. DATA PREP (Blender)
   .npz SMPL-X animations -> Alembic .abc (GeometryCache) or .fbx (SkeletalMesh)

2. IMPORT (Unreal Python scripts)
   Import body .abc, clothing .abc, textures, HDRIs, hair grooms, shoes

3. SEQUENCE GENERATION (Python, runs in WSL2)
   be_generate_sequences_crowd.py -> be_seq.csv (body placement, textures, clothing)
   be_generate_camera_animations.py -> be_camera_animations.json (camera keyframes)

4. LEVEL SEQUENCE CREATION (Unreal Python)
   create_level_sequences_csv.py reads CSV + JSON, spawns actors, builds LevelSequences

5. RENDERING (Unreal Movie Render Queue)
   create_movie_render_queue.py sets up MRQ jobs
   render_movie_render_queue.py or start_batch_render.py executes rendering

6. POST-PROCESSING (bash/Python)
   Extract camera GT from EXR, segmentation masks, depth maps, create MP4s
```

---

## How Bodies Work

- **SMPL-X** body model (locked head, neutral, UV 2023) with per-subject baked shapes
- Animations stored as `.npz` files (poses + trans at mocap framerate, downsampled to 30fps)
- Blender exports to **Alembic .abc** with pose correctives baked into geometry
- Imported into UE as **GeometryCache** assets at `/Engine/PS/Bedlam/SMPLX_LH/{subject}/`
- Coordinate conversion: Blender meters (Y-up) -> UE centimeters, scale `[100, -100, 100]`, rotation `[90, 0, 0]`
- Body frame range starts at frame 1 (Blender convention)
- FBX SkeletalMesh path used only for camera joint-tracking, rendered with hidden material

## How Clothing Works (Current System -- CLO3D GeometryCache)

**This system is being replaced by Chaos Cloth (see sections below).**

### Current Pipeline
1. Clothing is **pre-simulated externally in CLO3D** (not in this codebase)
2. Exported as Alembic `.abc` with 100 warmup frames + N animation frames
3. Imported via `unreal/import/import_abc_clothing.py` as **GeometryCache** with `frame_start=101` (skips warmup)
4. Stored at `/Engine/PS/Bedlam/Clothing/{subject}/{subject}_{anim}_clo`
5. In scenes, clothing is a **separate GeometryCacheActor** placed at the same world position as the body
6. Both body and clothing get independent `MovieSceneGeometryCacheTrack` entries in the LevelSequence
7. Clothing materials created from master material `/Engine/PS/Bedlam/Core/Materials/M_Clothing` with diffuse + normal texture parameters

### Key Clothing Code Paths
| File | Role |
|------|------|
| `unreal/import/import_abc_clothing.py` | Import CLO3D .abc as GeometryCache (frame_start=101) |
| `unreal/import/import_abc_clothing_batch.py` | Batch import with multiprocessing |
| `unreal/import/import_clothing_textures.py` | Import diffuse/normal textures, create MaterialInstances |
| `unreal/import/outfits/create_outfit_materials.py` | Create outfit material variants |
| `config/motion2outfit_training.json` | Maps (subject, animation) -> outfit name -> texture list |
| `tools/sequence_generation/be_generate_sequences_crowd.py` | Selects random clothing textures during sequence generation |
| `unreal/render/Core/Python/create_level_sequences_csv.py` | `add_geometry_cache()` spawns clothing GeometryCacheActor, `SequenceBody` dataclass holds clothing_path |

### Clothing-Body Relationship
- **No skeletal attachment** -- clothing and body are independent GeometryCache meshes at the same transform
- Both use `manual_tick=True` and `looping=False` for proper temporal sampling
- Layer naming convention: `be_actor_XX_body`, `be_actor_XX_clothing`, `be_actor_XX_hair` (for segmentation masks)
- Alternative approach exists: `BE_ClothingOverlayActor` blueprint applies clothing as texture overlay on body geometry (no separate mesh)

### Clothing Assignment Flow
```
motion2outfit_training.json["motions"][subject][animation] -> outfit_name
motion2outfit_training.json["textures"][outfit_name] -> [texture_01, texture_02, ...]
Random texture selected -> stored in be_seq.csv comment field as texture_clothing=outfit_texture
create_level_sequences_csv.py loads GeometryCache and MaterialInstance for that outfit/texture combo
```

### Known UE Bug
Importing with Python using `frame_start=101, frame_end=0` produces an invalid last frame (duplicates start frame). The GUI import doesn't have this issue. See comment in `import_abc_clothing.py:69-72`.

## How Hair Works

- Strand-based groom assets (VineFX) at `/Engine/PS/Bedlam/Hair/VineFX/`
- Per-body **GroomBindingAsset** created via `create_groom_bindings.py` (binds groom to specific body shape)
- Attached as `GroomComponent` child of the body's GeometryCacheActor via `SubobjectDataSubsystem`
- Hair color via material instances at `/Engine/PS/Bedlam/Core/Materials/Hair/MI_Hair_{color}`

## LevelSequence Structure

Each rendered sequence contains:
- **Body**: GeometryCacheActor (spawnable) with GeometryCacheTrack + TransformTrack
- **Clothing**: Separate GeometryCacheActor or BE_ClothingOverlayActor (spawnable)
- **Hair**: GroomComponent attached to body actor
- **SkeletalMesh** (optional): Hidden, used only for camera joint tracking
- **Camera system**: BE_CameraRoot -> BE_CameraOperator (SpringArm) -> CineCameraActor + BE_CameraTarget
- **HDRI**: Template-based environment lighting
- **Warmup**: 10 frames rendered with negative numbers, deleted in post-processing
- **Frame rate**: 30fps hardcoded

Key function: `add_geometry_cache()` in `create_level_sequences_csv.py:85` handles spawning, material assignment, groom attachment, layer assignment, and track creation.

## Movie Render Queue

- Primary pass: PNG + optional EXR, 7 temporal samples (motion blur), DX12 rasterizer
- Depth/mask pass: EXR, 1 sample (no motion blur), Cryptomatte segmentation
- 32 engine warmup frames for Lumen, 10 rendered warmup frames
- Batch rendering launches separate UE editor instances per batch to manage memory
- Camera GT stored in EXR metadata via custom `BE_MRQ` plugin

## Unreal Python API Patterns

All UE automation uses the `unreal` Python module (Editor Script Plugin). Key patterns:
- Asset loading: `unreal.EditorAssetLibrary.load_asset(path)` or `unreal.load_asset(path)`
- Actor spawning: `unreal.get_editor_subsystem(unreal.EditorActorSubsystem).spawn_actor_from_class(...)`
- Spawnable actors: `level_sequence.add_spawnable_from_instance(actor)` then destroy template
- Track channels: `[0]=X, [1]=Y, [2]=Z, [3]=Roll, [4]=Pitch, [5]=Yaw`
- Import: `unreal.AssetImportTask()` + `unreal.AssetToolsHelpers.get_asset_tools().import_asset_tasks([task])`

## Conventions

- UE asset paths use forward slashes: `/Engine/PS/Bedlam/...`
- Body assets: `{subject}_{animation_id}` (e.g., `it_4170_M_2000`)
- Clothing assets: `{subject}_{animation_id}_clo` suffix
- Subject naming: `{prefix}_{id}_{size}` (e.g., `it_4170_M`, `gr_christine_021_3XL`)
- Coordinate system: Blender Y-up meters -> UE Z-up centimeters
- MOYO dataset is special-cased (no simulated clothing)

---

# Chaos Cloth Migration: CLO3D GeometryCache -> UE Chaos Cloth + Chaos Caching

The goal is to replace the external CLO3D simulation + GeometryCache import pipeline with Unreal Engine's native **Chaos Cloth** simulation, cached via the **Chaos Caching** system for deterministic MRQ playback.

## UE Python API Limitations (UE 5.7)

The UE Python API (`unreal` module) exposes runtime/component operations but **does NOT expose the authoring layer** needed for Chaos Cloth setup. This is the reason a C++ plugin is required.

### What Python CAN Do
| Operation | API |
|-----------|-----|
| Import FBX as SkeletalMesh/StaticMesh | `AssetImportTask` + `FbxImportUI` |
| Create `ChaosClothAsset` (empty shell) | `ChaosClothAssetFactory` |
| Set `physics_asset`, `skeleton`, `materials` on a ChaosClothAsset | Direct property access |
| Spawn `ChaosClothComponent` on an actor | Standard component API |
| Bind cloth component to leader skeleton | `set_leader_pose_component()`, `bind_cloth_to_leader_pose_component()` |
| Add collision sources to cloth | `add_cloth_collision_source(skeletal_mesh_comp, physics_asset)` |
| Tune runtime solver parameters | `ClothingSimulationInteractor` (substeps, iterations, stiffness) |
| Enable/disable simulation, set blend weight | `ChaosClothComponent` properties |
| Spawn `ChaosCacheManager` actor | Standard actor spawning |
| Set Chaos Cache mode (RECORD/PLAY) | `CacheMode` enum property |
| Create LevelSequence, add tracks, set up MRQ | Full `MovieScene*` API |

### What Python CANNOT Do (Requires C++)
| Operation | Why |
|-----------|-----|
| **Build the Dataflow graph** inside a ChaosClothAsset | Dataflow is the core of the asset -- defines simulation mesh, render mesh, all cloth properties. Dataflow node creation/connection is editor-only C++ API with no Python bindings |
| **Generate simulation mesh** from a source StaticMesh/SkeletalMesh | This is a Dataflow node operation (`ChaosClothAssetTerminalNode`, `ImportNode`, `TransferSkinWeightsNode`) |
| **Paint or assign cloth weight maps** (max distance, backstop, etc.) | Weight data lives inside Dataflow node attributes; `ChaosClothAssetInteractor` only tunes runtime values, not authored weight maps |
| **Configure cloth simulation properties at asset level** | Stiffness, damping, collision thickness, self-collision, etc. are Dataflow node properties -- the runtime interactor offers limited subset only |
| **Transfer skin weights** from body SkeletalMesh to cloth | `TransferSkinWeightsNode` is a Dataflow node |
| **Record a Chaos Cache** programmatically from editor (trigger play + capture) | `ChaosCacheManager` recording requires simulation ticking in a controlled play session; no "bake in editor" Python command exists |

### Consequence
A **C++ Editor plugin** (`BedlamClothSetup`) is required to bridge the gap. It performs all Dataflow/authoring operations in C++ and exposes them as **editor console commands** callable from Python for pipeline orchestration.

---

## Independent Cloth Simulation Program

**Location:** `unreal/cloth_simulation/`

This is a standalone program that takes one body FBX + one garment FBX, sets up Chaos Cloth simulation, and records the result via Chaos Caching. It serves as a **reference implementation** for future integration into the full BEDLAM2 pipeline.

### UE Project

- **Path:** `unreal/cloth_simulation/cloth_simulation/cloth_simulation.uproject`
- **Engine:** UE 5.7
- **Existing game module:** `cloth_simulation` (default scaffolding, Source/cloth_simulation/)
- **Plugin to create:** `Plugins/BedlamClothSetup/` (Editor-only plugin)

### Required UE Plugins (must be enabled in .uproject)

| Plugin | Purpose |
|--------|---------|
| `ChaosClothAsset` | Core Chaos Cloth asset types, Dataflow nodes |
| `ChaosClothAssetEditor` | Editor tools, Dataflow node registration |
| `ChaosCloth` | Chaos Cloth simulation runtime |
| `ChaosCaching` | Simulation recording/playback (`ChaosCacheManager`) |
| `ChaosCachingEditor` | Editor support for cache recording, Take Recorder integration |
| `GeometryCache` | GeometryCache asset type (body playback) |
| `PythonScriptPlugin` | Python editor scripting |
| `EditorScriptingUtilities` | `EditorAssetLibrary`, etc. |

### Test Files

```
unreal/cloth_simulation/cloth_body_test-files/
  body/body.fbx          Body SkeletalMesh with skeleton + animation
  clothing/garment.fbx   Clothing garment mesh (Static Mesh)
```

### Architecture: C++ Plugin + Python Orchestration

The program uses **two layers**:

1. **C++ Plugin (`BedlamClothSetup`)** -- performs everything the Python API cannot:
   - Creates ChaosClothAsset with a fully configured Dataflow graph
   - Transfers skin weights from body to cloth
   - Assigns weight maps (auto-paint all vertices as dynamic, or load from file)
   - Configures simulation parameters (gravity, stiffness, damping, collision)
   - Triggers Chaos Cache recording of a simulation run

2. **Python script** -- orchestrates the pipeline by calling C++ console commands:
   - Imports FBX files (body SkeletalMesh, garment StaticMesh) using `AssetImportTask`
   - Calls C++ commands for cloth asset creation and simulation setup
   - Calls C++ command to trigger cache recording
   - (Optional) Sets up LevelSequence with the cached result for preview

This split keeps the architecture consistent with bedlam2_render's existing Python-driven pipeline and ensures the C++ plugin is reusable when integrated into the main project.

### C++ Plugin: `BedlamClothSetup`

#### Plugin Structure
```
Plugins/BedlamClothSetup/
  BedlamClothSetup.uplugin
  Source/BedlamClothSetup/
    BedlamClothSetup.Build.cs
    Public/
      BedlamClothSetup.h           Module definition
      BedlamClothSetupCommands.h   Console command declarations
    Private/
      BedlamClothSetup.cpp         Module startup, command registration
      BedlamClothSetupCommands.cpp Command implementations
```

#### Module Dependencies (Build.cs)
The plugin needs access to these UE modules:
```
PublicDependencyModuleNames:
  "Core", "CoreUObject", "Engine", "UnrealEd"

PrivateDependencyModuleNames:
  "ChaosClothAsset",          // UChaosClothAsset, Dataflow nodes
  "ChaosClothAssetEngine",    // Runtime cloth types
  "ChaosClothAssetTools",     // Cloth asset authoring tools
  "Dataflow",                 // FDataflowGraph, FDataflowNode
  "DataflowCore",             // Core Dataflow types
  "DataflowEngine",           // Dataflow evaluation
  "DataflowEditor",           // Editor Dataflow operations
  "ChaosCaching",             // FChaosCacheManager, recording
  "SkeletalMeshUtilitiesCommon", // Skin weight transfer
  "MeshDescription",          // FMeshDescription for mesh access
  "StaticMeshDescription",    // Static mesh to mesh description
  "ClothingSystemRuntimeCommon", // Base clothing types
  "PhysicsCore"               // Physics asset types
```

Note: The exact module names may vary in UE 5.7. Check `Engine/Plugins/ChaosClothAsset/` and `Engine/Source/Runtime/Experimental/Chaos/` for actual module names. Use UE's `UnrealBuildTool` error messages to resolve missing modules.

#### Console Commands to Implement

The plugin registers these editor console commands (callable from Python via `unreal.SystemLibrary.execute_console_command()` or `unreal.EditorLevelLibrary.editor_execute_command()`):

**1. `BedlamCloth.CreateClothAsset`**
```
BedlamCloth.CreateClothAsset <GarmentMeshPath> <BodySkeletalMeshPath> <OutputAssetPath>

Example:
BedlamCloth.CreateClothAsset /Game/ClothSimulation/Clothing/garment /Game/ClothSimulation/Body/body /Game/ClothSimulation/ClothAssets/CA_garment
```
What it does:
- Loads the garment StaticMesh (or SkeletalMesh) and body SkeletalMesh
- Creates a new `UChaosClothAsset` at the output path
- Builds a Dataflow graph with nodes:
  - **Import node**: Imports the garment mesh as the simulation and render mesh
  - **TransferSkinWeights node**: Transfers skin weights from body SKM to garment
  - **SimulationConfig node**: Sets simulation parameters (gravity, stiffness, damping, collision thickness, self-collision)
  - **Terminal node**: Connects everything to produce the final cloth asset
- Sets the physics asset (from the body SKM) for collision
- Sets the skeleton reference (from the body SKM)
- Evaluates the Dataflow to generate the cloth asset data
- Saves the asset

**2. `BedlamCloth.SetWeightMap`**
```
BedlamCloth.SetWeightMap <ClothAssetPath> <Mode> [FilePath]

Modes:
  "all_dynamic"  - All vertices are fully dynamic (max distance = large value)
  "auto"         - Vertices near body attachment points are kinematic, rest dynamic
  "file"         - Load weight map from external file (CSV or JSON vertex weights)

Example:
BedlamCloth.SetWeightMap /Game/ClothSimulation/ClothAssets/CA_garment all_dynamic
BedlamCloth.SetWeightMap /Game/ClothSimulation/ClothAssets/CA_garment file C:/weights/garment_weights.csv
```
What it does:
- Loads the ChaosClothAsset
- Modifies the Dataflow graph's weight map node(s)
- For `all_dynamic`: sets MaxDistance to a large value for all vertices
- For `auto`: computes distance from each cloth vertex to nearest body vertex; vertices within threshold get MaxDistance=0 (kinematic), others get large MaxDistance
- For `file`: reads per-vertex MaxDistance values from the file
- Re-evaluates the Dataflow and saves

**3. `BedlamCloth.ConfigureSimulation`**
```
BedlamCloth.ConfigureSimulation <ClothAssetPath> [Key=Value ...]

Example:
BedlamCloth.ConfigureSimulation /Game/ClothSimulation/ClothAssets/CA_garment Gravity=-980 Stiffness=0.8 Damping=0.01 CollisionThickness=1.0 SelfCollision=true Substeps=2
```
What it does:
- Loads the ChaosClothAsset
- Updates simulation parameters in the Dataflow graph's config node(s)
- Re-evaluates and saves

**4. `BedlamCloth.RecordChaosCache`**
```
BedlamCloth.RecordChaosCache <ClothAssetPath> <BodySkeletalMeshPath> <AnimationAssetPath> <OutputCacheName> [NumFrames]

Example:
BedlamCloth.RecordChaosCache /Game/ClothSimulation/ClothAssets/CA_garment /Game/ClothSimulation/Body/body /Game/ClothSimulation/Body/body_Anim /Game/ClothSimulation/Cache/CC_garment 300
```
What it does:
- Spawns a temporary SkeletalMeshActor for the body with the animation
- Spawns a ChaosClothComponent actor with the cloth asset, bound to the body via `SetLeaderPoseComponent`
- Adds collision source (body SKM + physics asset)
- Creates a `ChaosCacheManager` actor in RECORD mode
- Steps through the animation frame by frame, ticking the simulation and recording each frame to the Chaos Cache
- Saves the recorded cache asset
- Cleans up temporary actors

### Python Orchestration Script

**Location:** `unreal/cloth_simulation/run_cloth_pipeline.py`

This script runs inside the UE editor (via Python Editor Script Plugin) and orchestrates the full pipeline:

```python
# Pseudo-code outline -- actual implementation in next session

import unreal

# === CONFIGURATION ===
BODY_FBX = r"C:\...\cloth_body_test-files\body\body.fbx"
GARMENT_FBX = r"C:\...\cloth_body_test-files\clothing\garment.fbx"
UE_ROOT = "/Game/ClothSimulation"

# === STEP 1: IMPORT FBX FILES (Python can do this) ===
# Import body.fbx as SkeletalMesh with animation
# Import garment.fbx as StaticMesh
# Result: /Game/ClothSimulation/Body/body, /Game/ClothSimulation/Clothing/garment

# === STEP 2: CREATE CLOTH ASSET (C++ command) ===
# Calls: BedlamCloth.CreateClothAsset <garment_path> <body_path> <output_path>

# === STEP 3: SET WEIGHT MAP (C++ command) ===
# Calls: BedlamCloth.SetWeightMap <cloth_path> all_dynamic

# === STEP 4: CONFIGURE SIMULATION (C++ command, optional) ===
# Calls: BedlamCloth.ConfigureSimulation <cloth_path> Gravity=-980 ...

# === STEP 5: RECORD CHAOS CACHE (C++ command) ===
# Calls: BedlamCloth.RecordChaosCache <cloth_path> <body_path> <anim_path> <cache_path> <num_frames>

# === DONE: Chaos Cache asset is ready for MRQ playback ===
```

### Key UE C++ Headers to Reference

When implementing the C++ plugin, these are the critical headers:

| Header | Purpose |
|--------|---------|
| `ChaosClothAsset/ChaosClothAsset.h` | `UChaosClothAsset` class |
| `Dataflow/DataflowGraph.h` | `FDataflowGraph`, node creation |
| `Dataflow/DataflowNode.h` | `FDataflowNode` base, node connections |
| `Dataflow/DataflowNodeFactory.h` | Creating nodes by type name |
| `ChaosClothAsset/ClothAssetDataflowNodes.h` | Cloth-specific Dataflow nodes (import, sim config, terminal) |
| `ChaosClothAsset/ClothAssetBuilderEditor.h` | Asset building utilities |
| `ChaosCaching/ChaosCacheManager.h` | `AChaosCacheManager`, recording API |
| `ChaosCaching/ChaosCacheCollection.h` | Cache storage |
| `Engine/SkeletalMesh.h` | `USkeletalMesh`, skin weight access |
| `Engine/StaticMesh.h` | `UStaticMesh` |
| `Engine/PhysicsAsset.h` | `UPhysicsAsset` |
| `ClothingSystemRuntimeCommon/ClothingAssetBase.h` | Base clothing types |

Note: Exact header paths may differ in UE 5.7. Use IDE header search or compile errors to locate them. The Chaos Cloth Dataflow system is under `Engine/Plugins/ChaosClothAsset/`.

### Dataflow Graph Structure for Cloth Asset

The Dataflow graph inside a ChaosClothAsset defines the complete cloth setup. The minimal graph needed:

```
[Import Node] --> [Transfer Skin Weights Node] --> [Set Simulation Config Node] --> [Terminal Node]
     |                      |
     |                 Body SKM ref
     |
  Garment mesh (SM or SKM)
```

**Import Node** (`FChaosClothAssetImportNode` or similar):
- Source: the garment mesh (StaticMesh or SkeletalMesh asset reference)
- Outputs: simulation mesh + render mesh geometry

**Transfer Skin Weights Node** (`FChaosClothAssetTransferSkinWeightsNode`):
- Input: simulation mesh from Import Node + body SkeletalMesh reference
- Method: `CLOSEST_POINT_ON_SURFACE` or `INPAINT_WEIGHTS` (see `ChaosClothAssetTransferSkinWeightsMethod` enum)
- Target: `SIMULATION` or `ALL` (see `ChaosClothAssetTransferTargetMeshType` enum)
- Output: mesh with skin weights

**Simulation Config Node**:
- Sets physics properties: gravity scale, stiffness, damping, collision thickness, self-collision
- Sets max distance (weight map) -- can be uniform or per-vertex

**Terminal Node** (`FChaosClothAssetTerminalNode`):
- Connects all inputs to produce the final evaluated cloth asset
- References: physics asset, skeleton

### Chaos Caching Workflow (C++ Implementation)

The recording flow in C++:

1. **Setup scene**: Spawn body SKM actor + cloth actor in a temporary world or sublevel
2. **Attach cloth to body**: `ChaosClothComponent->SetLeaderPoseComponent(BodySKMComponent)`
3. **Add collision**: `ChaosClothComponent->AddClothCollisionSource(BodySKMComponent, PhysicsAsset)`
4. **Create ChaosCacheManager**: Set to `ECacheMode::Record`, configure cache name
5. **Play animation**: Step through body animation frame by frame, calling `World->Tick()` to advance simulation
6. **Stop recording**: Finalize the cache, save as asset
7. **Cleanup**: Destroy temporary actors

The resulting cache asset can be played back deterministically during MRQ rendering by setting `ECacheMode::Play`.

---

## Integration into BEDLAM2 Pipeline (Future)

After the independent program is validated, the `BedlamClothSetup` plugin integrates into the main BEDLAM2 rendering pipeline. The main BEDLAM2 UE project will need to be upgraded to **UE 5.7** (or whichever version the plugin targets).

### New End-to-End Pipeline (Chaos Cloth)

```
1. DATA PREP (Blender) -- unchanged
   .npz SMPL-X animations -> .abc (body GeometryCache) + .fbx (body SkeletalMesh)

2. IMPORT (Unreal Python scripts)
   Import body .abc (GeometryCache, unchanged)
   Import body .fbx (SkeletalMesh, already exists for camera tracking -- now also drives cloth)
   Import garment .fbx as StaticMesh (NEW -- replaces CLO3D .abc import)
   Import clothing textures (unchanged)

3. CLOTH SIMULATION (BedlamClothSetup C++ plugin, called from Python)
   For each (subject, animation, outfit):
     a. BedlamCloth.CreateClothAsset  garment_SM  body_SKM  -> ChaosClothAsset
     b. BedlamCloth.SetWeightMap      cloth_asset  <mode>
     c. BedlamCloth.ConfigureSimulation cloth_asset <params>
     d. BedlamCloth.RecordChaosCache  cloth_asset body_SKM anim -> ChaosCacheAsset

4. SEQUENCE GENERATION (Python, runs in WSL2) -- unchanged
   be_seq.csv + be_camera_animations.json

5. LEVEL SEQUENCE CREATION (Unreal Python) -- modified
   create_level_sequences_csv.py changes:
   - Clothing actor: ChaosClothComponent actor (replaces GeometryCacheActor)
   - Playback: ChaosCacheManager in PLAY mode (replaces GeometryCacheTrack)
   - Material assignment: on SkeletalMeshComponent/ChaosClothComponent (replaces GeometryCacheComponent)
   - Layer naming: be_actor_XX_clothing (unchanged convention, different actor type)
   - Body actor: GeometryCacheActor (unchanged)

6. RENDERING (MRQ) -- minor changes
   - Chaos Cache playback is deterministic (same as GeometryCache)
   - Warmup: cloth simulation warmup handled by Chaos Cache (pre-baked), so existing
     32 engine + 10 rendered warmup frames remain sufficient for Lumen/raytracing only
   - Temporal samples, DX12, Cryptomatte segmentation: unchanged

7. POST-PROCESSING -- unchanged
```

### What Changes in the Main Pipeline
| Component | Current (CLO3D) | New (Chaos Cloth) |
|-----------|----------------|-------------------|
| External sim tool | CLO3D (separate app) | None (UE-native) |
| Clothing import | `.abc` GeometryCache import | `.fbx` StaticMesh import |
| Clothing import script | `import_abc_clothing.py` | New script using `AssetImportTask` for SM |
| Cloth setup | N/A (pre-simulated) | `BedlamClothSetup` C++ plugin commands |
| Clothing in LevelSequence | `GeometryCacheActor` + `GeometryCacheTrack` | `ChaosClothComponent` actor + `ChaosCacheManager` (PLAY mode) |
| Material target | `GeometryCacheComponent.set_material()` | `ChaosClothComponent` material slots |
| Warmup frames | 100 CLO3D warmup (skipped on import) | Baked into Chaos Cache during recording step |

### What Stays the Same
- Body pipeline (GeometryCache from Blender ABC)
- Sequence generation (`be_seq.csv` format, clothing texture selection via `motion2outfit_training.json`)
- Camera system (BE_CameraRoot, SpringArm, CineCameraActor)
- Rendering pipeline (MRQ, PNG/EXR passes, temporal samples)
- Post-processing (GT extraction, segmentation masks, depth, MP4)
- Hair/groom system
- Segmentation mask layer naming convention
- Batch rendering orchestration

---

## Unreal Python API Patterns

All UE automation uses the `unreal` Python module (Editor Script Plugin). Key patterns:
- Asset loading: `unreal.EditorAssetLibrary.load_asset(path)` or `unreal.load_asset(path)`
- Actor spawning: `unreal.get_editor_subsystem(unreal.EditorActorSubsystem).spawn_actor_from_class(...)`
- Spawnable actors: `level_sequence.add_spawnable_from_instance(actor)` then destroy template
- Track channels: `[0]=X, [1]=Y, [2]=Z, [3]=Roll, [4]=Pitch, [5]=Yaw`
- Import: `unreal.AssetImportTask()` + `unreal.AssetToolsHelpers.get_asset_tools().import_asset_tasks([task])`
- Console commands from Python: `unreal.SystemLibrary.execute_console_command(world, "BedlamCloth.CreateClothAsset ...")` or via editor utility

### FBX Import Pattern (for cloth pipeline)
```python
task = unreal.AssetImportTask()
task.filename = r"C:\path\to\mesh.fbx"
task.destination_path = "/Game/ClothSimulation/Body"
task.destination_name = "body"
task.replace_existing = True
task.automated = True
task.save = True

options = unreal.FbxImportUI()
options.import_mesh = True
options.import_as_skeletal = True       # True for body, False for garment SM
options.import_animations = True        # True for body, False for garment
options.import_materials = False
options.import_textures = False
options.create_physics_asset = True     # Auto-generate physics asset for body
options.skeleton = None                 # Or reference existing skeleton for re-import
task.options = options

unreal.AssetToolsHelpers.get_asset_tools().import_asset_tasks([task])
```
