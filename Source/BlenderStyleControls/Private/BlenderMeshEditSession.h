#pragma once

#include "CoreMinimal.h"
#include "UObject/GCObject.h"

class AActor;
class FEditorViewportClient;
class FPrimitiveDrawInterface;
class FSceneView;
class FViewport;
class UStaticMesh;
class UMaterialInterface;
class UWorld;
struct FMeshDescription;

/** Creates a unique unsaved 100 cm six-quad project primitive. */
UStaticMesh* CreateBlenderQuadCube();

enum class EBlenderMeshSelectionMode : uint8 { Vertex, Edge, Face };
enum class EBlenderSelectionOperation : uint8 { Replace, Add, Subtract, Intersect };
enum class EBlenderPickAction : uint8 { Normal, Loop, Ring, ShortestPath, Linked };

struct FBlenderMeshVertexRef
{
	int32 ObjectIndex = INDEX_NONE;
	int32 VertexId = INDEX_NONE;
	FVector WorldPosition = FVector::ZeroVector;
};

/**
 * A transient, multi-object edit session. Source MeshDescriptions are never modified.
 * Contract: Begin validates every actor before hiding anything. Selection IDs are source
 * MeshDescription IDs initially; topology edits rebuild mappings to the working polygons.
 * All element selections resolve to a unique set of working vertices.
 * PreviewTransform always maps the BeginTransform snapshot, in world space. Confirm adds
 * one local undo entry; cancel restores that snapshot. Topology history includes complete
 * polygon meshes, corner attributes, selections and active elements. Commit duplicates only changed
 * objects and changes component mesh references in one editor transaction. No files save.
 * Focused checks: two rotated/scaled instances + untouched third; shared-face union;
 * cancel/no-op; independent duplicates; undo; source UV/color/topology preservation.
 */
class FBlenderMeshEditSession final : public FGCObject
{
public:
	FBlenderMeshEditSession();
	virtual ~FBlenderMeshEditSession() override;

	bool Begin(UWorld* World, const TArray<AActor*>& Actors, FText& Error);
	bool IsActive() const;
	UWorld* GetWorld() const;
	int32 GetObjectCount() const;
	void Cancel();
	bool Commit(TArray<UStaticMesh*>& CreatedAssets, FText& Error);
	EBlenderMeshSelectionMode GetSelectionMode() const;
	void SetSelectionMode(EBlenderMeshSelectionMode Mode);
	void SelectAll(bool bSelect);
	bool Pick(FEditorViewportClient* ViewportClient, FViewport* Viewport, bool bToggle);
	bool Pick(FEditorViewportClient* ViewportClient, FViewport* Viewport, EBlenderPickAction Action,
		bool bExtend = false, bool bSubtract = false, bool bFill = false);
	bool BoxSelect(FEditorViewportClient* ViewportClient, FViewport* Viewport, const FIntRect& Rect, EBlenderSelectionOperation Operation);
	void SetXRay(bool bEnabled);
	bool IsXRay() const;
	void SetPreviewMaterial(UMaterialInterface* Material);
	bool SelectLoop(int32 ObjectIndex, int32 EdgeID, bool bRing, bool bExtend, int32 ActiveSeedID = INDEX_NONE);
	bool SelectShortestPath(int32 ObjectIndex, EBlenderMeshSelectionMode Mode, int32 TargetID, bool bFill = false);
	bool GrowSelection(bool bGrow);
	bool SelectLinked(bool bSelect);
	bool GetActiveElement(int32& OutObjectIndex, EBlenderMeshSelectionMode& OutMode, int32& OutElementID) const;
	FBox GetGeometryBounds(bool bSelectedOnly = false) const;
	bool SelectElement(int32 ObjectIndex, EBlenderMeshSelectionMode Mode, int32 ElementID, bool bAdd);
	TArray<int32> GetElementIDs(int32 ObjectIndex, EBlenderMeshSelectionMode Mode) const;
	TArray<int32> GetSelectedElementIDs(int32 ObjectIndex, EBlenderMeshSelectionMode Mode) const;
	TArray<FBlenderMeshVertexRef> GetSelectedVertices() const;
	FVector GetSelectionCenter() const;
	FQuat GetReferenceRotation() const;
	void BeginTransform();
	void PreviewTransform(TFunctionRef<FVector(const FVector&)> WorldPositionMap);
	void ConfirmTransform();
	void CancelTransform();
	bool Undo();
	bool Redo();
	void Render(const FSceneView* View, FViewport* Viewport, FPrimitiveDrawInterface* PDI);
	FText GetStatusText() const;
	AActor* GetSourceActor(int32 ObjectIndex) const;
	UStaticMesh* GetSourceMesh(int32 ObjectIndex) const;
	bool GetVertexWorldPosition(int32 ObjectIndex, int32 VertexId, FVector& OutPosition) const;
	bool HasChanges() const;
	bool CopyMeshDescription(int32 ObjectIndex, FMeshDescription& OutMesh) const;
	bool BeginExtrude(bool bAlongNormals, FText& Error);
	bool UpdateExtrude(double DistanceCm, const FVector& UniformWorldDirection = FVector::ZeroVector);
	FVector GetExtrudeDirection() const;
	bool FindLoopCutEdge(FEditorViewportClient* ViewportClient, FViewport* Viewport, int32& OutObject, int32& OutEdge) const;
	bool GetLoopCutPreview(int32 ObjectIndex, int32 Edge, TArray<TPair<FVector,FVector>>& OutSegments, FText& Error) const;
	bool BeginLoopCut(int32 ObjectIndex, int32 Edge, FText& Error);
	bool UpdateLoopCut(double Fraction);
	bool GetLoopCutSlideGuide(FVector& Start, FVector& End) const;
	bool IsTopologyEditing() const;
	void AcceptTopologyEdit();
	void CancelTopologyEdit();

	virtual void AddReferencedObjects(FReferenceCollector& Collector) override;
	virtual FString GetReferencerName() const override { return TEXT("FBlenderMeshEditSession"); }

private:
	struct FImpl;
	TUniquePtr<FImpl> Impl;
};
