# Chaos Cloth Simulation Pipeline - Asset Import
#
# Imports the body SkeletalMesh (with skeleton and animation), physics asset,
# and clothing Static Mesh into the Unreal Engine content browser.
#
# Run inside Unreal Editor via: Execute Python Script
#
# Required plugins: Python Editor Script Plugin

import time
from pathlib import Path
import unreal

from config import (
    BODY_SKM_FBX_PATH,
    PHYSICS_ASSET_FBX_PATH,
    CLOTHING_SM_PATH,
    CLOTHING_TEXTURE_PATH,
    UE_BODY_FOLDER,
    UE_CLOTHING_FOLDER,
    UE_MATERIALS_FOLDER,
    CONVERSION_SCALE,
    CONVERSION_ROTATION,
    FLIP_U,
    FLIP_V,
)


def import_body_skeletal_mesh(fbx_path, destination_folder):
    """
    Import body FBX as SkeletalMesh with skeleton and animation.
    Also generates a default PhysicsAsset for collision.

    Returns: (skeletal_mesh_path, anim_sequence_path, physics_asset_path) or None on failure.
    """
    fbx_path = Path(fbx_path)
    if not fbx_path.exists():
        unreal.log_error(f"Body FBX file not found: {fbx_path}")
        return None

    asset_name = fbx_path.stem
    asset_path = f"{destination_folder}/{asset_name}"

    if unreal.EditorAssetLibrary.does_asset_exist(asset_path):
        unreal.log(f"Body SKM already imported: {asset_path}")
        # Try to find associated assets
        anim_path = f"{destination_folder}/{asset_name}_Anim"
        physics_path = f"{destination_folder}/{asset_name}_PhysicsAsset"
        return asset_path, anim_path, physics_path

    unreal.log(f"Importing body SkeletalMesh: {fbx_path}")

    fbx_options = unreal.FbxImportUI()
    fbx_options.set_editor_property("import_as_skeletal", True)
    fbx_options.set_editor_property("import_animations", True)
    fbx_options.set_editor_property("import_materials", False)
    fbx_options.set_editor_property("import_textures", False)
    fbx_options.set_editor_property("create_physics_asset", True)

    # Skeletal mesh import settings
    skm_settings = fbx_options.get_editor_property("skeletal_mesh_import_data")
    skm_settings.set_editor_property("import_morph_targets", False)
    skm_settings.set_editor_property("update_skeleton_reference_pose", True)

    # Animation import settings
    anim_settings = fbx_options.get_editor_property("anim_sequence_import_data")
    anim_settings.set_editor_property("import_bone_tracks", True)
    anim_settings.set_editor_property("use_default_sample_rate", False)
    anim_settings.set_editor_property("custom_sample_rate", 30)

    task = unreal.AssetImportTask()
    task.set_editor_property("automated", True)
    task.set_editor_property("filename", str(fbx_path))
    task.set_editor_property("destination_path", destination_folder)
    task.set_editor_property("destination_name", "")
    task.set_editor_property("replace_existing", True)
    task.set_editor_property("save", True)
    task.set_editor_property("options", fbx_options)

    unreal.AssetToolsHelpers.get_asset_tools().import_asset_tasks([task])
    unreal.EditorAssetLibrary.save_directory(destination_folder)

    # Verify import
    if not unreal.EditorAssetLibrary.does_asset_exist(asset_path):
        unreal.log_error(f"Failed to import body SKM: {asset_path}")
        return None

    # UE auto-names associated assets
    anim_path = f"{destination_folder}/{asset_name}_Anim"
    physics_path = f"{destination_folder}/{asset_name}_PhysicsAsset"

    unreal.log(f"  Body SKM imported: {asset_path}")
    if unreal.EditorAssetLibrary.does_asset_exist(anim_path):
        unreal.log(f"  AnimSequence imported: {anim_path}")
    else:
        anim_path = None
        unreal.log_warning("  No AnimSequence found (FBX may not contain animation)")
    if unreal.EditorAssetLibrary.does_asset_exist(physics_path):
        unreal.log(f"  PhysicsAsset generated: {physics_path}")
    else:
        physics_path = None
        unreal.log_warning("  No PhysicsAsset generated")

    return asset_path, anim_path, physics_path


def import_physics_asset(fbx_path, destination_folder):
    """
    Import a custom physics asset from FBX.
    Use this when you have a hand-crafted or low-poly collision body
    instead of the auto-generated capsule approximation.

    Returns: asset path or None on failure.
    """
    if fbx_path is None:
        unreal.log("No custom physics asset path provided, using auto-generated.")
        return None

    fbx_path = Path(fbx_path)
    if not fbx_path.exists():
        unreal.log_error(f"Physics asset FBX not found: {fbx_path}")
        return None

    asset_name = fbx_path.stem
    asset_path = f"{destination_folder}/{asset_name}"

    if unreal.EditorAssetLibrary.does_asset_exist(asset_path):
        unreal.log(f"Physics asset already imported: {asset_path}")
        return asset_path

    unreal.log(f"Importing physics asset: {fbx_path}")

    fbx_options = unreal.FbxImportUI()
    fbx_options.set_editor_property("import_as_skeletal", True)
    fbx_options.set_editor_property("import_animations", False)
    fbx_options.set_editor_property("import_materials", False)
    fbx_options.set_editor_property("import_textures", False)
    fbx_options.set_editor_property("create_physics_asset", True)

    task = unreal.AssetImportTask()
    task.set_editor_property("automated", True)
    task.set_editor_property("filename", str(fbx_path))
    task.set_editor_property("destination_path", destination_folder)
    task.set_editor_property("destination_name", "")
    task.set_editor_property("replace_existing", True)
    task.set_editor_property("save", True)
    task.set_editor_property("options", fbx_options)

    unreal.AssetToolsHelpers.get_asset_tools().import_asset_tasks([task])
    unreal.EditorAssetLibrary.save_directory(destination_folder)

    physics_path = f"{destination_folder}/{asset_name}_PhysicsAsset"
    if unreal.EditorAssetLibrary.does_asset_exist(physics_path):
        unreal.log(f"  Physics asset imported: {physics_path}")
        return physics_path

    unreal.log_error(f"Failed to import physics asset: {physics_path}")
    return None


def import_clothing_static_mesh(mesh_path, destination_folder):
    """
    Import clothing as a Static Mesh.

    The clothing starts as a Static Mesh and will later be transferred onto
    the body's skeleton as a cloth-enabled SkeletalMesh section by
    setup_cloth_simulation.py.

    Returns: asset path or None on failure.
    """
    mesh_path = Path(mesh_path)
    if not mesh_path.exists():
        unreal.log_error(f"Clothing mesh file not found: {mesh_path}")
        return None

    asset_name = mesh_path.stem
    asset_path = f"{destination_folder}/{asset_name}"

    if unreal.EditorAssetLibrary.does_asset_exist(asset_path):
        unreal.log(f"Clothing SM already imported: {asset_path}")
        return asset_path

    unreal.log(f"Importing clothing Static Mesh: {mesh_path}")

    fbx_options = unreal.FbxImportUI()
    fbx_options.set_editor_property("import_as_skeletal", False)
    fbx_options.set_editor_property("import_mesh", True)
    fbx_options.set_editor_property("import_animations", False)
    fbx_options.set_editor_property("import_materials", True)
    fbx_options.set_editor_property("import_textures", True)

    # Static mesh import settings
    sm_settings = fbx_options.get_editor_property("static_mesh_import_data")
    sm_settings.set_editor_property("combine_meshes", True)

    task = unreal.AssetImportTask()
    task.set_editor_property("automated", True)
    task.set_editor_property("filename", str(mesh_path))
    task.set_editor_property("destination_path", destination_folder)
    task.set_editor_property("destination_name", "")
    task.set_editor_property("replace_existing", True)
    task.set_editor_property("save", True)
    task.set_editor_property("options", fbx_options)

    unreal.AssetToolsHelpers.get_asset_tools().import_asset_tasks([task])
    unreal.EditorAssetLibrary.save_directory(destination_folder)

    if not unreal.EditorAssetLibrary.does_asset_exist(asset_path):
        unreal.log_error(f"Failed to import clothing SM: {asset_path}")
        return None

    unreal.log(f"  Clothing SM imported: {asset_path}")
    return asset_path


def import_clothing_texture(texture_path, destination_folder):
    """
    Import a clothing diffuse texture.

    Returns: asset path or None.
    """
    if texture_path is None:
        return None

    texture_path = Path(texture_path)
    if not texture_path.exists():
        unreal.log_error(f"Clothing texture not found: {texture_path}")
        return None

    asset_name = texture_path.stem
    asset_path = f"{destination_folder}/{asset_name}"

    if unreal.EditorAssetLibrary.does_asset_exist(asset_path):
        unreal.log(f"Texture already imported: {asset_path}")
        return asset_path

    unreal.log(f"Importing clothing texture: {texture_path}")

    task = unreal.AssetImportTask()
    task.set_editor_property("automated", True)
    task.set_editor_property("filename", str(texture_path))
    task.set_editor_property("destination_path", destination_folder)
    task.set_editor_property("destination_name", "")
    task.set_editor_property("replace_existing", True)
    task.set_editor_property("save", True)

    unreal.AssetToolsHelpers.get_asset_tools().import_asset_tasks([task])
    unreal.EditorAssetLibrary.save_directory(destination_folder)

    if unreal.EditorAssetLibrary.does_asset_exist(asset_path):
        unreal.log(f"  Texture imported: {asset_path}")
        return asset_path

    unreal.log_error(f"Failed to import texture: {asset_path}")
    return None


def import_all():
    """
    Import all assets needed for the cloth simulation pipeline.

    Returns: dict with asset paths, or None on critical failure.
    """
    unreal.log("=" * 60)
    unreal.log("Cloth Simulation Pipeline: Importing assets")
    unreal.log("=" * 60)

    start_time = time.perf_counter()
    results = {}

    # 1. Import body SkeletalMesh (+ animation + physics asset)
    body_result = import_body_skeletal_mesh(BODY_SKM_FBX_PATH, UE_BODY_FOLDER)
    if body_result is None:
        unreal.log_error("CRITICAL: Body SKM import failed. Aborting.")
        return None
    results["body_skm_path"], results["anim_sequence_path"], results["physics_asset_path"] = body_result

    # 2. Import custom physics asset (if provided)
    if PHYSICS_ASSET_FBX_PATH is not None:
        custom_physics = import_physics_asset(PHYSICS_ASSET_FBX_PATH, UE_BODY_FOLDER)
        if custom_physics is not None:
            results["physics_asset_path"] = custom_physics

    # 3. Import clothing Static Mesh
    clothing_path = import_clothing_static_mesh(CLOTHING_SM_PATH, UE_CLOTHING_FOLDER)
    if clothing_path is None:
        unreal.log_error("CRITICAL: Clothing SM import failed. Aborting.")
        return None
    results["clothing_sm_path"] = clothing_path

    # 4. Import clothing texture (optional)
    results["clothing_texture_path"] = import_clothing_texture(
        CLOTHING_TEXTURE_PATH, UE_MATERIALS_FOLDER
    )

    elapsed = time.perf_counter() - start_time
    unreal.log(f"Asset import complete. Time: {elapsed:.1f}s")
    unreal.log(f"  Body SKM:       {results['body_skm_path']}")
    unreal.log(f"  AnimSequence:    {results.get('anim_sequence_path', 'None')}")
    unreal.log(f"  PhysicsAsset:   {results.get('physics_asset_path', 'None')}")
    unreal.log(f"  Clothing SM:    {results['clothing_sm_path']}")
    unreal.log(f"  Clothing Tex:   {results.get('clothing_texture_path', 'None')}")

    return results


######################################################################
# Main
######################################################################
if __name__ == "__main__":
    unreal.log("=" * 60)
    unreal.log("Running: %s" % __file__)
    import_all()
