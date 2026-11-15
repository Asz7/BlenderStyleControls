#pragma once

#include "CoreMinimal.h"
#include "EdMode.h"

class FScopedTransaction;
class FBlenderMeshEditSession;
class SEditorViewport;
class UPackage;
class FBlenderViewportNavigation;
class FBlenderViewportPresentation;
class FJsonValue;

enum class EBlenderTransformOperation : uint8
{
	None,
	Move,
	Rotate,
	Scale
};

enum class EBlenderTopologyOperation : uint8
{
	None,
	ExtrudeRegion,
	ExtrudeAlongNormals,
	LoopHover,
	LoopSlide
};

/** Optional Blender-style actor and isolated multi-object mesh controls for level viewports. */
class FBlenderStyleEdMode : public FEdMode
{
public:
	static const FEditorModeID ModeId;
	FBlenderStyleEdMode();
	virtual ~FBlenderStyleEdMode() override;

	virtual void Enter() override;
	virtual void Exit() override;
	virtual bool IsCompatibleWith(FEditorModeID OtherModeID) const override;
	virtual bool UsesTransformWidget() const override;
	virtual bool UsesTransformWidget(UE::Widget::EWidgetMode CheckMode) const override;
	virtual bool DisallowMouseDeltaTracking() const override;
	virtual bool CanAutoSave() const override;
	virtual bool InputKey(FEditorViewportClient* Client, FViewport* Viewport, FKey Key, EInputEvent Event) override;
	virtual bool InputAxis(FEditorViewportClient* Client, FViewport* Viewport, int32 ControllerId, FKey Key, float Delta, float DeltaTime) override;
	virtual bool MouseMove(FEditorViewportClient* Client, FViewport* Viewport, int32 X, int32 Y) override;
	virtual bool CapturedMouseMove(FEditorViewportClient* Client, FViewport* Viewport, int32 X, int32 Y) override;
	virtual bool InputDelta(FEditorViewportClient* Client, FViewport* Viewport, FVector& Drag, FRotator& Rot, FVector& Scale) override;
	virtual bool LostFocus(FEditorViewportClient* Client, FViewport* Viewport) override;
	virtual void Tick(FEditorViewportClient* Client, float DeltaTime) override;
	virtual void DrawHUD(FEditorViewportClient* Client, FViewport* Viewport, const FSceneView* View, FCanvas* Canvas) override;
	virtual void Render(const FSceneView* View, FViewport* Viewport, FPrimitiveDrawInterface* PDI) override;
	virtual void MapChangeNotify() override;

	bool IsTransforming() const { return Operation != EBlenderTransformOperation::None; }
	bool IsTopologyModal() const { return TopologyOperation != EBlenderTopologyOperation::None; }
	bool IsMeshEditing() const;
	bool BeginMeshEdit(FEditorViewportClient* Client, FViewport* Viewport);
	bool EndMeshEdit(bool bAccept);
	FBlenderMeshEditSession* GetMeshEditSession() const { return MeshSession.Get(); }
	bool GetActiveAxisGuide(FVector& Center, FVector& Direction, FLinearColor& Color) const;
	void CancelActiveTransform();
	FText GetStatusText() const;
	bool StartTransform(EBlenderTransformOperation InOperation, FEditorViewportClient* Client, FViewport* Viewport, bool bDuplicate = false);
	void AddPrimitive(const FString& EngineMeshPath);
	FBlenderViewportPresentation* GetPresentation() const { return Presentation.Get(); }
	bool IsSelecting() const { return bBoxArmed || bPointerPending; }

private:
	friend void AppendBlenderViewportVerification(FEditorViewportClient*, FViewport*, TArray<TSharedPtr<FJsonValue>>&, int32&);
	struct FActorState
	{
		TWeakObjectPtr<AActor> Actor;
		FTransform Transform;
	};

	void ConfirmActiveTransform();
	void ResetTransformState();
	void ApplyTransform();
	void UpdateMouse(FEditorViewportClient* Client, FViewport* Viewport, int32 X, int32 Y);
	void SetAxis(int32 InAxis);
	bool HandleNumericKey(FKey Key);
	bool HasNumericValue(double& OutValue) const;
	FVector GetConstraintAxis() const;
	FVector GetMouseTranslation() const;
	double GetMouseRotationDegrees() const;
	double GetMouseScale() const;
	void OpenAddMenu(FEditorViewportClient* Client, FViewport* Viewport);
	void CacheModalMouse(FEditorViewportClient* Client, FViewport* Viewport, const FVector& Center);
	void CacheTopologyDirection(const FVector& Direction);
	bool CanStartTopologyTool(FEditorViewportClient* Client, FViewport* Viewport) const;
	bool StartExtrude(bool bAlongNormals, FEditorViewportClient* Client, FViewport* Viewport);
	bool StartLoopCut(FEditorViewportClient* Client, FViewport* Viewport);
	void OpenExtrudeMenu(FEditorViewportClient* Client, FViewport* Viewport);
	bool HandleTopologyInput(FEditorViewportClient* Client, FViewport* Viewport, FKey Key, EInputEvent Event);
	void UpdateTopologyMouse(FEditorViewportClient* Client, FViewport* Viewport, int32 X, int32 Y);
	void UpdateLoopHover();
	bool GetTopologyValue(double& OutValue) const;
	bool ApplyTopologyEdit();
	void FinishTopologyEdit(bool bKeepDisplacement);
	void AbortTopologyEdit();
	void ResetTopologyState();
	void HandleMapChange(uint32 Flags);
	void HandlePreBeginPIE(bool bIsSimulating);
	void HandleEndPIE(bool bIsSimulating);
	void HandleModeChanged(const FEditorModeID& ChangedModeId, bool bIsEnteringMode);
	void RememberPackage(UPackage* Package);
	void RestoreOriginalSelection();
	void ShowNotice(const FText& Message);
	static bool IsUsableActor(AActor* Actor, UWorld* World);
	static TArray<AActor*> FilterTopLevelActors(const TArray<AActor*>& Actors);
	static bool IsControlDown(FViewport* Viewport);
	static bool IsShiftDown(FViewport* Viewport);
	void SyncPresentation();
	bool HandleSelectionInput(FEditorViewportClient* Client, FViewport* Viewport, FKey Key, EInputEvent Event);
	bool UpdateSelectionPointer(FEditorViewportClient* Client, FViewport* Viewport, int32 X, int32 Y);
	void CancelSelectionGesture();
	void ApplySelectionGesture();
	void DrawSelectionHUD(FViewport* Viewport, FCanvas* Canvas);
	FBox GetNavigationBounds(FEditorViewportClient* Client, bool bAll) const;
	void OpenShadingMenu(FEditorViewportClient* Client);
	void RestoreViewportControls();

	EBlenderTransformOperation Operation = EBlenderTransformOperation::None;
	EBlenderTopologyOperation TopologyOperation = EBlenderTopologyOperation::None;
	FVector TopologyDirection = FVector::UpVector;
	FVector2D TopologyScreenDirection = FVector2D(1, 0);
	double TopologyUnitsPerPixel = 1.0;
	double TopologyValue = 0.0;
	bool bTopologyPrecision = false;
	int32 LoopObject = INDEX_NONE;
	int32 LoopEdge = INDEX_NONE;
	TArray<TPair<FVector, FVector>> LoopPreview;
	FText TopologyHint;
	uint64 MeshSessionSerial = 0;
	int32 Axis = INDEX_NONE;
	bool bLocalAxis = false;
	bool bDuplicateOperation = false;
	bool bApplying = false;
	bool bSnap = false;
	bool bSuppressLeftRelease = false;
	bool bSuppressRightRelease = false;
	bool bChangedWidgetVisibility = false;
	bool bOriginalShowWidget = true;
	FString NumericInput;
	TArray<FActorState> ActorStates;
	TArray<TWeakObjectPtr<AActor>> OriginalSelection;
	TArray<TWeakObjectPtr<AActor>> DuplicatedActors;
	TMap<TWeakObjectPtr<UPackage>, bool> OriginalPackageDirty;
	TUniquePtr<FScopedTransaction> Transaction;
	TUniquePtr<FBlenderMeshEditSession> MeshSession;
	TUniquePtr<FBlenderViewportNavigation> Navigation;
	TUniquePtr<FBlenderViewportPresentation> Presentation;
	bool bViewportControlsActive = false;
	int32 PreviousShading = 1;
	bool bBoxArmed = false;
	bool bPointerPending = false;
	bool bBoxDragging = false;
	bool bGestureShift = false;
	bool bGestureControl = false;
	bool bGestureAlt = false;
	FKey GestureButton;
	FIntPoint GestureStart = FIntPoint::ZeroValue;
	FIntPoint GestureEnd = FIntPoint::ZeroValue;
	FEditorViewportClient* GestureClient = nullptr;
	FViewport* GestureViewport = nullptr;
	FText LastNotice;
	TWeakObjectPtr<UWorld> TransformWorld;
	FEditorViewportClient* ActiveClient = nullptr;
	FViewport* ActiveViewport = nullptr;
	FVector Pivot = FVector::ZeroVector;
	FQuat LocalOrientation = FQuat::Identity;
	FVector ViewRight = FVector::RightVector;
	FVector ViewUp = FVector::UpVector;
	FVector ViewForward = FVector::ForwardVector;
	FMatrix InverseViewProjection = FMatrix::Identity;
	FIntRect ViewRect;
	FVector2D StartMouse = FVector2D::ZeroVector;
	FVector2D LastMouse = FVector2D::ZeroVector;
	FVector2D VirtualMouse = FVector2D::ZeroVector;
	FVector2D PivotPixel = FVector2D::ZeroVector;
	double WorldUnitsPerPixel = 1.0;
	bool bHasViewProjection = false;
	TWeakObjectPtr<UWorld> PlacementWorld;
	FVector PlacementLocation = FVector::ZeroVector;
	FVector PlacementNormal = FVector::UpVector;
	bool bPlacementHit = false;
	bool bHasPlacement = false;
	FDelegateHandle MapChangeHandle;
	FDelegateHandle BeginPIEHandle;
	FDelegateHandle EndPIEHandle;
	FDelegateHandle ModeChangeHandle;
};
