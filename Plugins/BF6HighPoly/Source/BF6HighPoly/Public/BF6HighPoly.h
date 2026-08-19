#pragma once

#include "CoreMinimal.h"
#include "Modules/ModuleInterface.h"

// The Unreal binding module. It owns the loaded bf6_core.dll and, once the
// decode modules are ported, turns the core's plain-data output into Unreal
// meshes and materials. Right now it just proves the bridge works.
class FBF6HighPolyModule : public IModuleInterface
{
public:
	virtual void StartupModule() override;
	virtual void ShutdownModule() override;

private:
	void* DllHandle = nullptr;
};
