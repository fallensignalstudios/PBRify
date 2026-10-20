#pragma once

#include <cstdint>
#include <functional>
#include <vector>

/**
 * Image -> PBR map generation.
 *
 * This part deliberately only depends on the C++ standard library so it can be built and
 * unit tested outside the editor (see Tests/ in the plugin root). The editor module converts
 * UTexture2D data to and from these types.
 */
namespace PBRifyCore
{
	/** 8-bit RGBA image, rows top to bottom. */
	struct FRGBA8Image
	{
		int Width = 0;
		int Height = 0;
		std::vector<uint8_t> Pixels; // Width * Height * 4

		bool IsValid() const
		{
			return Width > 0 && Height > 0 && Pixels.size() == size_t(Width) * size_t(Height) * 4;
		}
	};

	/** 16-bit single channel image, rows top to bottom. */
	struct FGray16Image
	{
		int Width = 0;
		int Height = 0;
		std::vector<uint16_t> Pixels; // Width * Height
	};

	struct FSettings
	{
		/** Wrap filters around the image edges so seamless inputs produce seamless maps. */
		bool bTileable = false;

		/** 0 keeps the photo's lighting in the base color, 1 flattens large-scale light and shadow out of it. */
		float DelightStrength = 0.5f;

		/** By default bright = raised, dark = recessed. */
		bool bInvertHeight = false;
		/** Weights of the detail bands that make up the height map, from pixel-level grain up to big shapes. */
		float FineDetail = 0.2f;
		float MediumDetail = 0.6f;
		float LargeDetail = 1.0f;
		float ShapeDetail = 1.0f;
		float HeightContrast = 1.0f;

		float NormalStrength = 1.0f;
		/** Unreal uses DirectX style (Y-) normal maps. Set this for OpenGL style (Y+). */
		bool bFlipGreen = false;

		float AOStrength = 1.0f;

		float RoughnessBase = 0.7f;
		float RoughnessVariation = 0.3f;
		float Metallic = 0.0f;
	};

	struct FMaps
	{
		FRGBA8Image BaseColor; // sRGB
		FRGBA8Image Normal;    // tangent space, DirectX style unless bFlipGreen
		FRGBA8Image ORM;       // R = ambient occlusion, G = roughness, B = metallic (linear)
		FGray16Image Height;   // 0 = deepest, 65535 = highest
	};

	/** Calls Body(0 .. Count-1), possibly in parallel. Body is safe to call concurrently for different indices. */
	using FParallelFor = std::function<void(int Count, const std::function<void(int)>& Body)>;

	/** Area-averaging downscale so the longest side is at most MaxSize. Returns a copy of Source if it already fits or MaxSize <= 0. */
	FRGBA8Image Downscale(const FRGBA8Image& Source, int MaxSize);

	/** Generates all PBR maps from a color image. Returns empty maps if Source is invalid. */
	FMaps GenerateMaps(const FRGBA8Image& Source, const FSettings& Settings, const FParallelFor& ParallelFor = nullptr);
}
