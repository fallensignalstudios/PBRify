#include "PBRifyMaterialBuilder.h"

#include "PBRifyAssetUtils.h"
#include "PBRifyGenerator.h"

#include "AssetToolsModule.h"
#include "Engine/Texture2D.h"
#include "Factories/MaterialFactoryNew.h"
#include "Factories/MaterialInstanceConstantFactoryNew.h"
#include "IAssetTools.h"
#include "MaterialEditingLibrary.h"
#include "Materials/Material.h"
#include "Materials/MaterialExpressionCameraVectorWS.h"
#include "Materials/MaterialExpressionCustom.h"
#include "Materials/MaterialExpressionDDX.h"
#include "Materials/MaterialExpressionDDY.h"
#include "Materials/MaterialExpressionLinearInterpolate.h"
#include "Materials/MaterialExpressionMultiply.h"
#include "Materials/MaterialExpressionScalarParameter.h"
#include "Materials/MaterialExpressionTextureCoordinate.h"
#include "Materials/MaterialExpressionTextureObjectParameter.h"
#include "Materials/MaterialExpressionTextureSampleParameter2D.h"
#include "Materials/MaterialExpressionTransform.h"
#include "Materials/MaterialInstanceConstant.h"
#include "Misc/PackageName.h"

#define LOCTEXT_NAMESPACE "PBRify"

namespace PBRifyMaterialParams
{
	static const FName BaseColor(TEXT("BaseColor"));
	static const FName Normal(TEXT("Normal"));
	static const FName Height(TEXT("Height"));
	static const FName ORM(TEXT("ORM"));
	static const FName Tiling(TEXT("Tiling"));
	static const FName ParallaxDepth(TEXT("ParallaxDepth"));
	static const FName ParallaxSteps(TEXT("ParallaxSteps"));
	static const FName ReferencePlane(TEXT("ReferencePlane"));
	static const FName NormalStrength(TEXT("NormalStrength"));
	static const FName AOStrength(TEXT("AOStrength"));
	static const FName RoughnessScale(TEXT("RoughnessScale"));
}

namespace PBRifyMaterialBuilderPrivate
{
	// Parallax occlusion mapping: march the view ray down through the height field and return the UV
	// where it hits. Height 1 = top surface, 0 = deepest. RefPlane is the height that stays put on
	// the mesh surface: higher areas appear to stick out of it, lower areas sink in.
	// Gradients are passed in (instead of using ddx/ddy here) so this also compiles for Nanite.
	static const TCHAR* const ParallaxHLSL = TEXT(R"HLSL(
float3 V = normalize(ViewTS);
float NumSteps = clamp(round(Steps), 4.0, 64.0);
float StepSize = 1.0 / NumSteps;
float2 RayUV = -V.xy / max(V.z, 0.15) * Depth;
float2 StepUV = RayUV * StepSize;
float2 CurUV = UV - RayUV * (1.0 - RefPlane);
float RayHeight = 1.0;
float SurfHeight = HeightTex.SampleGrad(HeightTexSampler, CurUV, UVddx, UVddy).r;
float2 PrevUV = CurUV;
float PrevGap = RayHeight - SurfHeight;
[loop]
for (int i = 0; i < 64; i++)
{
	if (i >= NumSteps || RayHeight <= SurfHeight)
	{
		break;
	}
	PrevUV = CurUV;
	PrevGap = RayHeight - SurfHeight;
	CurUV += StepUV;
	RayHeight -= StepSize;
	SurfHeight = HeightTex.SampleGrad(HeightTexSampler, CurUV, UVddx, UVddy).r;
}
float CurGap = RayHeight - SurfHeight;
return lerp(PrevUV, CurUV, saturate(PrevGap / max(PrevGap - CurGap, 1e-4)));
)HLSL");

	static const TCHAR* const NormalStrengthHLSL = TEXT("return normalize(float3(N.xy * Strength, max(N.z, 1e-4)));");

	template <typename TExpression>
	TExpression* AddExpression(UMaterial* Material, int32 X, int32 Y)
	{
		return CastChecked<TExpression>(UMaterialEditingLibrary::CreateMaterialExpression(Material, TExpression::StaticClass(), X, Y));
	}

	UMaterialExpressionScalarParameter* AddScalarParameter(UMaterial* Material, FName Name, FName Group, float DefaultValue, float SliderMax, int32 X, int32 Y)
	{
		UMaterialExpressionScalarParameter* Parameter = AddExpression<UMaterialExpressionScalarParameter>(Material, X, Y);
		Parameter->ParameterName = Name;
		Parameter->Group = Group;
		Parameter->DefaultValue = DefaultValue;
		Parameter->SliderMin = 0.0f;
		Parameter->SliderMax = SliderMax;
		return Parameter;
	}

	template <typename TParameter>
	TParameter* AddTextureParameter(UMaterial* Material, FName Name, UTexture* DefaultTexture, int32 X, int32 Y)
	{
		TParameter* Parameter = AddExpression<TParameter>(Material, X, Y);
		Parameter->ParameterName = Name;
		Parameter->Group = TEXT("Textures");
		Parameter->Texture = DefaultTexture;
		Parameter->SamplerType = UMaterialExpressionTextureBase::GetSamplerTypeForTexture(DefaultTexture);
		return Parameter;
	}

	UMaterialExpressionCustom* AddCustom(UMaterial* Material, const TCHAR* Description, const TCHAR* Code, ECustomMaterialOutputType OutputType, int32 X, int32 Y)
	{
		UMaterialExpressionCustom* Custom = AddExpression<UMaterialExpressionCustom>(Material, X, Y);
		Custom->Description = Description;
		Custom->Code = Code;
		Custom->OutputType = OutputType;
		Custom->Inputs.Reset();
		return Custom;
	}

	void AddCustomInput(UMaterialExpressionCustom* Custom, FName Name, UMaterialExpression* From, int32 OutputIndex = 0)
	{
		FCustomInput& Input = Custom->Inputs.AddDefaulted_GetRef();
		Input.InputName = Name;
		Input.Input.Connect(OutputIndex, From);
	}

	/** Parallax UVs jump at height edges, which wrecks automatic mip selection. Take mips from the smooth, un-shifted UVs instead. */
	void UseExplicitGradients(UMaterialExpressionTextureSample* Sample, UMaterialExpression* UVddx, UMaterialExpression* UVddy)
	{
		Sample->MipValueMode = TMVM_Derivative;
		Sample->CoordinatesDX.Connect(0, UVddx);
		Sample->CoordinatesDY.Connect(0, UVddy);
	}

	void ConnectProperty(UMaterial* Material, EMaterialProperty Property, UMaterialExpression* From, int32 OutputIndex = 0)
	{
		if (FExpressionInput* Input = Material->GetExpressionInputForProperty(Property))
		{
			Input->Connect(OutputIndex, From);
		}
	}

	// Texture sample output pins.
	constexpr int32 RGBOutput = 0;
	constexpr int32 ROutput = 1;
	constexpr int32 GOutput = 2;
	constexpr int32 BOutput = 3;

	void BuildMasterGraph(UMaterial* Material, const FPBRifyResult& Textures)
	{
		// UVs and their screen-space gradients
		UMaterialExpressionTextureCoordinate* TexCoord = AddExpression<UMaterialExpressionTextureCoordinate>(Material, -1800, 0);
		UMaterialExpressionScalarParameter* Tiling = AddScalarParameter(Material, PBRifyMaterialParams::Tiling, TEXT("UV"), 1.0f, 16.0f, -1800, 100);
		UMaterialExpressionMultiply* TiledUV = AddExpression<UMaterialExpressionMultiply>(Material, -1600, 40);
		TiledUV->A.Connect(0, TexCoord);
		TiledUV->B.Connect(0, Tiling);

		UMaterialExpressionDDX* UVddx = AddExpression<UMaterialExpressionDDX>(Material, -1400, 160);
		UVddx->Value.Connect(0, TiledUV);
		UMaterialExpressionDDY* UVddy = AddExpression<UMaterialExpressionDDY>(Material, -1400, 240);
		UVddy->Value.Connect(0, TiledUV);

		// Camera direction in tangent space
		UMaterialExpressionCameraVectorWS* CameraVector = AddExpression<UMaterialExpressionCameraVectorWS>(Material, -1800, 340);
		UMaterialExpressionTransform* ViewTS = AddExpression<UMaterialExpressionTransform>(Material, -1600, 340);
		ViewTS->Input.Connect(0, CameraVector);
		ViewTS->TransformSourceType = TRANSFORMSOURCE_World;
		ViewTS->TransformType = TRANSFORM_Tangent;

		// Parallax
		UMaterialExpressionTextureObjectParameter* HeightTexture = AddTextureParameter<UMaterialExpressionTextureObjectParameter>(
			Material, PBRifyMaterialParams::Height, Textures.Height, -1400, -200);
		UMaterialExpressionScalarParameter* Depth = AddScalarParameter(Material, PBRifyMaterialParams::ParallaxDepth, TEXT("Parallax"), 0.05f, 0.2f, -1400, 340);
		UMaterialExpressionScalarParameter* Steps = AddScalarParameter(Material, PBRifyMaterialParams::ParallaxSteps, TEXT("Parallax"), 24.0f, 64.0f, -1400, 420);
		UMaterialExpressionScalarParameter* RefPlane = AddScalarParameter(Material, PBRifyMaterialParams::ReferencePlane, TEXT("Parallax"), 0.5f, 1.0f, -1400, 500);

		UMaterialExpressionCustom* Parallax = AddCustom(Material, TEXT("PBRify Parallax Occlusion"), ParallaxHLSL, CMOT_Float2, -1100, 100);
		AddCustomInput(Parallax, TEXT("HeightTex"), HeightTexture);
		AddCustomInput(Parallax, TEXT("UV"), TiledUV);
		AddCustomInput(Parallax, TEXT("UVddx"), UVddx);
		AddCustomInput(Parallax, TEXT("UVddy"), UVddy);
		AddCustomInput(Parallax, TEXT("ViewTS"), ViewTS);
		AddCustomInput(Parallax, TEXT("Depth"), Depth);
		AddCustomInput(Parallax, TEXT("Steps"), Steps);
		AddCustomInput(Parallax, TEXT("RefPlane"), RefPlane);

		// Surface textures, all sampled at the parallax-shifted UVs
		UMaterialExpressionTextureSampleParameter2D* BaseColor = AddTextureParameter<UMaterialExpressionTextureSampleParameter2D>(
			Material, PBRifyMaterialParams::BaseColor, Textures.BaseColor, -700, -300);
		BaseColor->Coordinates.Connect(0, Parallax);
		UseExplicitGradients(BaseColor, UVddx, UVddy);

		UMaterialExpressionTextureSampleParameter2D* Normal = AddTextureParameter<UMaterialExpressionTextureSampleParameter2D>(
			Material, PBRifyMaterialParams::Normal, Textures.Normal, -700, 0);
		Normal->Coordinates.Connect(0, Parallax);
		UseExplicitGradients(Normal, UVddx, UVddy);

		UMaterialExpressionTextureSampleParameter2D* ORM = AddTextureParameter<UMaterialExpressionTextureSampleParameter2D>(
			Material, PBRifyMaterialParams::ORM, Textures.ORM, -700, 300);
		ORM->Coordinates.Connect(0, Parallax);
		UseExplicitGradients(ORM, UVddx, UVddy);

		// Live tweakables
		UMaterialExpressionScalarParameter* NormalStrength = AddScalarParameter(Material, PBRifyMaterialParams::NormalStrength, TEXT("Surface"), 1.0f, 4.0f, -450, 160);
		UMaterialExpressionCustom* ScaledNormal = AddCustom(Material, TEXT("PBRify Normal Strength"), NormalStrengthHLSL, CMOT_Float3, -250, 0);
		AddCustomInput(ScaledNormal, TEXT("N"), Normal, RGBOutput);
		AddCustomInput(ScaledNormal, TEXT("Strength"), NormalStrength);

		UMaterialExpressionScalarParameter* AOStrength = AddScalarParameter(Material, PBRifyMaterialParams::AOStrength, TEXT("Surface"), 1.0f, 1.0f, -450, 460);
		UMaterialExpressionLinearInterpolate* AO = AddExpression<UMaterialExpressionLinearInterpolate>(Material, -250, 300);
		AO->ConstA = 1.0f;
		AO->B.Connect(ROutput, ORM);
		AO->Alpha.Connect(0, AOStrength);

		UMaterialExpressionScalarParameter* RoughnessScale = AddScalarParameter(Material, PBRifyMaterialParams::RoughnessScale, TEXT("Surface"), 1.0f, 2.0f, -450, 560);
		UMaterialExpressionMultiply* Roughness = AddExpression<UMaterialExpressionMultiply>(Material, -250, 420);
		Roughness->A.Connect(GOutput, ORM);
		Roughness->B.Connect(0, RoughnessScale);

		ConnectProperty(Material, MP_BaseColor, BaseColor, RGBOutput);
		ConnectProperty(Material, MP_Normal, ScaledNormal);
		ConnectProperty(Material, MP_AmbientOcclusion, AO);
		ConnectProperty(Material, MP_Roughness, Roughness);
		ConnectProperty(Material, MP_Metallic, ORM, BOutput);
	}

	UTexture2D* GetTextureForParameter(FName ParameterName, const FPBRifyResult& Textures)
	{
		if (ParameterName == PBRifyMaterialParams::BaseColor) return Textures.BaseColor;
		if (ParameterName == PBRifyMaterialParams::Normal) return Textures.Normal;
		if (ParameterName == PBRifyMaterialParams::Height) return Textures.Height;
		if (ParameterName == PBRifyMaterialParams::ORM) return Textures.ORM;
		return nullptr;
	}

	/** If the textures the master was created with got deleted, it won't compile; point it at the new ones. */
	void RestoreMissingDefaults(UMaterial* Material, const FPBRifyResult& Textures)
	{
		bool bChanged = false;
		for (UMaterialExpression* Expression : Material->GetExpressions())
		{
			UMaterialExpressionTextureSampleParameter* Parameter = Cast<UMaterialExpressionTextureSampleParameter>(Expression);
			if (Parameter && !Parameter->Texture)
			{
				if (UTexture2D* Texture = GetTextureForParameter(Parameter->ParameterName, Textures))
				{
					Parameter->Modify();
					Parameter->Texture = Texture;
					bChanged = true;
				}
			}
		}
		if (bChanged)
		{
			UMaterialEditingLibrary::RecompileMaterial(Material);
			Material->MarkPackageDirty();
		}
	}
}

UMaterial* PBRifyMaterialBuilder::FindOrCreateMasterMaterial(const FString& MasterMaterialPath, const FPBRifyResult& DefaultTextures, FText& OutError)
{
	using namespace PBRifyMaterialBuilderPrivate;

	// Accept both "/Game/Foo/M_Bar" and "/Game/Foo/M_Bar.M_Bar".
	FString PackageName = MasterMaterialPath.TrimStartAndEnd();
	int32 DotIndex = INDEX_NONE;
	if (PackageName.FindChar(TEXT('.'), DotIndex))
	{
		PackageName = PackageName.Left(DotIndex);
	}
	if (!FPackageName::IsValidLongPackageName(PackageName))
	{
		OutError = FText::Format(LOCTEXT("BadMasterPath", "Master material path \"{0}\" isn't a valid content path (something like /Game/PBRify/M_PBRify_Master)."),
			FText::FromString(MasterMaterialPath));
		return nullptr;
	}

	const FString PackagePath = FPackageName::GetLongPackagePath(PackageName);
	const FString AssetName = FPackageName::GetShortName(PackageName);

	if (UObject* Existing = PBRifyAssetUtils::FindAsset(PackagePath, AssetName))
	{
		UMaterial* Material = Cast<UMaterial>(Existing);
		if (!Material)
		{
			OutError = FText::Format(LOCTEXT("MasterNotMaterial", "\"{0}\" exists but isn't a material. Change the Master Material Path setting."),
				FText::FromString(PackageName));
			return nullptr;
		}
		RestoreMissingDefaults(Material, DefaultTextures);
		return Material;
	}

	UMaterialFactoryNew* Factory = NewObject<UMaterialFactoryNew>();
	UMaterial* Material = Cast<UMaterial>(FAssetToolsModule::GetModule().Get().CreateAsset(AssetName, PackagePath, UMaterial::StaticClass(), Factory));
	if (!Material)
	{
		OutError = FText::Format(LOCTEXT("MasterCreateFailed", "Couldn't create the master material at {0}."), FText::FromString(PackageName));
		return nullptr;
	}

	// With Substrate enabled, new materials start with a Slab node wired into Front Material, which
	// would override the classic inputs we connect below. Start from an empty graph instead.
	UMaterialEditingLibrary::DeleteAllMaterialExpressions(Material);
	if (FExpressionInput* FrontMaterial = Material->GetExpressionInputForProperty(MP_FrontMaterial))
	{
		FrontMaterial->Expression = nullptr;
	}

	BuildMasterGraph(Material, DefaultTextures);
	UMaterialEditingLibrary::RecompileMaterial(Material);
	Material->MarkPackageDirty();
	return Material;
}

UMaterialInstanceConstant* PBRifyMaterialBuilder::CreateOrUpdateInstance(
	UMaterial* Master, const FString& PackagePath, const FString& DesiredName,
	const FPBRifyResult& Textures, float ParallaxDepth, bool bUpdateExisting, FText& OutError)
{
	FString AssetName = DesiredName;
	UMaterialInstanceConstant* Instance = PBRifyAssetUtils::FindReusableAsset<UMaterialInstanceConstant>(PackagePath, AssetName, bUpdateExisting);
	if (Instance)
	{
		Instance->Modify();
		Instance->SetParentEditorOnly(Master);
	}
	else
	{
		UMaterialInstanceConstantFactoryNew* Factory = NewObject<UMaterialInstanceConstantFactoryNew>();
		Factory->InitialParent = Master;
		Instance = Cast<UMaterialInstanceConstant>(
			FAssetToolsModule::GetModule().Get().CreateAsset(AssetName, PackagePath, UMaterialInstanceConstant::StaticClass(), Factory));
		if (!Instance)
		{
			OutError = FText::Format(LOCTEXT("InstanceCreateFailed", "Couldn't create material instance {0}."), FText::FromString(AssetName));
			return nullptr;
		}
	}

	Instance->SetTextureParameterValueEditorOnly(FMaterialParameterInfo(PBRifyMaterialParams::BaseColor), Textures.BaseColor);
	Instance->SetTextureParameterValueEditorOnly(FMaterialParameterInfo(PBRifyMaterialParams::Normal), Textures.Normal);
	Instance->SetTextureParameterValueEditorOnly(FMaterialParameterInfo(PBRifyMaterialParams::Height), Textures.Height);
	Instance->SetTextureParameterValueEditorOnly(FMaterialParameterInfo(PBRifyMaterialParams::ORM), Textures.ORM);
	Instance->SetScalarParameterValueEditorOnly(FMaterialParameterInfo(PBRifyMaterialParams::ParallaxDepth), ParallaxDepth);
	Instance->PostEditChange();
	Instance->MarkPackageDirty();
	return Instance;
}

#undef LOCTEXT_NAMESPACE
