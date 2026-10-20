#pragma once

#include "CoreMinimal.h"
#include "Templates/Casts.h"

namespace PBRifyAssetUtils
{
	/** Returns the asset at PackagePath/AssetName (loading it if needed), or null if there isn't one. */
	UObject* FindAsset(const FString& PackagePath, const FString& AssetName);

	/** Returns a name based on AssetName that no asset in PackagePath uses yet. */
	FString MakeUniqueAssetName(const FString& PackagePath, const FString& AssetName);

	/**
	 * Finds an existing T at PackagePath/InOutAssetName so it can be updated in place. If the name is
	 * taken by a different kind of asset, or bAllowReuse is false, InOutAssetName is changed to a free
	 * name and null is returned.
	 */
	template <typename T>
	T* FindReusableAsset(const FString& PackagePath, FString& InOutAssetName, bool bAllowReuse)
	{
		UObject* Existing = FindAsset(PackagePath, InOutAssetName);
		if (!Existing)
		{
			return nullptr;
		}
		if (T* Typed = Cast<T>(Existing); Typed && bAllowReuse)
		{
			return Typed;
		}
		InOutAssetName = MakeUniqueAssetName(PackagePath, InOutAssetName);
		return nullptr;
	}
}
