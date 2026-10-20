#pragma once

#include "CoreMinimal.h"
#include "Engine/DeveloperSettings.h"
#include "PBRifySettings.generated.h"

/**
 * How PBRify turns an image into PBR maps.
 * Lives in Editor Preferences > Plugins > PBRify, and is also shown by "PBRify: Generate with Options...".
 */
UCLASS(config = EditorPerProjectUserSettings, meta = (DisplayName = "PBRify"))
class PBRIFY_API UPBRifySettings : public UDeveloperSettings
{
	GENERATED_BODY()

public:
	virtual FName GetContainerName() const override { return TEXT("Editor"); }
	virtual FName GetCategoryName() const override { return TEXT("Plugins"); }

	// Output

	/** Longest side of the generated maps. Bigger sources are downscaled first. 0 keeps the source size. */
	UPROPERTY(config, EditAnywhere, Category = "Output", meta = (ClampMin = "0", UIMin = "256", UIMax = "8192"))
	int32 MaxResolution = 2048;

	/** Treat the source as a seamless texture so the generated maps tile too. Leave off for photos. */
	UPROPERTY(config, EditAnywhere, Category = "Output")
	bool bTileable = false;

	/** Folder, relative to the source texture, to put generated assets in. Empty puts them next to the source. */
	UPROPERTY(config, EditAnywhere, Category = "Output")
	FString OutputSubfolder;

	/** Also create a material instance with parallax depth, ready to drop onto a mesh. */
	UPROPERTY(config, EditAnywhere, Category = "Output")
	bool bCreateMaterial = true;

	/** Master material shared by all generated material instances. Created automatically the first time. */
	UPROPERTY(config, EditAnywhere, Category = "Output", meta = (EditCondition = "bCreateMaterial"))
	FString MasterMaterialPath = TEXT("/Game/PBRify/M_PBRify_Master");

	// Base Color

	/** How much of the photo's large-scale lighting (sun gradients, big soft shadows) to remove from the base color. */
	UPROPERTY(config, EditAnywhere, Category = "Base Color", meta = (ClampMin = "0", ClampMax = "1"))
	float DelightStrength = 0.5f;

	// Height

	/** By default bright areas are raised and dark areas are recessed. Flip that here. */
	UPROPERTY(config, EditAnywhere, Category = "Height")
	bool bInvertHeight = false;

	/** Pixel-level grain (surface texture, noise). */
	UPROPERTY(config, EditAnywhere, Category = "Height", meta = (ClampMin = "0", UIMax = "2"))
	float FineDetail = 0.2f;

	/** Small features (bricks, letters, panel lines). */
	UPROPERTY(config, EditAnywhere, Category = "Height", meta = (ClampMin = "0", UIMax = "2"))
	float MediumDetail = 0.6f;

	/** Medium features (frames, trims, signs). */
	UPROPERTY(config, EditAnywhere, Category = "Height", meta = (ClampMin = "0", UIMax = "2"))
	float LargeDetail = 1.0f;

	/** Big shapes (windows, doors, walls). */
	UPROPERTY(config, EditAnywhere, Category = "Height", meta = (ClampMin = "0", UIMax = "2"))
	float ShapeDetail = 1.0f;

	UPROPERTY(config, EditAnywhere, Category = "Height", meta = (ClampMin = "0.1", UIMax = "4"))
	float HeightContrast = 1.0f;

	// Normal

	UPROPERTY(config, EditAnywhere, Category = "Normal", meta = (ClampMin = "0", UIMax = "5"))
	float NormalStrength = 1.0f;

	/** Produce an OpenGL style (Y+) normal map, for use outside Unreal. Unreal itself wants this off. */
	UPROPERTY(config, EditAnywhere, Category = "Normal", AdvancedDisplay)
	bool bFlipGreen = false;

	// Ambient Occlusion

	UPROPERTY(config, EditAnywhere, Category = "Ambient Occlusion", meta = (ClampMin = "0", UIMax = "3"))
	float AOStrength = 1.0f;

	// Roughness / Metallic

	/** Average roughness. 0 = mirror, 1 = fully matte. */
	UPROPERTY(config, EditAnywhere, Category = "Roughness and Metallic", meta = (ClampMin = "0", ClampMax = "1"))
	float RoughnessBase = 0.7f;

	/** How much busy, textured areas get rougher and smooth areas (glass, paint) get glossier. */
	UPROPERTY(config, EditAnywhere, Category = "Roughness and Metallic", meta = (ClampMin = "0", ClampMax = "1"))
	float RoughnessVariation = 0.3f;

	UPROPERTY(config, EditAnywhere, Category = "Roughness and Metallic", meta = (ClampMin = "0", ClampMax = "1"))
	float Metallic = 0.0f;

	// Material

	/** Parallax depth (in UV units) set on generated material instances. You can tweak it live on the instance afterwards. */
	UPROPERTY(config, EditAnywhere, Category = "Material", meta = (ClampMin = "0", UIMax = "0.2", EditCondition = "bCreateMaterial"))
	float ParallaxDepth = 0.05f;
};
