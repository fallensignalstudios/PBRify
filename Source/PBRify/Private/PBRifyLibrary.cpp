#include "PBRifyLibrary.h"

#include "PBRifyModule.h"
#include "PBRifySettings.h"

FPBRifyResult UPBRifyLibrary::PBRifyTexture(UTexture2D* SourceTexture, bool bUpdateExisting)
{
	FPBRifyResult Result;
	FText Error;
	if (!PBRify::GeneratePBRAssets(SourceTexture, *GetDefault<UPBRifySettings>(), Result, Error, bUpdateExisting))
	{
		UE_LOG(LogPBRify, Error, TEXT("Couldn't PBRify %s: %s"), *GetNameSafe(SourceTexture), *Error.ToString());
		return FPBRifyResult();
	}
	return Result;
}
