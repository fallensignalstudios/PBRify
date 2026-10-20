#include "PBRifyOptionsDialog.h"

#include "PBRifySettings.h"

#include "Editor.h"
#include "IDetailsView.h"
#include "Modules/ModuleManager.h"
#include "PropertyEditorModule.h"
#include "Styling/AppStyle.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/SWindow.h"

#define LOCTEXT_NAMESPACE "PBRify"

bool PBRifyOptionsDialog::Show(int32 NumTextures)
{
	UPBRifySettings* Settings = GetMutableDefault<UPBRifySettings>();

	FDetailsViewArgs DetailsViewArgs;
	DetailsViewArgs.bAllowSearch = false;
	DetailsViewArgs.bHideSelectionTip = true;
	DetailsViewArgs.NameAreaSettings = FDetailsViewArgs::HideNameArea;
	TSharedRef<IDetailsView> DetailsView = FModuleManager::LoadModuleChecked<FPropertyEditorModule>("PropertyEditor").CreateDetailView(DetailsViewArgs);
	DetailsView->SetObject(Settings);

	TSharedRef<SWindow> Window = SNew(SWindow)
		.Title(FText::Format(LOCTEXT("OptionsTitle", "PBRify ({0} {0}|plural(one=texture,other=textures))"), NumTextures))
		.ClientSize(FVector2D(520.0, 760.0))
		.SupportsMinimize(false)
		.SupportsMaximize(false);

	bool bGenerate = false;
	const TWeakPtr<SWindow> WeakWindow = Window;
	auto CloseWith = [&bGenerate, WeakWindow](bool bInGenerate)
	{
		bGenerate = bInGenerate;
		if (const TSharedPtr<SWindow> PinnedWindow = WeakWindow.Pin())
		{
			PinnedWindow->RequestDestroyWindow();
		}
		return FReply::Handled();
	};

	Window->SetContent(
		SNew(SBorder)
		.BorderImage(FAppStyle::GetBrush("ToolPanel.GroupBorder"))
		[
			SNew(SVerticalBox)
			+ SVerticalBox::Slot()
			.FillHeight(1.0f)
			[
				DetailsView
			]
			+ SVerticalBox::Slot()
			.AutoHeight()
			.HAlign(HAlign_Right)
			.Padding(8.0f)
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot()
				.AutoWidth()
				.Padding(4.0f, 0.0f)
				[
					SNew(SButton)
					.ButtonStyle(&FAppStyle::Get().GetWidgetStyle<FButtonStyle>("PrimaryButton"))
					.Text(LOCTEXT("Generate", "Generate"))
					.OnClicked_Lambda([CloseWith]() { return CloseWith(true); })
				]
				+ SHorizontalBox::Slot()
				.AutoWidth()
				.Padding(4.0f, 0.0f)
				[
					SNew(SButton)
					.Text(LOCTEXT("Cancel", "Cancel"))
					.OnClicked_Lambda([CloseWith]() { return CloseWith(false); })
				]
			]
		]);

	GEditor->EditorAddModalWindow(Window);

	// Remember the tweaks for next time, whichever button was pressed.
	Settings->SaveConfig();
	return bGenerate;
}

#undef LOCTEXT_NAMESPACE
