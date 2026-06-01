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

// Editor utilities
#include "EditorAssetLibrary.h"

DEFINE_LOG_CATEGORY_STATIC(LogBedlamCloth, Log, All);

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

	// Max distance — small range so the garment stays close to (hugs) the body
	// instead of drifting metres away. Low is the value used when there is no
	// weight map; set both Low and High so the limit is small regardless.
	{
		auto* Typed = static_cast<FChaosClothAssetSimulationMaxDistanceConfigNode*>(MaxDistNode.Get());
		Typed->MaxDistance.Low  = 4.f;
		Typed->MaxDistance.High = 4.f;
	}

	// Stretch & Bending — use defaults (PBD, stiffness 1.0). These give the cloth
	// structural integrity (resistance to stretching and folding). Without these
	// config nodes there are no such constraints and the mesh deforms without limit.

	// Gravity, Collision, Damping, Solver — use UE defaults.
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

// ===========================================================================
// BedlamCloth.SetWeightMap
// ===========================================================================
void FBedlamClothSetupCommands::SetWeightMap(const TArray<FString>& Args, UWorld* World)
{
	if (Args.Num() < 2)
	{
		UE_LOG(LogBedlamCloth, Error,
			TEXT("Usage: BedlamCloth.SetWeightMap <ClothAssetPath> <Mode> [FilePath]\n")
			TEXT("  Modes: all_dynamic, auto, file"));
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
		// All vertices fully dynamic: MaxDistance = [0, 1000] with no per-vertex weight map
		MaxDistNode->MaxDistance.Low  = 0.f;
		MaxDistNode->MaxDistance.High = 1000.f;

		UE_LOG(LogBedlamCloth, Log, TEXT("Set MaxDistance to all_dynamic: Low=0 High=1000"));
	}
	else if (Mode == TEXT("auto"))
	{
		UE_LOG(LogBedlamCloth, Warning,
			TEXT("'auto' mode not yet implemented. Requires per-vertex proximity computation."));
		return;
	}
	else if (Mode == TEXT("file"))
	{
		if (Args.Num() < 3)
		{
			UE_LOG(LogBedlamCloth, Error, TEXT("'file' mode requires a file path argument"));
			return;
		}
		UE_LOG(LogBedlamCloth, Warning,
			TEXT("'file' mode not yet implemented. Requires WeightMapNode insertion for per-vertex control."));
		return;
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
			TEXT("        Substeps (int), Iterations (int), MaxDistance (float),\n")
			TEXT("        CollisionThickness (float)"));
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
			float MaxDist = FCString::Atof(**Val);
			Node->MaxDistance.Low  = 0.f;
			Node->MaxDistance.High = MaxDist;
			UE_LOG(LogBedlamCloth, Log, TEXT("  MaxDistance = [0, %f]"), MaxDist);
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

struct FBedlamRecordSession
{
	bool   bActive      = false;
	int32  TargetFrames = 0;   // number of fixed-timestep frames to record
	int32  FrameCount   = 0;   // frames elapsed since PIE started
	int32  NumFrames    = 0;   // requested frame count (0 = derived from anim length)
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
	if (GRecordSession.FrameCount >= GRecordSession.TargetFrames)
	{
		UE_LOG(LogBedlamCloth, Log, TEXT("Record: reached %d/%d frames. Ending PIE."),
			GRecordSession.FrameCount, GRecordSession.TargetFrames);
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
	// resume after the editor->PIE world duplication, so the body stays frozen at
	// frame 0 (which leaves the cloth static). Re-start the animation directly on
	// the PIE-world body actor.
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
				UE_LOG(LogBedlamCloth, Log, TEXT("Record: started animation on PIE body '%s'."),
					*SimSkelActor->GetActorLabel());
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

// PIE has ended. Restore the timestep, tear down delegates/ticker and report.
// (Step 3 will verify recorded frames and save the cache collection here.)
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
		TEXT("Record: PIE ended after %d frames (Step 2). Finalize/save will be added in Step 3."),
		GRecordSession.FrameCount);

	if (GRecordSession.TickerHandle.IsValid())
	{
		FTSTicker::GetCoreTicker().RemoveTicker(GRecordSession.TickerHandle);
	}
	FEditorDelegates::PostPIEStarted.Remove(GRecordSession.PostPIEStartedHandle);
	FEditorDelegates::EndPIE.Remove(GRecordSession.EndPIEHandle);

	GRecordSession.Reset();
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
// BedlamCloth.RecordChaosCache
//
// STEP 1 (current): assemble the recording scene in the editor world.
//   - load cloth / body / anim assets
//   - create the output ChaosCacheCollection asset
//   - spawn body (ASkeletalMeshActor playing the anim), cloth
//     (UChaosClothComponent bound to the body), and an AChaosCacheManager
//     configured to record the cloth component.
//   No PIE / recording is launched yet (steps 2-4).
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
	if (GRecordSession.bActive)
	{
		UE_LOG(LogBedlamCloth, Error, TEXT("A record session is already in progress. Aborting."));
		return;
	}

	const float AnimLength   = AnimSeq->GetPlayLength();
	const int32 TargetFrames = (NumFrames > 0)
		? NumFrames
		: FMath::CeilToInt(AnimLength * BedlamRecordFrameRate);

	GRecordSession.Reset();
	GRecordSession.bActive         = true;
	GRecordSession.TargetFrames    = TargetFrames;
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

	FRequestPlaySessionParams PlayParams;
	PlayParams.WorldType         = EPlaySessionWorldType::PlayInEditor;
	PlayParams.SessionDestination = EPlaySessionDestinationType::InProcess;
	GEditor->RequestPlaySession(PlayParams);

	UE_LOG(LogBedlamCloth, Log,
		TEXT("SUCCESS (Step 2): PIE requested. Will record %d frames (NumFrames=%d, AnimLength=%.3fs @ %.0ffps) then auto-end."),
		TargetFrames, NumFrames, AnimLength, BedlamRecordFrameRate);
}
