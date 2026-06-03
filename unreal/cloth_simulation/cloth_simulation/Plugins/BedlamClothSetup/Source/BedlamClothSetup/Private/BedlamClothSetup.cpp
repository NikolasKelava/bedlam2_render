#include "BedlamClothSetup.h"
#include "BedlamClothSetupCommands.h"
#include "HAL/IConsoleManager.h"

#define LOCTEXT_NAMESPACE "FBedlamClothSetupModule"

void FBedlamClothSetupModule::StartupModule()
{
	CmdCreateClothAsset = IConsoleManager::Get().RegisterConsoleCommand(
		TEXT("BedlamCloth.CreateClothAsset"),
		TEXT("Create a ChaosClothAsset from a garment mesh and body skeletal mesh. Args: <GarmentMeshPath> <BodySKMPath> <OutputPath>"),
		FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(&FBedlamClothSetupCommands::CreateClothAsset),
		ECVF_Default
	);

	CmdSetWeightMap = IConsoleManager::Get().RegisterConsoleCommand(
		TEXT("BedlamCloth.SetWeightMap"),
		TEXT("Set weight map on a ChaosClothAsset. Args: <ClothAssetPath> <Mode> [FilePath]"),
		FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(&FBedlamClothSetupCommands::SetWeightMap),
		ECVF_Default
	);

	CmdConfigureSimulation = IConsoleManager::Get().RegisterConsoleCommand(
		TEXT("BedlamCloth.ConfigureSimulation"),
		TEXT("Configure simulation parameters on a ChaosClothAsset. Args: <ClothAssetPath> [Key=Value ...]"),
		FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(&FBedlamClothSetupCommands::ConfigureSimulation),
		ECVF_Default
	);

	CmdRecordChaosCache = IConsoleManager::Get().RegisterConsoleCommand(
		TEXT("BedlamCloth.RecordChaosCache"),
		TEXT("Record Chaos Cache from cloth simulation. Args: <ClothAssetPath> <BodySKMPath> <AnimPath> <OutputCachePath> [NumFrames]"),
		FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(&FBedlamClothSetupCommands::RecordChaosCache),
		ECVF_Default
	);

	CmdShowGraph = IConsoleManager::Get().RegisterConsoleCommand(
		TEXT("BedlamCloth.ShowGraph"),
		TEXT("Rebuild the visual Dataflow graph for a script-created cloth asset so its nodes are visible in the editor. Args: <ClothAssetPath>"),
		FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(&FBedlamClothSetupCommands::ShowGraph),
		ECVF_Default
	);

	UE_LOG(LogTemp, Log, TEXT("BedlamClothSetup: 5 console commands registered."));
}

void FBedlamClothSetupModule::ShutdownModule()
{
	if (CmdCreateClothAsset)   IConsoleManager::Get().UnregisterConsoleObject(CmdCreateClothAsset);
	if (CmdSetWeightMap)       IConsoleManager::Get().UnregisterConsoleObject(CmdSetWeightMap);
	if (CmdConfigureSimulation) IConsoleManager::Get().UnregisterConsoleObject(CmdConfigureSimulation);
	if (CmdRecordChaosCache)   IConsoleManager::Get().UnregisterConsoleObject(CmdRecordChaosCache);
	if (CmdShowGraph)          IConsoleManager::Get().UnregisterConsoleObject(CmdShowGraph);
}

#undef LOCTEXT_NAMESPACE

IMPLEMENT_MODULE(FBedlamClothSetupModule, BedlamClothSetup)
