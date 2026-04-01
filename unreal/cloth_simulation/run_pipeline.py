# Chaos Cloth Simulation Pipeline - Main Orchestrator
#
# Runs the full cloth simulation pipeline end-to-end:
#   1. Import body SkeletalMesh, physics asset, and clothing Static Mesh
#   2. Set up Chaos Cloth simulation on the clothing
#   3. Create a LevelSequence with cloth-simulated actors
#
# This is a simplified, single-garment version of the BEDLAM2 render pipeline,
# designed as a reference for how to set up Chaos Cloth in Unreal Engine 5.3.2
# via Python scripting.
#
# Usage:
#   1. Configure paths in config.py
#   2. Run this script in Unreal Editor: File -> Execute Python Script
#      Or via command line: UE5Editor.exe <project> -ExecutePythonScript="<path>/run_pipeline.py"
#
# Required UE plugins:
#   - Python Editor Script Plugin
#   - Editor Scripting Utilities
#   - Sequencer Scripting
#   - Chaos Cloth (enabled by default in UE 5.3+)
#
# Pipeline overview:
#
#   +-----------+     +----------------+     +------------------+
#   |  IMPORT   | --> | CLOTH SETUP    | --> | LEVEL SEQUENCE   |
#   |           |     |                |     |                  |
#   | Body FBX  |     | Create cloth   |     | Body SKM actor   |
#   | Clothing  |     | asset from SM  |     | + AnimSequence   |
#   | SM/FBX    |     | Transfer skin  |     | + cloth sim      |
#   | Physics   |     | weights        |     | Camera           |
#   | asset     |     | Configure sim  |     |                  |
#   +-----------+     | params         |     +------------------+
#                     | Set physics    |
#                     | asset          |
#                     +----------------+
#
# Differences from the main BEDLAM2 pipeline:
#   - Body uses SkeletalMesh (not GeometryCache) because Chaos Cloth needs a skeleton
#   - Clothing is a cloth-enabled section on the body SKM (not a separate GeometryCacheActor)
#   - No sequence generation CSV -- single body + single garment
#   - No batch rendering orchestration -- single LevelSequence output
#   - No post-processing pipeline
#
# For production integration with BEDLAM2:
#   - The body GeometryCache would still be used for the rendered body mesh
#   - The body SkeletalMesh (currently hidden, used for camera tracking) would
#     additionally drive the Chaos Cloth simulation
#   - The cloth-simulated clothing would replace the CLO3D GeometryCache clothing
#   - Cloth simulation warmup would replace the CLO3D 100-frame warmup

import time
import unreal

# Import pipeline modules (must be on sys.path or in same directory)
from import_assets import import_all
from setup_cloth_simulation import create_clothing_asset_on_skeletal_mesh
from create_cloth_level_sequence import create_level_sequence


def run_pipeline():
    """
    Execute the full Chaos Cloth simulation pipeline.
    """
    unreal.log("=" * 60)
    unreal.log("CHAOS CLOTH SIMULATION PIPELINE")
    unreal.log("=" * 60)

    pipeline_start = time.perf_counter()

    # ==================================================================
    # Step 1: Import assets
    # ==================================================================
    unreal.log("")
    unreal.log("STEP 1/3: Importing assets")
    unreal.log("-" * 40)

    import_results = import_all()
    if import_results is None:
        unreal.log_error("Pipeline aborted: asset import failed")
        return False

    body_skm_path = import_results["body_skm_path"]
    anim_sequence_path = import_results.get("anim_sequence_path")
    physics_asset_path = import_results.get("physics_asset_path")
    clothing_sm_path = import_results["clothing_sm_path"]

    # ==================================================================
    # Step 2: Set up Chaos Cloth simulation
    # ==================================================================
    unreal.log("")
    unreal.log("STEP 2/3: Setting up Chaos Cloth simulation")
    unreal.log("-" * 40)

    cloth_asset_name = create_clothing_asset_on_skeletal_mesh(
        body_skm_path=body_skm_path,
        clothing_sm_path=clothing_sm_path,
        physics_asset_path=physics_asset_path,
    )

    if cloth_asset_name is None:
        unreal.log_warning(
            "Cloth setup requires manual intervention (see warnings above). "
            "After completing manual setup, re-run this script or run "
            "create_cloth_level_sequence.py directly."
        )
        # Continue to create the level sequence anyway -- the cloth simulation
        # will just not be active until the cloth asset is properly set up.

    # ==================================================================
    # Step 3: Create LevelSequence
    # ==================================================================
    unreal.log("")
    unreal.log("STEP 3/3: Creating LevelSequence")
    unreal.log("-" * 40)

    sequence_path = create_level_sequence(
        body_skm_path=body_skm_path,
        anim_sequence_path=anim_sequence_path,
        physics_asset_path=physics_asset_path,
        num_frames=0,  # auto-detect from animation
    )

    # ==================================================================
    # Summary
    # ==================================================================
    elapsed = time.perf_counter() - pipeline_start
    unreal.log("")
    unreal.log("=" * 60)
    unreal.log("PIPELINE COMPLETE")
    unreal.log("=" * 60)
    unreal.log(f"Total time: {elapsed:.1f}s")
    unreal.log(f"Body SKM:        {body_skm_path}")
    unreal.log(f"AnimSequence:    {anim_sequence_path}")
    unreal.log(f"PhysicsAsset:    {physics_asset_path}")
    unreal.log(f"Clothing SM:     {clothing_sm_path}")
    unreal.log(f"Cloth asset:     {cloth_asset_name or 'MANUAL SETUP REQUIRED'}")
    unreal.log(f"LevelSequence:   {sequence_path or 'FAILED'}")
    unreal.log("")

    if sequence_path:
        unreal.log("Next steps:")
        if cloth_asset_name is None:
            unreal.log("  1. Complete manual cloth setup (see warnings above)")
            unreal.log("  2. Open the LevelSequence in the Sequencer editor")
        else:
            unreal.log("  1. Open the LevelSequence in the Sequencer editor")

        unreal.log("  - Verify cloth simulation in Play/Simulate mode")
        unreal.log("  - Adjust cloth weight painting if needed (MaxDistance map)")
        unreal.log("  - Set up Movie Render Queue for final rendering")
        return True
    else:
        unreal.log_error("Pipeline failed at LevelSequence creation")
        return False


######################################################################
# Main
######################################################################
if __name__ == "__main__":
    run_pipeline()
