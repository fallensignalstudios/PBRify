#include "PBRifyGenerator.h"

#include "PBRifyAssetUtils.h"
#include "PBRifyMaterialBuilder.h"
#include "PBRifyModule.h"
#include "PBRifySettings.h"
#include "Processing/PBRifyCore.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "Async/ParallelFor.h"
#include "Editor.h"
#include "Engine/Texture2D.h"
#include "Framework/Notifications/NotificationManager.h"
#include "ImageCore.h"
#include "Materials/MaterialInstanceConstant.h"
#include "Misc/MessageDialog.h"
#include "Misc/PackageName.h"
#include "Misc/ScopedSlowTask.h"
#include "Widgets/Notifications/SNotificationList.h"

#define LOCTEXT_NAMESPACE "PBRify"

namespace PBRifyGeneratorPrivate
{
	PBRifyCore::FSettings ToCoreSettings(const UPBRifySettings& Settings)
	{
		PBRifyCore::FSettings Core;
		Core.bTileable = Settings.bTileable;
		Core.DelightStrength = Settings.DelightStrength;
		Core.bInvertHeight = Settings.bInvertHeight;
		Core.FineDetail = Settings.FineDetail;
		Core.MediumDetail = Settings.MediumDetail;
		Core.LargeDetail = Settings.LargeDetail;
		Core.ShapeDetail = Settings.ShapeDetail;
		Core.HeightContrast = Settings.HeightContrast;
		Core.NormalStrength = Settings.NormalStrength;
		Core.bFlipGreen = Settings.bFlipGreen;
		Core.AOStrength = Settings.AOStrength;
		Core.RoughnessBase = Settings.RoughnessBase;
		Core.RoughnessVariation = Settings.RoughnessVariation;
		Core.Metallic = Settings.Metallic;
		return Core;
	}

	void RunParallel(int Count, const std::function<void(int)>& Body)
	{
		ParallelFor(Count, [&Body](int32 Index) { Body(Index); });
	}

	bool ReadSourcePixels(UTexture2D* Texture, PBRifyCore::FRGBA8Image& OutImage, FText& OutError)
	{
		if (!Texture->Source.IsValid())
		{
			OutError = LOCTEXT("NoSourceData", "The texture has no source data (it may have been created at runtime).");
			return false;
		}

		FImage SourceImage;
		if (!Texture->Source.GetMipImage(SourceImage, 0, 0, 0))
		{
			OutError = LOCTEXT("ReadFailed", "Couldn't read the texture's source pixels.");
			return false;
		}

		// 8-bit sources hold display-ready values whatever the texture's sRGB flag says, so take them as they are.
		if (SourceImage.Format == ERawImageFormat::BGRA8 || SourceImage.Format == ERawImageFormat::G8)
		{
			SourceImage.GammaSpace = EGammaSpace::sRGB;
		}

		FImage Converted;
		SourceImage.CopyTo(Converted, ERawImageFormat::BGRA8, EGammaSpace::sRGB);
		const TArrayView64<FColor> Colors = Converted.AsBGRA8();
		const int64 NumPixels = int64(Converted.SizeX) * Converted.SizeY;

		OutImage.Width = Converted.SizeX;
		OutImage.Height = Converted.SizeY;
		OutImage.Pixels.resize(size_t(NumPixels) * 4);
		for (int64 Index = 0; Index < NumPixels; ++Index)
		{
			uint8* Out = &OutImage.Pixels[size_t(Index) * 4];
			Out[0] = Colors[Index].R;
			Out[1] = Colors[Index].G;
			Out[2] = Colors[Index].B;
			Out[3] = 255;
		}
		return true;
	}

	TArray64<uint8> ToBGRA(const PBRifyCore::FRGBA8Image& Image)
	{
		TArray64<uint8> Data;
		Data.SetNumUninitialized(int64(Image.Pixels.size()));
		for (int64 Index = 0; Index < Data.Num(); Index += 4)
		{
			Data[Index + 0] = Image.Pixels[size_t(Index) + 2];
			Data[Index + 1] = Image.Pixels[size_t(Index) + 1];
			Data[Index + 2] = Image.Pixels[size_t(Index) + 0];
			Data[Index + 3] = Image.Pixels[size_t(Index) + 3];
		}
		return Data;
	}

	/**
	 * Creates the texture asset. If it already exists and bUpdateExisting is set, it's updated in place
	 * so materials using it keep working; otherwise a new asset with a free name is made.
	 */
	UTexture2D* WriteTexture(
		const FString& PackagePath, const FString& DesiredName, bool bUpdateExisting, int32 Width, int32 Height,
		ETextureSourceFormat Format, const uint8* Data, TextureCompressionSettings Compression, bool bSRGB, TextureGroup LODGroup)
	{
		FString AssetName = DesiredName;
		UTexture2D* Texture = PBRifyAssetUtils::FindReusableAsset<UTexture2D>(PackagePath, AssetName, bUpdateExisting);
		const bool bIsNew = Texture == nullptr;
		if (bIsNew)
		{
			UPackage* Package = CreatePackage(*(PackagePath / AssetName));
			Texture = NewObject<UTexture2D>(Package, FName(*AssetName), RF_Public | RF_Standalone | RF_Transactional);
		}
		else
		{
			Texture->Modify();
			Texture->PreEditChange(nullptr);
		}

		Texture->Source.Init(Width, Height, /*NumSlices*/ 1, /*NumMips*/ 1, Format, Data);
		Texture->CompressionSettings = Compression;
		Texture->SRGB = bSRGB;
		Texture->LODGroup = LODGroup;
		Texture->MipGenSettings = TMGS_FromTextureGroup;
		// Non-power-of-two textures get no mips (so they don't stream, and parallax shimmers in the
		// distance). Let the engine stretch them; on a mesh it's invisible since the UVs cover the whole texture.
		Texture->PowerOfTwoMode = FMath::IsPowerOfTwo(Width) && FMath::IsPowerOfTwo(Height)
			? ETexturePowerOfTwoSetting::None
			: ETexturePowerOfTwoSetting::StretchToPowerOfTwo;
		Texture->PostEditChange();

		if (bIsNew)
		{
			FAssetRegistryModule::AssetCreated(Texture);
		}
		Texture->MarkPackageDirty();
		return Texture;
	}

	/** "T_Storefront" -> "Storefront" */
	FString GetBaseName(const UTexture2D* Source)
	{
		const FString Name = Source->GetName();
		return Name.StartsWith(TEXT("T_")) && Name.Len() > 2 ? Name.RightChop(2) : Name;
	}

	bool GetOutputPath(const UTexture2D* Source, const UPBRifySettings& Settings, FString& OutPath, FText& OutError)
	{
		OutPath = FPackageName::GetLongPackagePath(Source->GetPackage()->GetName());

		FString Subfolder = Settings.OutputSubfolder.TrimStartAndEnd();
		while (Subfolder.RemoveFromStart(TEXT("/"))) {}
		while (Subfolder.RemoveFromEnd(TEXT("/"))) {}
		if (!Subfolder.IsEmpty())
		{
			OutPath /= Subfolder;
		}

		if (!FPackageName::IsValidLongPackageName(OutPath))
		{
			OutError = FText::Format(LOCTEXT("BadOutputPath", "\"{0}\" isn't a valid folder to save into. Check the Output Subfolder setting."), FText::FromString(OutPath));
			return false;
		}
		return true;
	}

	struct FOutputNames
	{
		explicit FOutputNames(const FString& BaseName)
			: BaseColor(FString::Printf(TEXT("T_%s_BaseColor"), *BaseName))
			, Normal(FString::Printf(TEXT("T_%s_Normal"), *BaseName))
			, Height(FString::Printf(TEXT("T_%s_Height"), *BaseName))
			, ORM(FString::Printf(TEXT("T_%s_ORM"), *BaseName))
			, Material(FString::Printf(TEXT("MI_%s"), *BaseName))
		{
		}

		FString BaseColor;
		FString Normal;
		FString Height;
		FString ORM;
		FString Material;
	};

	/** Names of the outputs for BaseName that already exist as assets in OutputPath. */
	TArray<FString> FindExistingOutputs(const FString& OutputPath, const FString& BaseName, const UPBRifySettings& Settings)
	{
		const FOutputNames Names(BaseName);
		TArray<FString> Candidates = { Names.BaseColor, Names.Normal, Names.Height, Names.ORM };
		if (Settings.bCreateMaterial)
		{
			Candidates.Add(Names.Material);
		}

		TArray<FString> Existing;
		for (const FString& Name : Candidates)
		{
			if (PBRifyAssetUtils::FindAsset(OutputPath, Name))
			{
				Existing.Add(Name);
			}
		}
		return Existing;
	}

	/**
	 * Decides, per source, whether to update outputs that already exist or make new copies, asking the
	 * user when needed. Done up front so questions don't pop up halfway through the progress bar.
	 */
	TArray<bool> AskWhichToUpdate(const TArray<UTexture2D*>& Sources, const UPBRifySettings& Settings)
	{
		TArray<bool> UpdateExisting;
		TSet<FString> OutputsInBatch;
		TOptional<bool> AnswerForAll;
		for (const UTexture2D* Source : Sources)
		{
			bool bUpdate = true;
			FString OutputPath;
			FText IgnoredError; // GeneratePBRAssets reports it properly later
			if (Source && GetOutputPath(Source, Settings, OutputPath, IgnoredError))
			{
				const FString BaseName = GetBaseName(Source);
				bool bAlreadyInBatch = false;
				OutputsInBatch.Add(OutputPath / BaseName, &bAlreadyInBatch);

				if (bAlreadyInBatch)
				{
					// e.g. "T_Brick" and "Brick" in one selection: don't let the second overwrite the first.
					bUpdate = false;
				}
				else if (const TArray<FString> Existing = FindExistingOutputs(OutputPath, BaseName, Settings); !Existing.IsEmpty())
				{
					if (AnswerForAll.IsSet())
					{
						bUpdate = AnswerForAll.GetValue();
					}
					else
					{
						const EAppReturnType::Type Answer = FMessageDialog::Open(EAppMsgType::YesNoYesAllNoAll, FText::Format(
							LOCTEXT("UpdateExisting", "These assets for {0} already exist:\n\n{1}\n\nUpdate them in place?\n\nYes: replace their contents (anything using them keeps working).\nNo: leave them alone and create new copies."),
							FText::FromString(Source->GetName()),
							FText::FromString(FString::Join(Existing, TEXT("\n")))));
						bUpdate = Answer == EAppReturnType::Yes || Answer == EAppReturnType::YesAll;
						if (Answer == EAppReturnType::YesAll || Answer == EAppReturnType::NoAll)
						{
							AnswerForAll = bUpdate;
						}
					}
				}
			}
			UpdateExisting.Add(bUpdate);
		}
		return UpdateExisting;
	}
}

TArray<UObject*> FPBRifyResult::GetAssets() const
{
	TArray<UObject*> Assets;
	auto AddIfSet = [&Assets](UObject* Asset)
	{
		if (Asset)
		{
			Assets.Add(Asset);
		}
	};
	AddIfSet(BaseColor);
	AddIfSet(Normal);
	AddIfSet(Height);
	AddIfSet(ORM);
	AddIfSet(Material);
	return Assets;
}

bool PBRify::GeneratePBRAssets(UTexture2D* Source, const UPBRifySettings& Settings, FPBRifyResult& OutResult, FText& OutError, bool bUpdateExisting)
{
	using namespace PBRifyGeneratorPrivate;

	OutResult = FPBRifyResult();
	if (!Source)
	{
		OutError = LOCTEXT("NoSource", "No source texture.");
		return false;
	}

	FString OutputPath;
	if (!GetOutputPath(Source, Settings, OutputPath, OutError))
	{
		return false;
	}

	FScopedSlowTask SlowTask(4.0f, FText::Format(LOCTEXT("GeneratingOne", "PBRifying {0}..."), FText::FromString(Source->GetName())));

	SlowTask.EnterProgressFrame(0.5f, LOCTEXT("Reading", "Reading source pixels"));
	PBRifyCore::FRGBA8Image Image;
	if (!ReadSourcePixels(Source, Image, OutError))
	{
		return false;
	}
	Image = PBRifyCore::Downscale(Image, Settings.MaxResolution);

	SlowTask.EnterProgressFrame(2.0f, LOCTEXT("Processing", "Generating maps"));
	const double StartTime = FPlatformTime::Seconds();
	const PBRifyCore::FMaps Maps = PBRifyCore::GenerateMaps(Image, ToCoreSettings(Settings), &RunParallel);
	UE_LOG(LogPBRify, Log, TEXT("Generated %dx%d maps for %s in %.2fs"), Image.Width, Image.Height, *Source->GetName(), FPlatformTime::Seconds() - StartTime);

	SlowTask.EnterProgressFrame(1.0f, LOCTEXT("Writing", "Creating textures"));
	const FOutputNames Names(GetBaseName(Source));
	const int32 Width = Image.Width;
	const int32 Height = Image.Height;

	const TArray64<uint8> BaseColorData = ToBGRA(Maps.BaseColor);
	OutResult.BaseColor = WriteTexture(OutputPath, Names.BaseColor, bUpdateExisting, Width, Height,
		TSF_BGRA8, BaseColorData.GetData(), TC_Default, /*bSRGB*/ true, TEXTUREGROUP_World);

	const TArray64<uint8> NormalData = ToBGRA(Maps.Normal);
	OutResult.Normal = WriteTexture(OutputPath, Names.Normal, bUpdateExisting, Width, Height,
		TSF_BGRA8, NormalData.GetData(), TC_Normalmap, /*bSRGB*/ false, TEXTUREGROUP_WorldNormalMap);

	OutResult.Height = WriteTexture(OutputPath, Names.Height, bUpdateExisting, Width, Height,
		TSF_G16, reinterpret_cast<const uint8*>(Maps.Height.Pixels.data()), TC_Grayscale, /*bSRGB*/ false, TEXTUREGROUP_World);

	const TArray64<uint8> ORMData = ToBGRA(Maps.ORM);
	OutResult.ORM = WriteTexture(OutputPath, Names.ORM, bUpdateExisting, Width, Height,
		TSF_BGRA8, ORMData.GetData(), TC_Masks, /*bSRGB*/ false, TEXTUREGROUP_World);

	SlowTask.EnterProgressFrame(0.5f, LOCTEXT("Material", "Building material"));
	if (Settings.bCreateMaterial)
	{
		UMaterial* Master = PBRifyMaterialBuilder::FindOrCreateMasterMaterial(Settings.MasterMaterialPath, OutResult, OutError);
		if (!Master)
		{
			return false;
		}
		OutResult.Material = PBRifyMaterialBuilder::CreateOrUpdateInstance(
			Master, OutputPath, Names.Material, OutResult, Settings.ParallaxDepth, bUpdateExisting, OutError);
		if (!OutResult.Material)
		{
			return false;
		}
	}
	return true;
}

void PBRify::GenerateForTextures(const TArray<UTexture2D*>& Sources)
{
	const UPBRifySettings& Settings = *GetDefault<UPBRifySettings>();
	const TArray<bool> UpdateExisting = PBRifyGeneratorPrivate::AskWhichToUpdate(Sources, Settings);

	TArray<UObject*> CreatedAssets;
	TArray<FString> Failures;
	{
		FScopedSlowTask SlowTask(float(Sources.Num()), LOCTEXT("GeneratingAll", "PBRify: generating PBR materials..."));
		SlowTask.MakeDialog(/*bShowCancelButton*/ true);

		for (int32 SourceIndex = 0; SourceIndex < Sources.Num(); ++SourceIndex)
		{
			if (SlowTask.ShouldCancel())
			{
				break;
			}
			SlowTask.EnterProgressFrame(1.0f);

			UTexture2D* Source = Sources[SourceIndex];
			FPBRifyResult Result;
			FText Error;
			if (GeneratePBRAssets(Source, Settings, Result, Error, UpdateExisting[SourceIndex]))
			{
				CreatedAssets.Append(Result.GetAssets());
			}
			else
			{
				UE_LOG(LogPBRify, Error, TEXT("Couldn't PBRify %s: %s"), *GetNameSafe(Source), *Error.ToString());
				Failures.Add(FString::Printf(TEXT("%s: %s"), *GetNameSafe(Source), *Error.ToString()));
			}
		}
	}

	if (CreatedAssets.Num() > 0 && GEditor)
	{
		GEditor->SyncBrowserToObjects(CreatedAssets);
	}

	FNotificationInfo Info(Failures.IsEmpty()
		? FText::Format(LOCTEXT("Done", "PBRify created {0} assets. Drag the MI_ material onto a mesh!"), CreatedAssets.Num())
		: FText::Format(LOCTEXT("DoneWithErrors", "PBRify hit a snag:\n{0}"), FText::FromString(FString::Join(Failures, TEXT("\n")))));
	Info.ExpireDuration = Failures.IsEmpty() ? 5.0f : 10.0f;
	if (TSharedPtr<SNotificationItem> Notification = FSlateNotificationManager::Get().AddNotification(Info))
	{
		Notification->SetCompletionState(Failures.IsEmpty() ? SNotificationItem::CS_Success : SNotificationItem::CS_Fail);
	}
}

#undef LOCTEXT_NAMESPACE
