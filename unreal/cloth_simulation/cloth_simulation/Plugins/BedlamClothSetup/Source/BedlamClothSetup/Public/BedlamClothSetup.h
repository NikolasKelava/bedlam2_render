#pragma once

#include "Modules/ModuleManager.h"

struct IConsoleCommand;

class FBedlamClothSetupModule : public IModuleInterface
{
public:
	virtual void StartupModule() override;
	virtual void ShutdownModule() override;

private:
	IConsoleCommand* CmdCreateClothAsset = nullptr;
	IConsoleCommand* CmdSetWeightMap = nullptr;
	IConsoleCommand* CmdConfigureSimulation = nullptr;
	IConsoleCommand* CmdRecordChaosCache = nullptr;
};
