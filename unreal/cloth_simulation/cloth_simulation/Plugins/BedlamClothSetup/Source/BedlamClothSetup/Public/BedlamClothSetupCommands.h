#pragma once

#include "CoreMinimal.h"

class FBedlamClothSetupCommands
{
public:
	static void CreateClothAsset(const TArray<FString>& Args, UWorld* World);
	static void SetWeightMap(const TArray<FString>& Args, UWorld* World);
	static void ConfigureSimulation(const TArray<FString>& Args, UWorld* World);
	static void RecordChaosCache(const TArray<FString>& Args, UWorld* World);
};
