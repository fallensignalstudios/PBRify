#pragma once

#include "CoreMinimal.h"
#include "Kismet/BlueprintFunctionLibrary.h"
#include "PBRifyGenerator.h"
#include "PBRifyLibrary.generated.h"

class UTexture2D;

/** Lets Editor Utility Blueprints and Python run PBRify, e.g. unreal.PBRifyLibrary.pbrify_texture(texture). */
UCLASS()
class PBRIFY_API UPBRifyLibrary : public UBlueprintFunctionLibrary
{
	GENERATED_BODY()

public:
	/**
	 * Turns a texture into PBR maps (and a parallax material instance, if enabled) using the PBRify settings in Editor Preferences.
	 * On failure the result is empty and the reason is in the Output Log.
	 */
	UFUNCTION(BlueprintCallable, Category = "PBRify")
	static FPBRifyResult PBRifyTexture(UTexture2D* SourceTexture);
};
