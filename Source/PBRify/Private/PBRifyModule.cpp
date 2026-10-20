#include "PBRifyModule.h"

#include "PBRifyGenerator.h"
#include "PBRifyOptionsDialog.h"

#include "ContentBrowserMenuContexts.h"
#include "Engine/Texture2D.h"
#include "Modules/ModuleManager.h"
#include "Styling/AppStyle.h"
#include "ToolMenus.h"

#define LOCTEXT_NAMESPACE "PBRify"

DEFINE_LOG_CATEGORY(LogPBRify);

namespace PBRifyModulePrivate
{
	TArray<UTexture2D*> LoadTextures(const TArray<FAssetData>& Assets)
	{
		TArray<UTexture2D*> Textures;
		for (const FAssetData& Asset : Assets)
		{
			if (UTexture2D* Texture = Cast<UTexture2D>(Asset.GetAsset()))
			{
				Textures.Add(Texture);
			}
		}
		return Textures;
	}

	void AddTextureMenuEntries(FToolMenuSection& Section)
	{
		const UContentBrowserAssetContextMenuContext* Context = Section.FindContext<UContentBrowserAssetContextMenuContext>();
		if (!Context)
		{
			return;
		}

		TArray<FAssetData> TextureAssets;
		for (const FAssetData& Asset : Context->SelectedAssets)
		{
			if (Asset.IsInstanceOf(UTexture2D::StaticClass()))
			{
				TextureAssets.Add(Asset);
			}
		}
		if (TextureAssets.IsEmpty())
		{
			return;
		}

		Section.AddMenuEntry(
			"PBRify_Generate",
			LOCTEXT("GenerateLabel", "PBRify: Generate PBR Material"),
			LOCTEXT("GenerateTooltip", "Create base color, normal, height and ORM textures plus a parallax material from this image, using your saved PBRify settings."),
			FSlateIcon(FAppStyle::GetAppStyleSetName(), "ClassIcon.Material"),
			FUIAction(FExecuteAction::CreateLambda([TextureAssets]()
			{
				PBRify::GenerateForTextures(LoadTextures(TextureAssets));
			})));

		Section.AddMenuEntry(
			"PBRify_GenerateWithOptions",
			LOCTEXT("GenerateWithOptionsLabel", "PBRify: Generate with Options..."),
			LOCTEXT("GenerateWithOptionsTooltip", "Tweak the PBRify settings, then generate."),
			FSlateIcon(FAppStyle::GetAppStyleSetName(), "Icons.Settings"),
			FUIAction(FExecuteAction::CreateLambda([TextureAssets]()
			{
				if (PBRifyOptionsDialog::Show(TextureAssets.Num()))
				{
					PBRify::GenerateForTextures(LoadTextures(TextureAssets));
				}
			})));
	}
}

void FPBRifyModule::StartupModule()
{
	UToolMenus::RegisterStartupCallback(FSimpleMulticastDelegate::FDelegate::CreateRaw(this, &FPBRifyModule::RegisterMenus));
}

void FPBRifyModule::ShutdownModule()
{
	UToolMenus::UnRegisterStartupCallback(this);
	UToolMenus::UnregisterOwner(this);
}

void FPBRifyModule::RegisterMenus()
{
	FToolMenuOwnerScoped OwnerScoped(this);

	// Extend the shared asset menu (every asset type's menu inherits from it) and only show our
	// entries when the selection contains textures.
	UToolMenu* Menu = UToolMenus::Get()->ExtendMenu("ContentBrowser.AssetContextMenu");
	FToolMenuSection& Section = Menu->FindOrAddSection("GetAssetActions");
	Section.AddDynamicEntry("PBRify", FNewToolMenuSectionDelegate::CreateStatic(&PBRifyModulePrivate::AddTextureMenuEntries));
}

IMPLEMENT_MODULE(FPBRifyModule, PBRify)

#undef LOCTEXT_NAMESPACE
