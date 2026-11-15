#pragma once

#include "CoreMinimal.h"
#include "BlenderPreviewLighting.h"

class FEditorViewportClient;
class UMaterialInterface;
class FSceneView;
class FViewport;
class FPrimitiveDrawInterface;

struct FBlenderViewportOverlaySettings
{
	bool bShowOverlays = true;
	bool bWireframe = false;
	bool bGrid = true;
	bool bSelectionOutline = true;
	bool bExtras = true;
	bool bFaceOrientation = false;
	bool bTextInfo = true;
	bool bOrigins = false;
	float WireOpacity = 1.0f;
	float WireThickness = 1.0f;
};

enum class EBlenderViewportShading : uint8
{
	Wireframe,
	Solid,
	Material,
	Rendered
};

/** Temporary, editor-viewport-only display state. Never edits scene assets. */
class FBlenderViewportPresentation
{
public:
	FBlenderViewportPresentation();
	~FBlenderViewportPresentation();

	FBlenderViewportPresentation(const FBlenderViewportPresentation&) = delete;
	FBlenderViewportPresentation& operator=(const FBlenderViewportPresentation&) = delete;

	/** Register all live level viewports; the optional client must also be a live level viewport. */
	void ActivateViewport(FEditorViewportClient* ViewportClient);
	void Tick(FEditorViewportClient* ViewportClient);
	void Restore();

	EBlenderViewportShading GetShading() const;
	void SetShading(EBlenderViewportShading Shading);
	bool CanXRay() const;
	bool IsXRay() const;
	void ToggleXRay();

	/** Keeps sculpt/transparent preview materials enabled while a mesh-edit preview exists. */
	void SetMeshEditActive(bool bActive);
	/** Null means use the edit preview's original material slots. Not a material to save. */
	UMaterialInterface* GetPreviewMaterial();
	void SetOnChanged(TFunction<void()> InCallback);
	const FBlenderPreviewLightingSettings& GetPreviewSettings() const;
	void SetPreviewSettings(const FBlenderPreviewLightingSettings& Settings);
	const FBlenderViewportOverlaySettings& GetOverlaySettings() const;
	void SetOverlaySettings(const FBlenderViewportOverlaySettings& Settings);
	FBlenderPreviewLighting* GetLighting() const;
	void RenderOverlays(const FSceneView* View, FViewport* Viewport, FPrimitiveDrawInterface* PDI);

private:
	struct FImpl;
	TUniquePtr<FImpl> Impl;
};
