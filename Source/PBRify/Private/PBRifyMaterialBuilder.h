#pragma once

#include "CoreMinimal.h"

class UMaterial;
class UMaterialInstanceConstant;
struct FPBRifyResult;

namespace PBRifyMaterialBuilder
{
	/**
	 * Loads the shared PBRify master material, or builds it if it doesn't exist yet. The graph does
	 * parallax occlusion mapping with the height map, then feeds base color, normal and ORM through it.
	 * DefaultTextures become the texture parameters' defaults (they need matching sampler types).
	 */
	UMaterial* FindOrCreateMasterMaterial(const FString& MasterMaterialPath, const FPBRifyResult& DefaultTextures, FText& OutError);

	/** Creates (or updates) a material instance of Master that uses the given textures. */
	UMaterialInstanceConstant* CreateOrUpdateInstance(
		UMaterial* Master, const FString& PackagePath, const FString& DesiredName,
		const FPBRifyResult& Textures, float ParallaxDepth, FText& OutError);
}
