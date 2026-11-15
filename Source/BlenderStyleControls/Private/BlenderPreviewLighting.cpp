#include "BlenderPreviewLighting.h"

#include "Components/PrimitiveComponent.h"
#include "Editor.h"
#include "EditorViewportClient.h"
#include "Engine/Texture2D.h"
#include "Engine/TextureCube.h"
#include "Engine/World.h"
#include "FinalPostProcessSettings.h"
#include "HAL/IConsoleManager.h"
#include "LevelEditorViewport.h"
#include "MaterialDomain.h"
#include "MaterialShared.h"
#include "Materials/Material.h"
#include "Materials/MaterialExpressionCameraVectorWS.h"
#include "Materials/MaterialExpressionCustom.h"
#include "Materials/MaterialExpressionScalarParameter.h"
#include "Materials/MaterialExpressionSceneTexture.h"
#include "Materials/MaterialExpressionTextureObjectParameter.h"
#include "Materials/MaterialExpressionTransform.h"
#include "Materials/MaterialExpressionVectorParameter.h"
#include "Materials/MaterialExpressionWorldPosition.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "RenderingThread.h"
#include "RenderUtils.h"
#include "SceneView.h"
#include "SceneViewExtension.h"
#include "SceneViewExtensionContext.h"
#include "TextureCompiler.h"
#include "UObject/StrongObjectPtr.h"
#include "UObject/UObjectIterator.h"

namespace BlenderPreviewLightingPrivate
{
	struct FSharedState
	{
		bool bEnabled = false;
		bool bMaterialReady = false;
		bool bBackgroundMaterialReady = false;
		bool bSelectedWorldFound = false;
		bool bSelectedMatCapFound = false;
		FBlenderPreviewLightingSettings Settings;
		TArray<FEditorViewportClient*> Clients;
		TStrongObjectPtr<UMaterial> Material;
		TStrongObjectPtr<UMaterialInstanceDynamic> MID;
		TStrongObjectPtr<UMaterial> BackgroundMaterial;
		TStrongObjectPtr<UMaterialInstanceDynamic> BackgroundMID;
		TStrongObjectPtr<UTextureCube> Cube;
		TStrongObjectPtr<UTexture2D> MatCap;
		FString LoadedWorld;
		FString LoadedMatCap;
		FString Error;
		FString LastDiagnosticKey;
		int32 SetupViewCount = 0;
		int32 InjectedViewCount = 0;
		int32 LastMaterialNodeCount = 0;
		float LastInjectedSolid = -1.0f;
		bool bLastViewHadState = false;
		TWeakObjectPtr<UWorld> SkyWorld;
		TArray<FPrimitiveComponentId> SkyPrimitives;
		double LastSkyScan = -100.0;

		bool IsLive(const FEditorViewportClient* Candidate) const
		{
			if (!Candidate || !GEditor || GEditor->PlayWorld || GEditor->bIsSimulatingInEditor || !Clients.Contains(Candidate)) return false;
			// Compare first, dereference only a currently registered level viewport.
			for (FLevelEditorViewportClient* Live : GEditor->GetLevelViewportClients())
			{
				if (Live == Candidate)
				{
					UWorld* World = Live->GetWorld();
					return World && World->WorldType == EWorldType::Editor && !World->IsGameWorld();
				}
			}
			return false;
		}

		bool IsTarget(const FSceneViewFamily& Family) const
		{
			if (!bEnabled || (Settings.Shading != 1 && Settings.Shading != 2)) return false;
			for (FEditorViewportClient* Client : Clients)
			{
				if (IsLive(Client) && Client->Viewport == Family.RenderTarget && Client->GetScene() == Family.Scene) return true;
			}
			return false;
		}

		void HideSkyPrimitives(FSceneView& View, UWorld* World)
		{
			const double Now = FPlatformTime::Seconds();
			if (SkyWorld.Get() != World || Now - LastSkyScan > 1.0)
			{
				SkyWorld = World;
				LastSkyScan = Now;
				SkyPrimitives.Reset();
				for (TObjectIterator<UPrimitiveComponent> It; It; ++It)
				{
					UPrimitiveComponent* Component = *It;
					if (!IsValid(Component) || Component->GetWorld() != World || !Component->IsRegistered()) continue;
					for (int32 Slot = 0; Slot < Component->GetNumMaterials(); ++Slot)
					{
						UMaterialInterface* Interface = Component->GetMaterial(Slot);
						const UMaterial* Base = Interface ? Interface->GetMaterial() : nullptr;
						if (Base && Base->bIsSky)
						{
							SkyPrimitives.Add(Component->GetPrimitiveSceneId());
							break;
						}
					}
				}
			}
			for (FPrimitiveComponentId Id : SkyPrimitives) View.HiddenPrimitives.Add(Id);
		}
	};

	static FString SafeStem(FString Stem)
	{
		Stem.ReplaceInline(TEXT("+"), TEXT("_plus_"));
		for (TCHAR& Character : Stem)
		{
			if (!FChar::IsAlnum(Character) && Character != TEXT('_')) Character = TEXT('_');
		}
		return Stem;
	}

	static void LoadTextures(FSharedState& State)
	{
		TArray<UTexture*> TexturesToFinish;
		if (State.LoadedWorld != State.Settings.WorldStem || !State.Cube.IsValid())
		{
			State.LoadedWorld = State.Settings.WorldStem;
			const FString Name = TEXT("T_World_") + SafeStem(State.LoadedWorld);
			const FString Path = TEXT("/BlenderStyleControls/PreviewV14/World/") + Name + TEXT(".") + Name;
			UTextureCube* Cube = LoadObject<UTextureCube>(nullptr, *Path, nullptr, LOAD_NoWarn);
			State.bSelectedWorldFound = Cube != nullptr;
			if (!Cube) Cube = LoadObject<UTextureCube>(nullptr, TEXT("/Engine/MapTemplates/Sky/SunsetAmbientCubemap.SunsetAmbientCubemap"));
			State.Cube.Reset(Cube);
			if (Cube) TexturesToFinish.Add(Cube);
		}
		if (State.LoadedMatCap != State.Settings.MatCapStem || !State.MatCap.IsValid())
		{
			State.LoadedMatCap = State.Settings.MatCapStem;
			const FString Name = TEXT("T_MatCap_") + SafeStem(State.LoadedMatCap);
			const FString Path = TEXT("/BlenderStyleControls/PreviewV14/MatCap/") + Name + TEXT(".") + Name;
			UTexture2D* Texture = LoadObject<UTexture2D>(nullptr, *Path, nullptr, LOAD_NoWarn);
			State.bSelectedMatCapFound = Texture != nullptr;
			if (!Texture) Texture = LoadObject<UTexture2D>(nullptr, TEXT("/Engine/EngineResources/WhiteSquareTexture.WhiteSquareTexture"));
			State.MatCap.Reset(Texture);
			if (Texture) TexturesToFinish.Add(Texture);
		}
		// A loaded asset may still render its default texture while its first build runs.
		// Wait only for newly selected preview textures, never all project textures.
		if (!TexturesToFinish.IsEmpty()) FTextureCompilingManager::Get().FinishCompilation(TexturesToFinish);
	}

	template<class T> static T* Expression(UMaterial* Material)
	{
		T* Result = NewObject<T>(Material, NAME_None, RF_Transient);
		Material->GetExpressionCollection().AddExpression(Result);
		return Result;
	}

	static void AddInput(UMaterialExpressionCustom* Custom, const TCHAR* Name, UMaterialExpression* Value, int32 Output = 0)
	{
		FCustomInput Input;
		Input.InputName = FName(Name);
		Input.Input.Connect(Output, Value);
		Custom->Inputs.Add(Input);
	}

	static void BuildMaterial(FSharedState& State)
	{
		if (State.Material.IsValid() || !State.Cube.IsValid() || !State.MatCap.IsValid()) return;
		UMaterial* Material = NewObject<UMaterial>(GetTransientPackage(), NAME_None, RF_Transient);
		State.Material.Reset(Material);
		Material->MaterialDomain = MD_PostProcess;
		Material->BlendableLocation = BL_SceneColorAfterTonemapping;
		Material->BlendablePriority = 1000;
		Material->bIsBlendable = false;
		auto* Custom = Expression<UMaterialExpressionCustom>(Material);
		Custom->OutputType = CMOT_Float3;
		Custom->Description = TEXT("Blender editor-only solid and HDRI background");
		Custom->Inputs.Reset();

		auto* Scene = Expression<UMaterialExpressionSceneTexture>(Material);
		Scene->SceneTextureId = PPI_PostProcessInput0;
		AddInput(Custom, TEXT("Scene"), Scene);
		auto* Depth = Expression<UMaterialExpressionSceneTexture>(Material);
		Depth->SceneTextureId = PPI_SceneDepth;
		AddInput(Custom, TEXT("Depth"), Depth);
		auto* Normal = Expression<UMaterialExpressionSceneTexture>(Material);
		Normal->SceneTextureId = PPI_WorldNormal;
		AddInput(Custom, TEXT("WorldNormal"), Normal);
		auto* ViewNormal = Expression<UMaterialExpressionTransform>(Material);
		ViewNormal->TransformSourceType = TRANSFORMSOURCE_World;
		ViewNormal->TransformType = TRANSFORM_View;
		ViewNormal->Input.Connect(0, Normal);
		AddInput(Custom, TEXT("ViewNormal"), ViewNormal);
		auto* Camera = Expression<UMaterialExpressionCameraVectorWS>(Material);
		AddInput(Custom, TEXT("CameraVector"), Camera);
		auto* Position = Expression<UMaterialExpressionWorldPosition>(Material);
		Position->WorldPositionShaderOffset = WPT_CameraRelative;
		AddInput(Custom, TEXT("WorldPosition"), Position);
		auto* ShadingModel = Expression<UMaterialExpressionSceneTexture>(Material);
		ShadingModel->SceneTextureId = PPI_ShadingModelID;
		AddInput(Custom, TEXT("ShadingModel"), ShadingModel);

		auto AddScalar = [&](const TCHAR* Name, float Default)
		{
			auto* Node = Expression<UMaterialExpressionScalarParameter>(Material);
			Node->ParameterName = FName(Name);
			Node->DefaultValue = Default;
			AddInput(Custom, Name, Node);
		};
		for (const TCHAR* Name : {TEXT("Solid"), TEXT("SolidMode"), TEXT("StudioIndex"), TEXT("StudioRotation"), TEXT("BackgroundRotation"), TEXT("BackgroundBlur"), TEXT("SceneWorld"), TEXT("Cavity"), TEXT("Outline"), TEXT("MatCapFlip")}) AddScalar(Name, 0.0f);
		AddScalar(TEXT("StudioIntensity"), 1.0f);
		AddScalar(TEXT("BackgroundOpacity"), 1.0f);
		auto AddColor = [&](const TCHAR* Name, const FLinearColor& Default)
		{
			auto* Node = Expression<UMaterialExpressionVectorParameter>(Material);
			Node->ParameterName = FName(Name);
			Node->DefaultValue = Default;
			AddInput(Custom, Name, Node);
		};
		AddColor(TEXT("SolidColor"), State.Settings.SolidColor);
		AddColor(TEXT("BackgroundColor"), State.Settings.BackgroundColor);
		auto* Cube = Expression<UMaterialExpressionTextureObjectParameter>(Material);
		Cube->ParameterName = TEXT("EnvironmentTexture");
		Cube->Texture = State.Cube.Get();
		Cube->SamplerType = SAMPLERTYPE_LinearColor;
		AddInput(Custom, TEXT("EnvironmentTexture"), Cube);
		auto* Cap = Expression<UMaterialExpressionTextureObjectParameter>(Material);
		Cap->ParameterName = TEXT("MatCapTexture");
		Cap->Texture = State.MatCap.Get();
		Cap->SamplerType = State.MatCap->SRGB ? SAMPLERTYPE_Color : SAMPLERTYPE_LinearColor;
		AddInput(Custom, TEXT("MatCapTexture"), Cap);

		Custom->Code = TEXT(R"HLSL(
float3 result = Scene.rgb;
float depth = Depth.r;
float normalLength = dot(WorldNormal.xyz, WorldNormal.xyz);
bool geometry = depth > 0.0 && depth < 9999999.0;
if (!geometry)
{
    if (SceneWorld > 0.5) return result;
    float3 background = BackgroundColor.rgb;
    if (Solid < 0.5)
    {
        float3 direction = normalize(-CameraVector.xyz);
        float s = sin(BackgroundRotation), c = cos(BackgroundRotation);
        direction.xy = float2(c * direction.x - s * direction.y, s * direction.x + c * direction.y);
        float3 environment = TextureCubeSampleLevel(EnvironmentTexture, EnvironmentTextureSampler, direction, BackgroundBlur * 7.0).rgb;
        environment = environment / (1.0 + environment);
        background = lerp(background, environment, saturate(BackgroundOpacity));
    }
    // After-tonemapping output is already display encoded. Encode only new colors;
    // the original Scene branch must pass through without a second conversion.
    background = max(background, 0.0);
    return saturate(lerp(background * 12.92, 1.055 * pow(background, 1.0 / 2.4) - 0.055, step(0.0031308, background)));
}
if (Solid < 0.5) return result;
float3 n = normalize(ViewNormal.xyz);
if (normalLength < 0.05 || ShadingModel.r < 0.5)
{
    // Unlit source materials omit GBuffer normals; retain their geometry in solid view.
    float3 geometricNormal = normalize(cross(ddy(WorldPosition.xyz), ddx(WorldPosition.xyz)) + 0.0000001);
    if (dot(geometricNormal, CameraVector.xyz) < 0.0) geometricNormal = -geometricNormal;
    n = normalize(mul(geometricNormal, (float3x3)ResolvedView.TranslatedWorldToView));
}
float s = sin(StudioRotation), c = cos(StudioRotation);
float3 studioNormal = n;
studioNormal.xy = float2(c * n.x - s * n.y, s * n.x + c * n.y);
// View-space Z points away from the camera. Four broad, camera-relative lamps.
float key = saturate(dot(studioNormal, normalize(float3(-0.45, 0.65, -0.75))));
float fill = saturate(dot(studioNormal, normalize(float3(0.75, 0.20, -0.45))));
float lower = saturate(dot(studioNormal, normalize(float3(-0.15, -0.70, -0.30))));
float rim = pow(1.0 - saturate(abs(n.z)), 2.5);
float lighting = 0.38 + 0.47 * key + 0.19 * fill + 0.07 * lower;
if (StudioIndex > 0.5 && StudioIndex < 1.5) lighting = 0.58 + 0.28 * key + 0.13 * fill;
if (StudioIndex > 1.5) lighting = 0.30 + 0.42 * key + 0.11 * fill + 0.42 * rim;
result = SolidColor.rgb * lighting * StudioIntensity;
if (SolidMode > 0.5 && SolidMode < 1.5)
{
    // Orient the capture to the local viewing ray, including off-center perspective.
    // This basis is independent of the studio rig rotation.
    float3 viewDirection = normalize(mul(CameraVector.xyz, (float3x3)ResolvedView.TranslatedWorldToView));
    float3 captureRight = normalize(cross(viewDirection, float3(0.0, 1.0, 0.0)) + 0.0000001);
    float3 captureUp = normalize(cross(captureRight, viewDirection));
    float2 matcapUV = float2(dot(n, captureRight), -dot(n, captureUp)) * 0.495 + 0.5;
    if (MatCapFlip > 0.5) matcapUV.x = 1.0 - matcapUV.x;
    result = Texture2DSample(MatCapTexture, MatCapTextureSampler, saturate(matcapUV)).rgb * StudioIntensity;
}
if (SolidMode > 1.5) result = SolidColor.rgb * StudioIntensity;
if (Cavity > 0.5 || Outline > 0.5)
{
    float2 uv = GetDefaultSceneTextureUV(Parameters, 1);
    float2 stepUV = View.BufferSizeAndInvSize.zw;
    float2 directions[4] = { float2(1,0), float2(-1,0), float2(0,1), float2(0,-1) };
    float depthEdge = 0.0, normalEdge = 0.0;
    bool validGBufferNormal = normalLength >= 0.05 && ShadingModel.r >= 0.5;
    float3 wn = normalize(WorldNormal.xyz + 0.000001);
    [unroll] for (int i = 0; i < 4; ++i)
    {
        float2 sampleUV = saturate(uv + directions[i] * stepUV * 1.25);
        float nearbyDepth = SceneTextureLookup(sampleUV, 1, false).r;
        float3 nearbyNormal = SceneTextureLookup(sampleUV, 8, false).xyz;
        depthEdge = max(depthEdge, abs(nearbyDepth - depth) / max(depth, 1.0));
        if (validGBufferNormal && dot(nearbyNormal, nearbyNormal) >= 0.05)
            normalEdge = max(normalEdge, 1.0 - saturate(dot(wn, normalize(nearbyNormal))));
    }
    if (Cavity > 0.5) result *= 1.0 - 0.30 * saturate(normalEdge * 2.5);
    if (Outline > 0.5) result = lerp(result, float3(0.055, 0.065, 0.075), saturate(max(depthEdge * 30.0, normalEdge * 1.25)));
}
result = max(result, 0.0);
return saturate(lerp(result * 12.92, 1.055 * pow(result, 1.0 / 2.4) - 0.055, step(0.0031308, result)));
)HLSL");
		Material->GetEditorOnlyData()->EmissiveColor.Connect(0, Custom);
		Material->PostEditChange();
		// PostEditChange uses PrecompileMode::None in UE 5.4. A non-null shader map
		// does not guarantee the postprocess VS/PS exist; precompile before first use.
		Material->ForceRecompileForRendering(EMaterialShaderPrecompileMode::Synchronous);
		if (FMaterialResource* Resource = Material->GetMaterialResource(GMaxRHIFeatureLevel))
		{
			Resource->FinishCompilation();
			State.bMaterialReady = Resource->IsCompilationFinished() && Resource->GetCompileErrors().IsEmpty()
				&& Resource->IsGameThreadShaderMapComplete() && Material->IsComplete();
			State.Error = FString::Join(Resource->GetCompileErrors(), TEXT("; "));
		}
		State.MID.Reset(UMaterialInstanceDynamic::Create(Material, GetTransientPackage()));
	}

	static void BuildBackgroundMaterial(FSharedState& State)
	{
		if (State.BackgroundMaterial.IsValid() || !State.Cube.IsValid()) return;
		UMaterial* Material = NewObject<UMaterial>(GetTransientPackage(), NAME_None, RF_Transient);
		State.BackgroundMaterial.Reset(Material);
		Material->MaterialDomain = MD_PostProcess;
		// UE 5.4 default translucent materials use AfterDOF. Replace the empty opaque
		// background before that separate translucency is composed, never after it.
		Material->BlendableLocation = BL_SceneColorBeforeDOF;
		Material->BlendablePriority = 1000;
		Material->bIsBlendable = false;
		auto* Custom = Expression<UMaterialExpressionCustom>(Material);
		Custom->OutputType = CMOT_Float3;
		Custom->Description = TEXT("Blender HDRI background before separate translucency");
		Custom->Inputs.Reset();
		auto* Scene = Expression<UMaterialExpressionSceneTexture>(Material);
		Scene->SceneTextureId = PPI_PostProcessInput0;
		AddInput(Custom, TEXT("Scene"), Scene);
		auto* Depth = Expression<UMaterialExpressionSceneTexture>(Material);
		Depth->SceneTextureId = PPI_SceneDepth;
		AddInput(Custom, TEXT("Depth"), Depth);
		auto* Camera = Expression<UMaterialExpressionCameraVectorWS>(Material);
		AddInput(Custom, TEXT("CameraVector"), Camera);
		for (const TCHAR* Name : {TEXT("BackgroundRotation"), TEXT("BackgroundBlur"), TEXT("BackgroundOpacity")})
		{
			auto* Node = Expression<UMaterialExpressionScalarParameter>(Material);
			Node->ParameterName = FName(Name);
			Node->DefaultValue = 0.0f;
			AddInput(Custom, Name, Node);
		}
		auto* Color = Expression<UMaterialExpressionVectorParameter>(Material);
		Color->ParameterName = TEXT("BackgroundColor");
		Color->DefaultValue = State.Settings.BackgroundColor;
		AddInput(Custom, TEXT("BackgroundColor"), Color);
		auto* Cube = Expression<UMaterialExpressionTextureObjectParameter>(Material);
		Cube->ParameterName = TEXT("EnvironmentTexture");
		Cube->Texture = State.Cube.Get();
		Cube->SamplerType = SAMPLERTYPE_LinearColor;
		AddInput(Custom, TEXT("EnvironmentTexture"), Cube);
		Custom->Code = TEXT(R"HLSL(
if (Depth.r > 0.0 && Depth.r < 9999999.0) return Scene.rgb;
float3 direction = normalize(-CameraVector.xyz);
float s = sin(BackgroundRotation), c = cos(BackgroundRotation);
direction.xy = float2(c * direction.x - s * direction.y, s * direction.x + c * direction.y);
float3 environment = TextureCubeSampleLevel(EnvironmentTexture, EnvironmentTextureSampler, direction, BackgroundBlur * 7.0).rgb;
// Scene input is automatically un-pre-exposed by the material translator at
// BeforeDOF; the postprocess pass applies PreExposure once to this linear output.
// Tonemapping and display encoding run later, after translucency composition.
return max(lerp(BackgroundColor.rgb, environment, saturate(BackgroundOpacity)), 0.0);
)HLSL");
		Material->GetEditorOnlyData()->EmissiveColor.Connect(0, Custom);
		Material->PostEditChange();
		Material->ForceRecompileForRendering(EMaterialShaderPrecompileMode::Synchronous);
		if (FMaterialResource* Resource = Material->GetMaterialResource(GMaxRHIFeatureLevel))
		{
			Resource->FinishCompilation();
			State.bBackgroundMaterialReady = Resource->IsCompilationFinished() && Resource->GetCompileErrors().IsEmpty()
				&& Resource->IsGameThreadShaderMapComplete() && Material->IsComplete();
			if (!Resource->GetCompileErrors().IsEmpty()) State.Error += TEXT(" Background: ") + FString::Join(Resource->GetCompileErrors(), TEXT("; "));
		}
		State.BackgroundMID.Reset(UMaterialInstanceDynamic::Create(Material, GetTransientPackage()));
	}

	static void UpdateParameters(FSharedState& State)
	{
		UMaterialInstanceDynamic* MID = State.MID.Get();
		if (!MID) return;
		const auto& S = State.Settings;
		MID->SetScalarParameterValue(TEXT("Solid"), S.Shading == 1 && !S.bXRay ? 1.0f : 0.0f);
		MID->SetScalarParameterValue(TEXT("SolidMode"), S.SolidMode == 1 && !State.bSelectedMatCapFound ? 0.0f : float(S.SolidMode));
		MID->SetScalarParameterValue(TEXT("StudioIndex"), float(S.StudioIndex));
		MID->SetScalarParameterValue(TEXT("StudioIntensity"), FMath::Max(0.0f, S.StudioIntensity));
		MID->SetScalarParameterValue(TEXT("StudioRotation"), FMath::DegreesToRadians(S.StudioRotationDegrees));
		MID->SetScalarParameterValue(TEXT("BackgroundRotation"), FMath::DegreesToRadians(S.BackgroundRotationDegrees));
		MID->SetScalarParameterValue(TEXT("BackgroundBlur"), FMath::Clamp(S.BackgroundBlur, 0.0f, 1.0f));
		MID->SetScalarParameterValue(TEXT("BackgroundOpacity"), S.Shading == 1 ? 0.0f : FMath::Clamp(S.BackgroundOpacity, 0.0f, 1.0f));
		MID->SetScalarParameterValue(TEXT("SceneWorld"), S.Shading == 2 && S.bUseSceneWorld ? 1.0f : 0.0f);
		MID->SetScalarParameterValue(TEXT("Cavity"), S.bCavity ? 1.0f : 0.0f);
		MID->SetScalarParameterValue(TEXT("Outline"), S.bOutline ? 1.0f : 0.0f);
		MID->SetScalarParameterValue(TEXT("MatCapFlip"), S.bMatCapFlip ? 1.0f : 0.0f);
		MID->SetVectorParameterValue(TEXT("SolidColor"), S.SolidColor);
		MID->SetVectorParameterValue(TEXT("BackgroundColor"), S.BackgroundColor);
		MID->SetTextureParameterValue(TEXT("EnvironmentTexture"), State.Cube.Get());
		MID->SetTextureParameterValue(TEXT("MatCapTexture"), State.MatCap.Get());
		if (UMaterialInstanceDynamic* Background = State.BackgroundMID.Get())
		{
			Background->SetScalarParameterValue(TEXT("BackgroundRotation"), FMath::DegreesToRadians(S.BackgroundRotationDegrees));
			Background->SetScalarParameterValue(TEXT("BackgroundBlur"), FMath::Clamp(S.BackgroundBlur, 0.0f, 1.0f));
			Background->SetScalarParameterValue(TEXT("BackgroundOpacity"), FMath::Clamp(S.BackgroundOpacity, 0.0f, 1.0f));
			Background->SetVectorParameterValue(TEXT("BackgroundColor"), S.BackgroundColor);
			Background->SetTextureParameterValue(TEXT("EnvironmentTexture"), State.Cube.Get());
		}
	}

	class FPreviewViewExtension final : public FSceneViewExtensionBase
	{
	public:
		FPreviewViewExtension(const FAutoRegister& AutoRegister, const TSharedRef<FSharedState>& InState)
			: FSceneViewExtensionBase(AutoRegister), State(InState) {}

		virtual bool IsActiveThisFrame_Internal(const FSceneViewExtensionContext& Context) const override
		{
			if (!State->bEnabled || (State->Settings.Shading != 1 && State->Settings.Shading != 2) || !Context.Viewport) return false;
			for (FEditorViewportClient* Client : State->Clients)
				if (State->IsLive(Client) && Client->Viewport == Context.Viewport) return true;
			return false;
		}

		virtual void SetupViewFamily(FSceneViewFamily& Family) override
		{
			if (!State->IsTarget(Family)) return;
			const auto& S = State->Settings;
			auto& Flags = Family.EngineShowFlags;
			Flags.SetMaterials(true);
			Flags.SetLighting(true);
			Flags.SetPostProcessing(true);
			Flags.SetPostProcessMaterial(true);
			Flags.SetAmbientCubemap(true);
			Flags.SetEyeAdaptation(false);
			Flags.SetDepthOfField(false);
			Flags.SetMotionBlur(false);
			Flags.SetTonemapper(true);
			const bool SceneLights = S.Shading == 2 && S.bUseSceneLights;
			const bool SceneWorld = S.Shading == 2 && S.bUseSceneWorld;
			Flags.SetDirectLighting(SceneLights);
			if (!SceneLights)
			{
				Flags.SetDynamicShadows(false);
				Flags.SetGlobalIllumination(false);
				Flags.SetLumenGlobalIllumination(false);
				Flags.SetIndirectLightingCache(false);
				Flags.SetVolumetricLightmap(false);
			}
			if (!SceneWorld)
			{
				Flags.SetSkyLighting(false);
				Flags.SetAtmosphere(false);
				Flags.SetFog(false);
				Flags.SetVolumetricFog(false);
				Flags.SetReflectionEnvironment(false);
				Flags.SetLumenReflections(false);
				Flags.SetScreenSpaceReflections(false);
			}
		}

		virtual void SetupView(FSceneViewFamily& Family, FSceneView& View) override
		{
			if (!State->IsTarget(Family) || View.bIsSceneCapture || View.bIsReflectionCapture) return;
			++State->SetupViewCount;
			State->bLastViewHadState = View.State != nullptr;
			const auto& S = State->Settings;
			auto& PP = View.FinalPostProcessSettings;
			PP.AutoExposureMethod = AEM_Manual;
			PP.AutoExposureApplyPhysicalCameraExposure = false;
			PP.AutoExposureBias = 0.0f;
			PP.AutoExposureBiasCurve = nullptr;
			PP.AutoExposureMinBrightness = 1.0f;
			PP.AutoExposureMaxBrightness = 1.0f;
			PP.DepthOfFieldScale = 0.0f;
			PP.MotionBlurAmount = 0.0f;
			if (S.Shading == 1 || !S.bUseSceneLights)
			{
				PP.IndirectLightingIntensity = 0.0f;
				PP.DynamicGlobalIlluminationMethod = EDynamicGlobalIlluminationMethod::None;
			}
			if (S.Shading == 1 || !S.bUseSceneWorld)
			{
				PP.ReflectionMethod = EReflectionMethod::None;
				PP.ContributingCubemaps.Reset();
				if (S.Shading == 2 && State->Cube.IsValid() && IsUsingGBuffers(View.GetShaderPlatform()))
				{
					FFinalPostProcessSettings::FCubemapEntry Entry;
					Entry.AmbientCubemap = State->Cube.Get();
					Entry.AmbientCubemapTintMulScaleValue = FLinearColor::White * FMath::Max(0.0f, S.EnvironmentIntensity);
					PP.ContributingCubemaps.Add(Entry);
				}
				State->HideSkyPrimitives(View, Family.Scene ? Family.Scene->GetWorld() : nullptr);
			}
			// Translucent X-ray previews do not write scene depth. A far-depth background
			// replacement would erase them, so keep that existing preview unobstructed.
			if (S.Shading == 1 && !S.bXRay && State->bMaterialReady && State->MID.IsValid() && IsUsingGBuffers(View.GetShaderPlatform()))
			{
				State->MID->OverrideBlendableSettings(View, 1.0f);
				++State->InjectedViewCount;
			}
			else if (S.Shading == 2 && !S.bUseSceneWorld && State->bBackgroundMaterialReady && State->BackgroundMID.IsValid())
			{
				State->BackgroundMID->OverrideBlendableSettings(View, 1.0f);
				++State->InjectedViewCount;
			}
		}

		virtual void BeginRenderViewFamily(FSceneViewFamily& Family) override
		{
			const bool bTarget = State->IsTarget(Family);
			if (!bTarget) return;
			State->LastMaterialNodeCount = 0;
			State->LastInjectedSolid = -1.0f;
			UMaterialInstanceDynamic* InjectedMID = nullptr;
			for (const FSceneView* View : Family.Views)
			{
				FBlendableEntry* Iterator = nullptr;
				while (FPostProcessMaterialNode* Node = View->FinalPostProcessSettings.BlendableManager.IterateBlendables<FPostProcessMaterialNode>(Iterator))
				{
					UMaterialInterface* Interface = Node->GetMaterialInterface();
					if (Interface && (Interface->GetMaterial() == State->Material.Get() || Interface->GetMaterial() == State->BackgroundMaterial.Get()))
					{
						++State->LastMaterialNodeCount;
						InjectedMID = Cast<UMaterialInstanceDynamic>(Interface);
						if (InjectedMID) State->LastInjectedSolid = InjectedMID->K2_GetScalarParameterValue(TEXT("Solid"));
					}
				}
			}
			if (!FParse::Param(FCommandLine::Get(), TEXT("BlenderPreviewLightingDiag"))
				&& !FParse::Param(FCommandLine::Get(), TEXT("BlenderControlsPreviewVisual"))) return;
			const auto& S = State->Settings;
			const FString Key = FString::Printf(TEXT("%d:%d:%s:%s:%.3f:%.3f:%d:%d:%d:%d"), S.Shading, S.SolidMode,
				*S.WorldStem, *S.MatCapStem, S.BackgroundOpacity, S.BackgroundBlur, S.bCavity, S.bOutline, S.bXRay, State->LastMaterialNodeCount);
			if (State->LastDiagnosticKey == Key) return;
			State->LastDiagnosticKey = Key;
			UTexture* NodeCube = InjectedMID ? InjectedMID->K2_GetTextureParameterValue(TEXT("EnvironmentTexture")) : nullptr;
			UTexture* NodeCap = InjectedMID ? InjectedMID->K2_GetTextureParameterValue(TEXT("MatCapTexture")) : nullptr;
			const auto* DisableMaterials = IConsoleManager::Get().FindTConsoleVariableDataInt(TEXT("r.PostProcessing.DisableMaterials"));
			UE_LOG(LogTemp, Display, TEXT("BlenderPreviewDiag target=%d state=%d setup=%d injected=%d nodes=%d complete=%d location=%d shading=%d solidMode=%d sourceSolid=%.2f nodeSolid=%.2f nodeOpacity=%.2f nodeColor=%s nodeCube=%s nodeCap=%s Post=%d PPMaterial=%d VisualizeShading=%d DisableMaterials=%d"),
				bTarget, State->bLastViewHadState, State->SetupViewCount, State->InjectedViewCount, State->LastMaterialNodeCount,
				InjectedMID && InjectedMID->GetMaterial()->IsComplete(), InjectedMID ? int32(InjectedMID->GetMaterial()->BlendableLocation) : -1, S.Shading, S.SolidMode,
				State->MID.IsValid() ? State->MID->K2_GetScalarParameterValue(TEXT("Solid")) : -1.0f, State->LastInjectedSolid,
				InjectedMID ? InjectedMID->K2_GetScalarParameterValue(TEXT("BackgroundOpacity")) : -1.0f,
				InjectedMID ? *InjectedMID->K2_GetVectorParameterValue(S.Shading == 2 ? TEXT("BackgroundColor") : TEXT("SolidColor")).ToString() : TEXT("none"),
				*GetNameSafe(NodeCube), *GetNameSafe(NodeCap), int32(Family.EngineShowFlags.PostProcessing),
				int32(Family.EngineShowFlags.PostProcessMaterial), int32(Family.EngineShowFlags.VisualizeShadingModels),
				DisableMaterials ? DisableMaterials->GetValueOnGameThread() : -1);
		}
	private:
		TSharedRef<FSharedState> State;
	};
}

struct FBlenderPreviewLighting::FImpl
{
	TSharedRef<BlenderPreviewLightingPrivate::FSharedState> State = MakeShared<BlenderPreviewLightingPrivate::FSharedState>();
	TSharedPtr<BlenderPreviewLightingPrivate::FPreviewViewExtension, ESPMode::ThreadSafe> Extension;
};

FBlenderPreviewLighting::FBlenderPreviewLighting() : Impl(MakeUnique<FImpl>())
{
	Impl->Extension = FSceneViewExtensions::NewExtension<BlenderPreviewLightingPrivate::FPreviewViewExtension>(Impl->State);
}

FBlenderPreviewLighting::~FBlenderPreviewLighting()
{
	Impl->State->bEnabled = false;
	Impl->State->Clients.Reset();
	Impl->Extension.Reset();
	// Final teardown only: queued views must release references before transient resources do.
	FlushRenderingCommands();
}

void FBlenderPreviewLighting::SetEnabled(bool bEnabled)
{
	Impl->State->bEnabled = bEnabled;
	if (bEnabled)
	{
		BlenderPreviewLightingPrivate::LoadTextures(*Impl->State);
		BlenderPreviewLightingPrivate::BuildMaterial(*Impl->State);
		BlenderPreviewLightingPrivate::BuildBackgroundMaterial(*Impl->State);
		BlenderPreviewLightingPrivate::UpdateParameters(*Impl->State);
	}
}

void FBlenderPreviewLighting::SetViewports(const TArray<FEditorViewportClient*>& Viewports)
{
	Impl->State->Clients = Viewports;
}

void FBlenderPreviewLighting::SetSettings(const FBlenderPreviewLightingSettings& Settings)
{
	Impl->State->Settings = Settings;
	if (Impl->State->bEnabled)
	{
		BlenderPreviewLightingPrivate::LoadTextures(*Impl->State);
		BlenderPreviewLightingPrivate::BuildMaterial(*Impl->State);
		BlenderPreviewLightingPrivate::BuildBackgroundMaterial(*Impl->State);
		BlenderPreviewLightingPrivate::UpdateParameters(*Impl->State);
	}
}

FString FBlenderPreviewLighting::GetStatusText() const
{
	const auto& State = *Impl->State;
	return FString::Printf(TEXT("PP=%s; backgroundPP=%s; enabled=%d; shading=%d; XRayPP=%s; HDRI=%s (%s); MatCap=%s (%s); registered=%d; lastNodes=%d; lastSolid=%.1f; deferred GBuffer required; background before AfterDOF translucency; background rotation only; %s"),
		State.bMaterialReady ? TEXT("ready") : TEXT("not ready"), State.bBackgroundMaterialReady ? TEXT("ready") : TEXT("not ready"), State.bEnabled ? 1 : 0, State.Settings.Shading,
		State.Settings.Shading == 1 && State.Settings.bXRay ? TEXT("bypass") : TEXT("normal"), *State.Settings.WorldStem,
		State.bSelectedWorldFound ? TEXT("plugin asset") : TEXT("fallback"), *State.Settings.MatCapStem,
		State.bSelectedMatCapFound ? TEXT("plugin asset") : TEXT("studio fallback"), State.Clients.Num(), State.LastMaterialNodeCount, State.LastInjectedSolid, *State.Error);
}

bool FBlenderPreviewLighting::IsMaterialReady() const { return Impl->State->bMaterialReady && Impl->State->bBackgroundMaterialReady; }
bool FBlenderPreviewLighting::HasSelectedWorldTexture() const { return Impl->State->bSelectedWorldFound; }
bool FBlenderPreviewLighting::HasSelectedMatCapTexture() const { return Impl->State->bSelectedMatCapFound; }
bool FBlenderPreviewLighting::IsRegisteredViewport(const FEditorViewportClient* Client) const { return Impl->State->IsLive(Client); }
