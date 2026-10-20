#pragma once

#include "CoreMinimal.h"
#include "Modules/ModuleInterface.h"

DECLARE_LOG_CATEGORY_EXTERN(LogPBRify, Log, All);

class FPBRifyModule : public IModuleInterface
{
public:
	virtual void StartupModule() override;
	virtual void ShutdownModule() override;

private:
	/** Adds the PBRify entries to the Content Browser's right-click menu for textures. */
	void RegisterMenus();
};
