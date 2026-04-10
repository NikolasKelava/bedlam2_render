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

// Chaos Cloth Dataflow Nodes
#include "ChaosClothAsset/StaticMeshImportNode.h"
#include "ChaosClothAsset/TransferSkinWeightsNode.h"
#include "ChaosClothAsset/SetPhysicsAssetNode.h"
#include "ChaosClothAsset/SimulationMaxDistanceConfigNode.h"
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
	auto GravityNode        = CreateDataflowNode(*Graph, DataflowAsset, FName("FChaosClothAssetSimulationGravityConfigNode"),        FName("Gravity"));
	auto CollisionNode      = CreateDataflowNode(*Graph, DataflowAsset, FName("FChaosClothAssetSimulationCollisionConfigNode"),      FName("Collision"));
	auto SelfCollisionNode  = CreateDataflowNode(*Graph, DataflowAsset, FName("FChaosClothAssetSimulationSelfCollisionConfigNode_v2"), FName("SelfCollision"));
	auto DampingNode        = CreateDataflowNode(*Graph, DataflowAsset, FName("FChaosClothAssetSimulationDampingConfigNode"),        FName("Damping"));
	auto SolverNode         = CreateDataflowNode(*Graph, DataflowAsset, FName("FChaosClothAssetSimulationSolverConfigNode"),         FName("Solver"));
	auto TerminalNode       = CreateDataflowNode(*Graph, DataflowAsset, FName("FChaosClothAssetTerminalNode_v2"),       FName("Terminal"));

	if (!ImportNode || !TransferNode || !PhysAssetNode || !MaxDistNode ||
		!GravityNode || !CollisionNode || !SelfCollisionNode || !DampingNode ||
		!SolverNode || !TerminalNode)
	{
		UE_LOG(LogBedlamCloth, Error, TEXT("One or more Dataflow nodes failed to create. Aborting."));
		return;
	}

	UE_LOG(LogBedlamCloth, Log, TEXT("All 10 Dataflow nodes created."));

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

	// Max distance — default all-dynamic (large range so all vertices can move freely)
	{
		auto* Typed = static_cast<FChaosClothAssetSimulationMaxDistanceConfigNode*>(MaxDistNode.Get());
		Typed->MaxDistance.Low  = 0.f;
		Typed->MaxDistance.High = 1000.f;
	}

	// Gravity, Collision, SelfCollision, Damping, Solver — use UE defaults

	// -----------------------------------------------------------------------
	// 5. Connect the node chain via Collection passthrough
	// -----------------------------------------------------------------------
	ConnectCollectionPins(*Graph, ImportNode,        TransferNode);
	ConnectCollectionPins(*Graph, TransferNode,      PhysAssetNode);
	ConnectCollectionPins(*Graph, PhysAssetNode,     MaxDistNode);
	ConnectCollectionPins(*Graph, MaxDistNode,       GravityNode);
	ConnectCollectionPins(*Graph, GravityNode,       CollisionNode);
	ConnectCollectionPins(*Graph, CollisionNode,     SelfCollisionNode);
	ConnectCollectionPins(*Graph, SelfCollisionNode, DampingNode);
	ConnectCollectionPins(*Graph, DampingNode,       SolverNode);

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

void FBedlamClothSetupCommands::RecordChaosCache(const TArray<FString>& Args, UWorld* World)
{
	if (Args.Num() < 4)
	{
		UE_LOG(LogBedlamCloth, Error, TEXT("Usage: BedlamCloth.RecordChaosCache <ClothAssetPath> <BodySKMPath> <AnimPath> <OutputCachePath> [NumFrames]"));
		return;
	}
	UE_LOG(LogBedlamCloth, Log, TEXT("RecordChaosCache called: Cloth=%s Body=%s Anim=%s Cache=%s"), *Args[0], *Args[1], *Args[2], *Args[3]);
}
