#include "BedlamClothSetupCommands.h"

DEFINE_LOG_CATEGORY_STATIC(LogBedlamCloth, Log, All);

void FBedlamClothSetupCommands::CreateClothAsset(const TArray<FString>& Args, UWorld* World)
{
	if (Args.Num() < 3)
	{
		UE_LOG(LogBedlamCloth, Error, TEXT("Usage: BedlamCloth.CreateClothAsset <GarmentMeshPath> <BodySkeletalMeshPath> <OutputAssetPath>"));
		return;
	}
	UE_LOG(LogBedlamCloth, Log, TEXT("CreateClothAsset called: Garment=%s Body=%s Output=%s"), *Args[0], *Args[1], *Args[2]);
}

void FBedlamClothSetupCommands::SetWeightMap(const TArray<FString>& Args, UWorld* World)
{
	if (Args.Num() < 2)
	{
		UE_LOG(LogBedlamCloth, Error, TEXT("Usage: BedlamCloth.SetWeightMap <ClothAssetPath> <Mode> [FilePath]"));
		return;
	}
	UE_LOG(LogBedlamCloth, Log, TEXT("SetWeightMap called: Asset=%s Mode=%s"), *Args[0], *Args[1]);
}

void FBedlamClothSetupCommands::ConfigureSimulation(const TArray<FString>& Args, UWorld* World)
{
	if (Args.Num() < 2)
	{
		UE_LOG(LogBedlamCloth, Error, TEXT("Usage: BedlamCloth.ConfigureSimulation <ClothAssetPath> [Key=Value ...]"));
		return;
	}
	UE_LOG(LogBedlamCloth, Log, TEXT("ConfigureSimulation called: Asset=%s"), *Args[0]);
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
