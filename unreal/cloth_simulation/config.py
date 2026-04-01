# Chaos Cloth Simulation Pipeline - Configuration
#
# All configurable paths and parameters for the cloth simulation pipeline.
# Edit these values to match your local asset locations before running.

# ==============================================================================
# Input file paths (local disk)
# ==============================================================================

# Path to the body SkeletalMesh FBX file (with skeleton + optional animation)
BODY_SKM_FBX_PATH = r"C:\bedlam2\cloth_test\body\body.fbx"

# Path to the physics asset FBX (collision bodies for the body)
# If None, a physics asset will be auto-generated from the body SKM on import.
PHYSICS_ASSET_FBX_PATH = None

# Path to the clothing Static Mesh file (FBX or USD)
CLOTHING_SM_PATH = r"C:\bedlam2\cloth_test\clothing\garment.fbx"

# Optional: path to clothing diffuse texture
CLOTHING_TEXTURE_PATH = None

# ==============================================================================
# Unreal Engine asset paths (content browser)
# ==============================================================================

# Root folder for all cloth simulation test assets
UE_ASSET_ROOT = "/Game/ClothSimulation"

# Sub-folders
UE_BODY_FOLDER = UE_ASSET_ROOT + "/Body"
UE_CLOTHING_FOLDER = UE_ASSET_ROOT + "/Clothing"
UE_MATERIALS_FOLDER = UE_ASSET_ROOT + "/Materials"
UE_SEQUENCES_FOLDER = UE_ASSET_ROOT + "/Sequences"

# ==============================================================================
# Level Sequence settings
# ==============================================================================

SEQUENCE_NAME = "LS_ClothTest"
FRAME_RATE = 30
WARMUP_FRAMES = 10  # rendered warmup frames (negative frame numbers, deleted in post)
ENGINE_WARMUP_FRAMES = 32  # engine warmup for Lumen/cloth settling

# Cloth simulation needs extra warmup to let the garment settle on the body.
# This is on top of the standard warmup frames.
CLOTH_SETTLE_FRAMES = 30

# ==============================================================================
# Actor placement defaults
# ==============================================================================

BODY_LOCATION_X = 0.0
BODY_LOCATION_Y = 0.0
BODY_LOCATION_Z = 0.0
BODY_ROTATION_ROLL = 0.0
BODY_ROTATION_PITCH = 0.0
BODY_ROTATION_YAW = 0.0

# ==============================================================================
# Chaos Cloth simulation parameters
# ==============================================================================

# Solver iterations (higher = more accurate, slower)
CLOTH_SOLVER_ITERATIONS = 10
CLOTH_SOLVER_SUBSTEPS = 2

# Mass properties
CLOTH_MASS_DENSITY = 0.35  # kg/m^2 (typical for light fabric)

# Stretch stiffness (0.0 = fully elastic, 1.0 = rigid)
CLOTH_STRETCH_STIFFNESS_WARP = 1.0
CLOTH_STRETCH_STIFFNESS_WEFT = 1.0
CLOTH_STRETCH_STIFFNESS_BIAS = 0.8

# Bending stiffness
CLOTH_BENDING_STIFFNESS = 0.5

# Damping (0.0 = no damping, 1.0 = fully damped)
CLOTH_DAMPING_COEFFICIENT = 0.04

# Aerodynamics
CLOTH_DRAG_COEFFICIENT = 0.035
CLOTH_LIFT_COEFFICIENT = 0.02

# Collision thickness (cm) - padding between cloth and collision bodies
CLOTH_COLLISION_THICKNESS = 1.0

# Self-collision (disabled by default for performance)
CLOTH_SELF_COLLISION_ENABLED = False
CLOTH_SELF_COLLISION_RADIUS = 1.0

# Max distance: how far cloth vertices can move from their skinned position (cm).
# 0 = pinned, higher = more freedom. Set to -1 to use per-vertex painted values.
CLOTH_MAX_DISTANCE_DEFAULT = 50.0

# Backstop: prevents cloth from penetrating inward past skinned position
CLOTH_BACKSTOP_DISTANCE = 5.0
CLOTH_BACKSTOP_RADIUS = 10.0

# Gravity scale (1.0 = normal gravity)
CLOTH_GRAVITY_SCALE = 1.0

# ==============================================================================
# Blender-to-UE coordinate conversion
# ==============================================================================

# Standard BEDLAM2 conversion: Blender meters Y-up -> UE centimeters Z-up
CONVERSION_SCALE = [100.0, -100.0, 100.0]
CONVERSION_ROTATION = [90.0, 0.0, 0.0]
FLIP_U = False
FLIP_V = True
