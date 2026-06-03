#include "BedlamClothSetupCommands.h"

// Engine types
#include "Engine/StaticMesh.h"
#include "Engine/SkeletalMesh.h"
#include "PhysicsEngine/PhysicsAsset.h"
#include "Animation/Skeleton.h"

// Dataflow framework
#include "Dataflow/DataflowObject.h"
#include "Dataflow/DataflowGraph.h"
#include "Dataflow/DataflowNode.h"
#include "Dataflow/DataflowNodeFactory.h"
#include "Dataflow/DataflowBlueprintLibrary.h"

// Chaos Cloth Asset
#include "ChaosClothAsset/ClothAsset.h"
#include "ChaosClothAsset/ClothComponent.h"

// Chaos Caching
#include "Chaos/CacheManagerActor.h"
#include "Chaos/CacheCollection.h"
#include "Chaos/ChaosCache.h"

// Scene / actors for cache recording
#include "Editor.h"
#include "Editor/EditorEngine.h"
#include "PlayInEditorDataTypes.h"
#include "Engine/World.h"
#include "Animation/SkeletalMeshActor.h"
#include "Components/SkeletalMeshComponent.h"
#include "Animation/AnimSequence.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Containers/Ticker.h"
#include "Misc/App.h"

// Chaos Cloth Dataflow Nodes
#include "ChaosClothAsset/StaticMeshImportNode.h"
#include "ChaosClothAsset/TransferSkinWeightsNode.h"
#include "ChaosClothAsset/SetPhysicsAssetNode.h"
#include "ChaosClothAsset/SimulationMaxDistanceConfigNode.h"
#include "ChaosClothAsset/SimulationStretchConfigNode.h"
#include "ChaosClothAsset/SimulationBendingConfigNode.h"
#include "ChaosClothAsset/SimulationGravityConfigNode.h"
#include "ChaosClothAsset/SimulationCollisionConfigNode.h"
#include "ChaosClothAsset/SimulationSelfCollisionConfigNode.h"
#include "ChaosClothAsset/SimulationDampingConfigNode.h"
#include "ChaosClothAsset/SimulationSolverConfigNode.h"
#include "ChaosClothAsset/TerminalNode.h"

// Weight maps (SetWeightMap auto/file)
#include "ChaosClothAsset/CollectionClothFacade.h"   // FCollectionClothConstFacade (sim vertex access)
#include "ChaosClothAsset/ClothCollectionGroup.h"    // ClothCollectionGroup::SimVertices3D
#include "Dataflow/DataflowCollectionAddScalarVertexPropertyNode.h" // PaintWeightMap node
#include "Rendering/SkeletalMeshModel.h"             // FSkeletalMeshModel / LODModels
#include "Rendering/SkeletalMeshLODModel.h"          // FSkeletalMeshLODModel::GetVertices
#include "SkeletalMeshTypes.h"                        // FSoftSkinVertex
#include "Misc/FileHelper.h"                          // file mode + record-status marker
#include "Misc/PackageName.h"                         // package path -> on-disk filename
#include "HAL/FileManager.h"                          // delete stale marker

// Editor utilities
#include "EditorAssetLibrary.h"

DEFINE_LOG_CATEGORY_STATIC(LogBedlamCloth, Log, All);

// ===========================================================================
// Cloth simulation tuning — EDIT THESE to change how the garment behaves.
// These are the defaults baked into every asset by CreateClothAsset. They can
// still be overridden per-asset afterwards with BedlamCloth.ConfigureSimulation
// / BedlamCloth.SetWeightMap.
//
// IMPORTANT: these are WeightedValue configs with NO weight map, so the sim
// uses only `.Low` — but we always set Low == High so the value is unambiguous
// and survives a later weight map being painted in.
// ===========================================================================
namespace BedlamClothDefaults
{
	// How far (cm) a cloth particle may drift from its skinned position on the
	// body (MaxDistance). This is the main knob for the "cloth sticks to the
	// body / to a passing hand" problem:
	//   - smaller (e.g. the old 4 cm) hugs the body rigidly, so the garment
	//     gets dragged along by anything it touches (a hand brushing it sticks);
	//   - larger lets the garment hang/swing and fall away naturally.
	// Stay within ~8-15 cm: too large and the garment drifts/flies off the body.
	static constexpr float MaxDistanceCm = 10.f;

	// PBD stretch stiffness, internally clamped to [0,1]. Keep high so the
	// garment does not visibly stretch; 1.0 = effectively inextensible.
	static constexpr float StretchStiffness = 1.f;

	// PBD bending stiffness, internally clamped to [0,1]. Lower = floppier, more
	// natural folds and drape; higher (1.0) = stiff, card-like. Lowered from the
	// node default of 1.0 so the cloth flows rather than moving as a rigid shell.
	static constexpr float BendingStiffness = 0.5f;

	// Global velocity (point) damping [0,1]. Small values keep the cloth lively;
	// larger values slow it down and can read as sluggish/sticky. 0.01 is the UE
	// default and a good starting point.
	static constexpr float DampingCoefficient = 0.01f;

	// --- Collision (applies to every mode) ------------------------------------
	// Collision against the body's physics asset is always on (the cloth solver's
	// simple colliders + RecordChaosCache's AddCollisionSource). These knobs make
	// it more robust so dynamic cloth rests on the body instead of passing through
	// the coarse capsule collision. NOTE: collision only prevents penetration — it
	// does NOT hold an un-anchored garment up. A fully dynamic garment
	// (SetWeightMap all_dynamic, no kinematic pinning) will still slide off under
	// gravity; keep some anchoring (auto/file, or a finite MaxDistance) for a
	// wearable result.
	//
	// Added thickness (cm) of the body collision shapes; larger = caught a little
	// further from the surface (fewer gaps), at the cost of a looser-looking drape.
	static constexpr float CollisionThicknessCm = 1.0f;
	// Cloth<->body friction; higher = grips the body more (slides off less).
	static constexpr float FrictionCoefficient = 0.8f;
	// Continuous collision detection: stops fast-moving cloth particles tunnelling
	// through the thin capsule collision in one step. The main lever against the
	// cloth "falling through" the body; recording is offline so the cost is fine.
	static constexpr bool bUseContinuousCollision = true;

	// --- SetWeightMap knobs ---------------------------------------------------
	// "all_dynamic": uniform MaxDistance (Low==High) large enough to be
	// effectively unconstrained, so the whole garment is free.
	static constexpr float AllDynamicMaxDistanceCm = 1000.f;

	// For weight-mapped modes (auto/file) the per-vertex weight (0..1) interpolates
	// MaxDistance between these two: weight 0 -> Low (kinematic/pinned to body),
	// weight 1 -> High (free to drift up to this many cm).
	static constexpr float WeightMapKinematicLowCm = 0.f;
	static constexpr float WeightMapDynamicHighCm  = 20.f;

	// "auto" proximity ramp (cm): sim vertices within Threshold of the body are
	// kinematic (weight 0); past Threshold+Band they are fully dynamic (weight 1);
	// linear in between. Both are overridable as trailing command arguments.
	static constexpr float AutoKinematicThresholdCm = 2.f;
	static constexpr float AutoDynamicBandCm        = 8.f;
}

// ---------------------------------------------------------------------------
// Helper: create a Dataflow node by registered type name
// ---------------------------------------------------------------------------
static TSharedPtr<FDataflowNode> CreateDataflowNode(
	UE::Dataflow::FGraph& Graph,
	UDataflow* OwningAsset,
	const FName& TypeName,
	const FName& NodeName)
{
	UE::Dataflow::FNewNodeParameters Params;
	Params.Type = TypeName;
	Params.Name = NodeName;
	Params.Guid = FGuid::NewGuid();
	Params.OwningObject = OwningAsset;

	TSharedPtr<FDataflowNode> Node =
		UE::Dataflow::FNodeFactory::GetInstance()->NewNodeFromRegisteredType(Graph, Params);

	if (!Node)
	{
		UE_LOG(LogBedlamCloth, Error, TEXT("Failed to create Dataflow node type='%s' name='%s'"),
			*TypeName.ToString(), *NodeName.ToString());
	}
	return Node;
}

// ---------------------------------------------------------------------------
// Helper: connect two nodes via their "Collection" passthrough pins
// ---------------------------------------------------------------------------
static bool ConnectCollectionPins(
	UE::Dataflow::FGraph& Graph,
	TSharedPtr<FDataflowNode>& FromNode,
	TSharedPtr<FDataflowNode>& ToNode)
{
	FDataflowOutput* Out = FromNode->FindOutput(FName("Collection"));
	FDataflowInput* In = ToNode->FindInput(FName("Collection"));
	if (Out && In)
	{
		Graph.Connect(Out, In);
		return true;
	}
	UE_LOG(LogBedlamCloth, Warning,
		TEXT("ConnectCollection failed: %s.Collection(%s) -> %s.Collection(%s)"),
		*FromNode->GetName().ToString(), Out ? TEXT("ok") : TEXT("null"),
		*ToNode->GetName().ToString(), In ? TEXT("ok") : TEXT("null"));
	return false;
}

// ===========================================================================
// BedlamCloth.CreateClothAsset
// ===========================================================================
void FBedlamClothSetupCommands::CreateClothAsset(const TArray<FString>& Args, UWorld* World)
{
	if (Args.Num() < 3)
	{
		UE_LOG(LogBedlamCloth, Error, TEXT("Usage: BedlamCloth.CreateClothAsset <GarmentMeshPath> <BodySkeletalMeshPath> <OutputAssetPath>"));
		return;
	}

	const FString& GarmentPath = Args[0];
	const FString& BodyPath    = Args[1];
	const FString& OutputPath  = Args[2];

	UE_LOG(LogBedlamCloth, Log, TEXT("CreateClothAsset: Garment=%s Body=%s Output=%s"),
		*GarmentPath, *BodyPath, *OutputPath);

	// -----------------------------------------------------------------------
	// 1. Load source assets
	// -----------------------------------------------------------------------
	UStaticMesh* GarmentMesh = LoadObject<UStaticMesh>(nullptr, *GarmentPath);
	if (!GarmentMesh)
	{
		UE_LOG(LogBedlamCloth, Error, TEXT("Failed to load garment StaticMesh: %s"), *GarmentPath);
		return;
	}

	USkeletalMesh* BodyMesh = LoadObject<USkeletalMesh>(nullptr, *BodyPath);
	if (!BodyMesh)
	{
		UE_LOG(LogBedlamCloth, Error, TEXT("Failed to load body SkeletalMesh: %s"), *BodyPath);
		return;
	}

	UPhysicsAsset* PhysAsset = BodyMesh->GetPhysicsAsset();
	USkeleton* Skeleton = BodyMesh->GetSkeleton();

	UE_LOG(LogBedlamCloth, Log, TEXT("Source assets loaded. PhysicsAsset=%s Skeleton=%s"),
		PhysAsset ? *PhysAsset->GetName() : TEXT("null"),
		Skeleton  ? *Skeleton->GetName()  : TEXT("null"));

	// -----------------------------------------------------------------------
	// 2. Create UDataflow asset (holds the graph)
	// -----------------------------------------------------------------------
	const FString DataflowPath = OutputPath + TEXT("_DF");
	const FString DataflowName = FPackageName::GetShortName(DataflowPath);

	UPackage* DFPackage = CreatePackage(*DataflowPath);
	UDataflow* DataflowAsset = NewObject<UDataflow>(
		DFPackage, *DataflowName, RF_Public | RF_Standalone);

	if (!DataflowAsset)
	{
		UE_LOG(LogBedlamCloth, Error, TEXT("Failed to create UDataflow asset at %s"), *DataflowPath);
		return;
	}

	TSharedPtr<UE::Dataflow::FGraph> Graph = DataflowAsset->GetDataflow();
	if (!Graph.IsValid())
	{
		UE_LOG(LogBedlamCloth, Error, TEXT("UDataflow has no valid graph"));
		return;
	}

	// -----------------------------------------------------------------------
	// 3. Create all Dataflow nodes
	// -----------------------------------------------------------------------
	auto ImportNode         = CreateDataflowNode(*Graph, DataflowAsset, FName("FChaosClothAssetStaticMeshImportNode_v2"),              FName("Import"));
	auto TransferNode       = CreateDataflowNode(*Graph, DataflowAsset, FName("FChaosClothAssetTransferSkinWeightsNode"),            FName("TransferWeights"));
	auto PhysAssetNode      = CreateDataflowNode(*Graph, DataflowAsset, FName("FChaosClothAssetSetPhysicsAssetNode"),                FName("SetPhysAsset"));
	auto MaxDistNode        = CreateDataflowNode(*Graph, DataflowAsset, FName("FChaosClothAssetSimulationMaxDistanceConfigNode"),    FName("MaxDist"));
	auto StretchNode        = CreateDataflowNode(*Graph, DataflowAsset, FName("FChaosClothAssetSimulationStretchConfigNode"),        FName("Stretch"));
	auto BendingNode        = CreateDataflowNode(*Graph, DataflowAsset, FName("FChaosClothAssetSimulationBendingConfigNode"),        FName("Bending"));
	auto GravityNode        = CreateDataflowNode(*Graph, DataflowAsset, FName("FChaosClothAssetSimulationGravityConfigNode"),        FName("Gravity"));
	auto CollisionNode      = CreateDataflowNode(*Graph, DataflowAsset, FName("FChaosClothAssetSimulationCollisionConfigNode"),      FName("Collision"));
	auto DampingNode        = CreateDataflowNode(*Graph, DataflowAsset, FName("FChaosClothAssetSimulationDampingConfigNode"),        FName("Damping"));
	auto SolverNode         = CreateDataflowNode(*Graph, DataflowAsset, FName("FChaosClothAssetSimulationSolverConfigNode"),         FName("Solver"));
	auto TerminalNode       = CreateDataflowNode(*Graph, DataflowAsset, FName("FChaosClothAssetTerminalNode_v2"),       FName("Terminal"));

	if (!ImportNode || !TransferNode || !PhysAssetNode || !MaxDistNode ||
		!StretchNode || !BendingNode || !GravityNode || !CollisionNode ||
		!DampingNode || !SolverNode || !TerminalNode)
	{
		UE_LOG(LogBedlamCloth, Error, TEXT("One or more Dataflow nodes failed to create. Aborting."));
		return;
	}

	UE_LOG(LogBedlamCloth, Log, TEXT("All 11 Dataflow nodes created."));

	// -----------------------------------------------------------------------
	// 4. Set node properties
	// -----------------------------------------------------------------------

	// Import node — garment mesh reference
	{
		auto* Typed = static_cast<FChaosClothAssetStaticMeshImportNode_v2*>(ImportNode.Get());
		Typed->StaticMesh = GarmentMesh;
	}

	// Transfer skin weights — body SKM reference
	{
		auto* Typed = static_cast<FChaosClothAssetTransferSkinWeightsNode*>(TransferNode.Get());
		Typed->SkeletalMesh = BodyMesh;
	}

	// Physics asset
	if (PhysAsset)
	{
		auto* Typed = static_cast<FChaosClothAssetSetPhysicsAssetNode*>(PhysAssetNode.Get());
		Typed->PhysicsAsset = PhysAsset;
	}

	// Max distance — how far the garment may move from its skinned body position.
	// Loosened from the old 4 cm so the cloth drapes/swings and falls away from
	// the body (and from a hand brushing it) instead of sticking rigidly. See
	// BedlamClothDefaults::MaxDistanceCm to tune. Low is the value used when there
	// is no weight map; set both Low and High so the limit is unambiguous.
	{
		auto* Typed = static_cast<FChaosClothAssetSimulationMaxDistanceConfigNode*>(MaxDistNode.Get());
		Typed->MaxDistance.Low  = BedlamClothDefaults::MaxDistanceCm;
		Typed->MaxDistance.High = BedlamClothDefaults::MaxDistanceCm;
	}

	// Stretch & Bending give the cloth structural integrity (resistance to
	// stretching and folding); without these config nodes the mesh deforms
	// without limit. Stretch stays stiff (no visible stretch); Bending is lowered
	// from the node default of 1.0 so the cloth folds/flows naturally rather than
	// moving like a rigid shell. Tune via BedlamClothDefaults.
	{
		auto* Typed = static_cast<FChaosClothAssetSimulationStretchConfigNode*>(StretchNode.Get());
		Typed->StretchStiffness.Low  = BedlamClothDefaults::StretchStiffness;
		Typed->StretchStiffness.High = BedlamClothDefaults::StretchStiffness;
	}
	{
		auto* Typed = static_cast<FChaosClothAssetSimulationBendingConfigNode*>(BendingNode.Get());
		Typed->BendingStiffness.Low  = BedlamClothDefaults::BendingStiffness;
		Typed->BendingStiffness.High = BedlamClothDefaults::BendingStiffness;
	}

	// Damping — global point damping; small keeps the cloth lively. Tune via
	// BedlamClothDefaults::DampingCoefficient.
	{
		auto* Typed = static_cast<FChaosClothAssetSimulationDampingConfigNode*>(DampingNode.Get());
		Typed->DampingCoefficientWeighted.Low  = BedlamClothDefaults::DampingCoefficient;
		Typed->DampingCoefficientWeighted.High = BedlamClothDefaults::DampingCoefficient;
	}

	// Collision — collide robustly against the body's physics asset. Collision is
	// already enabled by default (bEnableSimpleColliders); we add a little
	// thickness + friction and turn on CCD so dynamic cloth rests on the body
	// instead of tunnelling through the coarse capsule collision. Tune via
	// BedlamClothDefaults. (Does not hold an un-anchored garment up — see notes.)
	{
		auto* Typed = static_cast<FChaosClothAssetSimulationCollisionConfigNode*>(CollisionNode.Get());
		Typed->CollisionThicknessImported.ImportedValue = BedlamClothDefaults::CollisionThicknessCm;
		Typed->FrictionCoefficientWeighted.Low  = BedlamClothDefaults::FrictionCoefficient;
		Typed->FrictionCoefficientWeighted.High = BedlamClothDefaults::FrictionCoefficient;
		Typed->bUseCCD = BedlamClothDefaults::bUseContinuousCollision;
	}

	// Gravity, Solver — use UE defaults.
	// Self-collision intentionally omitted: it is the dominant cost on this dense
	// (~36k particle) sim mesh and can be re-introduced once the sim is validated.

	// -----------------------------------------------------------------------
	// 5. Connect the node chain via Collection passthrough
	// -----------------------------------------------------------------------
	ConnectCollectionPins(*Graph, ImportNode,    TransferNode);
	ConnectCollectionPins(*Graph, TransferNode,  PhysAssetNode);
	ConnectCollectionPins(*Graph, PhysAssetNode, MaxDistNode);
	ConnectCollectionPins(*Graph, MaxDistNode,   StretchNode);
	ConnectCollectionPins(*Graph, StretchNode,   BendingNode);
	ConnectCollectionPins(*Graph, BendingNode,   GravityNode);
	ConnectCollectionPins(*Graph, GravityNode,   CollisionNode);
	ConnectCollectionPins(*Graph, CollisionNode, DampingNode);
	ConnectCollectionPins(*Graph, DampingNode,   SolverNode);

	// Solver → Terminal: terminal v2 uses dynamic LOD inputs (not plain "Collection")
	{
		FDataflowOutput* SolverOut = SolverNode->FindOutput(FName("Collection"));
		FDataflowInput*  TermIn    = nullptr;

		// Try known pin names for the terminal's first LOD input
		TermIn = TerminalNode->FindInput(FName("CollectionLod0"));
		if (!TermIn) TermIn = TerminalNode->FindInput(FName("CollectionLods"));
		if (!TermIn) TermIn = TerminalNode->FindInput(FName("Collection"));

		// Fallback: take the first input available on the terminal node
		if (!TermIn)
		{
			for (FDataflowInput* Input : TerminalNode->GetInputs())
			{
				TermIn = Input;
				UE_LOG(LogBedlamCloth, Log, TEXT("Terminal: using first input '%s'"),
					*Input->GetName().ToString());
				break;
			}
		}

		if (SolverOut && TermIn)
		{
			Graph->Connect(SolverOut, TermIn);
			UE_LOG(LogBedlamCloth, Log, TEXT("Connected Solver → Terminal (pin: %s)"),
				*TermIn->GetName().ToString());
		}
		else
		{
			UE_LOG(LogBedlamCloth, Error, TEXT("Could not connect Solver → Terminal (SolverOut=%s TermIn=%s)"),
				SolverOut ? TEXT("ok") : TEXT("null"), TermIn ? TEXT("ok") : TEXT("null"));
		}
	}

	// -----------------------------------------------------------------------
	// 6. Create UChaosClothAsset and associate the Dataflow
	// -----------------------------------------------------------------------
	const FString ClothAssetName = FPackageName::GetShortName(OutputPath);
	UPackage* ClothPackage = CreatePackage(*OutputPath);
	UChaosClothAsset* ClothAsset = NewObject<UChaosClothAsset>(
		ClothPackage, *ClothAssetName, RF_Public | RF_Standalone);

	if (!ClothAsset)
	{
		UE_LOG(LogBedlamCloth, Error, TEXT("Failed to create UChaosClothAsset at %s"), *OutputPath);
		return;
	}

	ClothAsset->GetDataflowInstance().SetDataflowAsset(DataflowAsset);
	ClothAsset->GetDataflowInstance().SetDataflowTerminal(FName("Terminal"));

	if (Skeleton)  ClothAsset->SetSkeleton(Skeleton);
	if (PhysAsset) ClothAsset->SetPhysicsAsset(PhysAsset);

	// -----------------------------------------------------------------------
	// 7. Evaluate the Dataflow to build the cloth asset
	// -----------------------------------------------------------------------
	UE_LOG(LogBedlamCloth, Log, TEXT("Evaluating Dataflow to build cloth asset..."));
	UDataflowBlueprintLibrary::EvaluateTerminalNodeByName(
		DataflowAsset, FName("Terminal"), ClothAsset);

	// -----------------------------------------------------------------------
	// 8. Save both assets
	// -----------------------------------------------------------------------
	DataflowAsset->MarkPackageDirty();
	ClothAsset->MarkPackageDirty();

	const bool bSavedDF    = UEditorAssetLibrary::SaveAsset(DataflowPath, false);
	const bool bSavedCloth = UEditorAssetLibrary::SaveAsset(OutputPath,   false);

	if (bSavedDF && bSavedCloth)
	{
		UE_LOG(LogBedlamCloth, Log, TEXT("SUCCESS: Cloth asset created and saved at %s"), *OutputPath);
	}
	else
	{
		UE_LOG(LogBedlamCloth, Warning,
			TEXT("Cloth asset created but save may have issues. Dataflow=%s ClothAsset=%s"),
			bSavedDF ? TEXT("OK") : TEXT("FAIL"), bSavedCloth ? TEXT("OK") : TEXT("FAIL"));
	}
}

// Name of the PaintWeightMap node that SetWeightMap auto/file splices into the
// graph. Used both to create/reuse it and to detect (in ConfigureSimulation)
// whether a per-vertex weight map is currently active on the MaxDistance node.
static const FName GBedlamWeightMapNodeName(TEXT("BedlamWeightMap"));

// ---------------------------------------------------------------------------
// Helper: find a node by type name in an existing Dataflow graph
// ---------------------------------------------------------------------------
static FDataflowNode* FindNodeByType(UE::Dataflow::FGraph& Graph, const FName& TypeName)
{
	for (TSharedPtr<FDataflowNode>& Node : Graph.GetNodes())
	{
		if (Node.IsValid() && Node->GetType() == TypeName)
		{
			return Node.Get();
		}
	}
	return nullptr;
}

// ---------------------------------------------------------------------------
// Helper: find a node by instance name in an existing Dataflow graph
// ---------------------------------------------------------------------------
static FDataflowNode* FindNodeByName(UE::Dataflow::FGraph& Graph, const FName& NodeName)
{
	for (TSharedPtr<FDataflowNode>& Node : Graph.GetNodes())
	{
		if (Node.IsValid() && Node->GetName() == NodeName)
		{
			return Node.Get();
		}
	}
	return nullptr;
}

// ---------------------------------------------------------------------------
// Helper: load a cloth asset and get its Dataflow graph (returns false on failure)
// ---------------------------------------------------------------------------
static bool LoadClothAndGraph(
	const FString& ClothAssetPath,
	UChaosClothAsset*& OutClothAsset,
	UDataflow*& OutDataflow,
	TSharedPtr<UE::Dataflow::FGraph>& OutGraph)
{
	OutClothAsset = LoadObject<UChaosClothAsset>(nullptr, *ClothAssetPath);
	if (!OutClothAsset)
	{
		UE_LOG(LogBedlamCloth, Error, TEXT("Failed to load ChaosClothAsset: %s"), *ClothAssetPath);
		return false;
	}

	OutDataflow = OutClothAsset->GetDataflowInstance().GetDataflowAsset();
	if (!OutDataflow)
	{
		UE_LOG(LogBedlamCloth, Error, TEXT("ClothAsset has no associated UDataflow asset"));
		return false;
	}

	OutGraph = OutDataflow->GetDataflow();
	if (!OutGraph.IsValid())
	{
		UE_LOG(LogBedlamCloth, Error, TEXT("UDataflow has no valid graph"));
		return false;
	}

	return true;
}

// ---------------------------------------------------------------------------
// Helper: re-evaluate a cloth asset's Dataflow and save both assets
// ---------------------------------------------------------------------------
static void ReevaluateAndSave(
	UChaosClothAsset* ClothAsset,
	UDataflow* DataflowAsset,
	const FString& ClothAssetPath)
{
	UE_LOG(LogBedlamCloth, Log, TEXT("Re-evaluating Dataflow..."));
	UDataflowBlueprintLibrary::EvaluateTerminalNodeByName(
		DataflowAsset, FName("Terminal"), ClothAsset);

	DataflowAsset->MarkPackageDirty();
	ClothAsset->MarkPackageDirty();

	const FString DFAssetPath = DataflowAsset->GetOutermost()->GetName();
	UEditorAssetLibrary::SaveAsset(DFAssetPath, false);
	UEditorAssetLibrary::SaveAsset(ClothAssetPath, false);

	UE_LOG(LogBedlamCloth, Log, TEXT("Assets saved."));
}

// ---------------------------------------------------------------------------
// Helper: ensure a PaintWeightMap node feeds per-vertex weights into the chain
// just before the MaxDistance config node, and set its weights.
//
// The MaxDistance config node reads a weight map *by name* from the cloth
// collection at evaluation time (default name "MaxDistance"). A generic
// FDataflowCollectionAddScalarVertexPropertyNode ("PaintWeightMap") writes a
// float attribute of that name into the SimVertices3D group — which the cloth
// facade recognises as a weight map. We splice one in front of MaxDistance:
//     <prev> -> PaintWeightMap -> MaxDistance
// On the first call the node is created+inserted; later calls reuse it (so the
// command is idempotent and re-runnable). Weights are clamped to [0,1] by the
// node; index i corresponds to SimVertices3D vertex i.
// ---------------------------------------------------------------------------
static bool InsertOrUpdateSimWeightMapNode(
	UE::Dataflow::FGraph& Graph,
	UDataflow* DataflowAsset,
	const FName& WeightMapName,
	const TArray<float>& VertexWeights)
{
	using FPaintNode = FDataflowCollectionAddScalarVertexPropertyNode;
	static const FName PaintNodeType("FDataflowCollectionAddScalarVertexPropertyNode");
	const FName& PaintNodeName = GBedlamWeightMapNodeName;

	FDataflowNode* MaxDist = FindNodeByType(Graph, FName("FChaosClothAssetSimulationMaxDistanceConfigNode"));
	if (!MaxDist)
	{
		UE_LOG(LogBedlamCloth, Error, TEXT("MaxDistance config node not found; cannot attach weight map."));
		return false;
	}
	FDataflowInput* MaxDistIn = MaxDist->FindInput(FName("Collection"));
	if (!MaxDistIn)
	{
		UE_LOG(LogBedlamCloth, Error, TEXT("MaxDistance node has no Collection input."));
		return false;
	}

	// Reuse the Bedlam paint node if a previous SetWeightMap already inserted one.
	FPaintNode* Paint = nullptr;
	for (TSharedPtr<FDataflowNode>& N : Graph.GetNodes())
	{
		if (N.IsValid() && N->GetName() == PaintNodeName && N->GetType() == PaintNodeType)
		{
			Paint = static_cast<FPaintNode*>(N.Get());
			break;
		}
	}

	if (!Paint)
	{
		TSharedPtr<FDataflowNode> PaintShared =
			CreateDataflowNode(Graph, DataflowAsset, PaintNodeType, PaintNodeName);
		if (!PaintShared)
		{
			return false;
		}
		Paint = static_cast<FPaintNode*>(PaintShared.Get());

		FDataflowInput*  PaintIn  = PaintShared->FindInput(FName("Collection"));
		FDataflowOutput* PaintOut = PaintShared->FindOutput(FName("Collection"));
		if (!PaintIn || !PaintOut)
		{
			UE_LOG(LogBedlamCloth, Error, TEXT("PaintWeightMap node missing Collection pins."));
			return false;
		}

		// Splice in front of MaxDistance: reroute whatever feeds MaxDistance
		// through the new node instead.
		FDataflowOutput* PrevOut = MaxDistIn->GetConnection();
		if (PrevOut)
		{
			Graph.Disconnect(PrevOut, MaxDistIn);
			Graph.Connect(PrevOut, PaintIn);
		}
		else
		{
			UE_LOG(LogBedlamCloth, Warning,
				TEXT("MaxDistance Collection input had no upstream connection; weight map node left unfed."));
		}
		Graph.Connect(PaintOut, MaxDistIn);
		UE_LOG(LogBedlamCloth, Log, TEXT("Inserted PaintWeightMap node '%s' before MaxDistance."),
			*PaintNodeName.ToString());
	}

	Paint->Name            = WeightMapName.ToString();
	Paint->TargetGroup.Name = UE::Chaos::ClothAsset::ClothCollectionGroup::SimVertices3D;
	Paint->OverrideType    = EDataflowWeightMapOverrideType::ReplaceAll;
	Paint->VertexWeights   = VertexWeights;

	UE_LOG(LogBedlamCloth, Log,
		TEXT("Weight map '%s' set with %d vertex weights (group SimVertices3D)."),
		*WeightMapName.ToString(), VertexWeights.Num());
	return true;
}

// ---------------------------------------------------------------------------
// Helper: read sim vertex count (and optionally positions) from a cloth asset's
// baked collection (LOD0). Returns false if there is no baked collection.
// ---------------------------------------------------------------------------
static bool GetSimVertices(
	UChaosClothAsset* ClothAsset,
	int32& OutNumSimVertices,
	TConstArrayView<FVector3f>* OutPositions = nullptr)
{
	// Call the const overload of GetClothCollections() (the non-const one is
	// deprecated in 5.7); we only read from the collection here.
	const TArray<TSharedRef<const FManagedArrayCollection>>& Collections =
		const_cast<const UChaosClothAsset*>(ClothAsset)->GetClothCollections();
	if (Collections.Num() == 0)
	{
		UE_LOG(LogBedlamCloth, Error,
			TEXT("Cloth asset has no baked collection; run CreateClothAsset first."));
		return false;
	}
	UE::Chaos::ClothAsset::FCollectionClothConstFacade Facade(Collections[0]);
	OutNumSimVertices = Facade.GetNumSimVertices3D();
	if (OutPositions)
	{
		*OutPositions = Facade.GetSimPosition3D();
	}
	return OutNumSimVertices > 0;
}

// ---------------------------------------------------------------------------
// Helper (auto mode): compute a per-sim-vertex weight (0..1) from proximity to
// the body. Vertices within ThresholdCm of the nearest body vertex are kinematic
// (weight 0); past ThresholdCm+BandCm they are fully dynamic (weight 1); linear
// between. Uses a uniform spatial grid over body vertices for speed.
// ---------------------------------------------------------------------------
static bool ComputeAutoWeights(
	UChaosClothAsset* ClothAsset,
	USkeletalMesh* BodyMesh,
	float ThresholdCm,
	float BandCm,
	TArray<float>& OutWeights)
{
	int32 NumSim = 0;
	TConstArrayView<FVector3f> SimPos;
	if (!GetSimVertices(ClothAsset, NumSim, &SimPos))
	{
		return false;
	}

	FSkeletalMeshModel* Model = BodyMesh->GetImportedModel();
	if (!Model || Model->LODModels.Num() == 0)
	{
		UE_LOG(LogBedlamCloth, Error, TEXT("Body mesh has no imported model / LOD0."));
		return false;
	}
	TArray<FSoftSkinVertex> BodyVerts;
	Model->LODModels[0].GetVertices(BodyVerts);
	if (BodyVerts.Num() == 0)
	{
		UE_LOG(LogBedlamCloth, Error, TEXT("Body mesh LOD0 has 0 vertices."));
		return false;
	}

	const float Band    = FMath::Max(BandCm, KINDA_SMALL_NUMBER);
	const float Radius  = ThresholdCm + Band;            // beyond this, weight is 1
	const float Cell    = FMath::Max(Radius, 1.0f);      // grid cell == search radius
	const float InvCell = 1.0f / Cell;

	auto CellOf = [InvCell](const FVector3f& P)
	{
		return FIntVector(
			FMath::FloorToInt(P.X * InvCell),
			FMath::FloorToInt(P.Y * InvCell),
			FMath::FloorToInt(P.Z * InvCell));
	};

	TMap<FIntVector, TArray<int32>> Grid;
	Grid.Reserve(BodyVerts.Num());
	for (int32 i = 0; i < BodyVerts.Num(); ++i)
	{
		Grid.FindOrAdd(CellOf(BodyVerts[i].Position)).Add(i);
	}

	OutWeights.SetNumUninitialized(NumSim);
	const float Radius2 = Radius * Radius;
	int32 NumKinematic = 0;
	double SumDist = 0.0;

	for (int32 v = 0; v < NumSim; ++v)
	{
		const FVector3f P = SimPos[v];
		const FIntVector C = CellOf(P);
		float BestSq = Radius2;
		for (int32 dz = -1; dz <= 1; ++dz)
		{
			for (int32 dy = -1; dy <= 1; ++dy)
			{
				for (int32 dx = -1; dx <= 1; ++dx)
				{
					if (const TArray<int32>* Bucket = Grid.Find(C + FIntVector(dx, dy, dz)))
					{
						for (int32 bi : *Bucket)
						{
							const float D2 = FVector3f::DistSquared(P, BodyVerts[bi].Position);
							if (D2 < BestSq)
							{
								BestSq = D2;
							}
						}
					}
				}
			}
		}

		const float Dist = FMath::Sqrt(BestSq);
		float W;
		if (Dist <= ThresholdCm)   { W = 0.f; }
		else if (Dist >= Radius)   { W = 1.f; }
		else                       { W = (Dist - ThresholdCm) / Band; }
		OutWeights[v] = W;
		if (W <= 0.f) { ++NumKinematic; }
		SumDist += Dist;
	}

	UE_LOG(LogBedlamCloth, Log,
		TEXT("auto: %d sim verts vs %d body verts. Threshold=%.1f Band=%.1f cm. Kinematic(weight=0)=%d (%.0f%%), meanDist=%.2fcm."),
		NumSim, BodyVerts.Num(), ThresholdCm, BandCm, NumKinematic,
		NumSim > 0 ? 100.f * NumKinematic / NumSim : 0.f,
		NumSim > 0 ? (float)(SumDist / NumSim) : 0.f);
	return true;
}

// ---------------------------------------------------------------------------
// Helper (file mode): parse per-vertex weights from a text file. Accepts any
// mix of commas/whitespace/newlines as separators; values are clamped to [0,1].
// One value per sim vertex (index order). Pads/truncates to NumSim with a warning.
// ---------------------------------------------------------------------------
static bool LoadWeightsFromFile(const FString& FilePath, int32 NumSim, TArray<float>& OutWeights)
{
	FString Text;
	if (!FFileHelper::LoadFileToString(Text, *FilePath))
	{
		UE_LOG(LogBedlamCloth, Error, TEXT("Failed to read weight file: %s"), *FilePath);
		return false;
	}

	// Normalise commas to spaces, then split on whitespace/newlines.
	Text.ReplaceInline(TEXT(","), TEXT(" "));
	TArray<FString> Tokens;
	Text.ParseIntoArrayWS(Tokens);

	TArray<float> Values;
	Values.Reserve(Tokens.Num());
	for (const FString& Tok : Tokens)
	{
		Values.Add(FMath::Clamp((float)FCString::Atod(*Tok), 0.f, 1.f));
	}

	if (Values.Num() == 0)
	{
		UE_LOG(LogBedlamCloth, Error, TEXT("No numeric weights parsed from %s"), *FilePath);
		return false;
	}
	if (Values.Num() != NumSim)
	{
		UE_LOG(LogBedlamCloth, Warning,
			TEXT("Weight file has %d values but sim mesh has %d vertices; padding/truncating to fit."),
			Values.Num(), NumSim);
	}

	OutWeights.SetNumZeroed(NumSim);
	const int32 N = FMath::Min(Values.Num(), NumSim);
	for (int32 i = 0; i < N; ++i)
	{
		OutWeights[i] = Values[i];
	}
	UE_LOG(LogBedlamCloth, Log, TEXT("file: loaded %d weights (using %d) from %s"),
		Values.Num(), N, *FilePath);
	return true;
}

// ===========================================================================
// BedlamCloth.SetWeightMap
// ===========================================================================
void FBedlamClothSetupCommands::SetWeightMap(const TArray<FString>& Args, UWorld* World)
{
	if (Args.Num() < 2)
	{
		UE_LOG(LogBedlamCloth, Error,
			TEXT("Usage: BedlamCloth.SetWeightMap <ClothAssetPath> <Mode> [args]\n")
			TEXT("  all_dynamic                                : uniform, fully dynamic (weight map ignored)\n")
			TEXT("  auto <BodySKMPath> [ThresholdCm] [BandCm]  : kinematic near the body, dynamic away from it\n")
			TEXT("  file <FilePath>                            : per-vertex weights 0..1, one per sim vertex"));
		return;
	}

	const FString& ClothAssetPath = Args[0];
	const FString& Mode = Args[1];

	UE_LOG(LogBedlamCloth, Log, TEXT("SetWeightMap: Asset=%s Mode=%s"), *ClothAssetPath, *Mode);

	// --- Load cloth asset and its Dataflow graph ---
	UChaosClothAsset* ClothAsset = nullptr;
	UDataflow* DataflowAsset = nullptr;
	TSharedPtr<UE::Dataflow::FGraph> Graph;

	if (!LoadClothAndGraph(ClothAssetPath, ClothAsset, DataflowAsset, Graph))
	{
		return;
	}

	// --- Find the MaxDistance config node ---
	auto* MaxDistNode = static_cast<FChaosClothAssetSimulationMaxDistanceConfigNode*>(
		FindNodeByType(*Graph, FName("FChaosClothAssetSimulationMaxDistanceConfigNode")));

	if (!MaxDistNode)
	{
		UE_LOG(LogBedlamCloth, Error, TEXT("MaxDistance config node not found in Dataflow graph"));
		return;
	}

	// --- Apply mode ---
	if (Mode == TEXT("all_dynamic"))
	{
		// Uniform & fully dynamic. Low==High so the value is unambiguous and any
		// existing weight map (from a prior auto/file run) has no effect — the
		// interpolation between equal bounds is constant. (Footgun fix: the old
		// Low=0/High=1000 made every vertex kinematic because, with no weight map,
		// only Low is used.)
		MaxDistNode->MaxDistance.Low  = BedlamClothDefaults::AllDynamicMaxDistanceCm;
		MaxDistNode->MaxDistance.High = BedlamClothDefaults::AllDynamicMaxDistanceCm;

		UE_LOG(LogBedlamCloth, Warning,
			TEXT("all_dynamic: MaxDistance Low=High=%.0f. This removes ALL kinematic anchoring, so the garment "
			     "is free and will sag/fall off the body. Re-pin with SetWeightMap auto, or set a sane distance "
			     "with ConfigureSimulation MaxDistance=<cm> (which keeps the painted anchoring if present)."),
			BedlamClothDefaults::AllDynamicMaxDistanceCm);
	}
	else if (Mode == TEXT("auto"))
	{
		if (Args.Num() < 3)
		{
			UE_LOG(LogBedlamCloth, Error,
				TEXT("'auto' mode requires a body SkeletalMesh path: SetWeightMap <Cloth> auto <BodySKMPath> [ThresholdCm] [BandCm]"));
			return;
		}
		const FString& BodyPath = Args[2];
		const float ThresholdCm = (Args.Num() >= 4) ? FCString::Atof(*Args[3]) : BedlamClothDefaults::AutoKinematicThresholdCm;
		const float BandCm      = (Args.Num() >= 5) ? FCString::Atof(*Args[4]) : BedlamClothDefaults::AutoDynamicBandCm;

		USkeletalMesh* BodyMesh = LoadObject<USkeletalMesh>(nullptr, *BodyPath);
		if (!BodyMesh)
		{
			UE_LOG(LogBedlamCloth, Error, TEXT("Failed to load body SkeletalMesh: %s"), *BodyPath);
			return;
		}

		TArray<float> Weights;
		if (!ComputeAutoWeights(ClothAsset, BodyMesh, ThresholdCm, BandCm, Weights))
		{
			return;
		}
		if (!InsertOrUpdateSimWeightMapNode(*Graph, DataflowAsset, FName(*MaxDistNode->MaxDistance.WeightMap), Weights))
		{
			return;
		}
		MaxDistNode->MaxDistance.Low  = BedlamClothDefaults::WeightMapKinematicLowCm;
		MaxDistNode->MaxDistance.High = BedlamClothDefaults::WeightMapDynamicHighCm;
	}
	else if (Mode == TEXT("file"))
	{
		if (Args.Num() < 3)
		{
			UE_LOG(LogBedlamCloth, Error, TEXT("'file' mode requires a file path: SetWeightMap <Cloth> file <FilePath>"));
			return;
		}
		const FString& FilePath = Args[2];

		int32 NumSim = 0;
		if (!GetSimVertices(ClothAsset, NumSim))
		{
			return;
		}

		TArray<float> Weights;
		if (!LoadWeightsFromFile(FilePath, NumSim, Weights))
		{
			return;
		}
		if (!InsertOrUpdateSimWeightMapNode(*Graph, DataflowAsset, FName(*MaxDistNode->MaxDistance.WeightMap), Weights))
		{
			return;
		}
		MaxDistNode->MaxDistance.Low  = BedlamClothDefaults::WeightMapKinematicLowCm;
		MaxDistNode->MaxDistance.High = BedlamClothDefaults::WeightMapDynamicHighCm;
	}
	else
	{
		UE_LOG(LogBedlamCloth, Error, TEXT("Unknown mode '%s'. Valid modes: all_dynamic, auto, file"), *Mode);
		return;
	}

	// --- Re-evaluate and save ---
	ReevaluateAndSave(ClothAsset, DataflowAsset, ClothAssetPath);

	UE_LOG(LogBedlamCloth, Log, TEXT("SUCCESS: Weight map updated for %s"), *ClothAssetPath);
}

void FBedlamClothSetupCommands::ConfigureSimulation(const TArray<FString>& Args, UWorld* World)
{
	if (Args.Num() < 2)
	{
		UE_LOG(LogBedlamCloth, Error,
			TEXT("Usage: BedlamCloth.ConfigureSimulation <ClothAssetPath> [Key=Value ...]\n")
			TEXT("  Keys: Gravity (scale), Damping (0-1), SelfCollision (true only),\n")
			TEXT("        Substeps (int), Iterations (int), CollisionThickness (cm),\n")
			TEXT("        Friction (cloth<->body grip), CCD (true/false; anti-tunnelling),\n")
			TEXT("        MaxDistance (cm; if a weight map from SetWeightMap auto/file is\n")
			TEXT("          active it sets the dynamic High and keeps the kinematic Low=0,\n")
			TEXT("          otherwise it sets a uniform Low=High distance)"));
		return;
	}

	const FString& ClothAssetPath = Args[0];
	UE_LOG(LogBedlamCloth, Log, TEXT("ConfigureSimulation: Asset=%s"), *ClothAssetPath);

	// --- Load cloth asset and its Dataflow graph ---
	UChaosClothAsset* ClothAsset = nullptr;
	UDataflow* DataflowAsset = nullptr;
	TSharedPtr<UE::Dataflow::FGraph> Graph;

	if (!LoadClothAndGraph(ClothAssetPath, ClothAsset, DataflowAsset, Graph))
	{
		return;
	}

	// --- Parse Key=Value pairs ---
	TMap<FString, FString> Params;
	for (int32 i = 1; i < Args.Num(); ++i)
	{
		FString Key, Value;
		if (Args[i].Split(TEXT("="), &Key, &Value))
		{
			Params.Add(Key, Value);
		}
		else
		{
			UE_LOG(LogBedlamCloth, Warning, TEXT("Ignoring malformed param (no '='): %s"), *Args[i]);
		}
	}

	int32 AppliedCount = 0;

	// --- Gravity ---
	if (const FString* Val = Params.Find(TEXT("Gravity")))
	{
		auto* Node = static_cast<FChaosClothAssetSimulationGravityConfigNode*>(
			FindNodeByType(*Graph, FName("FChaosClothAssetSimulationGravityConfigNode")));
		if (Node)
		{
			float GravityScale = FCString::Atof(**Val);
			Node->GravityScaleWeighted.Low  = GravityScale;
			Node->GravityScaleWeighted.High = GravityScale;
			UE_LOG(LogBedlamCloth, Log, TEXT("  Gravity scale = %f"), GravityScale);
			++AppliedCount;
		}
		else
		{
			UE_LOG(LogBedlamCloth, Warning, TEXT("  GravityConfigNode not found in graph"));
		}
	}

	// --- Damping ---
	if (const FString* Val = Params.Find(TEXT("Damping")))
	{
		auto* Node = static_cast<FChaosClothAssetSimulationDampingConfigNode*>(
			FindNodeByType(*Graph, FName("FChaosClothAssetSimulationDampingConfigNode")));
		if (Node)
		{
			float Damping = FCString::Atof(**Val);
			Node->DampingCoefficientWeighted.Low  = Damping;
			Node->DampingCoefficientWeighted.High = Damping;
			UE_LOG(LogBedlamCloth, Log, TEXT("  Damping = %f"), Damping);
			++AppliedCount;
		}
		else
		{
			UE_LOG(LogBedlamCloth, Warning, TEXT("  DampingConfigNode not found in graph"));
		}
	}

	// --- SelfCollision ---
	if (const FString* Val = Params.Find(TEXT("SelfCollision")))
	{
		// bUseSelfCollisions is private in v2 node and defaults to true.
		// Self-collision is enabled by the node's presence in the graph.
		// To disable: would need to disconnect the node from the chain (not yet implemented).
		bool bEnable = Val->ToBool();
		if (bEnable)
		{
			UE_LOG(LogBedlamCloth, Log, TEXT("  SelfCollision = true (already enabled via node presence)"));
			++AppliedCount;
		}
		else
		{
			UE_LOG(LogBedlamCloth, Warning,
				TEXT("  SelfCollision=false not supported. Remove the SelfCollision node from the graph to disable."));
		}
	}

	// --- Substeps ---
	if (const FString* Val = Params.Find(TEXT("Substeps")))
	{
		auto* Node = static_cast<FChaosClothAssetSimulationSolverConfigNode*>(
			FindNodeByType(*Graph, FName("FChaosClothAssetSimulationSolverConfigNode")));
		if (Node)
		{
			int32 Substeps = FCString::Atoi(**Val);
			Node->NumSubstepsImported.ImportedValue = Substeps;
			UE_LOG(LogBedlamCloth, Log, TEXT("  Substeps = %d"), Substeps);
			++AppliedCount;
		}
		else
		{
			UE_LOG(LogBedlamCloth, Warning, TEXT("  SolverConfigNode not found in graph"));
		}
	}

	// --- Iterations ---
	if (const FString* Val = Params.Find(TEXT("Iterations")))
	{
		auto* Node = static_cast<FChaosClothAssetSimulationSolverConfigNode*>(
			FindNodeByType(*Graph, FName("FChaosClothAssetSimulationSolverConfigNode")));
		if (Node)
		{
			int32 Iterations = FCString::Atoi(**Val);
			Node->NumIterations = Iterations;
			UE_LOG(LogBedlamCloth, Log, TEXT("  Iterations = %d"), Iterations);
			++AppliedCount;
		}
		else
		{
			UE_LOG(LogBedlamCloth, Warning, TEXT("  SolverConfigNode not found in graph"));
		}
	}

	// --- MaxDistance ---
	if (const FString* Val = Params.Find(TEXT("MaxDistance")))
	{
		auto* Node = static_cast<FChaosClothAssetSimulationMaxDistanceConfigNode*>(
			FindNodeByType(*Graph, FName("FChaosClothAssetSimulationMaxDistanceConfigNode")));
		if (Node)
		{
			const float MaxDist = FCString::Atof(**Val);
			// If SetWeightMap auto/file painted a per-vertex weight map (the
			// BedlamWeightMap node is in the graph), this tweak must NOT flatten it:
			// keep Low at the kinematic value (0) so the pinned region survives, and
			// set High = X as the dynamic reach for the loose parts. Forcing Low=0
			// also recovers from a prior all_dynamic that left Low high.
			// Without a weight map, only Low is used by the sim, so set Low==High=X
			// for a predictable uniform max distance.
			const bool bHasWeightMap = (FindNodeByName(*Graph, GBedlamWeightMapNodeName) != nullptr);
			if (bHasWeightMap)
			{
				Node->MaxDistance.Low  = BedlamClothDefaults::WeightMapKinematicLowCm;
				Node->MaxDistance.High = MaxDist;
				UE_LOG(LogBedlamCloth, Log,
					TEXT("  MaxDistance High=%f (weight map active; Low kept at %.0f to preserve kinematic anchoring)"),
					MaxDist, BedlamClothDefaults::WeightMapKinematicLowCm);
			}
			else
			{
				Node->MaxDistance.Low  = MaxDist;
				Node->MaxDistance.High = MaxDist;
				UE_LOG(LogBedlamCloth, Log, TEXT("  MaxDistance = %f (Low=High, uniform)"), MaxDist);
			}
			++AppliedCount;
		}
		else
		{
			UE_LOG(LogBedlamCloth, Warning, TEXT("  MaxDistanceConfigNode not found in graph"));
		}
	}

	// --- CollisionThickness ---
	if (const FString* Val = Params.Find(TEXT("CollisionThickness")))
	{
		auto* Node = static_cast<FChaosClothAssetSimulationCollisionConfigNode*>(
			FindNodeByType(*Graph, FName("FChaosClothAssetSimulationCollisionConfigNode")));
		if (Node)
		{
			float Thickness = FCString::Atof(**Val);
			Node->CollisionThicknessImported.ImportedValue = Thickness;
			UE_LOG(LogBedlamCloth, Log, TEXT("  CollisionThickness = %f"), Thickness);
			++AppliedCount;
		}
		else
		{
			UE_LOG(LogBedlamCloth, Warning, TEXT("  CollisionConfigNode not found in graph"));
		}
	}

	// --- Friction (cloth<->body) ---
	if (const FString* Val = Params.Find(TEXT("Friction")))
	{
		auto* Node = static_cast<FChaosClothAssetSimulationCollisionConfigNode*>(
			FindNodeByType(*Graph, FName("FChaosClothAssetSimulationCollisionConfigNode")));
		if (Node)
		{
			float Friction = FCString::Atof(**Val);
			Node->FrictionCoefficientWeighted.Low  = Friction;
			Node->FrictionCoefficientWeighted.High = Friction;
			UE_LOG(LogBedlamCloth, Log, TEXT("  Friction = %f"), Friction);
			++AppliedCount;
		}
		else
		{
			UE_LOG(LogBedlamCloth, Warning, TEXT("  CollisionConfigNode not found in graph"));
		}
	}

	// --- CCD (continuous collision detection; anti-tunnelling) ---
	if (const FString* Val = Params.Find(TEXT("CCD")))
	{
		auto* Node = static_cast<FChaosClothAssetSimulationCollisionConfigNode*>(
			FindNodeByType(*Graph, FName("FChaosClothAssetSimulationCollisionConfigNode")));
		if (Node)
		{
			Node->bUseCCD = Val->ToBool();
			UE_LOG(LogBedlamCloth, Log, TEXT("  CCD = %s"), Node->bUseCCD ? TEXT("true") : TEXT("false"));
			++AppliedCount;
		}
		else
		{
			UE_LOG(LogBedlamCloth, Warning, TEXT("  CollisionConfigNode not found in graph"));
		}
	}

	if (AppliedCount == 0)
	{
		UE_LOG(LogBedlamCloth, Warning, TEXT("No valid parameters were applied. Check key names."));
		return;
	}

	// --- Re-evaluate and save ---
	ReevaluateAndSave(ClothAsset, DataflowAsset, ClothAssetPath);

	UE_LOG(LogBedlamCloth, Log, TEXT("SUCCESS: ConfigureSimulation applied %d parameter(s) to %s"),
		AppliedCount, *ClothAssetPath);
}

// ---------------------------------------------------------------------------
// State for the asynchronous, PIE-driven Chaos Cache recording session.
// The RecordChaosCache command assembles the scene then kicks off PIE and
// returns; the rest of the flow runs through editor PIE delegates + a ticker.
// ---------------------------------------------------------------------------
// Pipeline frame rate; the play world is forced to this fixed timestep while
// recording so each frame advances the body anim and cloth solver by exactly
// 1/30s. This makes recording deterministic and immune to real-time hitches.
static constexpr double BedlamRecordFrameRate = 30.0;

// Frames the body is held on its first animation pose before playback starts, so
// the cloth can drape/settle onto the body. These settle frames are recorded at
// the start of the cache (the post/MRQ warmup step can discard them).
static constexpr int32 BedlamWarmupFrames = 30;

// ---------------------------------------------------------------------------
// Async-completion signal for RecordChaosCache.
//
// RecordChaosCache is asynchronous (it requests PIE and returns; the recording
// runs across later editor frames). To let a Python orchestrator — or any
// external watcher — tell when a recording has finished, we write a tiny status
// marker file *next to the cache asset* on disk. Its single-line content is one
// of:
//     RECORDING
//     DONE frames=<N> duration=<sec> cache=<LongPackageName>
//     FAILED reason=<text>
// Poll the file until it starts with DONE or FAILED. The marker is cleared at the
// start of each RecordChaosCache call, so its absence after the call returned
// means the command failed synchronously (before launching PIE) — check the log.
//
// IMPORTANT: do not busy-wait on it from Python running in the editor — Python
// runs on the game thread, so a blocking sleep-loop would stop PIE from ticking
// and the recording would never progress. Poll from a non-blocking context (an
// external process, or a slate/editor tick callback). See the README.
// ---------------------------------------------------------------------------
static FString RecordStatusFilePath(const FString& CachePath)
{
	FString Filename;
	if (FPackageName::TryConvertLongPackageNameToFilename(CachePath, Filename, TEXT(".recordstatus")))
	{
		return Filename;
	}
	return FString();
}

static void WriteRecordStatus(const FString& CachePath, const FString& Status)
{
	const FString Path = RecordStatusFilePath(CachePath);
	if (!Path.IsEmpty())
	{
		FFileHelper::SaveStringToFile(Status, *Path);
		UE_LOG(LogBedlamCloth, Log, TEXT("Record status [%s] -> %s"), *Status, *Path);
	}
}

static void ClearRecordStatus(const FString& CachePath)
{
	const FString Path = RecordStatusFilePath(CachePath);
	if (!Path.IsEmpty())
	{
		IFileManager::Get().Delete(*Path, /*RequireExists*/ false, /*EvenReadOnly*/ true, /*Quiet*/ true);
	}
}

struct FBedlamRecordSession
{
	bool   bActive      = false;
	int32  WarmupFrames = 0;   // settle frames before animation playback
	int32  AnimFrames   = 0;   // frames of actual animation to record
	int32  TargetFrames = 0;   // WarmupFrames + AnimFrames
	int32  FrameCount   = 0;   // frames elapsed since PIE started
	int32  NumFrames    = 0;   // requested anim frame count (0 = derived from anim length)
	bool   bAnimStarted = false;
	FString CachePath;
	FName   CacheName;

	// Fixed-timestep state to restore when the session ends.
	bool   bSavedUseFixedTimeStep = false;
	double SavedFixedDeltaTime    = 0.0;

	TWeakObjectPtr<UChaosCacheCollection> CacheCollection;
	TWeakObjectPtr<AChaosCacheManager>    EditorManager;
	TWeakObjectPtr<ASkeletalMeshActor>    BodyActor;
	TWeakObjectPtr<AActor>                ClothActor;
	TWeakObjectPtr<UAnimSequence>         Anim;
	TWeakObjectPtr<USkeletalMeshComponent> SimBodyComp;  // PIE-world body component

	FDelegateHandle           PostPIEStartedHandle;
	FDelegateHandle           EndPIEHandle;
	FTSTicker::FDelegateHandle TickerHandle;

	void Reset() { *this = FBedlamRecordSession(); }
};

static FBedlamRecordSession GRecordSession;

// Ticker: count fixed-timestep frames and request PIE end once the target is hit.
// One core-ticker call corresponds to one engine/PIE-world frame, so a real-time
// hitch costs a single frame rather than the whole budget.
static bool OnRecordTick(float /*DeltaTime*/)
{
	if (!GRecordSession.bActive)
	{
		return false; // stop ticking
	}

	++GRecordSession.FrameCount;

	// Warmup phase complete -> start the body animation (cloth has now settled).
	if (!GRecordSession.bAnimStarted && GRecordSession.FrameCount >= GRecordSession.WarmupFrames)
	{
		if (USkeletalMeshComponent* SimComp = GRecordSession.SimBodyComp.Get())
		{
			SimComp->SetPosition(0.f);
			SimComp->Play(/*bLooping*/ false);
			GRecordSession.bAnimStarted = true;
			UE_LOG(LogBedlamCloth, Log,
				TEXT("Record: warmup complete (%d frames); starting body animation."),
				GRecordSession.WarmupFrames);
		}
	}

	if (GRecordSession.FrameCount >= GRecordSession.TargetFrames)
	{
		UE_LOG(LogBedlamCloth, Log, TEXT("Record: reached %d/%d frames (%d warmup + %d anim). Ending PIE."),
			GRecordSession.FrameCount, GRecordSession.TargetFrames,
			GRecordSession.WarmupFrames, GRecordSession.AnimFrames);
		if (GEditor)
		{
			GEditor->RequestEndPlayMap();
		}
		GRecordSession.TickerHandle.Reset();
		return false; // stop ticking
	}
	return true; // keep ticking
}

// PIE has started: the cache manager's BeginPlay has begun recording. Force the
// play world onto a fixed 1/30s timestep and start the frame-counting ticker.
static void OnRecordPostPIEStarted(const bool bIsSimulating)
{
	if (!GRecordSession.bActive)
	{
		return;
	}

	// The PlayAnimation "playing" state set on the editor-world body does not
	// resume after the editor->PIE world duplication. Set up the animation
	// directly on the PIE-world body actor, but hold it on the first frame
	// (stopped) so the cloth can settle during the warmup phase. The ticker
	// starts playback once the warmup frames have elapsed.
	if (ASkeletalMeshActor* EditorBody = GRecordSession.BodyActor.Get())
	{
		AActor* SimBody = EditorUtilities::GetSimWorldCounterpartActor(EditorBody);
		if (ASkeletalMeshActor* SimSkelActor = Cast<ASkeletalMeshActor>(SimBody))
		{
			USkeletalMeshComponent* SimComp = SimSkelActor->GetSkeletalMeshComponent();
			UAnimSequence* Anim = GRecordSession.Anim.Get();
			if (SimComp && Anim)
			{
				SimComp->VisibilityBasedAnimTickOption = EVisibilityBasedAnimTickOption::AlwaysTickPoseAndRefreshBones;
				SimComp->bEnableUpdateRateOptimizations = false;
				SimComp->PlayAnimation(Anim, /*bLooping*/ false);
				SimComp->Stop();             // pause...
				SimComp->SetPosition(0.f);   // ...holding the first frame for settling
				GRecordSession.SimBodyComp = SimComp;
				UE_LOG(LogBedlamCloth, Log,
					TEXT("Record: PIE body '%s' set up, holding frame 0 for %d warmup frames."),
					*SimSkelActor->GetActorLabel(), GRecordSession.WarmupFrames);
			}
		}
		else
		{
			UE_LOG(LogBedlamCloth, Warning,
				TEXT("Record: could not resolve PIE-world body counterpart; animation may not play."));
		}
	}

	// Save and override the global timestep so each frame advances exactly 1/30s.
	GRecordSession.bSavedUseFixedTimeStep = FApp::UseFixedTimeStep();
	GRecordSession.SavedFixedDeltaTime    = FApp::GetFixedDeltaTime();
	FApp::SetFixedDeltaTime(1.0 / BedlamRecordFrameRate);
	FApp::SetUseFixedTimeStep(true);

	GRecordSession.FrameCount = 0;
	GRecordSession.TickerHandle = FTSTicker::GetCoreTicker().AddTicker(
		FTickerDelegate::CreateStatic(&OnRecordTick), 0.0f);

	UE_LOG(LogBedlamCloth, Log,
		TEXT("Record: PIE started. Fixed timestep %.4fs, recording %d frames..."),
		1.0 / BedlamRecordFrameRate, GRecordSession.TargetFrames);
}

// Deferred finalize (one tick after EndPIE): verify the recorded cache, save the
// collection asset, clean up the temporary actors, and tear down the session.
// Running this after PIE has fully torn down avoids the asset being re-dirtied by
// the teardown/cache-finalization that races a save made inside the EndPIE callback.
static bool OnRecordFinalize(float /*DeltaTime*/)
{
	if (!GRecordSession.bActive)
	{
		return false; // one-shot
	}

	// -----------------------------------------------------------------------
	// Verify the recorded cache and save the collection asset.
	// The collection is a content asset shared by the editor and PIE worlds, so
	// the frames recorded during PIE (finalized in EndEvaluate/EndRecord on
	// EndPlay) are present here.
	// -----------------------------------------------------------------------
	if (UChaosCacheCollection* Collection = GRecordSession.CacheCollection.Get())
	{
		UChaosCache* Cache = Collection->FindCache(GRecordSession.CacheName);
		const int32 NumRec = Cache ? static_cast<int32>(Cache->NumRecordedFrames) : 0;
		const float Dur    = Cache ? Cache->RecordedDuration : 0.f;

		if (Cache && NumRec > 0)
		{
			Collection->MarkPackageDirty();
			const bool bSaved = UEditorAssetLibrary::SaveAsset(GRecordSession.CachePath, false);
			UE_LOG(LogBedlamCloth, Log,
				TEXT("Record: cache '%s' recorded %d frames (%.3fs). Saved=%s -> %s"),
				*GRecordSession.CacheName.ToString(), NumRec, Dur,
				bSaved ? TEXT("OK") : TEXT("FAIL"), *GRecordSession.CachePath);
			WriteRecordStatus(GRecordSession.CachePath,
				FString::Printf(TEXT("%s frames=%d duration=%.3f cache=%s"),
					bSaved ? TEXT("DONE") : TEXT("FAILED reason=save_failed"),
					NumRec, Dur, *GRecordSession.CachePath));
		}
		else
		{
			UE_LOG(LogBedlamCloth, Warning,
				TEXT("Record: cache '%s' has %d recorded frames; not saving (no data captured)."),
				*GRecordSession.CacheName.ToString(), NumRec);
			WriteRecordStatus(GRecordSession.CachePath, TEXT("FAILED reason=no_frames_recorded"));
		}
	}
	else
	{
		UE_LOG(LogBedlamCloth, Warning, TEXT("Record: cache collection no longer valid; cannot save."));
		WriteRecordStatus(GRecordSession.CachePath, TEXT("FAILED reason=cache_collection_invalid"));
	}

	// -----------------------------------------------------------------------
	// Clean up the temporary editor-world actors so re-runs start clean and
	// only one cache manager is ever present.
	// -----------------------------------------------------------------------
	if (UWorld* EditorWorld = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr)
	{
		auto DestroyIfValid = [EditorWorld](AActor* Actor)
		{
			if (Actor)
			{
				EditorWorld->EditorDestroyActor(Actor, /*bShouldModifyLevel*/ true);
			}
		};
		DestroyIfValid(GRecordSession.EditorManager.Get());
		DestroyIfValid(GRecordSession.ClothActor.Get());
		DestroyIfValid(GRecordSession.BodyActor.Get());
	}

	UE_LOG(LogBedlamCloth, Log,
		TEXT("SUCCESS (Step 3): recording finished after %d frames; temp actors cleaned up."),
		GRecordSession.FrameCount);

	// -----------------------------------------------------------------------
	// Tear down delegates / ticker / state.
	// -----------------------------------------------------------------------
	if (GRecordSession.TickerHandle.IsValid())
	{
		FTSTicker::GetCoreTicker().RemoveTicker(GRecordSession.TickerHandle);
	}
	FEditorDelegates::PostPIEStarted.Remove(GRecordSession.PostPIEStartedHandle);
	FEditorDelegates::EndPIE.Remove(GRecordSession.EndPIEHandle);

	GRecordSession.Reset();
	return false; // one-shot ticker
}

// PIE has ended: restore the timestep and schedule the finalize (save + cleanup)
// for the next editor tick, once PIE has fully torn down.
static void OnRecordEndPIE(const bool bIsSimulating)
{
	if (!GRecordSession.bActive)
	{
		return;
	}

	// Restore the global timestep.
	FApp::SetUseFixedTimeStep(GRecordSession.bSavedUseFixedTimeStep);
	FApp::SetFixedDeltaTime(GRecordSession.SavedFixedDeltaTime);

	UE_LOG(LogBedlamCloth, Log,
		TEXT("Record: PIE ended after %d frames. Finalizing (save + cleanup) on next tick..."),
		GRecordSession.FrameCount);

	FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateStatic(&OnRecordFinalize), 0.0f);
}

// ---------------------------------------------------------------------------
// Helper: create (or load) a UChaosCacheCollection asset at the given path
// ---------------------------------------------------------------------------
static UChaosCacheCollection* CreateOrLoadCacheCollection(const FString& CachePath)
{
	// Reuse an existing collection if one is already present at this path
	if (UChaosCacheCollection* Existing = LoadObject<UChaosCacheCollection>(nullptr, *CachePath))
	{
		UE_LOG(LogBedlamCloth, Log, TEXT("Reusing existing CacheCollection at %s"), *CachePath);
		return Existing;
	}

	const FString AssetName = FPackageName::GetShortName(CachePath);
	UPackage* Package = CreatePackage(*CachePath);
	if (!Package)
	{
		UE_LOG(LogBedlamCloth, Error, TEXT("Failed to create package for CacheCollection at %s"), *CachePath);
		return nullptr;
	}

	UChaosCacheCollection* Collection = NewObject<UChaosCacheCollection>(
		Package, *AssetName, RF_Public | RF_Standalone);
	if (!Collection)
	{
		UE_LOG(LogBedlamCloth, Error, TEXT("Failed to create UChaosCacheCollection at %s"), *CachePath);
		return nullptr;
	}

	FAssetRegistryModule::AssetCreated(Collection);
	Collection->MarkPackageDirty();
	UEditorAssetLibrary::SaveAsset(CachePath, false);

	UE_LOG(LogBedlamCloth, Log, TEXT("Created CacheCollection asset at %s"), *CachePath);
	return Collection;
}

// ===========================================================================
// BedlamCloth.RecordChaosCache <ClothPath> <BodySKM> <Anim> <OutCachePath> [NumFrames]
//
// Records a Chaos Cache of the cloth simulating on the animated body:
//   1. Assemble the scene in the editor world: body (ASkeletalMeshActor), cloth
//      (UChaosClothComponent leader-posed to the body + collision source) and an
//      AChaosCacheManager (Record mode) observing the cloth; create the output
//      UChaosCacheCollection.
//   2. Launch PIE. On PostPIEStarted: force a fixed 1/30s timestep, set up the
//      PIE-world body animation held at frame 0, and start a frame-counting ticker.
//   3. Ticker: hold for BedlamWarmupFrames (cloth settles), then play the anim;
//      end PIE once warmup + anim frames are recorded.
//   4. On EndPIE: (deferred one tick) verify the recorded frames, save the cache
//      collection, and clean up the temporary actors.
// ===========================================================================
void FBedlamClothSetupCommands::RecordChaosCache(const TArray<FString>& Args, UWorld* World)
{
	if (Args.Num() < 4)
	{
		UE_LOG(LogBedlamCloth, Error, TEXT("Usage: BedlamCloth.RecordChaosCache <ClothAssetPath> <BodySKMPath> <AnimPath> <OutputCachePath> [NumFrames]"));
		return;
	}

	const FString& ClothPath  = Args[0];
	const FString& BodyPath   = Args[1];
	const FString& AnimPath   = Args[2];
	const FString& CachePath  = Args[3];
	const int32    NumFrames  = (Args.Num() >= 5) ? FCString::Atoi(*Args[4]) : 0;

	UE_LOG(LogBedlamCloth, Log, TEXT("RecordChaosCache: Cloth=%s Body=%s Anim=%s Cache=%s NumFrames=%d"),
		*ClothPath, *BodyPath, *AnimPath, *CachePath, NumFrames);

	// -----------------------------------------------------------------------
	// 0. Guard against concurrent sessions; recover from a stale one.
	//    If a previous PIE failed to start (so PostPIEStarted/EndPIE never
	//    fired) the session would otherwise stay "active" forever and block
	//    every future run. Only a genuinely in-progress PIE should abort.
	// -----------------------------------------------------------------------
	if (GRecordSession.bActive)
	{
		const bool bPIEInProgress = GEditor && GEditor->PlayWorld != nullptr;
		if (bPIEInProgress)
		{
			UE_LOG(LogBedlamCloth, Error, TEXT("A record session is already in progress (PIE active). Aborting."));
			return;
		}

		UE_LOG(LogBedlamCloth, Warning, TEXT("Clearing a stale record session before starting a new one."));
		if (GRecordSession.TickerHandle.IsValid())
		{
			FTSTicker::GetCoreTicker().RemoveTicker(GRecordSession.TickerHandle);
		}
		FEditorDelegates::PostPIEStarted.Remove(GRecordSession.PostPIEStartedHandle);
		FEditorDelegates::EndPIE.Remove(GRecordSession.EndPIEHandle);
		GRecordSession.Reset();
	}

	if (!GEditor)
	{
		UE_LOG(LogBedlamCloth, Error, TEXT("No GEditor available; RecordChaosCache requires the editor."));
		return;
	}

	// Clear any stale status marker. If the command returns synchronously below
	// (e.g. an asset fails to load) the marker stays absent, signalling to a
	// poller that recording never started. It's set to RECORDING just before PIE.
	ClearRecordStatus(CachePath);

	// -----------------------------------------------------------------------
	// 1. Load source assets
	// -----------------------------------------------------------------------
	UChaosClothAsset* ClothAsset = LoadObject<UChaosClothAsset>(nullptr, *ClothPath);
	if (!ClothAsset)
	{
		UE_LOG(LogBedlamCloth, Error, TEXT("Failed to load ChaosClothAsset: %s"), *ClothPath);
		return;
	}

	USkeletalMesh* BodyMesh = LoadObject<USkeletalMesh>(nullptr, *BodyPath);
	if (!BodyMesh)
	{
		UE_LOG(LogBedlamCloth, Error, TEXT("Failed to load body SkeletalMesh: %s"), *BodyPath);
		return;
	}

	UAnimSequence* AnimSeq = LoadObject<UAnimSequence>(nullptr, *AnimPath);
	if (!AnimSeq)
	{
		UE_LOG(LogBedlamCloth, Error, TEXT("Failed to load animation: %s"), *AnimPath);
		return;
	}

	UPhysicsAsset* PhysAsset = BodyMesh->GetPhysicsAsset();
	UE_LOG(LogBedlamCloth, Log, TEXT("Assets loaded. PhysicsAsset=%s AnimLength=%.3fs"),
		PhysAsset ? *PhysAsset->GetName() : TEXT("null"), AnimSeq->GetPlayLength());

	// -----------------------------------------------------------------------
	// 2. Create the output cache collection asset
	// -----------------------------------------------------------------------
	UChaosCacheCollection* CacheCollection = CreateOrLoadCacheCollection(CachePath);
	if (!CacheCollection)
	{
		return;
	}

	// -----------------------------------------------------------------------
	// 3. Resolve the editor world to assemble the scene in
	// -----------------------------------------------------------------------
	UWorld* EditorWorld = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
	if (!EditorWorld)
	{
		UE_LOG(LogBedlamCloth, Error, TEXT("No editor world available to assemble the recording scene."));
		return;
	}

	// -----------------------------------------------------------------------
	// 4. Spawn the body actor and play the animation
	// -----------------------------------------------------------------------
	ASkeletalMeshActor* BodyActor = EditorWorld->SpawnActor<ASkeletalMeshActor>();
	if (!BodyActor)
	{
		UE_LOG(LogBedlamCloth, Error, TEXT("Failed to spawn body SkeletalMeshActor."));
		return;
	}
	BodyActor->SetActorLabel(TEXT("BedlamCloth_Body"));

	USkeletalMeshComponent* BodyComp = BodyActor->GetSkeletalMeshComponent();
	BodyComp->SetSkeletalMeshAsset(BodyMesh);
	BodyComp->PlayAnimation(AnimSeq, /*bLooping*/ false);

	// The actors are typically off-screen from the default PIE camera. Skeletal
	// meshes default to OnlyTickPoseWhenRendered, which freezes the pose (and the
	// animation) when not rendered. Force the pose to always tick so the body
	// animation advances during recording regardless of what the PIE view shows.
	BodyComp->VisibilityBasedAnimTickOption = EVisibilityBasedAnimTickOption::AlwaysTickPoseAndRefreshBones;
	BodyComp->bEnableUpdateRateOptimizations = false;

	// -----------------------------------------------------------------------
	// 5. Spawn the cloth actor + component, bind it to the body
	// -----------------------------------------------------------------------
	AActor* ClothActor = EditorWorld->SpawnActor<AActor>();
	if (!ClothActor)
	{
		UE_LOG(LogBedlamCloth, Error, TEXT("Failed to spawn cloth actor."));
		return;
	}
	ClothActor->SetActorLabel(TEXT("BedlamCloth_Cloth"));

	UChaosClothComponent* ClothComp = NewObject<UChaosClothComponent>(ClothActor, TEXT("ClothComponent"));
	ClothActor->SetRootComponent(ClothComp);
	ClothComp->RegisterComponent();
	ClothActor->AddInstanceComponent(ClothComp);

	ClothComp->SetAsset(ClothAsset);
	ClothComp->VisibilityBasedAnimTickOption = EVisibilityBasedAnimTickOption::AlwaysTickPoseAndRefreshBones;
	ClothComp->AttachToComponent(BodyComp, FAttachmentTransformRules::SnapToTargetNotIncludingScale);
	ClothComp->SetLeaderPoseComponent(BodyComp);
	if (PhysAsset)
	{
		ClothComp->AddCollisionSource(BodyComp, PhysAsset);
	}

	// -----------------------------------------------------------------------
	// 6. Spawn the cache manager and observe the cloth component (Record mode)
	// -----------------------------------------------------------------------
	AChaosCacheManager* CacheManager = EditorWorld->SpawnActor<AChaosCacheManager>();
	if (!CacheManager)
	{
		UE_LOG(LogBedlamCloth, Error, TEXT("Failed to spawn AChaosCacheManager."));
		return;
	}
	CacheManager->SetActorLabel(TEXT("BedlamCloth_CacheManager"));

	const FName CacheName(*(FPackageName::GetShortName(CachePath) + TEXT("_cloth")));
	CacheManager->CacheCollection = CacheCollection;
	CacheManager->CacheMode = ECacheMode::Record;
	CacheManager->bStartOnBeginPlay = true;
	CacheManager->FindOrAddObservedComponent(ClothComp, CacheName);

	UE_LOG(LogBedlamCloth, Log,
		TEXT("Scene assembled. Body=%s Cloth=%s Manager=%s Observed=%d CacheName=%s"),
		*BodyActor->GetActorLabel(), *ClothActor->GetActorLabel(), *CacheManager->GetActorLabel(),
		CacheManager->GetObservedComponents().Num(), *CacheName.ToString());

	// -----------------------------------------------------------------------
	// 7. Launch PIE to drive the simulation; auto-end after the target duration
	// -----------------------------------------------------------------------
	const float AnimLength  = AnimSeq->GetPlayLength();
	const int32 AnimFrames  = (NumFrames > 0)
		? NumFrames
		: FMath::CeilToInt(AnimLength * BedlamRecordFrameRate);
	const int32 TargetFrames = BedlamWarmupFrames + AnimFrames;

	GRecordSession.Reset();
	GRecordSession.bActive         = true;
	GRecordSession.WarmupFrames    = BedlamWarmupFrames;
	GRecordSession.AnimFrames      = AnimFrames;
	GRecordSession.TargetFrames    = TargetFrames;
	GRecordSession.bAnimStarted    = false;
	GRecordSession.NumFrames       = NumFrames;
	GRecordSession.CachePath       = CachePath;
	GRecordSession.CacheName       = CacheName;
	GRecordSession.CacheCollection = CacheCollection;
	GRecordSession.EditorManager   = CacheManager;
	GRecordSession.BodyActor       = BodyActor;
	GRecordSession.ClothActor      = ClothActor;
	GRecordSession.Anim            = AnimSeq;

	GRecordSession.PostPIEStartedHandle = FEditorDelegates::PostPIEStarted.AddStatic(&OnRecordPostPIEStarted);
	GRecordSession.EndPIEHandle         = FEditorDelegates::EndPIE.AddStatic(&OnRecordEndPIE);

	// Commit point: we are about to launch PIE and run asynchronously. Publish the
	// "in progress" marker so a poller knows recording started; OnRecordFinalize
	// flips it to DONE/FAILED when the recording ends.
	WriteRecordStatus(CachePath, TEXT("RECORDING"));

	FRequestPlaySessionParams PlayParams;
	PlayParams.WorldType         = EPlaySessionWorldType::PlayInEditor;
	PlayParams.SessionDestination = EPlaySessionDestinationType::InProcess;
	GEditor->RequestPlaySession(PlayParams);

	UE_LOG(LogBedlamCloth, Log,
		TEXT("SUCCESS: PIE requested. Will record %d frames (%d warmup + %d anim, AnimLength=%.3fs @ %.0ffps) then auto-end. ")
		TEXT("Poll completion via status marker: %s"),
		TargetFrames, BedlamWarmupFrames, AnimFrames, AnimLength, BedlamRecordFrameRate,
		*RecordStatusFilePath(CachePath));
}
