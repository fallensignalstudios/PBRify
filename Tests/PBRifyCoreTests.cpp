// Standalone tests for the engine-agnostic PBRify core (no Unreal needed).
//
//   cmake -S Tests -B Tests/Build && cmake --build Tests/Build && ./Tests/Build/PBRifyCoreTests
//
// It can also process an image, which is handy for tuning without launching the editor:
//
//   ./Tests/Build/PBRifyCoreTests --process input.ppm output_dir [Setting=Value ...]
//
// Input is a binary PPM (P6). Outputs: basecolor.ppm, normal.ppm, orm.ppm and height.pgm (16-bit).

#include "PBRifyCore.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <thread>

using namespace PBRifyCore;

namespace
{
	int GFailures = 0;

	void Check(bool bCondition, const char* What, int Line)
	{
		if (!bCondition)
		{
			std::printf("  FAILED (line %d): %s\n", Line, What);
			++GFailures;
		}
	}
#define CHECK(Condition) Check((Condition), #Condition, __LINE__)

	FRGBA8Image MakeImage(int Width, int Height, uint8_t Value)
	{
		FRGBA8Image Image;
		Image.Width = Width;
		Image.Height = Height;
		Image.Pixels.assign(size_t(Width) * size_t(Height) * 4, Value);
		return Image;
	}

	void SetPixel(FRGBA8Image& Image, int X, int Y, uint8_t R, uint8_t G, uint8_t B)
	{
		uint8_t* Pixel = &Image.Pixels[(size_t(Y) * size_t(Image.Width) + size_t(X)) * 4];
		Pixel[0] = R;
		Pixel[1] = G;
		Pixel[2] = B;
		Pixel[3] = 255;
	}

	const uint8_t* GetPixel(const FRGBA8Image& Image, int X, int Y)
	{
		return &Image.Pixels[(size_t(Y) * size_t(Image.Width) + size_t(X)) * 4];
	}

	float GetHeight(const FGray16Image& Image, int X, int Y)
	{
		return float(Image.Pixels[size_t(Y) * size_t(Image.Width) + size_t(X)]) / 65535.0f;
	}

	/** Dark image with a bright square in the middle: should come out as a raised block. */
	FRGBA8Image MakeBlockImage()
	{
		FRGBA8Image Image = MakeImage(128, 128, 40);
		for (int Y = 44; Y < 84; ++Y)
		{
			for (int X = 44; X < 84; ++X)
			{
				SetPixel(Image, X, Y, 220, 220, 220);
			}
		}
		return Image;
	}

	void ThreadedParallelFor(int Count, const std::function<void(int)>& Body)
	{
		const int NumThreads = 4;
		std::vector<std::thread> Threads;
		for (int ThreadIndex = 0; ThreadIndex < NumThreads; ++ThreadIndex)
		{
			Threads.emplace_back([=, &Body]()
			{
				for (int Index = ThreadIndex; Index < Count; Index += NumThreads)
				{
					Body(Index);
				}
			});
		}
		for (std::thread& Thread : Threads)
		{
			Thread.join();
		}
	}

	void TestInvalidInput()
	{
		std::printf("Invalid input\n");
		const FMaps Maps = GenerateMaps(FRGBA8Image(), FSettings());
		CHECK(!Maps.BaseColor.IsValid());
		CHECK(Maps.Height.Pixels.empty());
	}

	void TestFlatImage()
	{
		std::printf("Flat image\n");
		const FMaps Maps = GenerateMaps(MakeImage(64, 48, 128), FSettings());
		CHECK(Maps.BaseColor.Width == 64 && Maps.BaseColor.Height == 48);
		CHECK(Maps.Normal.IsValid() && Maps.ORM.IsValid());
		CHECK(Maps.Height.Pixels.size() == size_t(64 * 48));

		bool bFlatNormals = true;
		bool bMidHeight = true;
		bool bUnoccluded = true;
		bool bSameColor = true;
		for (int Y = 0; Y < 48; ++Y)
		{
			for (int X = 0; X < 64; ++X)
			{
				const uint8_t* Normal = GetPixel(Maps.Normal, X, Y);
				bFlatNormals &= Normal[0] == 128 && Normal[1] == 128 && Normal[2] == 255;
				bMidHeight &= std::fabs(GetHeight(Maps.Height, X, Y) - 0.5f) < 0.01f;
				bUnoccluded &= GetPixel(Maps.ORM, X, Y)[0] > 230;
				bSameColor &= std::abs(int(GetPixel(Maps.BaseColor, X, Y)[0]) - 128) <= 1;
			}
		}
		CHECK(bFlatNormals);
		CHECK(bMidHeight);
		CHECK(bUnoccluded);
		CHECK(bSameColor);
	}

	void TestRaisedBlock()
	{
		std::printf("Bright square becomes a raised block\n");
		const FMaps Maps = GenerateMaps(MakeBlockImage(), FSettings());

		CHECK(GetHeight(Maps.Height, 64, 64) > GetHeight(Maps.Height, 10, 10) + 0.3f);

		// DirectX convention: X right, Y down. Each edge of the block faces outward.
		CHECK(GetPixel(Maps.Normal, 44, 64)[0] < 100); // left edge faces left
		CHECK(GetPixel(Maps.Normal, 83, 64)[0] > 156); // right edge faces right
		CHECK(GetPixel(Maps.Normal, 64, 44)[1] < 100); // top edge faces up (-Y)
		CHECK(GetPixel(Maps.Normal, 64, 83)[1] > 156); // bottom edge faces down (+Y)
		CHECK(std::abs(int(GetPixel(Maps.Normal, 64, 44)[0]) - 128) < 8); // and doesn't lean sideways

		// The groove right at the foot of the block is occluded, the top of the block isn't.
		CHECK(GetPixel(Maps.ORM, 42, 64)[0] < GetPixel(Maps.ORM, 64, 64)[0]);
		CHECK(GetPixel(Maps.ORM, 64, 64)[0] > 200);

		CHECK(GetPixel(Maps.ORM, 64, 64)[2] == 0); // not metallic
	}

	void TestRoughness()
	{
		std::printf("Busy texture is rougher than smooth areas\n");
		// Left half: fine noise (like brick or concrete). Right half: smooth (like glass or paint).
		FRGBA8Image Image = MakeImage(256, 128, 128);
		uint32_t Seed = 12345;
		for (int Y = 0; Y < 128; ++Y)
		{
			for (int X = 0; X < 128; ++X)
			{
				Seed = Seed * 1664525u + 1013904223u;
				const uint8_t Value = uint8_t(90 + (Seed >> 24) % 80);
				SetPixel(Image, X, Y, Value, Value, Value);
			}
		}
		const FMaps Maps = GenerateMaps(Image, FSettings());
		CHECK(GetPixel(Maps.ORM, 40, 64)[1] > GetPixel(Maps.ORM, 220, 64)[1] + 60);
	}

	void TestFlipGreenAndInvert()
	{
		std::printf("Flip green / invert height\n");
		FSettings Settings;
		Settings.bFlipGreen = true;
		const FMaps Flipped = GenerateMaps(MakeBlockImage(), Settings);
		CHECK(GetPixel(Flipped.Normal, 64, 44)[1] > 156);
		CHECK(GetPixel(Flipped.Normal, 64, 83)[1] < 100);
		CHECK(GetPixel(Flipped.Normal, 44, 64)[0] < 100); // red is unaffected

		Settings = FSettings();
		Settings.bInvertHeight = true;
		const FMaps Inverted = GenerateMaps(MakeBlockImage(), Settings);
		CHECK(GetHeight(Inverted.Height, 64, 64) < GetHeight(Inverted.Height, 10, 10) - 0.3f);
		CHECK(GetPixel(Inverted.Normal, 44, 64)[0] > 156); // a pit: left wall now faces right
	}

	void TestDelight()
	{
		std::printf("Delighting flattens a lighting gradient\n");
		FRGBA8Image Image = MakeImage(256, 64, 0);
		for (int Y = 0; Y < 64; ++Y)
		{
			for (int X = 0; X < 256; ++X)
			{
				const uint8_t Value = uint8_t(40 + X * 180 / 255);
				SetPixel(Image, X, Y, Value, Value, Value);
			}
		}

		FSettings Settings;
		Settings.DelightStrength = 0.0f;
		const FMaps Untouched = GenerateMaps(Image, Settings);
		CHECK(GetPixel(Untouched.BaseColor, 20, 32)[0] == GetPixel(Image, 20, 32)[0]);

		Settings.DelightStrength = 1.0f;
		const FMaps Delit = GenerateMaps(Image, Settings);
		const int OriginalSpread = GetPixel(Image, 200, 32)[0] - GetPixel(Image, 50, 32)[0];
		const int DelitSpread = GetPixel(Delit.BaseColor, 200, 32)[0] - GetPixel(Delit.BaseColor, 50, 32)[0];
		CHECK(DelitSpread < OriginalSpread / 2);
	}

	void TestTileable()
	{
		std::printf("Tileable input stays tileable\n");
		// A periodic pattern, plus the same pattern shifted by half its width. With wrapping, the
		// results should simply be shifted copies of each other (no edge effects anywhere).
		const int Width = 96;
		const int Height = 64;
		FRGBA8Image Image = MakeImage(Width, Height, 0);
		FRGBA8Image Shifted = MakeImage(Width, Height, 0);
		for (int Y = 0; Y < Height; ++Y)
		{
			for (int X = 0; X < Width; ++X)
			{
				const double Phase = 2.0 * 3.14159265358979 * double(X) / double(Width);
				const uint8_t Value = uint8_t(128 + 100 * std::sin(Phase) * std::cos(4.0 * 3.14159265358979 * double(Y) / double(Height)));
				SetPixel(Image, X, Y, Value, Value, Value);
				SetPixel(Shifted, (X + Width / 2) % Width, Y, Value, Value, Value);
			}
		}

		FSettings Settings;
		Settings.bTileable = true;
		const FMaps A = GenerateMaps(Image, Settings);
		const FMaps B = GenerateMaps(Shifted, Settings);
		float MaxError = 0.0f;
		for (int Y = 0; Y < Height; ++Y)
		{
			for (int X = 0; X < Width; ++X)
			{
				MaxError = std::max(MaxError, std::fabs(GetHeight(A.Height, X, Y) - GetHeight(B.Height, (X + Width / 2) % Width, Y)));
			}
		}
		CHECK(MaxError < 0.02f);
	}

	void TestDownscale()
	{
		std::printf("Downscale\n");
		FRGBA8Image Image = MakeImage(400, 200, 0);
		for (int Y = 0; Y < 200; ++Y)
		{
			for (int X = 0; X < 400; ++X)
			{
				const uint8_t Value = (X + Y) % 2 ? 200 : 100;
				SetPixel(Image, X, Y, Value, Value, Value);
			}
		}

		const FRGBA8Image Small = Downscale(Image, 100);
		CHECK(Small.Width == 100 && Small.Height == 50 && Small.IsValid());
		CHECK(std::abs(int(GetPixel(Small, 37, 21)[0]) - 150) <= 1);

		const FRGBA8Image Same = Downscale(Image, 0);
		CHECK(Same.Width == 400 && Same.Height == 200);
		CHECK(Downscale(Image, 1000).Width == 400);
		CHECK(Downscale(Image, 3).Height == 2);
	}

	void TestParallelMatchesSerial()
	{
		std::printf("Parallel matches serial\n");
		FRGBA8Image Image = MakeBlockImage();
		for (int Y = 0; Y < Image.Height; ++Y)
		{
			for (int X = 0; X < Image.Width; ++X)
			{
				SetPixel(Image, X, Y, uint8_t((X * 7 + Y * 13) & 255), uint8_t(X * 2), uint8_t(Y * 2));
			}
		}
		const FMaps Serial = GenerateMaps(Image, FSettings());
		const FMaps Parallel = GenerateMaps(Image, FSettings(), ThreadedParallelFor);
		CHECK(Serial.BaseColor.Pixels == Parallel.BaseColor.Pixels);
		CHECK(Serial.Normal.Pixels == Parallel.Normal.Pixels);
		CHECK(Serial.ORM.Pixels == Parallel.ORM.Pixels);
		CHECK(Serial.Height.Pixels == Parallel.Height.Pixels);
	}

	// --- Image processing mode ---

	bool ReadPPM(const std::string& Path, FRGBA8Image& Out)
	{
		std::ifstream File(Path, std::ios::binary);
		std::string Magic;
		int MaxValue = 0;
		File >> Magic >> Out.Width >> Out.Height >> MaxValue;
		File.get();
		if (!File || Magic != "P6" || MaxValue != 255)
		{
			return false;
		}
		std::vector<uint8_t> RGB(size_t(Out.Width) * size_t(Out.Height) * 3);
		File.read(reinterpret_cast<char*>(RGB.data()), std::streamsize(RGB.size()));
		Out.Pixels.resize(size_t(Out.Width) * size_t(Out.Height) * 4);
		for (size_t Index = 0; Index < RGB.size() / 3; ++Index)
		{
			std::memcpy(&Out.Pixels[Index * 4], &RGB[Index * 3], 3);
			Out.Pixels[Index * 4 + 3] = 255;
		}
		return bool(File);
	}

	void WritePPM(const std::string& Path, const FRGBA8Image& Image)
	{
		std::ofstream File(Path, std::ios::binary);
		File << "P6\n" << Image.Width << " " << Image.Height << "\n255\n";
		for (size_t Index = 0; Index < Image.Pixels.size(); Index += 4)
		{
			File.write(reinterpret_cast<const char*>(&Image.Pixels[Index]), 3);
		}
	}

	void WritePGM16(const std::string& Path, const FGray16Image& Image)
	{
		std::ofstream File(Path, std::ios::binary);
		File << "P5\n" << Image.Width << " " << Image.Height << "\n65535\n";
		for (uint16_t Value : Image.Pixels)
		{
			const char BigEndian[2] = { char(Value >> 8), char(Value & 0xFF) };
			File.write(BigEndian, 2);
		}
	}

	bool ApplySetting(FSettings& Settings, const std::string& Assignment)
	{
		const size_t Equals = Assignment.find('=');
		if (Equals == std::string::npos)
		{
			return false;
		}
		const std::string Name = Assignment.substr(0, Equals);
		const float Value = std::strtof(Assignment.c_str() + Equals + 1, nullptr);
		struct FFloatSetting { const char* Name; float FSettings::* Member; };
		struct FBoolSetting { const char* Name; bool FSettings::* Member; };
		const FFloatSetting Floats[] = {
			{ "DelightStrength", &FSettings::DelightStrength }, { "FineDetail", &FSettings::FineDetail },
			{ "MediumDetail", &FSettings::MediumDetail }, { "LargeDetail", &FSettings::LargeDetail },
			{ "ShapeDetail", &FSettings::ShapeDetail }, { "HeightContrast", &FSettings::HeightContrast },
			{ "NormalStrength", &FSettings::NormalStrength }, { "AOStrength", &FSettings::AOStrength },
			{ "RoughnessBase", &FSettings::RoughnessBase }, { "RoughnessVariation", &FSettings::RoughnessVariation },
			{ "Metallic", &FSettings::Metallic },
		};
		const FBoolSetting Bools[] = {
			{ "Tileable", &FSettings::bTileable }, { "InvertHeight", &FSettings::bInvertHeight }, { "FlipGreen", &FSettings::bFlipGreen },
		};
		for (const FFloatSetting& Setting : Floats)
		{
			if (Name == Setting.Name)
			{
				Settings.*Setting.Member = Value;
				return true;
			}
		}
		for (const FBoolSetting& Setting : Bools)
		{
			if (Name == Setting.Name)
			{
				Settings.*Setting.Member = Value != 0.0f;
				return true;
			}
		}
		return false;
	}

	int ProcessImage(int ArgCount, char** Args)
	{
		FRGBA8Image Image;
		if (!ReadPPM(Args[2], Image))
		{
			std::printf("Could not read %s (expected a binary P6 PPM)\n", Args[2]);
			return 1;
		}
		FSettings Settings;
		for (int ArgIndex = 4; ArgIndex < ArgCount; ++ArgIndex)
		{
			if (!ApplySetting(Settings, Args[ArgIndex]))
			{
				std::printf("Unknown setting: %s\n", Args[ArgIndex]);
				return 1;
			}
		}

		const FMaps Maps = GenerateMaps(Image, Settings, ThreadedParallelFor);
		const std::string OutDir = Args[3];
		WritePPM(OutDir + "/basecolor.ppm", Maps.BaseColor);
		WritePPM(OutDir + "/normal.ppm", Maps.Normal);
		WritePPM(OutDir + "/orm.ppm", Maps.ORM);
		WritePGM16(OutDir + "/height.pgm", Maps.Height);
		std::printf("Wrote maps for %dx%d image to %s\n", Image.Width, Image.Height, OutDir.c_str());
		return 0;
	}
}

int main(int ArgCount, char** Args)
{
	if (ArgCount >= 4 && std::strcmp(Args[1], "--process") == 0)
	{
		return ProcessImage(ArgCount, Args);
	}

	TestInvalidInput();
	TestFlatImage();
	TestRaisedBlock();
	TestRoughness();
	TestFlipGreenAndInvert();
	TestDelight();
	TestTileable();
	TestDownscale();
	TestParallelMatchesSerial();

	if (GFailures > 0)
	{
		std::printf("\n%d check(s) failed\n", GFailures);
		return 1;
	}
	std::printf("\nAll tests passed\n");
	return 0;
}
