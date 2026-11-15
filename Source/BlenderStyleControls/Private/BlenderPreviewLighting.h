#pragma once

#include "CoreMinimal.h"

class FEditorViewportClient;

/** Session-only display settings. Never written to the world or material assets. */
struct FBlenderPreviewLightingSettings
{
	int32 Shading = 1; // 0 wire, 1 solid, 2 material preview, 3 rendered (untouched).
	int32 SolidMode = 0; // 0 studio, 1 MatCap, 2 flat.
	int32 StudioIndex = 0; // 0 balanced, 1 soft, 2 rim.
	float StudioIntensity = 1.0f;
	float StudioRotationDegrees = 0.0f;
	FLinearColor SolidColor = FLinearColor(0.63f, 0.65f, 0.68f);
	FLinearColor BackgroundColor = FLinearColor(0.24f, 0.26f, 0.29f);
	FString WorldStem = TEXT("forest");
	FString MatCapStem = TEXT("basic_1");
	float EnvironmentIntensity = 1.0f;
	float BackgroundOpacity = 0.0f;
	float BackgroundBlur = 0.5f; // 0..1 mapped to cubemap mip levels.
	float BackgroundRotationDegrees = 0.0f; // Background ONLY; native ambient cubemap has no rotation API.
	bool bUseSceneLights = false;
	bool bUseSceneWorld = false;
	bool bCavity = false;
	bool bOutline = false;
	bool bMatCapFlip = false;
	bool bXRay = false; // Keep the existing translucent edit overlay; bypass solid replacement.
};

/** Per-view HDRI and solid shading. Owns transient materials, never scene components. */
class FBlenderPreviewLighting
{
public:
	FBlenderPreviewLighting();
	~FBlenderPreviewLighting();
	FBlenderPreviewLighting(const FBlenderPreviewLighting&) = delete;
	FBlenderPreviewLighting& operator=(const FBlenderPreviewLighting&) = delete;

	void SetEnabled(bool bEnabled);
	void SetViewports(const TArray<FEditorViewportClient*>& Viewports);
	void SetSettings(const FBlenderPreviewLightingSettings& Settings);
	FString GetStatusText() const;
	bool IsMaterialReady() const;
	bool HasSelectedWorldTexture() const;
	bool HasSelectedMatCapTexture() const;
	/** Same strict registration/live-editor-world test used by the extension. */
	bool IsRegisteredViewport(const FEditorViewportClient* Client) const;

private:
	struct FImpl;
	TUniquePtr<FImpl> Impl;
};
