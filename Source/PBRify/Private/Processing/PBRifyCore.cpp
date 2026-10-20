#include "PBRifyCore.h"

#include <algorithm>
#include <cmath>

namespace PBRifyCore::Detail
{
	// Filter sizes are fractions of the image's longest side, so a 1k and a 4k copy of the same
	// photo produce the same looking maps.
	constexpr float DelightScale = 0.15f;
	constexpr float FineScale = 0.002f;
	constexpr float MediumScale = 0.01f;
	constexpr float LargeScale = 0.04f;
	constexpr float ShapeScale = 0.25f;
	constexpr float CavitySmallScale = 0.004f;
	constexpr float CavityLargeScale = 0.02f;
	constexpr float RoughnessDetailScale = 0.003f;
	constexpr float RoughnessAreaScale = 0.01f;

	// Height slope -> normal tilt. Picked so NormalStrength 1 roughly matches the default parallax depth.
	constexpr float NormalScale = 0.04f;

	/** Single channel float image used for all intermediate work. */
	struct FChannel
	{
		int Width = 0;
		int Height = 0;
		std::vector<float> Data;

		FChannel() = default;
		FChannel(int InWidth, int InHeight, float Value = 0.0f)
			: Width(InWidth), Height(InHeight), Data(size_t(InWidth) * size_t(InHeight), Value)
		{
		}

		float& operator()(int X, int Y) { return Data[size_t(Y) * size_t(Width) + size_t(X)]; }
		float operator()(int X, int Y) const { return Data[size_t(Y) * size_t(Width) + size_t(X)]; }
		size_t Num() const { return Data.size(); }
	};

	struct FFilterContext
	{
		bool bWrap = false;
		FParallelFor ParallelFor;

		void For(int Count, const std::function<void(int)>& Body) const
		{
			if (ParallelFor)
			{
				ParallelFor(Count, Body);
				return;
			}
			for (int Index = 0; Index < Count; ++Index)
			{
				Body(Index);
			}
		}

		/** Maps an out-of-range coordinate back into [0, Count) by wrapping or clamping. */
		int Address(int Index, int Count) const
		{
			if (bWrap)
			{
				Index %= Count;
				return Index < 0 ? Index + Count : Index;
			}
			return std::clamp(Index, 0, Count - 1);
		}

		float Sample(const FChannel& Plane, int X, int Y) const
		{
			return Plane(Address(X, Plane.Width), Address(Y, Plane.Height));
		}
	};

	float Saturate(float Value)
	{
		return std::clamp(Value, 0.0f, 1.0f);
	}

	float SRGBToLinear(float Value)
	{
		return Value <= 0.04045f ? Value / 12.92f : std::pow((Value + 0.055f) / 1.055f, 2.4f);
	}

	float LinearToSRGB(float Value)
	{
		return Value <= 0.0031308f ? Value * 12.92f : 1.055f * std::pow(Value, 1.0f / 2.4f) - 0.055f;
	}

	uint8_t ToByte(float Value)
	{
		return uint8_t(Saturate(Value) * 255.0f + 0.5f);
	}

	void BoxBlurX(const FChannel& In, FChannel& Out, int Radius, const FFilterContext& Context)
	{
		const double Scale = 1.0 / double(2 * Radius + 1);
		Context.For(In.Height, [&](int Y)
		{
			double Sum = 0.0;
			for (int X = -Radius; X <= Radius; ++X)
			{
				Sum += Context.Sample(In, X, Y);
			}
			for (int X = 0; X < In.Width; ++X)
			{
				Out(X, Y) = float(Sum * Scale);
				Sum += Context.Sample(In, X + Radius + 1, Y) - Context.Sample(In, X - Radius, Y);
			}
		});
	}

	/** Vertical box blur. Works on strips of columns with one running sum per column, so memory is still read row by row. */
	void BoxBlurY(const FChannel& In, FChannel& Out, int Radius, const FFilterContext& Context)
	{
		constexpr int StripWidth = 64;
		const int NumStrips = (In.Width + StripWidth - 1) / StripWidth;
		const double Scale = 1.0 / double(2 * Radius + 1);
		Context.For(NumStrips, [&](int Strip)
		{
			const int StartX = Strip * StripWidth;
			const int EndX = std::min(StartX + StripWidth, In.Width);
			double Sums[StripWidth] = {};
			for (int Y = -Radius; Y <= Radius; ++Y)
			{
				const int SourceY = Context.Address(Y, In.Height);
				for (int X = StartX; X < EndX; ++X)
				{
					Sums[X - StartX] += In(X, SourceY);
				}
			}
			for (int Y = 0; Y < In.Height; ++Y)
			{
				const int AddY = Context.Address(Y + Radius + 1, In.Height);
				const int RemoveY = Context.Address(Y - Radius, In.Height);
				for (int X = StartX; X < EndX; ++X)
				{
					double& Sum = Sums[X - StartX];
					Out(X, Y) = float(Sum * Scale);
					Sum += double(In(X, AddY)) - double(In(X, RemoveY));
				}
			}
		});
	}

	/** Gaussian approximated by three box blurs (Kovesi, "Fast Almost-Gaussian Filtering"). Cost doesn't depend on Sigma. */
	FChannel GaussianBlur(const FChannel& In, float Sigma, const FFilterContext& Context)
	{
		if (Sigma < 0.5f)
		{
			return In;
		}

		constexpr int NumPasses = 3;
		const float Variance12 = 12.0f * Sigma * Sigma;
		int LowerWidth = int(std::floor(std::sqrt(Variance12 / float(NumPasses) + 1.0f)));
		if (LowerWidth % 2 == 0)
		{
			--LowerWidth;
		}
		const int UpperWidth = LowerWidth + 2;
		const float Lower = float(LowerWidth);
		const float IdealLowerCount = (Variance12 - float(NumPasses) * (Lower * Lower + 4.0f * Lower + 3.0f)) / (-4.0f * Lower - 4.0f);
		const int LowerCount = int(std::lround(IdealLowerCount));

		FChannel Result = In;
		FChannel Scratch(In.Width, In.Height);
		for (int Pass = 0; Pass < NumPasses; ++Pass)
		{
			const int Radius = ((Pass < LowerCount ? LowerWidth : UpperWidth) - 1) / 2;
			BoxBlurX(Result, Scratch, Radius, Context);
			BoxBlurY(Scratch, Result, Radius, Context);
		}
		return Result;
	}

	/** Remaps values so the given percentiles land on 0 and 1, which ignores a handful of extreme pixels. Flat input becomes 0.5. */
	void NormalizeRobust(FChannel& Plane, float LowPercentile, float HighPercentile)
	{
		const size_t Step = std::max<size_t>(1, Plane.Num() / 65536);
		std::vector<float> Samples;
		Samples.reserve(Plane.Num() / Step + 1);
		for (size_t Index = 0; Index < Plane.Num(); Index += Step)
		{
			Samples.push_back(Plane.Data[Index]);
		}

		auto Percentile = [&Samples](float Fraction)
		{
			const size_t Index = std::min(Samples.size() - 1, size_t(Fraction * float(Samples.size() - 1)));
			std::nth_element(Samples.begin(), Samples.begin() + std::ptrdiff_t(Index), Samples.end());
			return Samples[Index];
		};
		const float Low = Percentile(LowPercentile);
		const float High = Percentile(HighPercentile);

		if (High - Low < 1e-5f)
		{
			std::fill(Plane.Data.begin(), Plane.Data.end(), 0.5f);
			return;
		}
		const float InvRange = 1.0f / (High - Low);
		for (float& Value : Plane.Data)
		{
			Value = Saturate((Value - Low) * InvRange);
		}
	}

	FRGBA8Image MakeRGBA8(int Width, int Height)
	{
		FRGBA8Image Image;
		Image.Width = Width;
		Image.Height = Height;
		Image.Pixels.assign(size_t(Width) * size_t(Height) * 4, 255);
		return Image;
	}
}

namespace PBRifyCore
{
	FRGBA8Image Downscale(const FRGBA8Image& Source, int MaxSize)
	{
		const int Longest = std::max(Source.Width, Source.Height);
		if (MaxSize <= 0 || Longest <= MaxSize || !Source.IsValid())
		{
			return Source;
		}

		const double Scale = double(MaxSize) / double(Longest);
		FRGBA8Image Result = Detail::MakeRGBA8(
			std::max(1, int(std::lround(Source.Width * Scale))),
			std::max(1, int(std::lround(Source.Height * Scale))));

		const double StepX = double(Source.Width) / double(Result.Width);
		const double StepY = double(Source.Height) / double(Result.Height);
		for (int Y = 0; Y < Result.Height; ++Y)
		{
			const int StartY = int(Y * StepY);
			const int EndY = std::clamp(int((Y + 1) * StepY), StartY + 1, Source.Height);
			for (int X = 0; X < Result.Width; ++X)
			{
				const int StartX = int(X * StepX);
				const int EndX = std::clamp(int((X + 1) * StepX), StartX + 1, Source.Width);

				uint32_t Sum[4] = {};
				for (int SourceY = StartY; SourceY < EndY; ++SourceY)
				{
					for (int SourceX = StartX; SourceX < EndX; ++SourceX)
					{
						const uint8_t* Pixel = &Source.Pixels[(size_t(SourceY) * size_t(Source.Width) + size_t(SourceX)) * 4];
						for (int Channel = 0; Channel < 4; ++Channel)
						{
							Sum[Channel] += Pixel[Channel];
						}
					}
				}

				const uint32_t Count = uint32_t((EndX - StartX) * (EndY - StartY));
				uint8_t* Out = &Result.Pixels[(size_t(Y) * size_t(Result.Width) + size_t(X)) * 4];
				for (int Channel = 0; Channel < 4; ++Channel)
				{
					Out[Channel] = uint8_t((Sum[Channel] + Count / 2) / Count);
				}
			}
		}
		return Result;
	}

	FMaps GenerateMaps(const FRGBA8Image& Source, const FSettings& Settings, const FParallelFor& ParallelFor)
	{
		using namespace Detail;

		FMaps Maps;
		if (!Source.IsValid())
		{
			return Maps;
		}

		FFilterContext Context;
		Context.bWrap = Settings.bTileable;
		Context.ParallelFor = ParallelFor;

		const int Width = Source.Width;
		const int Height = Source.Height;
		const size_t NumPixels = size_t(Width) * size_t(Height);
		const float Size = float(std::max(Width, Height));

		// Decode. Lighting math happens in linear space, but height comes from perceived (gamma space)
		// brightness, which is closer to what an artist would paint by eye.
		FChannel LinearR(Width, Height), LinearG(Width, Height), LinearB(Width, Height);
		FChannel LinearLuminance(Width, Height), Luma(Width, Height);
		for (size_t Index = 0; Index < NumPixels; ++Index)
		{
			const float R = float(Source.Pixels[Index * 4 + 0]) / 255.0f;
			const float G = float(Source.Pixels[Index * 4 + 1]) / 255.0f;
			const float B = float(Source.Pixels[Index * 4 + 2]) / 255.0f;
			LinearR.Data[Index] = SRGBToLinear(R);
			LinearG.Data[Index] = SRGBToLinear(G);
			LinearB.Data[Index] = SRGBToLinear(B);
			LinearLuminance.Data[Index] = 0.2126f * LinearR.Data[Index] + 0.7152f * LinearG.Data[Index] + 0.0722f * LinearB.Data[Index];
			Luma.Data[Index] = 0.2126f * R + 0.7152f * G + 0.0722f * B;
		}

		// Base color: divide out the large-scale lighting (sun gradients, big soft shadows) so the
		// engine's lighting isn't fighting the light that was baked into the photo.
		Maps.BaseColor = MakeRGBA8(Width, Height);
		{
			const FChannel Lighting = GaussianBlur(LinearLuminance, Size * DelightScale, Context);
			double LuminanceSum = 0.0;
			for (float Value : LinearLuminance.Data)
			{
				LuminanceSum += Value;
			}
			const float MeanLuminance = float(LuminanceSum / double(NumPixels));

			for (size_t Index = 0; Index < NumPixels; ++Index)
			{
				const float FullGain = std::clamp(MeanLuminance / std::max(Lighting.Data[Index], 1e-3f), 0.25f, 4.0f);
				const float Gain = 1.0f + (FullGain - 1.0f) * Settings.DelightStrength;
				uint8_t* Out = &Maps.BaseColor.Pixels[Index * 4];
				Out[0] = ToByte(LinearToSRGB(Saturate(LinearR.Data[Index] * Gain)));
				Out[1] = ToByte(LinearToSRGB(Saturate(LinearG.Data[Index] * Gain)));
				Out[2] = ToByte(LinearToSRGB(Saturate(LinearB.Data[Index] * Gain)));
			}
		}

		// Height: split brightness into detail bands (difference of gaussians) and recombine them with
		// user weights. Everything coarser than the "shape" band is dropped, which also removes most
		// leftover lighting gradients. With all weights at 1 this is a plain high-pass.
		FChannel HeightMap(Width, Height);
		{
			FChannel Base = Luma;
			if (Settings.bInvertHeight)
			{
				for (float& Value : Base.Data)
				{
					Value = 1.0f - Value;
				}
			}
			const FChannel Fine = GaussianBlur(Base, Size * FineScale, Context);
			const FChannel Medium = GaussianBlur(Base, Size * MediumScale, Context);
			const FChannel Large = GaussianBlur(Base, Size * LargeScale, Context);
			const FChannel Shape = GaussianBlur(Base, Size * ShapeScale, Context);
			for (size_t Index = 0; Index < NumPixels; ++Index)
			{
				HeightMap.Data[Index] =
					Settings.FineDetail * (Base.Data[Index] - Fine.Data[Index]) +
					Settings.MediumDetail * (Fine.Data[Index] - Medium.Data[Index]) +
					Settings.LargeDetail * (Medium.Data[Index] - Large.Data[Index]) +
					Settings.ShapeDetail * (Large.Data[Index] - Shape.Data[Index]);
			}
			NormalizeRobust(HeightMap, 0.005f, 0.995f);
			for (float& Value : HeightMap.Data)
			{
				Value = Saturate((Value - 0.5f) * Settings.HeightContrast + 0.5f);
			}
		}

		// Normal: Sobel gradient of the height map.
		// DirectX convention: +X = right, +Y = down the image (green bright on downward facing slopes).
		Maps.Normal = MakeRGBA8(Width, Height);
		{
			const float Strength = Settings.NormalStrength * Size * NormalScale / 8.0f; // Sobel sums to 8x the gradient
			const float GreenSign = Settings.bFlipGreen ? 1.0f : -1.0f;
			Context.For(Height, [&](int Y)
			{
				for (int X = 0; X < Width; ++X)
				{
					auto H = [&](int OffsetX, int OffsetY) { return Context.Sample(HeightMap, X + OffsetX, Y + OffsetY); };
					const float DeltaX = (H(1, -1) + 2.0f * H(1, 0) + H(1, 1)) - (H(-1, -1) + 2.0f * H(-1, 0) + H(-1, 1));
					const float DeltaY = (H(-1, 1) + 2.0f * H(0, 1) + H(1, 1)) - (H(-1, -1) + 2.0f * H(0, -1) + H(1, -1));
					const float NX = -DeltaX * Strength;
					const float NY = GreenSign * DeltaY * Strength;
					const float InvLength = 1.0f / std::sqrt(NX * NX + NY * NY + 1.0f);

					uint8_t* Out = &Maps.Normal.Pixels[(size_t(Y) * size_t(Width) + size_t(X)) * 4];
					Out[0] = ToByte(NX * InvLength * 0.5f + 0.5f);
					Out[1] = ToByte(NY * InvLength * 0.5f + 0.5f);
					Out[2] = ToByte(InvLength * 0.5f + 0.5f);
				}
			});
		}

		// Ambient occlusion: how far each pixel sits below its surroundings at two scales (cavities),
		// plus a little darkening for the deepest areas overall.
		FChannel Cavity(Width, Height);
		{
			const FChannel SmallSurroundings = GaussianBlur(HeightMap, Size * CavitySmallScale, Context);
			const FChannel LargeSurroundings = GaussianBlur(HeightMap, Size * CavityLargeScale, Context);
			for (size_t Index = 0; Index < NumPixels; ++Index)
			{
				const float H = HeightMap.Data[Index];
				Cavity.Data[Index] = Saturate(
					3.0f * std::max(0.0f, SmallSurroundings.Data[Index] - H) +
					2.0f * std::max(0.0f, LargeSurroundings.Data[Index] - H));
			}
		}

		// Roughness: busy micro-texture (brick, wood grain, concrete) reads as rough, smooth areas (glass,
		// paint, plastic) as glossy, and crevices collect grime. A heuristic, but a believable one.
		FChannel Busyness(Width, Height);
		{
			const FChannel Smoothed = GaussianBlur(Luma, Size * RoughnessDetailScale, Context);
			for (size_t Index = 0; Index < NumPixels; ++Index)
			{
				Busyness.Data[Index] = std::fabs(Luma.Data[Index] - Smoothed.Data[Index]);
			}
			Busyness = GaussianBlur(Busyness, Size * RoughnessAreaScale, Context);
			NormalizeRobust(Busyness, 0.01f, 0.99f);
		}

		Maps.ORM = MakeRGBA8(Width, Height);
		const uint8_t Metallic = ToByte(Settings.Metallic);
		for (size_t Index = 0; Index < NumPixels; ++Index)
		{
			const float Depth = 1.0f - HeightMap.Data[Index];
			const float Occlusion = 1.0f - Settings.AOStrength * (Cavity.Data[Index] + 0.2f * Depth * Depth);
			const float Roughness = Settings.RoughnessBase
				+ Settings.RoughnessVariation * (2.0f * Busyness.Data[Index] - 1.0f)
				+ 0.25f * Cavity.Data[Index];

			uint8_t* Out = &Maps.ORM.Pixels[Index * 4];
			Out[0] = ToByte(Occlusion);
			Out[1] = ToByte(std::max(Roughness, 0.04f));
			Out[2] = Metallic;
		}

		Maps.Height.Width = Width;
		Maps.Height.Height = Height;
		Maps.Height.Pixels.resize(NumPixels);
		for (size_t Index = 0; Index < NumPixels; ++Index)
		{
			Maps.Height.Pixels[Index] = uint16_t(HeightMap.Data[Index] * 65535.0f + 0.5f);
		}

		return Maps;
	}
}
