#pragma once

#include "CoreMinimal.h"

class FBedlamClothSetupCommands
{
public:
	static void CreateClothAsset(const TArray<FString>& Args, UWorld* World);
	static void SetWeightMap(const TArray<FString>& Args, UWorld* World);
	static void ConfigureSimulation(const TArray<FString>& Args, UWorld* World);
	static void RecordChaosCache(const TArray<FString>& Args, UWorld* World);
	// Rebuild the visual (UEdGraphNode) graph for a script-created cloth asset so
	// its Dataflow nodes are visible/inspectable in the editor. Args: <ClothAssetPath>
	static void ShowGraph(const TArray<FString>& Args, UWorld* World);
};
