# Chaos Cloth Simulation Pipeline - Cloth Setup
#
# Sets up Chaos Cloth simulation on a clothing mesh driven by the body skeleton.
#
# UE 5.3.2 Chaos Cloth workflow:
#   1. The clothing Static Mesh is imported onto the body's SkeletalMesh as a
#      new mesh section (LOD section) with skin weights transferred from the body.
#   2. A ClothingAsset (ChaosClothAssetNv) is created from that section.
#   3. Cloth simulation parameters (stiffness, damping, collision) are configured.
#   4. The physics asset is assigned for body collision.
#
# LIMITATIONS (UE 5.3 Python API):
#   - Weight painting (MaxDistance, Backstop) is not fully scriptable. This script
#     sets uniform max distance values. For production quality, paint weights
#     manually in the SkeletalMesh editor after running this script.
#   - The Clothing panel operations (Section -> Create Cloth Asset) are partially
#     exposed. If ClothingAssetFactory is not available, a manual step is documented.
#
# For UE 5.4+ (Cloth Asset Editor with dataflow nodes), the workflow described in
# ChaosCloth_Description.txt applies. That system uses a separate ChaosClothAsset
# with a node graph, which is not yet exposed to the Python API.
#
# Run inside Unreal Editor via: Execute Python Script

import unreal

from config import (
    CLOTH_SOLVER_ITERATIONS,
    CLOTH_SOLVER_SUBSTEPS,
    CLOTH_MASS_DENSITY,
    CLOTH_STRETCH_STIFFNESS_WARP,
    CLOTH_STRETCH_STIFFNESS_WEFT,
    CLOTH_STRETCH_STIFFNESS_BIAS,
    CLOTH_BENDING_STIFFNESS,
    CLOTH_DAMPING_COEFFICIENT,
    CLOTH_DRAG_COEFFICIENT,
    CLOTH_LIFT_COEFFICIENT,
    CLOTH_COLLISION_THICKNESS,
    CLOTH_SELF_COLLISION_ENABLED,
    CLOTH_SELF_COLLISION_RADIUS,
    CLOTH_MAX_DISTANCE_DEFAULT,
    CLOTH_BACKSTOP_DISTANCE,
    CLOTH_BACKSTOP_RADIUS,
    CLOTH_GRAVITY_SCALE,
    UE_BODY_FOLDER,
    UE_CLOTHING_FOLDER,
)


def create_clothing_asset_on_skeletal_mesh(body_skm_path, clothing_sm_path, physics_asset_path):
    """
    Create a Chaos Cloth clothing asset on the body SkeletalMesh using the
    clothing Static Mesh geometry.

    This is the core cloth setup function. It:
    1. Loads the body SKM and clothing SM
    2. Creates a ClothingAsset from the clothing mesh
    3. Binds it to the body SKM's skeleton
    4. Configures simulation parameters
    5. Assigns the physics asset for collision

    Returns: the clothing asset name (str) or None on failure.

    IMPORTANT: After running this function, you should open the body SkeletalMesh
    in the editor and verify/adjust the cloth weight painting (MaxDistance map)
    for production-quality results.
    """
    unreal.log("=" * 60)
    unreal.log("Setting up Chaos Cloth simulation")
    unreal.log("=" * 60)

    # Load assets
    body_skm = unreal.load_asset(body_skm_path)
    if body_skm is None:
        unreal.log_error(f"Cannot load body SkeletalMesh: {body_skm_path}")
        return None

    clothing_sm = unreal.load_asset(clothing_sm_path)
    if clothing_sm is None:
        unreal.log_error(f"Cannot load clothing StaticMesh: {clothing_sm_path}")
        return None

    physics_asset = None
    if physics_asset_path:
        physics_asset = unreal.load_asset(physics_asset_path)
        if physics_asset is None:
            unreal.log_warning(f"Cannot load PhysicsAsset: {physics_asset_path}")
            unreal.log_warning("Cloth collision with body will not work without a PhysicsAsset.")

    # ------------------------------------------------------------------
    # Step 1: Create ClothingAsset from the clothing mesh
    # ------------------------------------------------------------------
    # In UE 5.3, ClothingAssetFactory creates a ChaosClothingAsset from mesh data.
    # The factory transfers the clothing SM's geometry onto the body's skeleton
    # with skin weight transfer.

    clothing_asset_name = clothing_sm.get_name() + "_ClothAsset"

    # Try to use the ClothingSystemEditorInterface to create the cloth asset
    # This replicates what the editor does when you right-click a mesh section
    # and select "Create Cloth Asset from Section"
    try:
        cloth_factory = unreal.ClothingAssetFactory()
        unreal.log("ClothingAssetFactory available - creating cloth asset programmatically")
    except Exception:
        cloth_factory = None
        unreal.log_warning(
            "ClothingAssetFactory not available in Python API. "
            "This is expected in some UE 5.3 builds."
        )

    if cloth_factory is not None:
        # Create the clothing asset from the static mesh
        # The factory handles:
        #   - Geometry extraction from the SM
        #   - Skin weight transfer from the body SKM
        #   - Creation of the simulation mesh (low-res proxy)
        #   - Binding to the skeleton

        success = _create_cloth_asset_via_factory(
            cloth_factory, body_skm, clothing_sm, clothing_asset_name, physics_asset
        )
        if success:
            _configure_cloth_parameters(body_skm, clothing_asset_name)
            unreal.EditorAssetLibrary.save_asset(body_skm_path)
            return clothing_asset_name

    # ------------------------------------------------------------------
    # Fallback: Use SkeletalMesh clothing API directly
    # ------------------------------------------------------------------
    # If the factory approach fails, we try the lower-level approach:
    # Import the clothing mesh as an additional LOD section on the body SKM,
    # then create a cloth asset from that section.

    unreal.log("Attempting direct SkeletalMesh clothing API...")
    success = _create_cloth_asset_direct(
        body_skm, body_skm_path, clothing_sm, clothing_sm_path,
        clothing_asset_name, physics_asset
    )

    if success:
        _configure_cloth_parameters(body_skm, clothing_asset_name)
        unreal.EditorAssetLibrary.save_asset(body_skm_path)
        return clothing_asset_name

    # ------------------------------------------------------------------
    # Manual fallback instructions
    # ------------------------------------------------------------------
    unreal.log_warning("=" * 60)
    unreal.log_warning("AUTOMATIC CLOTH SETUP FAILED")
    unreal.log_warning("=" * 60)
    unreal.log_warning("The Python API could not create the cloth asset automatically.")
    unreal.log_warning("Please perform these steps manually in the UE Editor:")
    unreal.log_warning("")
    unreal.log_warning(f"1. Open the body SkeletalMesh: {body_skm_path}")
    unreal.log_warning("2. In the Mesh Details panel, find the clothing section")
    unreal.log_warning(f"3. Right-click the clothing mesh section -> 'Create Cloth Asset from Section'")
    unreal.log_warning(f"4. Name it: {clothing_asset_name}")
    unreal.log_warning(f"5. In the Cloth Asset, set the Physics Asset to: {physics_asset_path}")
    unreal.log_warning("6. Paint the MaxDistance weight map (higher = more cloth freedom)")
    unreal.log_warning("7. Configure simulation parameters in the Cloth Config panel")
    unreal.log_warning("8. Save the SkeletalMesh")
    unreal.log_warning("")
    unreal.log_warning("After manual setup, run create_cloth_level_sequence.py to build the sequence.")
    unreal.log_warning("=" * 60)

    return None


def _create_cloth_asset_via_factory(cloth_factory, body_skm, clothing_sm, asset_name, physics_asset):
    """
    Create cloth asset using ClothingAssetFactory.
    This mirrors the editor's 'Create Cloth Asset' workflow.
    """
    try:
        # Create clothing asset from the static mesh source
        # The factory method signature varies by UE version
        clothing_asset = cloth_factory.create_clothing_asset_from_static_mesh(
            body_skm,      # target skeletal mesh
            clothing_sm,   # source static mesh
            asset_name,    # clothing asset name
            0              # LOD index
        )

        if clothing_asset is None:
            unreal.log_warning("ClothingAssetFactory.create_clothing_asset_from_static_mesh returned None")
            return False

        # Assign physics asset for collision
        if physics_asset is not None:
            _set_physics_asset_on_cloth(clothing_asset, physics_asset)

        unreal.log(f"Cloth asset created via factory: {asset_name}")
        return True

    except Exception as e:
        unreal.log_warning(f"Factory cloth creation failed: {e}")
        return False


def _create_cloth_asset_direct(body_skm, body_skm_path, clothing_sm, clothing_sm_path,
                                asset_name, physics_asset):
    """
    Fallback: Create cloth asset using direct SkeletalMesh API.

    This approach:
    1. Gets existing clothing assets on the SKM
    2. Uses the Clothing System Editor interface to create a new one
    """
    try:
        # Check if cloth asset already exists on this SKM
        num_clothing = body_skm.get_editor_property("mesh_clothing_assets")
        if num_clothing:
            for cloth_asset in num_clothing:
                if cloth_asset.get_name() == asset_name:
                    unreal.log(f"Cloth asset already exists: {asset_name}")
                    return True

        # Try creating via the clothing system editor utilities
        # This uses the internal editor APIs for cloth creation
        clothing_system = unreal.ClothingSystemEditorInterfaceModule.get()
        if clothing_system:
            factory = clothing_system.get_clothing_asset_factory()
            if factory:
                result = factory.import_cloth_asset_from_static_mesh(
                    body_skm,
                    clothing_sm,
                    asset_name,
                    0,  # LOD
                    0   # Section index
                )
                if result:
                    if physics_asset:
                        _set_physics_asset_on_cloth_by_name(body_skm, asset_name, physics_asset)
                    unreal.log(f"Cloth asset created via direct API: {asset_name}")
                    return True

    except Exception as e:
        unreal.log_warning(f"Direct cloth creation failed: {e}")

    # Try yet another approach: use the SkeletalMesh's built-in clothing methods
    try:
        # Some UE builds expose add_clothing_asset directly
        if hasattr(body_skm, "add_clothing_asset"):
            # Create a new ChaosClothingAssetNv
            cloth_asset = unreal.ChaosClothAssetNv()
            cloth_asset.set_editor_property("name", asset_name)
            body_skm.add_clothing_asset(cloth_asset)
            unreal.log(f"Cloth asset added via add_clothing_asset: {asset_name}")
            return True
    except Exception as e:
        unreal.log_warning(f"add_clothing_asset approach failed: {e}")

    return False


def _set_physics_asset_on_cloth(clothing_asset, physics_asset):
    """Set the physics asset on a clothing asset for collision."""
    try:
        # ChaosClothAssetNv stores physics asset reference in its config
        cloth_config = clothing_asset.get_editor_property("cloth_config")
        if cloth_config:
            cloth_config.set_editor_property("physics_asset", physics_asset)
            unreal.log(f"  Physics asset assigned to cloth: {physics_asset.get_name()}")
    except Exception as e:
        unreal.log_warning(f"  Could not set physics asset on cloth config: {e}")
        # Try alternative property path
        try:
            clothing_asset.set_editor_property("physics_asset", physics_asset)
        except Exception:
            pass


def _set_physics_asset_on_cloth_by_name(body_skm, cloth_asset_name, physics_asset):
    """Find a cloth asset by name on the SKM and set its physics asset."""
    clothing_assets = body_skm.get_editor_property("mesh_clothing_assets")
    if clothing_assets:
        for cloth_asset in clothing_assets:
            if cloth_asset.get_name() == cloth_asset_name:
                _set_physics_asset_on_cloth(cloth_asset, physics_asset)
                return


def _configure_cloth_parameters(body_skm, cloth_asset_name):
    """
    Configure Chaos Cloth simulation parameters on the cloth asset.

    These map to the nodes described in ChaosCloth_Description.txt:
    - SimulationStretchConfig -> stretch stiffness
    - SimulationBendingConfig -> bending stiffness
    - SimulationMassConfig -> mass density
    - SimulationAerodynamicsConfig -> drag/lift
    - SimulationDampingConfig -> damping
    - SimulationCollisionConfig -> collision thickness, physics asset
    - SimulationSelfCollisionSpheresConfig -> self-collision
    - SimulationMaxDistanceConfig -> max distance constraint
    - SimulationLongRangeAttachmentConfig -> tether springs
    """
    unreal.log("Configuring cloth simulation parameters...")

    clothing_assets = body_skm.get_editor_property("mesh_clothing_assets")
    if not clothing_assets:
        unreal.log_warning("No clothing assets found on SkeletalMesh")
        return

    cloth_asset = None
    for ca in clothing_assets:
        if ca.get_name() == cloth_asset_name:
            cloth_asset = ca
            break

    if cloth_asset is None:
        unreal.log_warning(f"Cloth asset '{cloth_asset_name}' not found on SkeletalMesh")
        return

    # Access the Chaos Cloth config
    # In UE 5.3, the config is accessed via ClothConfig on ChaosClothingAssetCommon
    try:
        cloth_config = cloth_asset.get_editor_property("cloth_config")
    except Exception:
        unreal.log_warning("Cannot access cloth_config property")
        return

    if cloth_config is None:
        unreal.log_warning("cloth_config is None")
        return

    # -- Solver settings --
    _try_set(cloth_config, "solver_iterations", CLOTH_SOLVER_ITERATIONS)
    _try_set(cloth_config, "sub_step_count", CLOTH_SOLVER_SUBSTEPS)

    # -- Mass --
    _try_set(cloth_config, "mass_mode", unreal.ClothMassMode.DENSITY)
    _try_set(cloth_config, "uniform_mass", CLOTH_MASS_DENSITY)

    # -- Stretch stiffness --
    _try_set(cloth_config, "stiffness_warp", CLOTH_STRETCH_STIFFNESS_WARP)
    _try_set(cloth_config, "stiffness_weft", CLOTH_STRETCH_STIFFNESS_WEFT)
    _try_set(cloth_config, "stiffness_bias", CLOTH_STRETCH_STIFFNESS_BIAS)

    # -- Bending --
    _try_set(cloth_config, "bending_stiffness", CLOTH_BENDING_STIFFNESS)

    # -- Damping --
    _try_set(cloth_config, "damping_coefficient", CLOTH_DAMPING_COEFFICIENT)

    # -- Aerodynamics (drag + lift) --
    _try_set(cloth_config, "drag_coefficient", CLOTH_DRAG_COEFFICIENT)
    _try_set(cloth_config, "lift_coefficient", CLOTH_LIFT_COEFFICIENT)

    # -- Collision --
    _try_set(cloth_config, "collision_thickness", CLOTH_COLLISION_THICKNESS)

    # -- Self-collision --
    if CLOTH_SELF_COLLISION_ENABLED:
        _try_set(cloth_config, "use_self_collisions", True)
        _try_set(cloth_config, "self_collision_radius", CLOTH_SELF_COLLISION_RADIUS)

    # -- Gravity --
    _try_set(cloth_config, "gravity_scale", CLOTH_GRAVITY_SCALE)

    # -- Max distance (uniform) --
    # For per-vertex painting, this must be done manually in the editor
    _try_set(cloth_config, "max_distance", CLOTH_MAX_DISTANCE_DEFAULT)

    # -- Backstop --
    _try_set(cloth_config, "backstop_distance", CLOTH_BACKSTOP_DISTANCE)
    _try_set(cloth_config, "backstop_radius", CLOTH_BACKSTOP_RADIUS)

    unreal.log("Cloth simulation parameters configured.")
    unreal.log(f"  Solver: {CLOTH_SOLVER_ITERATIONS} iterations, {CLOTH_SOLVER_SUBSTEPS} substeps")
    unreal.log(f"  Mass density: {CLOTH_MASS_DENSITY}")
    unreal.log(f"  Damping: {CLOTH_DAMPING_COEFFICIENT}")
    unreal.log(f"  Gravity scale: {CLOTH_GRAVITY_SCALE}")
    unreal.log(f"  Max distance: {CLOTH_MAX_DISTANCE_DEFAULT} cm")
    unreal.log(f"  Self-collision: {CLOTH_SELF_COLLISION_ENABLED}")


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

    # These paths should be set by run_pipeline.py or manually configured
    # For standalone usage, set these to your imported asset paths:
    body_skm_path = UE_BODY_FOLDER + "/body"
    clothing_sm_path = UE_CLOTHING_FOLDER + "/garment"
    physics_asset_path = UE_BODY_FOLDER + "/body_PhysicsAsset"

    result = create_clothing_asset_on_skeletal_mesh(
        body_skm_path, clothing_sm_path, physics_asset_path
    )
    if result:
        unreal.log(f"Cloth asset created: {result}")
    else:
        unreal.log_error("Cloth setup failed. See warnings above for manual steps.")
