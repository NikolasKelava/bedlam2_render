# BEDLAM2 Render Pipeline

BEDLAM2 is a synthetic 3D human motion video dataset (NeurIPS 2025). This repo (`bedlam2_render`) automates rendering in **Unreal Engine 5.3.2** -- from data prep through import, scene composition, rendering, and post-processing.

**Target platform:** Windows 11, UE 5.3.2, Blender 4.0.2+, Python 3.10.6+

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

stats/                      Motion statistics (frame counts)
```

## End-to-End Pipeline

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

## How Bodies Work

- **SMPL-X** body model (locked head, neutral, UV 2023) with per-subject baked shapes
- Animations stored as `.npz` files (poses + trans at mocap framerate, downsampled to 30fps)
- Blender exports to **Alembic .abc** with pose correctives baked into geometry
- Imported into UE as **GeometryCache** assets at `/Engine/PS/Bedlam/SMPLX_LH/{subject}/`
- Coordinate conversion: Blender meters (Y-up) -> UE centimeters, scale `[100, -100, 100]`, rotation `[90, 0, 0]`
- Body frame range starts at frame 1 (Blender convention)
- FBX SkeletalMesh path used only for camera joint-tracking, rendered with hidden material

## How Clothing Works (Current System -- CLO3D GeometryCache)

**This is the system targeted for migration to Chaos Cloth.**

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

## Migration Context: CLO3D GeometryCache -> UE Chaos Cloth

The goal is to replace the external CLO3D simulation + GeometryCache import pipeline with Unreal Engine's native **Chaos Cloth** simulation system. Key considerations:

### What Changes
- **Remove**: CLO3D external simulation step, ABC clothing import (`import_abc_clothing.py`), clothing GeometryCache playback
- **Add**: Chaos Cloth simulation setup on garment SkeletalMeshes driven by the body animation
- **Modify**: `create_level_sequences_csv.py` to use cloth-simulated SkeletalMesh actors instead of GeometryCacheActors for clothing
- **Modify**: Material assignment (currently applied to GeometryCacheComponent, would need to go on SkeletalMeshComponent)

### What Stays the Same
- Body pipeline (GeometryCache from Blender ABC)
- Sequence generation (`be_seq.csv` format, clothing texture selection)
- Camera system, rendering pipeline, post-processing
- Hair/groom system
- Segmentation mask layer naming (just different actor type)

### Key Technical Challenges
- Chaos Cloth requires a SkeletalMesh with cloth simulation data (paint weights, config) -- garments need to be set up as SkeletalMeshes bound to the body skeleton
- Body is GeometryCache (no skeleton) -- Chaos Cloth typically drives off a SkeletalMesh. May need the body's SkeletalMesh (currently hidden, used only for camera tracking) to drive cloth, or investigate GeometryCache-to-cloth binding
- CLO3D warmup frames (100 frames) would be replaced by UE cloth simulation warmup (currently 10 rendered + 32 engine warmup frames exist)
- Per-frame temporal sampling with `manual_tick=True` needs to work correctly with cloth sim
- Garment topology and UV mapping must be preserved for existing texture/material system
