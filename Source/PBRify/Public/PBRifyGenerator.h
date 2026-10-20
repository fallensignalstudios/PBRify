#pragma once

#include "CoreMinimal.h"
#include "PBRifyGenerator.generated.h"

class UMaterialInstanceConstant;
class UPBRifySettings;
class UTexture2D;

/** Everything PBRify produced for one source texture. */
USTRUCT(BlueprintType)
struct PBRIFY_API FPBRifyResult
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "PBRify")
	TObjectPtr<UTexture2D> BaseColor = nullptr;

	UPROPERTY(BlueprintReadOnly, Category = "PBRify")
	TObjectPtr<UTexture2D> Normal = nullptr;

	/** 16-bit grayscale. Also usable for Nanite displacement or landscape heightmaps. */
	UPROPERTY(BlueprintReadOnly, Category = "PBRify")
	TObjectPtr<UTexture2D> Height = nullptr;

	/** R = ambient occlusion, G = roughness, B = metallic. */
	UPROPERTY(BlueprintReadOnly, Category = "PBRify")
	TObjectPtr<UTexture2D> ORM = nullptr;

	/** Null if material creation is turned off in the settings. */
	UPROPERTY(BlueprintReadOnly, Category = "PBRify")
	TObjectPtr<UMaterialInstanceConstant> Material = nullptr;

	TArray<UObject*> GetAssets() const;
};

namespace PBRify
{
	/**
	 * Generates base color, normal, height and ORM textures from Source (plus a parallax material
	 * instance if enabled), next to the source asset. Existing outputs with the same names are updated in place.
	 */
	PBRIFY_API bool GeneratePBRAssets(UTexture2D* Source, const UPBRifySettings& Settings, FPBRifyResult& OutResult, FText& OutError);

	/** Runs GeneratePBRAssets on each texture with a progress dialog, then shows the results in the Content Browser. */
	PBRIFY_API void GenerateForTextures(const TArray<UTexture2D*>& Sources);
}
