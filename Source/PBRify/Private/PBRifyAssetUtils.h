#pragma once

#include "CoreMinimal.h"

namespace PBRifyAssetUtils
{
	/** Returns the asset at PackagePath/AssetName (loading it if needed), or null if there isn't one. */
	UObject* FindAsset(const FString& PackagePath, const FString& AssetName);

	/** Returns a name based on AssetName that no asset in PackagePath uses yet. */
	FString MakeUniqueAssetName(const FString& PackagePath, const FString& AssetName);

	/**
	 * Finds an existing T at PackagePath/InOutAssetName so it can be updated in place. If a different
	 * kind of asset already uses that name, InOutAssetName is changed to a free one and null is returned.
	 */
	template <typename T>
	T* FindReusableAsset(const FString& PackagePath, FString& InOutAssetName)
	{
		UObject* Existing = FindAsset(PackagePath, InOutAssetName);
		if (T* Typed = Cast<T>(Existing))
		{
			return Typed;
		}
		if (Existing)
		{
			InOutAssetName = MakeUniqueAssetName(PackagePath, InOutAssetName);
		}
		return nullptr;
	}
}
