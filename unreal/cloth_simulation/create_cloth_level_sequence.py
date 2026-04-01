# Chaos Cloth Simulation Pipeline - LevelSequence Creation
#
# Creates a LevelSequence with:
#   - Body SkeletalMeshActor (animated via AnimSequence)
#   - Clothing SkeletalMeshActor (cloth simulation driven by body skeleton)
#   - Static camera observing the scene
#
# The clothing actor shares the body's skeleton. When the body animation plays,
# the cloth simulation runs on the clothing mesh sections that have cloth assets
# assigned, producing physically-driven fabric motion.
#
# Run inside Unreal Editor via: Execute Python Script
#
# Required plugins: Python Editor Script Plugin, Sequencer Scripting

import unreal

from config import (
    SEQUENCE_NAME,
    FRAME_RATE,
    WARMUP_FRAMES,
    CLOTH_SETTLE_FRAMES,
    UE_SEQUENCES_FOLDER,
    BODY_LOCATION_X,
    BODY_LOCATION_Y,
    BODY_LOCATION_Z,
    BODY_ROTATION_ROLL,
    BODY_ROTATION_PITCH,
    BODY_ROTATION_YAW,
)


def create_level_sequence(
    body_skm_path,
    anim_sequence_path,
    physics_asset_path,
    num_frames,
    clothing_material=None,
    body_material=None,
):
    """
    Create a LevelSequence for cloth simulation preview and rendering.

    Args:
        body_skm_path: UE path to the body SkeletalMesh (with cloth asset attached)
        anim_sequence_path: UE path to the body AnimSequence
        physics_asset_path: UE path to the PhysicsAsset for cloth collision
        num_frames: number of animation frames to include
        clothing_material: optional material for the clothing mesh sections
        body_material: optional material for the body mesh

    Returns: path to the created LevelSequence, or None on failure.
    """
    unreal.log("=" * 60)
    unreal.log("Creating Cloth Simulation LevelSequence")
    unreal.log("=" * 60)

    # ------------------------------------------------------------------
    # Load assets
    # ------------------------------------------------------------------
    body_skm = unreal.load_asset(body_skm_path)
    if body_skm is None:
        unreal.log_error(f"Cannot load body SkeletalMesh: {body_skm_path}")
        return None

    anim_sequence = None
    if anim_sequence_path:
        anim_sequence = unreal.load_asset(anim_sequence_path)
        if anim_sequence is None:
            unreal.log_warning(f"Cannot load AnimSequence: {anim_sequence_path}")

    physics_asset = None
    if physics_asset_path:
        physics_asset = unreal.load_asset(physics_asset_path)

    # Determine frame count from animation if not specified
    if num_frames is None or num_frames <= 0:
        if anim_sequence:
            num_frames = int(anim_sequence.get_editor_property("number_of_frames"))
            unreal.log(f"Frame count from animation: {num_frames}")
        else:
            num_frames = 150
            unreal.log(f"Using default frame count: {num_frames}")

    # ------------------------------------------------------------------
    # Create LevelSequence asset
    # ------------------------------------------------------------------
    sequence_path = f"{UE_SEQUENCES_FOLDER}/{SEQUENCE_NAME}"

    # Delete existing sequence
    if unreal.EditorAssetLibrary.does_asset_exist(sequence_path):
        unreal.log(f"Deleting existing sequence: {sequence_path}")
        unreal.EditorAssetLibrary.delete_asset(sequence_path)

    asset_tools = unreal.AssetToolsHelpers.get_asset_tools()
    level_sequence = asset_tools.create_asset(
        asset_name=SEQUENCE_NAME,
        package_path=UE_SEQUENCES_FOLDER,
        asset_class=unreal.LevelSequence,
        factory=unreal.LevelSequenceFactoryNew(),
    )

    if level_sequence is None:
        unreal.log_error("Failed to create LevelSequence asset")
        return None

    unreal.log(f"Created LevelSequence: {sequence_path}")

    # Set frame rate and playback range
    display_rate = unreal.FrameRate(FRAME_RATE, 1)
    level_sequence.set_display_rate(display_rate)

    start_frame = -WARMUP_FRAMES
    end_frame = num_frames
    level_sequence.set_playback_start(start_frame)
    level_sequence.set_playback_end(end_frame)

    unreal.log(f"  Frame range: [{start_frame}, {end_frame}] at {FRAME_RATE}fps")
    unreal.log(f"  Warmup frames: {WARMUP_FRAMES} (rendered) + {CLOTH_SETTLE_FRAMES} (cloth settle)")

    # ------------------------------------------------------------------
    # Spawn and add Body SkeletalMeshActor
    # ------------------------------------------------------------------
    body_binding = _add_skeletal_mesh_actor(
        level_sequence=level_sequence,
        skeletal_mesh=body_skm,
        anim_sequence=anim_sequence,
        physics_asset=physics_asset,
        actor_label="ClothBody",
        layer_name="be_actor_00_body",
        start_frame=start_frame,
        end_frame=end_frame,
        x=BODY_LOCATION_X,
        y=BODY_LOCATION_Y,
        z=BODY_LOCATION_Z,
        roll=BODY_ROTATION_ROLL,
        pitch=BODY_ROTATION_PITCH,
        yaw=BODY_ROTATION_YAW,
        material=body_material,
        enable_cloth=True,
    )

    if body_binding is None:
        unreal.log_error("Failed to add body actor to sequence")
        return None

    # ------------------------------------------------------------------
    # Add a static camera
    # ------------------------------------------------------------------
    _add_static_camera(
        level_sequence=level_sequence,
        start_frame=start_frame,
        end_frame=end_frame,
        camera_location=unreal.Vector(300.0, 0.0, 100.0),
        camera_rotation=unreal.Rotator(0.0, 180.0, 0.0),
        focal_length=35.0,
    )

    # ------------------------------------------------------------------
    # Save
    # ------------------------------------------------------------------
    unreal.EditorAssetLibrary.save_asset(sequence_path)
    unreal.log(f"LevelSequence saved: {sequence_path}")

    return sequence_path


def _add_skeletal_mesh_actor(
    level_sequence,
    skeletal_mesh,
    anim_sequence,
    physics_asset,
    actor_label,
    layer_name,
    start_frame,
    end_frame,
    x, y, z,
    roll, pitch, yaw,
    material=None,
    enable_cloth=False,
):
    """
    Spawn a SkeletalMeshActor, configure it for cloth simulation,
    and add it as a spawnable to the LevelSequence.

    This follows the same spawn-configure-add_spawnable-destroy pattern
    used in the main BEDLAM2 pipeline (create_level_sequences_csv.py).
    """
    unreal.log(f"  Adding SkeletalMesh actor: {actor_label}")

    # 1. Spawn template actor in the level
    skm_actor = unreal.get_editor_subsystem(unreal.EditorActorSubsystem).spawn_actor_from_class(
        unreal.SkeletalMeshActor, unreal.Vector(0, 0, 0)
    )
    skm_actor.set_actor_label(actor_label)

    # 2. Configure the SkeletalMeshComponent
    skm_component = skm_actor.skeletal_mesh_component
    skm_component.set_skeletal_mesh(skeletal_mesh)

    # Set material if provided
    if material is not None:
        num_materials = skeletal_mesh.get_editor_property("materials")
        for i in range(len(num_materials)):
            skm_component.set_material(i, material)

    # Enable cloth simulation on this actor
    if enable_cloth:
        # Cloth simulation is automatically enabled when the SkeletalMesh has
        # ClothingAssets assigned to its mesh sections. We just need to ensure
        # the component settings support it.

        # Set the physics asset for cloth collision bodies
        if physics_asset is not None:
            skm_component.set_editor_property("physics_asset_override", physics_asset)
            unreal.log(f"    PhysicsAsset set for collision: {physics_asset.get_name()}")

        # Enable cloth simulation (it's on by default, but be explicit)
        _try_set(skm_component, "allow_cloth_actors", True)

        # Disable animation blueprint interference
        _try_set(skm_component, "animation_mode", unreal.AnimationMode.ANIMATION_SINGLE_NODE)

        unreal.log("    Cloth simulation enabled on SkeletalMeshComponent")

    # 3. Add to layer for segmentation mask naming (same pattern as BEDLAM2)
    layer_subsystem = unreal.get_editor_subsystem(unreal.LayersSubsystem)
    result = layer_subsystem.add_actor_to_layer(skm_actor, layer_name)
    if not result:
        unreal.log_warning(f"    Layer system unavailable, using folder: BEDLAM/masks/{layer_name}")
        skm_actor.set_folder_path(f"BEDLAM/masks/{layer_name}")

    # 4. Add as spawnable to the LevelSequence, then destroy template
    binding = level_sequence.add_spawnable_from_instance(skm_actor)
    unreal.get_editor_subsystem(unreal.EditorActorSubsystem).destroy_actor(skm_actor)

    # 5. Add animation track
    if anim_sequence is not None:
        anim_track = binding.add_track(unreal.MovieSceneSkeletalAnimationTrack)
        anim_section = anim_track.add_section()
        anim_section.params.animation = anim_sequence
        anim_section.set_range(start_frame, end_frame)
        unreal.log(f"    AnimSequence track added: frames [{start_frame}, {end_frame}]")

    # 6. Add transform track
    transform_track = binding.add_track(unreal.MovieScene3DTransformTrack)
    transform_section = transform_track.add_section()
    transform_section.set_start_frame_bounded(False)
    transform_section.set_end_frame_bounded(False)
    transform_channels = transform_section.get_all_channels()
    transform_channels[0].set_default(x)     # location X
    transform_channels[1].set_default(y)     # location Y
    transform_channels[2].set_default(z)     # location Z
    transform_channels[3].set_default(roll)  # roll
    transform_channels[4].set_default(pitch) # pitch
    transform_channels[5].set_default(yaw)   # yaw

    unreal.log(f"    Transform: ({x}, {y}, {z}), rotation: ({roll}, {pitch}, {yaw})")

    return binding


def _add_static_camera(
    level_sequence,
    start_frame,
    end_frame,
    camera_location,
    camera_rotation,
    focal_length=35.0,
):
    """
    Add a static CineCameraActor to the LevelSequence.

    This provides a simple observation camera for previewing the cloth simulation.
    For production rendering, replace this with the full BEDLAM2 camera system
    (BE_CameraRoot -> BE_CameraOperator -> CineCameraActor).
    """
    unreal.log("  Adding static camera")

    # Spawn camera actor
    camera_actor = unreal.get_editor_subsystem(unreal.EditorActorSubsystem).spawn_actor_from_class(
        unreal.CineCameraActor, unreal.Vector(0, 0, 0)
    )
    camera_actor.set_actor_label("ClothCamera")

    # Configure camera
    camera_component = camera_actor.get_cine_camera_component()
    camera_component.set_editor_property("current_focal_length", focal_length)

    # Add as spawnable
    camera_binding = level_sequence.add_spawnable_from_instance(camera_actor)
    unreal.get_editor_subsystem(unreal.EditorActorSubsystem).destroy_actor(camera_actor)

    # Transform track (camera position/rotation)
    transform_track = camera_binding.add_track(unreal.MovieScene3DTransformTrack)
    transform_section = transform_track.add_section()
    transform_section.set_start_frame_bounded(False)
    transform_section.set_end_frame_bounded(False)
    transform_channels = transform_section.get_all_channels()
    transform_channels[0].set_default(camera_location.x)
    transform_channels[1].set_default(camera_location.y)
    transform_channels[2].set_default(camera_location.z)
    transform_channels[3].set_default(0.0)                    # roll
    transform_channels[4].set_default(camera_rotation.pitch)  # pitch
    transform_channels[5].set_default(camera_rotation.yaw)    # yaw

    # Camera cut track (tells sequencer which camera is active)
    movie_scene = level_sequence.get_movie_scene()
    camera_cut_track = movie_scene.add_track(unreal.MovieSceneCameraCutTrack)
    camera_cut_section = camera_cut_track.add_section()

    camera_binding_id = unreal.MovieSceneObjectBindingID()
    camera_binding_id.set_editor_property("Guid", camera_binding.get_id())
    camera_cut_section.set_camera_binding_id(camera_binding_id)
    camera_cut_section.set_range(start_frame, end_frame)

    unreal.log(f"    Camera at ({camera_location.x}, {camera_location.y}, {camera_location.z})")
    unreal.log(f"    Focal length: {focal_length}mm")

    return camera_binding


def _try_set(obj, prop_name, value):
    """Try to set an editor property, log warning if it fails."""
    try:
        obj.set_editor_property(prop_name, value)
    except Exception as e:
        unreal.log_warning(f"  Cannot set '{prop_name}': {e}")


######################################################################
# Main
######################################################################
if __name__ == "__main__":
    unreal.log("=" * 60)
    unreal.log("Running: %s" % __file__)

    from config import UE_BODY_FOLDER, UE_CLOTHING_FOLDER

    # For standalone usage, set these to your imported asset paths
    body_skm_path = UE_BODY_FOLDER + "/body"
    anim_sequence_path = UE_BODY_FOLDER + "/body_Anim"
    physics_asset_path = UE_BODY_FOLDER + "/body_PhysicsAsset"

    result = create_level_sequence(
        body_skm_path=body_skm_path,
        anim_sequence_path=anim_sequence_path,
        physics_asset_path=physics_asset_path,
        num_frames=0,  # auto-detect from animation
    )

    if result:
        unreal.log(f"LevelSequence created: {result}")
    else:
        unreal.log_error("LevelSequence creation failed")
