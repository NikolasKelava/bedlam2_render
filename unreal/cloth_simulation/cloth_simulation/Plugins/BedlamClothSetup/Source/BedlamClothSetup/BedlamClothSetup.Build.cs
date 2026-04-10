// Copyright Epic Games, Inc. All Rights Reserved.

using UnrealBuildTool;

public class BedlamClothSetup : ModuleRules
{
	public BedlamClothSetup(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = ModuleRules.PCHUsageMode.UseExplicitOrSharedPCHs;

		PublicDependencyModuleNames.AddRange(
			new string[]
			{
				"Core",
				"CoreUObject",
				"Engine",
				"UnrealEd"
			}
		);

		PrivateDependencyModuleNames.AddRange(
			new string[]
			{
				"ChaosClothAsset",
				"ChaosClothAssetEngine",
				"ChaosClothAssetDataflowNodes",
				"ChaosClothAssetTools",
				"DataflowCore",
				"DataflowEngine",
				"DataflowNodes",
				"ChaosCaching",
				"EditorScriptingUtilities",
				"PhysicsCore",
				"AssetRegistry"
			}
		);
	}
}
