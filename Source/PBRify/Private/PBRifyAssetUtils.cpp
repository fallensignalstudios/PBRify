#include "PBRifyAssetUtils.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetToolsModule.h"
#include "IAssetTools.h"

UObject* PBRifyAssetUtils::FindAsset(const FString& PackagePath, const FString& AssetName)
{
	const FString ObjectPath = FString::Printf(TEXT("%s/%s.%s"), *PackagePath, *AssetName, *AssetName);

	if (UObject* InMemory = FindObject<UObject>(nullptr, *ObjectPath))
	{
		// A deleted asset can linger in memory until garbage collection; treat it as gone.
		return IsValid(InMemory) ? InMemory : nullptr;
	}

	const IAssetRegistry& AssetRegistry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry").Get();
	const FAssetData AssetData = AssetRegistry.GetAssetByObjectPath(FSoftObjectPath(ObjectPath));
	return AssetData.IsValid() ? AssetData.GetAsset() : nullptr;
}

FString PBRifyAssetUtils::MakeUniqueAssetName(const FString& PackagePath, const FString& AssetName)
{
	FString UniquePackageName;
	FString UniqueAssetName;
	FAssetToolsModule::GetModule().Get().CreateUniqueAssetName(PackagePath / AssetName, FString(), UniquePackageName, UniqueAssetName);
	return UniqueAssetName;
}
