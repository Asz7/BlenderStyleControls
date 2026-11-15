#include "BlenderStyleEdMode.h"
#include "BlenderMeshEditSession.h"
#include "BlenderViewportNavigation.h"
#include "BlenderViewportPresentation.h"

#include "CanvasItem.h"
#include "CanvasTypes.h"
#include "Components/SceneComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Editor.h"
#include "Editor/UnrealEdEngine.h"
#include "EditorModes.h"
#include "EditorModeManager.h"
#include "EditorViewportClient.h"
#include "Engine/Engine.h"
#include "Engine/Level.h"
#include "Engine/Selection.h"
#include "Engine/StaticMesh.h"
#include "Engine/StaticMeshActor.h"
#include "Engine/World.h"
#include "Framework/Application/SlateApplication.h"
#include "Framework/MultiBox/MultiBoxBuilder.h"
#include "Framework/Notifications/NotificationManager.h"
#include "LevelUtils.h"
#include "LevelEditorViewport.h"
#include "SceneManagement.h"
#include "SceneView.h"
#include "ScopedTransaction.h"
#include "SEditorViewport.h"
#include "Styling/CoreStyle.h"
#include "UObject/Package.h"
#include "UnrealClient.h"
#include "UnrealEdGlobals.h"
#include "Widgets/Notifications/SNotificationList.h"

#define LOCTEXT_NAMESPACE "BlenderStyleControls"

const FEditorModeID FBlenderStyleEdMode::ModeId = TEXT("EM_BlenderStyleControls");

namespace
{
	bool IsTextEntryFocused()
	{
		if (!FSlateApplication::IsInitialized())
		{
			return false;
		}
		const TSharedPtr<SWidget> Focused = FSlateApplication::Get().GetKeyboardFocusedWidget();
		return Focused.IsValid() && Focused->GetTypeAsString().Contains(TEXT("EditableText"));
	}

	bool IntersectPlane(const FVector& RayOrigin, const FVector& RayDirection, const FVector& PlaneOrigin, const FVector& Normal, FVector& OutPoint)
	{
		const double Denominator = FVector::DotProduct(RayDirection, Normal);
		if (FMath::Abs(Denominator) < 1.e-6)
		{
			return false;
		}
		OutPoint = RayOrigin + RayDirection * (FVector::DotProduct(PlaneOrigin - RayOrigin, Normal) / Denominator);
		return !OutPoint.ContainsNaN();
	}
}

FBlenderStyleEdMode::FBlenderStyleEdMode() = default;

FBlenderStyleEdMode::~FBlenderStyleEdMode()
{
	CancelActiveTransform();
	EndMeshEdit(false);
	RestoreViewportControls();
	FEditorDelegates::MapChange.Remove(MapChangeHandle);
	FEditorDelegates::PreBeginPIE.Remove(BeginPIEHandle);
	FEditorDelegates::EndPIE.Remove(EndPIEHandle);
}

void FBlenderStyleEdMode::Enter()
{
	FEdMode::Enter();
	bViewportControlsActive = true;
	Navigation = MakeUnique<FBlenderViewportNavigation>();
	Presentation = MakeUnique<FBlenderViewportPresentation>();
	Presentation->SetOnChanged([this] { SyncPresentation(); });
	if (GEditor) for (auto* Client : GEditor->GetLevelViewportClients())
	{
		Navigation->ActivateViewport(Client);
		Presentation->ActivateViewport(Client);
	}
	MapChangeHandle = FEditorDelegates::MapChange.AddRaw(this, &FBlenderStyleEdMode::HandleMapChange);
	BeginPIEHandle = FEditorDelegates::PreBeginPIE.AddRaw(this, &FBlenderStyleEdMode::HandlePreBeginPIE);
	EndPIEHandle = FEditorDelegates::EndPIE.AddRaw(this, &FBlenderStyleEdMode::HandleEndPIE);
	if (FEditorModeTools* ModeManager = GetModeManager())
	{
		ModeChangeHandle = ModeManager->OnEditorModeIDChanged().AddSP(this, &FBlenderStyleEdMode::HandleModeChanged);
	}
}

void FBlenderStyleEdMode::Exit()
{
	CancelActiveTransform();
	EndMeshEdit(false);
	RestoreViewportControls();
	FEditorDelegates::MapChange.Remove(MapChangeHandle);
	FEditorDelegates::PreBeginPIE.Remove(BeginPIEHandle);
	FEditorDelegates::EndPIE.Remove(EndPIEHandle);
	MapChangeHandle.Reset();
	BeginPIEHandle.Reset();
	EndPIEHandle.Reset();
	if (FEditorModeTools* ModeManager = GetModeManager())
	{
		ModeManager->OnEditorModeIDChanged().Remove(ModeChangeHandle);
	}
	ModeChangeHandle.Reset();
	bHasPlacement = false;
	PlacementWorld.Reset();
	FEdMode::Exit();
}

bool FBlenderStyleEdMode::IsCompatibleWith(FEditorModeID OtherModeID) const
{
	return OtherModeID == FBuiltinEditorModes::EM_Default;
}

bool FBlenderStyleEdMode::UsesTransformWidget() const
{
	return false;
}

bool FBlenderStyleEdMode::UsesTransformWidget(UE::Widget::EWidgetMode CheckMode) const
{
	return false;
}

bool FBlenderStyleEdMode::DisallowMouseDeltaTracking() const
{
	return true;
}

bool FBlenderStyleEdMode::CanAutoSave() const
{
	return !IsTransforming() && !IsTopologyModal() && !IsMeshEditing() && !IsSelecting();
}

bool FBlenderStyleEdMode::IsMeshEditing() const
{
	return MeshSession && MeshSession->IsActive();
}

void FBlenderStyleEdMode::ShowNotice(const FText& Message)
{
	LastNotice = Message;
	if (!Message.IsEmpty() && FSlateApplication::IsInitialized())
	{
		FNotificationInfo NotificationInfo(Message);
		NotificationInfo.ExpireDuration = 5.0f;
		NotificationInfo.bFireAndForget = true;
		FSlateNotificationManager::Get().AddNotification(NotificationInfo);
	}
	if (GEditor)
	{
		GEditor->RedrawLevelEditingViewports();
	}
}

bool FBlenderStyleEdMode::BeginMeshEdit(FEditorViewportClient* Client, FViewport* Viewport)
{
	if (IsMeshEditing() || IsTransforming() || IsTopologyModal() || !GEditor || GEditor->PlayWorld || !Client || !Viewport
		|| !Client->IsLevelEditorClient() || Client->IsTracking()
		|| Viewport->KeyState(EKeys::LeftMouseButton) || Viewport->KeyState(EKeys::MiddleMouseButton)
		|| Viewport->KeyState(EKeys::RightMouseButton))
	{
		return false;
	}
	UWorld* World = Client->GetWorld();
	if (!World || World->IsGameWorld() || World != GEditor->GetEditorWorldContext().World())
	{
		return false;
	}
	TArray<AActor*> Actors;
	for (FSelectionIterator It(*GEditor->GetSelectedActors()); It; ++It)
	{
		if (AActor* Actor = Cast<AActor>(*It))
		{
			// Reject an unsupported selection atomically; never silently edit a subset.
			if (!IsUsableActor(Actor, World))
			{
				ShowNotice(LOCTEXT("UneditableMeshSelection", "所选物体包含锁定或不可编辑的物体，无法进入网格编辑。"));
				return false;
			}
			Actors.Add(Actor);
		}
	}
	MeshSession = MakeUnique<FBlenderMeshEditSession>();
	FText Error;
	if (!MeshSession->Begin(World, Actors, Error))
	{
		MeshSession.Reset();
		ShowNotice(Error);
		return false;
	}
	LastNotice = FText::GetEmpty();
	++MeshSessionSerial;
	SyncPresentation();
	if (FEditorModeTools* ModeManager = GetModeManager())
	{
		bOriginalShowWidget = ModeManager->GetShowWidget();
		ModeManager->SetShowWidget(false);
		bChangedWidgetVisibility = true;
	}
	GEditor->RedrawLevelEditingViewports();
	return true;
}

bool FBlenderStyleEdMode::EndMeshEdit(bool bAccept)
{
	// Lifecycle calls discard the complete active operation, including its topology.
	// Only an explicit modal Esc/RMB keeps a zero-offset extrusion or centered cut.
	AbortTopologyEdit();
	if (!IsMeshEditing())
	{
		return false;
	}
	// Tab is handled only while idle. Lifecycle cancellation may arrive during G/S/R.
	CancelActiveTransform();
	if (bAccept)
	{
		TArray<UStaticMesh*> CreatedAssets;
		FText Error;
		if (!MeshSession->Commit(CreatedAssets, Error))
		{
			ShowNotice(Error);
			return false;
		}
		LastNotice = CreatedAssets.IsEmpty() ? LOCTEXT("MeshNoChange", "网格未更改，未创建副本。")
			: FText::Format(LOCTEXT("MeshCommitted", "已为 {0} 个修改物体创建独立模型；请按需保存新模型与关卡。"), FText::AsNumber(CreatedAssets.Num()));
	}
	else
	{
		MeshSession->Cancel();
		LastNotice = LOCTEXT("MeshCancelled", "已放弃本次网格编辑，恢复原模型。");
	}
	MeshSession.Reset();
	++MeshSessionSerial;
	ResetTransformState();
	SyncPresentation();
	if (GEditor)
	{
		GEditor->RedrawLevelEditingViewports();
	}
	return true;
}

bool FBlenderStyleEdMode::IsControlDown(FViewport* Viewport)
{
	return Viewport && (Viewport->KeyState(EKeys::LeftControl) || Viewport->KeyState(EKeys::RightControl));
}

bool FBlenderStyleEdMode::IsShiftDown(FViewport* Viewport)
{
	return Viewport && (Viewport->KeyState(EKeys::LeftShift) || Viewport->KeyState(EKeys::RightShift));
}

bool FBlenderStyleEdMode::IsUsableActor(AActor* Actor, UWorld* World)
{
	return IsValid(Actor) && Actor->GetWorld() == World && Actor->GetRootComponent()
		&& Actor->IsEditable() && !Actor->IsLockLocation() && !FLevelUtils::IsLevelLocked(Actor)
		&& !Actor->IsTemplate() && !Actor->IsActorBeingDestroyed();
}

TArray<AActor*> FBlenderStyleEdMode::FilterTopLevelActors(const TArray<AActor*>& Actors)
{
	TSet<AActor*> Selected(Actors);
	TArray<AActor*> Result;
	for (AActor* Actor : Actors)
	{
		bool bHasSelectedAncestor = false;
		for (AActor* Parent = Actor->GetAttachParentActor(); Parent; Parent = Parent->GetAttachParentActor())
		{
			if (Selected.Contains(Parent))
			{
				bHasSelectedAncestor = true;
				break;
			}
		}
		if (!bHasSelectedAncestor)
		{
			Result.Add(Actor);
		}
	}
	return Result;
}

void FBlenderStyleEdMode::RememberPackage(UPackage* Package)
{
	if (Package && !OriginalPackageDirty.Contains(Package))
	{
		OriginalPackageDirty.Add(Package, Package->IsDirty());
	}
}

bool FBlenderStyleEdMode::StartTransform(EBlenderTransformOperation InOperation, FEditorViewportClient* Client, FViewport* Viewport, bool bDuplicate)
{
	if (IsTransforming() || IsTopologyModal() || InOperation == EBlenderTransformOperation::None || !GEditor || !Client || !Viewport
		|| !Client->IsLevelEditorClient() || GEditor->PlayWorld || Client->IsTracking()
		|| Viewport->KeyState(EKeys::LeftMouseButton) || Viewport->KeyState(EKeys::MiddleMouseButton)
		|| Viewport->KeyState(EKeys::RightMouseButton))
	{
		return false;
	}
	UWorld* World = Client->GetWorld();
	if (!World || World->IsGameWorld() || World != GEditor->GetEditorWorldContext().World())
	{
		return false;
	}
	if (IsMeshEditing() && (MeshSession->GetWorld() != World || bDuplicate || MeshSession->GetSelectedVertices().IsEmpty()))
	{
		ShowNotice(bDuplicate ? LOCTEXT("NoMeshDuplication", "网格编辑暂不支持 Shift+D 复制；可选择面后按 E 挤出。")
			: LOCTEXT("NoMeshTransformSelection", "请先选择要变换的点、边或面。"));
		return false;
	}
	TArray<AActor*> Candidates;
	OriginalSelection.Reset();
	if (!IsMeshEditing())
	{
		for (FSelectionIterator It(*GEditor->GetSelectedActors()); It; ++It)
		{
			if (AActor* Actor = Cast<AActor>(*It))
			{
				OriginalSelection.Add(Actor);
				if (IsUsableActor(Actor, World))
				{
					Candidates.Add(Actor);
				}
			}
		}
	}
	if (!IsMeshEditing() && Candidates.IsEmpty())
	{
		OriginalSelection.Reset();
		return false;
	}
	Operation = InOperation;
	TransformWorld = World;
	ActiveClient = Client;
	ActiveViewport = Viewport;
	bDuplicateOperation = bDuplicate;
	LastNotice = FText::GetEmpty();
	if (IsMeshEditing())
	{
		Pivot = MeshSession->GetSelectionCenter();
		LocalOrientation = MeshSession->GetReferenceRotation();
		MeshSession->BeginTransform();
	}
	else
	{
		RememberPackage(World->GetOutermost());
		RememberPackage(World->GetCurrentLevel()->GetOutermost());
		for (AActor* Actor : Candidates)
		{
			RememberPackage(Actor->GetPackage());
			RememberPackage(Actor->GetLevel()->GetOutermost());
		}
		const FText TransactionName = bDuplicate ? LOCTEXT("DuplicateTransaction", "Blender 复制并移动")
			: InOperation == EBlenderTransformOperation::Move ? LOCTEXT("MoveTransaction", "Blender 移动")
			: InOperation == EBlenderTransformOperation::Rotate ? LOCTEXT("RotateTransaction", "Blender 旋转")
			: LOCTEXT("ScaleTransaction", "Blender 缩放");
		Transaction = MakeUnique<FScopedTransaction>(TransactionName);
		if (bDuplicate)
		{
			if (!GUnrealEd || FLevelUtils::IsLevelLocked(World->GetCurrentLevel()))
			{
				CancelActiveTransform();
				return false;
			}
			GEditor->GetSelectedActors()->Modify();
			TArray<AActor*> Copies;
			GUnrealEd->DuplicateActors(Candidates, Copies, World->GetCurrentLevel(), FVector::ZeroVector);
			Candidates.Reset();
			for (AActor* Copy : Copies)
			{
				if (IsValid(Copy))
				{
					DuplicatedActors.Add(Copy);
					if (IsUsableActor(Copy, World))
					{
						Candidates.Add(Copy);
					}
				}
			}
			if (Candidates.IsEmpty())
			{
				CancelActiveTransform();
				return false;
			}
			GEditor->SelectNone(false, true, false);
			for (AActor* Copy : Candidates)
			{
				GEditor->SelectActor(Copy, true, false);
			}
			GEditor->NoteSelectionChange();
		}
		Candidates = FilterTopLevelActors(Candidates);
		Pivot = FVector::ZeroVector;
		for (AActor* Actor : Candidates)
		{
			ActorStates.Add({Actor, Actor->GetActorTransform()});
			Pivot += Actor->GetActorLocation();
			Actor->Modify();
			Actor->GetRootComponent()->Modify();
		}
		Pivot /= ActorStates.Num();
		LocalOrientation = ActorStates[0].Transform.GetRotation();
	}
	if (FEditorModeTools* ModeManager = GetModeManager())
	{
		// The default selection mode remains active and ORs its widget flag with ours.
		// Hide the shared gizmo during modal input so a confirming click cannot grab it.
		if (!bChangedWidgetVisibility)
		{
			bOriginalShowWidget = ModeManager->GetShowWidget();
			ModeManager->SetShowWidget(false);
			bChangedWidgetVisibility = true;
		}
	}
	CacheModalMouse(Client, Viewport, Pivot);
	bSnap = IsControlDown(Viewport);
	GEditor->RedrawLevelEditingViewports();
	return true;
}

void FBlenderStyleEdMode::CacheModalMouse(FEditorViewportClient* Client, FViewport* Viewport, const FVector& Center)
{
	Pivot = Center;
	WorldUnitsPerPixel = 1.0;
	bHasViewProjection = false;
	StartMouse = LastMouse = VirtualMouse = FVector2D(Viewport->GetMouseX(), Viewport->GetMouseY());
	PivotPixel = StartMouse;
	ViewRight = Client->GetViewRotation().RotateVector(FVector::RightVector);
	ViewUp = Client->GetViewRotation().RotateVector(FVector::UpVector);
	ViewForward = Client->GetViewRotation().Vector();
	// Cache the actual view projection once: actor motion must not change the reference plane.
	if (Viewport->GetSizeXY().X > 0 && Viewport->GetSizeXY().Y > 0)
	{
		FSceneViewFamilyContext Family(FSceneViewFamily::ConstructionValues(Viewport, Client->GetScene(), Client->EngineShowFlags).SetRealtimeUpdate(Client->IsRealtime()));
		if (FSceneView* View = Client->CalcSceneView(&Family))
		{
			InverseViewProjection = View->ViewMatrices.GetInvViewProjectionMatrix();
			ViewRect = View->UnscaledViewRect;
			ViewRight = View->GetViewRight();
			ViewUp = View->GetViewUp();
			ViewForward = View->GetViewDirection();
			View->WorldToPixel(Pivot, PivotPixel);
			bHasViewProjection = ViewRect.Width() > 0 && ViewRect.Height() > 0;
			FVector Origin0, Direction0, Origin1, Direction1, Point0, Point1;
			View->DeprojectFVector2D(StartMouse, Origin0, Direction0);
			View->DeprojectFVector2D(StartMouse + FVector2D(1, 0), Origin1, Direction1);
			if (IntersectPlane(Origin0, Direction0, Pivot, ViewForward, Point0) && IntersectPlane(Origin1, Direction1, Pivot, ViewForward, Point1))
			{
				WorldUnitsPerPixel = FMath::Max((Point1 - Point0).Length(), 0.001);
			}
		}
	}
}

void FBlenderStyleEdMode::RestoreOriginalSelection()
{
	if (!GEditor)
	{
		return;
	}
	GEditor->SelectNone(false, true, false);
	for (const TWeakObjectPtr<AActor>& Actor : OriginalSelection)
	{
		if (Actor.IsValid())
		{
			GEditor->SelectActor(Actor.Get(), true, false, true);
		}
	}
	GEditor->NoteSelectionChange();
}

void FBlenderStyleEdMode::CancelActiveTransform()
{
	if (!IsTransforming() || bApplying)
	{
		return;
	}
	{
		TGuardValue<bool> Guard(bApplying, true);
		if (IsMeshEditing())
		{
			MeshSession->CancelTransform();
		}
		// FScopedTransaction::Cancel discards history; it does not restore object state.
		for (const FActorState& State : ActorStates)
		{
			if (AActor* Actor = State.Actor.Get())
			{
				Actor->SetActorTransform(State.Transform, false, nullptr, ETeleportType::TeleportPhysics);
				Actor->PostEditMove(true);
			}
		}
		if (bDuplicateOperation)
		{
			if (GEditor)
			{
				GEditor->SelectNone(false, true, false);
			}
			for (int32 Index = DuplicatedActors.Num() - 1; Index >= 0; --Index)
			{
				if (AActor* Copy = DuplicatedActors[Index].Get())
				{
					if (UWorld* World = Copy->GetWorld())
					{
						World->EditorDestroyActor(Copy, true);
					}
				}
			}
			RestoreOriginalSelection();
		}
		if (Transaction)
		{
			Transaction->Cancel();
			Transaction.Reset();
		}
		for (const auto& Entry : OriginalPackageDirty)
		{
			if (UPackage* Package = Entry.Key.Get())
			{
				Package->SetDirtyFlag(Entry.Value);
			}
		}
	}
	ResetTransformState();
	if (GEditor)
	{
		GEditor->RedrawLevelEditingViewports();
	}
}

void FBlenderStyleEdMode::ConfirmActiveTransform()
{
	if (!IsTransforming())
	{
		return;
	}
	double TypedValue;
	if (!NumericInput.IsEmpty() && !HasNumericValue(TypedValue))
	{
		return;
	}
	if (IsMeshEditing())
	{
		MeshSession->ConfirmTransform();
		ResetTransformState();
		if (GEditor)
		{
			GEditor->RedrawLevelEditingViewports();
		}
		return;
	}
	bool bChanged = bDuplicateOperation;
	for (const FActorState& State : ActorStates)
	{
		if (AActor* Actor = State.Actor.Get())
		{
			bChanged |= !Actor->GetActorTransform().Equals(State.Transform);
			Actor->PostEditMove(true);
		}
	}
	if (!bChanged)
	{
		CancelActiveTransform();
		return;
	}
	Transaction.Reset();
	ResetTransformState();
	if (GEditor)
	{
		GEditor->RedrawLevelEditingViewports();
	}
}

void FBlenderStyleEdMode::ResetTransformState()
{
	if (bChangedWidgetVisibility && !IsMeshEditing())
	{
		if (FEditorModeTools* ModeManager = GetModeManager())
		{
			ModeManager->SetShowWidget(bOriginalShowWidget);
		}
		bChangedWidgetVisibility = false;
	}
	Operation = EBlenderTransformOperation::None;
	Axis = INDEX_NONE;
	bLocalAxis = false;
	bDuplicateOperation = false;
	bSnap = false;
	NumericInput.Reset();
	ActorStates.Reset();
	OriginalSelection.Reset();
	DuplicatedActors.Reset();
	OriginalPackageDirty.Reset();
	TransformWorld.Reset();
	ActiveClient = nullptr;
	ActiveViewport = nullptr;
	bHasViewProjection = false;
	WorldUnitsPerPixel = 1.0;
}

FVector FBlenderStyleEdMode::GetConstraintAxis() const
{
	if (Axis == INDEX_NONE)
	{
		return ViewForward;
	}
	FVector Direction = FVector::ZeroVector;
	Direction[Axis] = 1.0;
	return bLocalAxis ? LocalOrientation.RotateVector(Direction) : Direction;
}

bool FBlenderStyleEdMode::GetActiveAxisGuide(FVector& Center, FVector& Direction, FLinearColor& Color) const
{
	const bool bExtruding = TopologyOperation == EBlenderTopologyOperation::ExtrudeRegion || TopologyOperation == EBlenderTopologyOperation::ExtrudeAlongNormals;
	if ((!IsTransforming() && !bExtruding) || Axis < 0 || Axis > 2)
	{
		return false;
	}
	static const FLinearColor AxisColors[] = {
		FLinearColor(0.9f, 0.1f, 0.1f), FLinearColor(0.2f, 0.8f, 0.2f), FLinearColor(0.15f, 0.35f, 1.0f)
	};
	Center = Pivot;
	Direction = bExtruding ? TopologyDirection : GetConstraintAxis().GetSafeNormal();
	Color = AxisColors[Axis];
	return true;
}

FVector FBlenderStyleEdMode::GetMouseTranslation() const
{
	const FVector2D Delta = VirtualMouse - StartMouse;
	FVector Movement = (ViewRight * Delta.X - ViewUp * Delta.Y) * WorldUnitsPerPixel;
	if (bHasViewProjection)
	{
		FVector StartOrigin, StartDirection, CurrentOrigin, CurrentDirection;
		FSceneView::DeprojectScreenToWorld(StartMouse, ViewRect, InverseViewProjection, StartOrigin, StartDirection);
		FSceneView::DeprojectScreenToWorld(VirtualMouse, ViewRect, InverseViewProjection, CurrentOrigin, CurrentDirection);
		FVector StartPoint, CurrentPoint;
		if (Axis != INDEX_NONE)
		{
			const FVector Direction = GetConstraintAxis();
			const FVector PlaneNormal = ViewForward - Direction * FVector::DotProduct(ViewForward, Direction);
			if (PlaneNormal.SizeSquared() > 0.01 && IntersectPlane(StartOrigin, StartDirection, Pivot, PlaneNormal, StartPoint)
				&& IntersectPlane(CurrentOrigin, CurrentDirection, Pivot, PlaneNormal, CurrentPoint))
			{
				return Direction * FVector::DotProduct(CurrentPoint - StartPoint, Direction);
			}
		}
		else if (IntersectPlane(StartOrigin, StartDirection, Pivot, ViewForward, StartPoint)
			&& IntersectPlane(CurrentOrigin, CurrentDirection, Pivot, ViewForward, CurrentPoint))
		{
			Movement = CurrentPoint - StartPoint;
		}
	}
	if (Axis != INDEX_NONE)
	{
		const FVector Direction = GetConstraintAxis();
		// A depth-facing axis has no useful screen projection; horizontal mouse motion controls it.
		const double ScreenProjection = FMath::Square(FVector::DotProduct(Direction, ViewRight)) + FMath::Square(FVector::DotProduct(Direction, ViewUp));
		return Direction * (ScreenProjection < 0.01 ? Delta.X * WorldUnitsPerPixel : FVector::DotProduct(Movement, Direction));
	}
	return Movement;
}

double FBlenderStyleEdMode::GetMouseRotationDegrees() const
{
	const FVector2D From = StartMouse - PivotPixel;
	const FVector2D To = VirtualMouse - PivotPixel;
	if (From.SizeSquared() < 400 || To.SizeSquared() < 25)
	{
		return (VirtualMouse.X - StartMouse.X) * 0.5;
	}
	const double Angle = FMath::Atan2(From.X * To.Y - From.Y * To.X, FVector2D::DotProduct(From, To));
	// Screen Y points down, so screen-space clockwise is negative about the camera forward axis.
	const double Facing = FVector::DotProduct(GetConstraintAxis(), ViewForward) < 0 ? 1.0 : -1.0;
	return FMath::RadiansToDegrees(Angle) * Facing;
}

double FBlenderStyleEdMode::GetMouseScale() const
{
	return 1.0 + (VirtualMouse.X - StartMouse.X - VirtualMouse.Y + StartMouse.Y) * 0.01;
}

bool FBlenderStyleEdMode::HasNumericValue(double& OutValue) const
{
	if (NumericInput.IsEmpty() || NumericInput == TEXT("-") || NumericInput == TEXT(".") || NumericInput == TEXT("-."))
	{
		return false;
	}
	OutValue = FCString::Atod(*NumericInput);
	return FMath::IsFinite(OutValue) && FMath::Abs(OutValue) <= 1.e9;
}

void FBlenderStyleEdMode::ApplyTransform()
{
	if (!IsTransforming() || bApplying || !TransformWorld.IsValid())
	{
		return;
	}
	double NumericValue = 0;
	const bool bNumeric = HasNumericValue(NumericValue);
	if (!NumericInput.IsEmpty() && !bNumeric)
	{
		return;
	}
	TGuardValue<bool> Guard(bApplying, true);
	FVector Translation = FVector::ZeroVector;
	double Amount = 0;
	FQuat Rotation = FQuat::Identity;
	if (Operation == EBlenderTransformOperation::Move)
	{
		Translation = GetMouseTranslation();
		if (bNumeric)
		{
			// Free numeric movement follows the mouse direction; X is the stable zero-motion fallback.
			Translation = (Axis != INDEX_NONE ? GetConstraintAxis() : Translation.GetSafeNormal(UE_SMALL_NUMBER, FVector::ForwardVector)) * NumericValue;
		}
		else if (bSnap && GEditor)
		{
			const double Grid = FMath::Max(double(GEditor->GetGridSize()), 0.001);
			if (Axis != INDEX_NONE)
			{
				const FVector Direction = GetConstraintAxis();
				Translation = Direction * FMath::GridSnap(FVector::DotProduct(Translation, Direction), Grid);
			}
			else
			{
				Translation = Translation.GridSnap(Grid);
			}
		}
	}
	else if (Operation == EBlenderTransformOperation::Rotate)
	{
		Amount = bNumeric ? NumericValue : GetMouseRotationDegrees();
		if (!bNumeric && bSnap && GEditor)
		{
			Amount = FMath::GridSnap(Amount, FMath::Max(double(GEditor->GetRotGridSize().Yaw), 0.001));
		}
		Rotation = FQuat(GetConstraintAxis().GetSafeNormal(), FMath::DegreesToRadians(Amount));
	}
	else
	{
		Amount = bNumeric ? NumericValue : GetMouseScale();
		if (!bNumeric && bSnap && GEditor)
		{
			Amount = FMath::GridSnap(Amount, FMath::Max(double(GEditor->GetScaleGridSize()), 0.001));
		}
	}
	if (IsMeshEditing())
	{
		const FVector Direction = GetConstraintAxis().GetSafeNormal();
		MeshSession->PreviewTransform([this, Translation, Rotation, Amount, Direction](const FVector& Position)
		{
			if (Operation == EBlenderTransformOperation::Move)
			{
				return Position + Translation;
			}
			const FVector Offset = Position - Pivot;
			if (Operation == EBlenderTransformOperation::Rotate)
			{
				return Pivot + Rotation.RotateVector(Offset);
			}
			// Geometry can express a world/local-axis stretch exactly, including shear.
			return Axis == INDEX_NONE ? Pivot + Offset * Amount
				: Position + Direction * (FVector::DotProduct(Offset, Direction) * (Amount - 1.0));
		});
	}
	for (const FActorState& State : ActorStates)
	{
		AActor* Actor = State.Actor.Get();
		if (!IsUsableActor(Actor, TransformWorld.Get()))
		{
			continue;
		}
		FTransform Result = State.Transform;
		if (Operation == EBlenderTransformOperation::Move)
		{
			Result.SetLocation(State.Transform.GetLocation() + Translation);
		}
		else if (Operation == EBlenderTransformOperation::Rotate)
		{
			Result.SetLocation(Pivot + Rotation.RotateVector(State.Transform.GetLocation() - Pivot));
			Result.SetRotation((Rotation * State.Transform.GetRotation()).GetNormalized());
		}
		else if (Axis == INDEX_NONE)
		{
			Result.SetLocation(Pivot + (State.Transform.GetLocation() - Pivot) * Amount);
			Result.SetScale3D(State.Transform.GetScale3D() * Amount);
		}
		else
		{
			// A general world-axis stretch would introduce shear. Actors expose local TRS only.
			FVector Scale = State.Transform.GetScale3D();
			Scale[Axis] *= Amount;
			Result.SetScale3D(Scale);
		}
		if (!Result.ContainsNaN())
		{
			Actor->SetActorTransform(Result, false, nullptr, ETeleportType::TeleportPhysics);
			Actor->PostEditMove(false);
		}
	}
	if (GEditor)
	{
		GEditor->RedrawLevelEditingViewports();
	}
}

void FBlenderStyleEdMode::SetAxis(int32 InAxis)
{
	const bool bActorScale = Operation == EBlenderTransformOperation::Scale && !IsMeshEditing();
	if (Axis != InAxis)
	{
		Axis = InAxis;
		bLocalAxis = bActorScale;
	}
	else if (bActorScale || bLocalAxis)
	{
		Axis = INDEX_NONE;
		bLocalAxis = false;
	}
	else
	{
		bLocalAxis = true;
	}
	ApplyTransform();
}

bool FBlenderStyleEdMode::HandleNumericKey(FKey Key)
{
	static const FKey TopDigits[] = {EKeys::Zero, EKeys::One, EKeys::Two, EKeys::Three, EKeys::Four, EKeys::Five, EKeys::Six, EKeys::Seven, EKeys::Eight, EKeys::Nine};
	static const FKey PadDigits[] = {EKeys::NumPadZero, EKeys::NumPadOne, EKeys::NumPadTwo, EKeys::NumPadThree, EKeys::NumPadFour, EKeys::NumPadFive, EKeys::NumPadSix, EKeys::NumPadSeven, EKeys::NumPadEight, EKeys::NumPadNine};
	for (int32 Digit = 0; Digit < 10; ++Digit)
	{
		if (Key == TopDigits[Digit] || Key == PadDigits[Digit])
		{
			if (NumericInput.Len() < 16)
			{
				NumericInput.AppendChar(TCHAR('0' + Digit));
			}
			if (IsTopologyModal()) ApplyTopologyEdit();
			else ApplyTransform();
			return true;
		}
	}
	if (Key == EKeys::Hyphen || Key == EKeys::Subtract)
	{
		NumericInput = NumericInput.StartsWith(TEXT("-")) ? NumericInput.RightChop(1) : TEXT("-") + NumericInput;
	}
	else if (Key == EKeys::Period || Key == EKeys::Decimal)
	{
		if (!NumericInput.Contains(TEXT(".")))
		{
			NumericInput += TEXT(".");
		}
	}
	else if (Key == EKeys::BackSpace)
	{
		NumericInput.LeftChopInline(1, EAllowShrinking::No);
	}
	else
	{
		return false;
	}
	if (IsTopologyModal()) ApplyTopologyEdit();
	else ApplyTransform();
	return true;
}

bool FBlenderStyleEdMode::CanStartTopologyTool(FEditorViewportClient* Client, FViewport* Viewport) const
{
	return bViewportControlsActive && IsMeshEditing() && !IsTopologyModal() && !IsTransforming() && !IsSelecting()
		&& GEditor && !GEditor->PlayWorld && Client && Viewport && Client->IsLevelEditorClient() && !Client->IsTracking()
		&& Client->GetWorld() == MeshSession->GetWorld() && Client->GetWorld() == GEditor->GetEditorWorldContext().World()
		&& (!Navigation || !Navigation->IsNavigating());
}

void FBlenderStyleEdMode::CacheTopologyDirection(const FVector& Direction)
{
	TopologyDirection = Direction.GetSafeNormal(UE_SMALL_NUMBER, FVector::UpVector);
	const FVector2D Projected(FVector::DotProduct(TopologyDirection, ViewRight), -FVector::DotProduct(TopologyDirection, ViewUp));
	const double Length = Projected.Size();
	// When the normal faces the camera there is no meaningful projected axis.
	// Keep a stable horizontal control instead of magnifying near-zero projections.
	TopologyScreenDirection = Length < 0.1 ? FVector2D(1, 0) : Projected / Length;
	TopologyUnitsPerPixel = WorldUnitsPerPixel / (Length < 0.1 ? 1.0 : Length);
}

bool FBlenderStyleEdMode::StartExtrude(bool bAlongNormals, FEditorViewportClient* Client, FViewport* Viewport)
{
	if (!CanStartTopologyTool(Client, Viewport)) return false;
	if (MeshSession->GetSelectionMode() != EBlenderMeshSelectionMode::Face)
	{
		ShowNotice(LOCTEXT("ExtrudeFacesOnly", "挤出需要面选择：按 3 切换到面模式，选择面后再按 E。"));
		return false;
	}
	FText Error;
	if (!MeshSession->BeginExtrude(bAlongNormals, Error))
	{
		ShowNotice(Error);
		return false;
	}
	TopologyOperation = bAlongNormals ? EBlenderTopologyOperation::ExtrudeAlongNormals : EBlenderTopologyOperation::ExtrudeRegion;
	ActiveClient = Client;
	ActiveViewport = Viewport;
	TransformWorld = Client->GetWorld();
	Axis = INDEX_NONE;
	NumericInput.Reset();
	TopologyValue = 0.0;
	TopologyHint = FText::GetEmpty();
	LastNotice = FText::GetEmpty();
	bSnap = IsControlDown(Viewport);
	bTopologyPrecision = IsShiftDown(Viewport);
	CacheModalMouse(Client, Viewport, MeshSession->GetSelectionCenter());
	CacheTopologyDirection(MeshSession->GetExtrudeDirection());
	Client->Invalidate();
	return true;
}

bool FBlenderStyleEdMode::StartLoopCut(FEditorViewportClient* Client, FViewport* Viewport)
{
	if (!CanStartTopologyTool(Client, Viewport)) return false;
	TopologyOperation = EBlenderTopologyOperation::LoopHover;
	ActiveClient = Client;
	ActiveViewport = Viewport;
	TransformWorld = Client->GetWorld();
	NumericInput.Reset();
	TopologyValue = 0.5;
	LoopObject = LoopEdge = INDEX_NONE;
	LoopPreview.Reset();
	LastNotice = FText::GetEmpty();
	TopologyHint = FText::GetEmpty();
	bSnap = IsControlDown(Viewport);
	bTopologyPrecision = IsShiftDown(Viewport);
	CacheModalMouse(Client, Viewport, MeshSession->GetGeometryBounds().GetCenter());
	UpdateLoopHover();
	return true;
}

void FBlenderStyleEdMode::OpenExtrudeMenu(FEditorViewportClient* Client, FViewport* Viewport)
{
	if (!CanStartTopologyTool(Client, Viewport) || !FSlateApplication::IsInitialized()) return;
	if (MeshSession->GetSelectionMode() != EBlenderMeshSelectionMode::Face)
	{
		ShowNotice(LOCTEXT("ExtrudeFacesOnly", "挤出需要面选择：按 3 切换到面模式，选择面后再按 E。"));
		return;
	}
	const TSharedPtr<SEditorViewport> Widget = Client->GetEditorViewportWidget();
	if (!Widget.IsValid()) return;
	const uint64 ExpectedSession = MeshSessionSerial;
	FMenuBuilder Menu(true, nullptr);
	Menu.BeginSection(TEXT("BlenderExtrude"), LOCTEXT("ExtrudeMenu", "挤出所选面"));
	const auto AddEntry = [this, &Menu, Client, Viewport, ExpectedSession](const FText& Label, const FText& Tooltip, bool bAlongNormals)
	{
		const auto CanExecute = [this, Client, Viewport, ExpectedSession]()
		{
			// A popup can outlive its viewport/session. Check membership before dereferencing it.
			return GEditor && bViewportControlsActive && MeshSessionSerial == ExpectedSession
				&& GEditor->GetLevelViewportClients().Contains(Client) && Client->Viewport == Viewport
				&& CanStartTopologyTool(Client, Viewport);
		};
		Menu.AddMenuEntry(Label, Tooltip, FSlateIcon(), FUIAction(
			FExecuteAction::CreateSPLambda(this, [this, Client, Viewport, bAlongNormals, CanExecute]()
			{
				if (CanExecute()) StartExtrude(bAlongNormals, Client, Viewport);
			}), FCanExecuteAction::CreateSPLambda(this, CanExecute)));
	};
	AddEntry(LOCTEXT("ExtrudeRegion", "挤出区域 · Region"), LOCTEXT("ExtrudeRegionTip", "沿所选区域的平均法线挤出；输入距离，单位厘米。"), false);
	AddEntry(LOCTEXT("ExtrudeNormals", "沿各自法线挤出 · Along Normals"), LOCTEXT("ExtrudeNormalsTip", "各顶点沿相邻所选面的平均法线移动；输入距离，单位厘米。"), true);
	Menu.EndSection();
	FSlateApplication::Get().PushMenu(Widget.ToSharedRef(), FWidgetPath(), Menu.MakeWidget(), FSlateApplication::Get().GetCursorPos(), FPopupTransitionEffect::ContextMenu);
}

void FBlenderStyleEdMode::UpdateLoopHover()
{
	if (TopologyOperation != EBlenderTopologyOperation::LoopHover || !IsMeshEditing() || !ActiveClient || !ActiveViewport) return;
	int32 Object = INDEX_NONE, Edge = INDEX_NONE;
	if (!MeshSession->FindLoopCutEdge(ActiveClient, ActiveViewport, Object, Edge))
	{
		LoopObject = LoopEdge = INDEX_NONE;
		LoopPreview.Reset();
		TopologyHint = LOCTEXT("LoopHoverHint", "将鼠标移到四边面边线上，预览一条环切。");
	}
	else if (Object != LoopObject || Edge != LoopEdge)
	{
		LoopObject = Object;
		LoopEdge = Edge;
		LoopPreview.Reset();
		FText Error;
		if (!MeshSession->GetLoopCutPreview(Object, Edge, LoopPreview, Error))
		{
			LoopPreview.Reset();
			TopologyHint = Error.IsEmpty() ? LOCTEXT("LoopNeedsQuads", "此处无法环切；需要连续的四边面条带。") : Error;
		}
		else TopologyHint = FText::GetEmpty();
	}
	ActiveClient->Invalidate();
}

bool FBlenderStyleEdMode::GetTopologyValue(double& OutValue) const
{
	if (TopologyOperation == EBlenderTopologyOperation::None || TopologyOperation == EBlenderTopologyOperation::LoopHover) return false;
	const bool bLoop = TopologyOperation == EBlenderTopologyOperation::LoopSlide;
	if (!NumericInput.IsEmpty())
	{
		if (!HasNumericValue(OutValue)) return false;
		if (bLoop && (OutValue < -1.0 || OutValue > 1.0)) return false;
		if (!bLoop && FMath::Abs(OutValue) > 1.e7) return false;
		// Blender exposes a signed slide factor: zero centers the loop, +/-1 approach its ends.
		// The session API uses the interpolation fraction between the two original edge endpoints.
		if (bLoop) OutValue = 0.5 + 0.5 * OutValue;
	}
	else
	{
		OutValue = (bLoop ? 0.5 : 0.0) + FVector2D::DotProduct(VirtualMouse - StartMouse, TopologyScreenDirection) * TopologyUnitsPerPixel;
		if (bSnap) OutValue = FMath::GridSnap(OutValue, bLoop ? 0.05 : FMath::Max(GEditor ? double(GEditor->GetGridSize()) : 1.0, 0.001));
	}
	if (!FMath::IsFinite(OutValue)) return false;
	OutValue = bLoop ? FMath::Clamp(OutValue, 0.001, 0.999) : FMath::Clamp(OutValue, -1.e7, 1.e7);
	return true;
}

bool FBlenderStyleEdMode::ApplyTopologyEdit()
{
	if (!IsTopologyModal() || !IsMeshEditing() || !MeshSession->IsTopologyEditing() || bApplying) return false;
	double Value = 0.0;
	if (!GetTopologyValue(Value)) return false;
	TGuardValue<bool> Guard(bApplying, true);
	const bool bUpdated = TopologyOperation == EBlenderTopologyOperation::LoopSlide
		? MeshSession->UpdateLoopCut(Value)
		: MeshSession->UpdateExtrude(Value, Axis == INDEX_NONE ? FVector::ZeroVector : TopologyDirection);
	if (bUpdated)
	{
		TopologyValue = Value;
		if (GEditor) GEditor->RedrawLevelEditingViewports();
	}
	return bUpdated;
}

void FBlenderStyleEdMode::ResetTopologyState()
{
	TopologyOperation = EBlenderTopologyOperation::None;
	TopologyDirection = FVector::UpVector;
	TopologyScreenDirection = FVector2D(1, 0);
	TopologyUnitsPerPixel = 1.0;
	TopologyValue = 0.0;
	bTopologyPrecision = false;
	LoopObject = LoopEdge = INDEX_NONE;
	LoopPreview.Reset();
	TopologyHint = FText::GetEmpty();
	NumericInput.Reset();
	Axis = INDEX_NONE;
	bSnap = false;
	ActiveClient = nullptr;
	ActiveViewport = nullptr;
	TransformWorld.Reset();
	bHasViewProjection = false;
	WorldUnitsPerPixel = 1.0;
}

void FBlenderStyleEdMode::AbortTopologyEdit()
{
	if (!IsTopologyModal()) return;
	if (IsMeshEditing() && MeshSession->IsTopologyEditing()) MeshSession->CancelTopologyEdit();
	ResetTopologyState();
	if (GEditor) GEditor->RedrawLevelEditingViewports();
}

void FBlenderStyleEdMode::FinishTopologyEdit(bool bKeepDisplacement)
{
	if (!IsTopologyModal()) return;
	if (!IsMeshEditing() || TopologyOperation == EBlenderTopologyOperation::LoopHover)
	{
		AbortTopologyEdit();
		return;
	}
	const bool bLoop = TopologyOperation == EBlenderTopologyOperation::LoopSlide;
	if (bKeepDisplacement)
	{
		if (!ApplyTopologyEdit()) return;
	}
	else if (!(bLoop ? MeshSession->UpdateLoopCut(0.5) : MeshSession->UpdateExtrude(0.0)))
	{
		AbortTopologyEdit();
		ShowNotice(LOCTEXT("TopologyResetFailed", "无法恢复本次操作，已撤销这次挤出或环切。"));
		return;
	}
	MeshSession->AcceptTopologyEdit();
	ResetTopologyState();
	LastNotice = bKeepDisplacement ? FText::GetEmpty()
		: bLoop ? LOCTEXT("LoopCentered", "已取消滑移，保留居中环切；Ctrl+Z 可撤销整次环切。")
		: LOCTEXT("ExtrudeZeroOffset", "已取消位移，保留零距离挤出（存在重叠顶点）；Ctrl+Z 可撤销整次挤出。");
	if (GEditor) GEditor->RedrawLevelEditingViewports();
}

bool FBlenderStyleEdMode::HandleTopologyInput(FEditorViewportClient* Client, FViewport* Viewport, FKey Key, EInputEvent Event)
{
	if (!IsTopologyModal()) return false;
	if (Client != ActiveClient || Viewport != ActiveViewport || !IsMeshEditing() || !TransformWorld.IsValid()
		|| !GEditor || GEditor->PlayWorld || Client->GetWorld() != MeshSession->GetWorld())
	{
		AbortTopologyEdit();
		return true;
	}
	// Explicit modifier releases matter even when FViewport still reports the old key state.
	if (Key == EKeys::LeftControl || Key == EKeys::RightControl)
		bSnap = Event != IE_Released || Viewport->KeyState(Key == EKeys::LeftControl ? EKeys::RightControl : EKeys::LeftControl);
	else bSnap = IsControlDown(Viewport);
	if (Key == EKeys::LeftShift || Key == EKeys::RightShift)
		bTopologyPrecision = Event != IE_Released || Viewport->KeyState(Key == EKeys::LeftShift ? EKeys::RightShift : EKeys::LeftShift);
	else bTopologyPrecision = IsShiftDown(Viewport);
	const bool bPressed = Event == IE_Pressed || Event == IE_DoubleClick;
	if (!bPressed && Event != IE_Repeat)
	{
		if (TopologyOperation != EBlenderTopologyOperation::LoopHover) ApplyTopologyEdit();
		return true;
	}
	if (bPressed && (Key == EKeys::Escape || Key == EKeys::RightMouseButton))
	{
		bSuppressRightRelease = Key == EKeys::RightMouseButton;
		FinishTopologyEdit(false);
		return true;
	}
	if (bSnap && Key == EKeys::Z)
	{
		if (bPressed) AbortTopologyEdit();
		return true;
	}
	if (bSnap && Key == EKeys::S)
	{
		if (bPressed) ShowNotice(LOCTEXT("FinishTopologyBeforeSaving", "请先完成挤出或环切，再按 Tab 提交网格，最后保存。"));
		return true;
	}
	if (bPressed && (Key == EKeys::Enter || Key == EKeys::LeftMouseButton))
	{
		bSuppressLeftRelease = Key == EKeys::LeftMouseButton;
		if (TopologyOperation == EBlenderTopologyOperation::LoopHover)
		{
			UpdateLoopHover();
			if (LoopPreview.IsEmpty()) return true;
			FText Error;
			if (!MeshSession->BeginLoopCut(LoopObject, LoopEdge, Error))
			{
				TopologyHint = Error;
				LoopPreview.Reset();
				return true;
			}
			TopologyOperation = EBlenderTopologyOperation::LoopSlide;
			LoopPreview.Reset();
			NumericInput.Reset();
			TopologyHint = FText::GetEmpty();
			TopologyValue = 0.5;
			CacheModalMouse(Client, Viewport, MeshSession->GetSelectionCenter());
			TopologyScreenDirection = FVector2D(1, 0);
			TopologyUnitsPerPixel = 1.0 / 400.0;
			FVector GuideStart, GuideEnd;
			if (MeshSession->GetLoopCutSlideGuide(GuideStart, GuideEnd))
			{
				const FVector Guide = GuideEnd - GuideStart;
				const FVector2D Projected(FVector::DotProduct(Guide, ViewRight), -FVector::DotProduct(Guide, ViewUp));
				const double Length = Projected.Size();
				if (FMath::IsFinite(Length) && Length > WorldUnitsPerPixel * 20.0)
				{
					TopologyScreenDirection = Projected / Length;
					TopologyUnitsPerPixel = WorldUnitsPerPixel / Length;
				}
			}
			Client->Invalidate();
		}
		else FinishTopologyEdit(true);
		return true;
	}
	if (TopologyOperation != EBlenderTopologyOperation::LoopHover)
	{
		if (TopologyOperation != EBlenderTopologyOperation::LoopSlide && !bSnap && (Key == EKeys::X || Key == EKeys::Y || Key == EKeys::Z))
		{
			if (bPressed)
			{
				const int32 RequestedAxis = Key == EKeys::X ? 0 : Key == EKeys::Y ? 1 : 2;
				Axis = Axis == RequestedAxis ? INDEX_NONE : RequestedAxis;
				FVector Direction = FVector::ZeroVector;
				if (Axis != INDEX_NONE) Direction[Axis] = 1.0;
				CacheTopologyDirection(Axis == INDEX_NONE ? MeshSession->GetExtrudeDirection() : Direction);
				ApplyTopologyEdit();
			}
			return true;
		}
		HandleNumericKey(Key);
	}
	// G/R/S, Tab, selection, undo/redo and native camera bindings cannot interrupt a modal topology operation.
	return true;
}

void FBlenderStyleEdMode::UpdateTopologyMouse(FEditorViewportClient* Client, FViewport* Viewport, int32 X, int32 Y)
{
	if (!IsTopologyModal()) return;
	if (Client != ActiveClient || Viewport != ActiveViewport)
	{
		AbortTopologyEdit();
		return;
	}
	if (TopologyOperation == EBlenderTopologyOperation::LoopHover)
	{
		UpdateLoopHover();
		return;
	}
	const FVector2D Position(X, Y);
	VirtualMouse += (Position - LastMouse) * (bTopologyPrecision ? 0.1 : 1.0);
	LastMouse = Position;
	ApplyTopologyEdit();
}

bool FBlenderStyleEdMode::InputKey(FEditorViewportClient* Client, FViewport* Viewport, FKey Key, EInputEvent Event)
{
	if (!Client || !Viewport || !Client->IsLevelEditorClient())
	{
		return false;
	}
	// Modal tools may consume modifier releases, but navigation must never retain stale keys.
	if (Navigation && (Key == EKeys::LeftShift || Key == EKeys::RightShift || Key == EKeys::LeftControl
		|| Key == EKeys::RightControl || Key == EKeys::LeftAlt || Key == EKeys::RightAlt))
		Navigation->InputKey(Client, Viewport, Key, Event, FBox(ForceInit));
	if (Event == IE_Released && ((Key == EKeys::LeftMouseButton && bSuppressLeftRelease) || (Key == EKeys::RightMouseButton && bSuppressRightRelease)))
	{
		bSuppressLeftRelease = bSuppressLeftRelease && Key != EKeys::LeftMouseButton;
		bSuppressRightRelease = bSuppressRightRelease && Key != EKeys::RightMouseButton;
		return true;
	}
	if (HandleTopologyInput(Client, Viewport, Key, Event)) return true;
	if (IsTransforming())
	{
		if (Client != ActiveClient || Viewport != ActiveViewport)
		{
			CancelActiveTransform();
			return false;
		}
		bSnap = IsControlDown(Viewport);
		if (Event != IE_Pressed && Event != IE_Repeat)
		{
			ApplyTransform();
			return true;
		}
		if (Key == EKeys::Escape || Key == EKeys::RightMouseButton)
		{
			bSuppressRightRelease = Key == EKeys::RightMouseButton;
			CancelActiveTransform();
			return true;
		}
		if (Key == EKeys::Enter || Key == EKeys::LeftMouseButton)
		{
			bSuppressLeftRelease = Key == EKeys::LeftMouseButton;
			ConfirmActiveTransform();
			return true;
		}
		if (IsControlDown(Viewport) && Key == EKeys::Z)
		{
			CancelActiveTransform();
			return true;
		}
		if (Key == EKeys::X || Key == EKeys::Y || Key == EKeys::Z)
		{
			if (Event == IE_Pressed)
			{
				SetAxis(Key == EKeys::X ? 0 : Key == EKeys::Y ? 1 : 2);
			}
			return true;
		}
		HandleNumericKey(Key);
		return true;
	}
	if (!GEditor || GEditor->PlayWorld || IsTextEntryFocused() || !Client->GetWorld() || Client->GetWorld()->IsGameWorld()) return false;
	if (Navigation && Navigation->IsNavigating())
	{
		if (Key == EKeys::Escape && Event == IE_Pressed) Navigation->Cancel();
		else Navigation->InputKey(Client, Viewport, Key, Event, FBox(ForceInit));
		return true;
	}
	if (HandleSelectionInput(Client, Viewport, Key, Event)) return true;
	const bool bControlNow = IsControlDown(Viewport), bShiftNow = IsShiftDown(Viewport);
	const bool bAltNow = Viewport->KeyState(EKeys::LeftAlt) || Viewport->KeyState(EKeys::RightAlt);
	if (Event == IE_Pressed && !Viewport->KeyState(EKeys::LeftMouseButton) && !Viewport->KeyState(EKeys::MiddleMouseButton) && !Viewport->KeyState(EKeys::RightMouseButton))
	{
		if (Key == EKeys::Z && !bControlNow && Presentation)
		{
			if (bAltNow) Presentation->ToggleXRay();
			else if (bShiftNow)
			{
				if (Presentation->GetShading() == EBlenderViewportShading::Wireframe) Presentation->SetShading(static_cast<EBlenderViewportShading>(PreviousShading));
				else { PreviousShading = static_cast<int32>(Presentation->GetShading()); Presentation->SetShading(EBlenderViewportShading::Wireframe); }
			}
			else OpenShadingMenu(Client);
			return true;
		}
		if (IsMeshEditing() && bControlNow && !bAltNow && (Key == EKeys::Add || Key == EKeys::Subtract))
		{
			MeshSession->GrowSelection(Key == EKeys::Add); Client->Invalidate(); return true;
		}
		if (IsMeshEditing() && Key == EKeys::L && !bAltNow)
		{
			if (bControlNow) MeshSession->SelectLinked(true);
			else MeshSession->Pick(Client, Viewport, EBlenderPickAction::Linked, true, bShiftNow);
			Client->Invalidate(); return true;
		}
	}
	const bool bNeedsBounds = Key == EKeys::MiddleMouseButton || Key == EKeys::Decimal || Key == EKeys::Home;
	if (Navigation && Navigation->InputKey(Client, Viewport, Key, Event, bNeedsBounds ? GetNavigationBounds(Client, Key == EKeys::Home) : FBox(ForceInit))) return true;
	if (IsMeshEditing())
	{
		if (!GEditor || GEditor->PlayWorld || Client->GetWorld() != MeshSession->GetWorld() || IsTextEntryFocused())
		{
			return false;
		}
		if (Event != IE_Pressed || Client->IsTracking() || Viewport->KeyState(EKeys::LeftMouseButton)
			|| Viewport->KeyState(EKeys::MiddleMouseButton) || Viewport->KeyState(EKeys::RightMouseButton))
		{
			return false;
		}
		const bool bControl = IsControlDown(Viewport);
		const bool bShift = IsShiftDown(Viewport);
		const bool bAlt = Viewport->KeyState(EKeys::LeftAlt) || Viewport->KeyState(EKeys::RightAlt);
		if (Key == EKeys::E && !bControl && !bShift)
		{
			if (bAlt) OpenExtrudeMenu(Client, Viewport);
			else StartExtrude(false, Client, Viewport);
			return true;
		}
		if (Key == EKeys::R && bControl && !bAlt && !bShift)
		{
			StartLoopCut(Client, Viewport);
			return true;
		}
		if (bControl && Key == EKeys::S)
		{
			ShowNotice(LOCTEXT("FinishMeshBeforeSaving", "请先按 Tab 完成网格编辑，再按 Ctrl+Shift+S 保存模型和关卡。"));
			return true;
		}
		if (bControl && (Key == EKeys::Z || Key == EKeys::Y))
		{
			if (Key == EKeys::Y || bShift)
			{
				MeshSession->Redo();
			}
			else
			{
				MeshSession->Undo();
			}
			LastNotice = FText::GetEmpty();
			GEditor->RedrawLevelEditingViewports();
			return true;
		}
		if (Key == EKeys::Escape)
		{
			EndMeshEdit(false);
			return true;
		}
		if (Key == EKeys::Tab && !bControl && !bAlt && !bShift)
		{
			EndMeshEdit(true);
			return true;
		}
		if (Key == EKeys::A && !bControl && !bShift)
		{
			MeshSession->SelectAll(!bAlt);
			LastNotice = FText::GetEmpty();
			GEditor->RedrawLevelEditingViewports();
			return true;
		}
		if (!bControl && !bAlt && !bShift && (Key == EKeys::One || Key == EKeys::Two || Key == EKeys::Three))
		{
			MeshSession->SetSelectionMode(Key == EKeys::One ? EBlenderMeshSelectionMode::Vertex : Key == EKeys::Two ? EBlenderMeshSelectionMode::Edge : EBlenderMeshSelectionMode::Face);
			LastNotice = FText::GetEmpty();
			GEditor->RedrawLevelEditingViewports();
			return true;
		}
		if (!bControl && !bAlt && !bShift && (Key == EKeys::G || Key == EKeys::R || Key == EKeys::S))
		{
			StartTransform(Key == EKeys::G ? EBlenderTransformOperation::Move : Key == EKeys::R ? EBlenderTransformOperation::Rotate : EBlenderTransformOperation::Scale, Client, Viewport);
			return true;
		}
		if ((bShift && (Key == EKeys::D || Key == EKeys::A)) || Key == EKeys::Delete || Key == EKeys::BackSpace
			|| Key == EKeys::X || Key == EKeys::E || Key == EKeys::I || Key == EKeys::K
			|| (bControl && (Key == EKeys::D || Key == EKeys::W || Key == EKeys::V || Key == EKeys::A)))
		{
			ShowNotice(LOCTEXT("UnsupportedMeshCommand", "暂不支持这项网格命令。当前支持 G/R/S、面模式 E/Alt+E 挤出和 Ctrl+R 单条四边面环切。"));
			return true;
		}
		if (Key == EKeys::Left || Key == EKeys::Right || Key == EKeys::Up || Key == EKeys::Down)
		{
			// Ortho/Alt-arrow nudges normally move selected actors in UE; this edit session
			// owns geometry movement and must not change their source component transforms.
			return true;
		}
		return false;
	}
	if (Event != IE_Pressed || !GEditor || GEditor->PlayWorld || IsTextEntryFocused()
		|| Client->IsTracking() || Viewport->KeyState(EKeys::LeftMouseButton)
		|| Viewport->KeyState(EKeys::MiddleMouseButton) || Viewport->KeyState(EKeys::RightMouseButton) || IsControlDown(Viewport)
		|| Viewport->KeyState(EKeys::LeftAlt) || Viewport->KeyState(EKeys::RightAlt))
	{
		return false;
	}
	if (Key == EKeys::Tab && !IsShiftDown(Viewport))
	{
		BeginMeshEdit(Client, Viewport);
		return true;
	}
	if (IsShiftDown(Viewport))
	{
		if (Key == EKeys::A)
		{
			OpenAddMenu(Client, Viewport);
			return true;
		}
		if (Key == EKeys::D)
		{
			return StartTransform(EBlenderTransformOperation::Move, Client, Viewport, true);
		}
		return false;
	}
	if (Key == EKeys::G || Key == EKeys::R || Key == EKeys::S)
	{
		// Reserve the mode's keys even with no editable selection (G must not toggle game view).
		StartTransform(Key == EKeys::G ? EBlenderTransformOperation::Move : Key == EKeys::R ? EBlenderTransformOperation::Rotate : EBlenderTransformOperation::Scale, Client, Viewport);
		return true;
	}
	return false;
}

void FBlenderStyleEdMode::UpdateMouse(FEditorViewportClient* Client, FViewport* Viewport, int32 X, int32 Y)
{
	if (!IsTransforming() || Client != ActiveClient || Viewport != ActiveViewport)
	{
		return;
	}
	const FVector2D Position(X, Y);
	VirtualMouse += (Position - LastMouse) * (IsShiftDown(Viewport) ? 0.1 : 1.0);
	LastMouse = Position;
	bSnap = IsControlDown(Viewport);
	ApplyTransform();
}

bool FBlenderStyleEdMode::MouseMove(FEditorViewportClient* Client, FViewport* Viewport, int32 X, int32 Y)
{
	if (IsTopologyModal())
	{
		UpdateTopologyMouse(Client, Viewport, X, Y);
		return true;
	}
	if (IsTransforming())
	{
		UpdateMouse(Client, Viewport, X, Y);
		return true;
	}
	if (UpdateSelectionPointer(Client, Viewport, X, Y)) return true;
	if (Navigation && Navigation->MouseMove(Client, Viewport, X, Y)) return true;
	return false;
}

bool FBlenderStyleEdMode::InputAxis(FEditorViewportClient* Client, FViewport* Viewport, int32 ControllerId, FKey Key, float Delta, float DeltaTime)
{
	if (IsTopologyModal()) return true;
	if (!Client || !Client->IsLevelEditorClient() || !GEditor || GEditor->PlayWorld) return false;
	return Navigation && Navigation->InputAxis(Client, Viewport, Key, Delta);
}

bool FBlenderStyleEdMode::CapturedMouseMove(FEditorViewportClient* Client, FViewport* Viewport, int32 X, int32 Y)
{
	return MouseMove(Client, Viewport, X, Y);
}

bool FBlenderStyleEdMode::InputDelta(FEditorViewportClient* Client, FViewport* Viewport, FVector& Drag, FRotator& Rot, FVector& Scale)
{
	return true;
}

bool FBlenderStyleEdMode::LostFocus(FEditorViewportClient* Client, FViewport* Viewport)
{
	if (IsTopologyModal() && Client == ActiveClient) AbortTopologyEdit();
	CancelSelectionGesture();
	if (Navigation) Navigation->Cancel();
	if (IsTransforming() && Client == ActiveClient)
	{
		CancelActiveTransform();
	}
	bSuppressLeftRelease = bSuppressRightRelease = false;
	return false;
}

void FBlenderStyleEdMode::Tick(FEditorViewportClient* Client, float DeltaTime)
{
	FEdMode::Tick(Client, DeltaTime);
	if (bViewportControlsActive && GEditor && !GEditor->PlayWorld && Client && Client->IsLevelEditorClient())
	{
		if (Navigation) Navigation->ActivateViewport(Client);
		if (Presentation) Presentation->Tick(Client);
	}
	if (IsMeshEditing() && (!GEditor || GEditor->PlayWorld || !IsValid(MeshSession->GetWorld())
		|| MeshSession->GetWorld() != GEditor->GetEditorWorldContext().World()))
	{
		EndMeshEdit(false);
	}
	if (IsTransforming() && (!TransformWorld.IsValid() || !GEditor || GEditor->PlayWorld
		|| TransformWorld.Get() != GEditor->GetEditorWorldContext().World()))
	{
		CancelActiveTransform();
	}
	if (IsTopologyModal() && (!TransformWorld.IsValid() || !GEditor || GEditor->PlayWorld
		|| !GEditor->GetLevelViewportClients().Contains(ActiveClient)
		|| ActiveClient->Viewport != ActiveViewport || TransformWorld.Get() != GEditor->GetEditorWorldContext().World()))
	{
		AbortTopologyEdit();
	}
}

void FBlenderStyleEdMode::HandleMapChange(uint32 Flags)
{
	CancelSelectionGesture();
	if (Navigation) Navigation->ForgetViewports();
	CancelActiveTransform();
	EndMeshEdit(false);
	bHasPlacement = false;
	PlacementWorld.Reset();
}

void FBlenderStyleEdMode::MapChangeNotify()
{
	HandleMapChange(0);
}

void FBlenderStyleEdMode::HandlePreBeginPIE(bool bIsSimulating)
{
	CancelActiveTransform();
	EndMeshEdit(false);
	RestoreViewportControls();
	bHasPlacement = false;
	PlacementWorld.Reset();
}

void FBlenderStyleEdMode::HandleEndPIE(bool bIsSimulating)
{
	bViewportControlsActive = true;
	if (GEditor) for (auto* Client : GEditor->GetLevelViewportClients())
	{
		if (Navigation) Navigation->ActivateViewport(Client);
		if (Presentation) Presentation->ActivateViewport(Client);
	}
	SyncPresentation();
}

void FBlenderStyleEdMode::HandleModeChanged(const FEditorModeID& ChangedModeId, bool bIsEnteringMode)
{
	if (ChangedModeId == ModeId && bIsEnteringMode && !bViewportControlsActive)
	{
		// UE may reactivate a pending-deactivation mode before its deferred Exit/Enter pair.
		// Its input mode is live again, so restore our per-viewport helpers synchronously too.
		HandleEndPIE(false);
	}
	if (ChangedModeId == ModeId && !bIsEnteringMode)
	{
		// UE5.4 defers Exit until its next mode-manager tick. The user-facing off state
		// is immediate, so restore the preview synchronously with the deactivation event.
		CancelActiveTransform();
		EndMeshEdit(false);
		RestoreViewportControls();
		bHasPlacement = false;
		PlacementWorld.Reset();
		bSuppressLeftRelease = false;
		bSuppressRightRelease = false;
	}
}

FText FBlenderStyleEdMode::GetStatusText() const
{
	if (IsTopologyModal())
	{
		if (TopologyOperation == EBlenderTopologyOperation::LoopHover)
		{
			FString Status = TEXT("环切预览 · 单条四边面环切\n左键插入并滑移 · Esc/右键取消预览");
			if (!TopologyHint.IsEmpty()) Status += TEXT("\n") + TopologyHint.ToString();
			return FText::FromString(Status);
		}
		double Value = 0.0;
		const bool bValid = GetTopologyValue(Value);
		const bool bLoop = TopologyOperation == EBlenderTopologyOperation::LoopSlide;
		const FString ValueLabel = FString::Printf(TEXT("%.3f%s"), bLoop ? 2.0 * TopologyValue - 1.0 : TopologyValue, bLoop ? TEXT("") : TEXT(" cm"));
		FString Status;
		if (bLoop)
		{
			Status = TEXT("环切 · 滑移系数 ") + ValueLabel + TEXT("（0 居中；端点保留微小间距）\n数字输入 -1..1 · Ctrl 吸附 · Shift 精细 · 回车/左键确认\nEsc/右键保留居中环切 · Ctrl+Z 撤销整次环切");
		}
		else
		{
			const TCHAR AxisNames[] = TEXT("XYZ");
			const FString DirectionLabel = Axis == INDEX_NONE
				? TopologyOperation == EBlenderTopologyOperation::ExtrudeAlongNormals ? TEXT("各自法线") : TEXT("区域平均法线")
				: FString::Printf(TEXT("世界 %c 轴"), AxisNames[Axis]);
			Status = TEXT("面挤出 · ") + DirectionLabel + TEXT(" · ") + ValueLabel
				+ TEXT("\nXYZ 锁世界轴 / 同轴再按恢复法线 · 数字 cm · Ctrl 吸附 · Shift 精细\n回车/左键确认 · Esc/右键保留零距离挤出（重叠顶点）· Ctrl+Z 撤销");
		}
		if (!NumericInput.IsEmpty()) Status += TEXT("\n输入：") + NumericInput + (bValid ? TEXT("") : bLoop ? TEXT("（请输入 -1..1）") : TEXT("（请输入有效距离，绝对值不超过 10000000 cm）"));
		return FText::FromString(Status);
	}
	if (IsSelecting()) return FText::FromString(bBoxArmed ? TEXT("框选：左键拖动加选 · 中键拖动减选 · Esc/右键取消")
		: TEXT("拖动框选 · Shift 加选 · Ctrl 减选 · Ctrl+Shift 交集 · Esc 取消"));
	if (!IsTransforming())
	{
		FString Status = IsMeshEditing()
			? MeshSession->GetStatusText().ToString() + TEXT("\nE 区域挤出 · Alt+E 挤出菜单 · Ctrl+R 环切 · G/R/S 变换\nAlt点击 循环 · Ctrl点击 路径 · B 框选 · Alt+Z 透视 · Ctrl小键盘± 扩选/缩选\n中键环绕 · Shift中键平移 · 滚轮缩放")
			: TEXT("Blender 对象模式  |  G 移动  R 旋转  S 缩放  |  Tab 编辑所选模型\nShift+A 添加 · Shift+D 复制 · B 框选 · Z 着色 · Alt+Z 透视\n中键环绕 · Shift中键平移 · 滚轮缩放 · 小键盘. 聚焦选择");
		if (!LastNotice.IsEmpty())
		{
			Status += TEXT("\n") + LastNotice.ToString();
		}
		return FText::FromString(Status);
	}
	const TCHAR* Name = Operation == EBlenderTransformOperation::Move ? TEXT("移动") : Operation == EBlenderTransformOperation::Rotate ? TEXT("旋转") : TEXT("缩放");
	const TCHAR* Unit = Operation == EBlenderTransformOperation::Move ? TEXT("cm") : Operation == EBlenderTransformOperation::Rotate ? TEXT("°") : TEXT("倍");
	FString AxisLabel;
	if (Axis == INDEX_NONE)
	{
		AxisLabel = Operation == EBlenderTransformOperation::Scale ? TEXT("统一 / 选择中心") : Operation == EBlenderTransformOperation::Move ? TEXT("视图平面") : TEXT("视线轴 / 选择中心");
	}
	else
	{
		const TCHAR AxisNames[] = TEXT("XYZ");
		AxisLabel = FString::Printf(TEXT("%c %s%s"), AxisNames[Axis], bLocalAxis ? TEXT("本地轴") : TEXT("世界轴"), Operation == EBlenderTransformOperation::Scale && !IsMeshEditing() ? TEXT(" / 各自原点") : bLocalAxis ? TEXT(" / 首个物体方向") : TEXT(" / 选择中心"));
	}
	double Number = 0;
	FString Value = NumericInput;
	if (NumericInput.IsEmpty())
	{
		Number = Operation == EBlenderTransformOperation::Move ? GetMouseTranslation().Length() : Operation == EBlenderTransformOperation::Rotate ? GetMouseRotationDegrees() : GetMouseScale();
		Value = FString::Printf(TEXT("%.3f"), Number);
	}
	const FString ModeLabel = IsMeshEditing() ? MeshSession->GetStatusText().ToString() : TEXT("Blender 对象模式");
	const TCHAR* AxisHint = Operation == EBlenderTransformOperation::Scale && !IsMeshEditing()
		? TEXT("XYZ 本地轴 / 再按取消") : TEXT("XYZ 世界轴 / 再按本地 / 三按取消");
	return FText::FromString(FString::Printf(TEXT("%s\n%s%s  |  %s  |  %s %s%s\n%s  ·  数字输入  ·  Ctrl 吸附  ·  Shift 精细  ·  回车/左键确认  ·  Esc/右键取消"),
		*ModeLabel, bDuplicateOperation ? TEXT("复制 + ") : TEXT(""), Name, *AxisLabel, *Value, Unit, bSnap && NumericInput.IsEmpty() ? TEXT(" [吸附]") : TEXT(""), AxisHint));
}

void FBlenderStyleEdMode::Render(const FSceneView* View, FViewport* Viewport, FPrimitiveDrawInterface* PDI)
{
	FEdMode::Render(View, Viewport, PDI);
	if (Presentation) Presentation->RenderOverlays(View, Viewport, PDI);
	if (IsMeshEditing() && (!Presentation || Presentation->GetOverlaySettings().bShowOverlays))
	{
		MeshSession->Render(View, Viewport, PDI);
	}
	if (PDI && Viewport == ActiveViewport && TopologyOperation == EBlenderTopologyOperation::LoopHover)
	{
		for (const auto& Segment : LoopPreview)
			PDI->DrawLine(Segment.Key, Segment.Value, FLinearColor(1.0f, 0.8f, 0.05f), SDPG_Foreground, 2.5f);
	}
	FVector Center, Direction;
	FLinearColor Color;
	if (View && PDI && GetActiveAxisGuide(Center, Direction, Color))
	{
		// The guide follows the exact constraint frame used by transform math.
		const double HalfLength = FMath::Max(100000.0, (View->ViewMatrices.GetViewOrigin() - Center).Length() * 4.0);
		PDI->DrawLine(Center - Direction * HalfLength, Center + Direction * HalfLength, Color, SDPG_Foreground, 1.5f);
	}
}

void FBlenderStyleEdMode::DrawHUD(FEditorViewportClient* Client, FViewport* Viewport, const FSceneView* View, FCanvas* Canvas)
{
	FEdMode::DrawHUD(Client, Viewport, View, Canvas);
	DrawSelectionHUD(Viewport, Canvas);
	if (Client && Client->IsLevelEditorClient() && Canvas && GEngine && (!Presentation || (Presentation->GetOverlaySettings().bShowOverlays && Presentation->GetOverlaySettings().bTextInfo)))
	{
		FCanvasTextItem Text(FVector2D(20, 95), GetStatusText(), FCoreStyle::GetDefaultFontStyle(TEXT("Regular"), 12), IsTransforming() || IsTopologyModal() ? FLinearColor(0.4f, 1.0f, 0.65f) : FLinearColor(0.85f, 0.95f, 1.0f));
		Text.EnableShadow(FLinearColor::Black);
		Canvas->DrawItem(Text);
	}
}

void FBlenderStyleEdMode::OpenAddMenu(FEditorViewportClient* Client, FViewport* Viewport)
{
	if (!Client || !Viewport || !FSlateApplication::IsInitialized() || !GEditor || GEditor->PlayWorld || IsMeshEditing())
	{
		return;
	}
	const TSharedPtr<SEditorViewport> Widget = Client->GetEditorViewportWidget();
	UWorld* World = Client->GetWorld();
	if (!Widget.IsValid() || !World || World->IsGameWorld())
	{
		return;
	}
	PlacementWorld = World;
	PlacementNormal = FVector::UpVector;
	const FViewportCursorLocation Cursor = Client->GetCursorWorldLocationFromMousePos();
	FHitResult Hit;
	FCollisionQueryParams Params(SCENE_QUERY_STAT(BlenderAddPrimitive), true);
	bPlacementHit = World->LineTraceSingleByChannel(Hit, Cursor.GetOrigin(), Cursor.GetOrigin() + Cursor.GetDirection() * 1000000.0, ECC_Visibility, Params);
	if (bPlacementHit)
	{
		PlacementLocation = Hit.ImpactPoint;
		PlacementNormal = Hit.ImpactNormal.GetSafeNormal();
	}
	else
	{
		PlacementLocation = Client->GetViewLocation() + Client->GetViewRotation().Vector() * 500.0;
	}
	bHasPlacement = true;
	// Only the mode is weakly bound. Placement data is captured now, before the menu steals focus.
	FMenuBuilder Menu(true, nullptr);
	Menu.BeginSection(TEXT("BlenderBasicShapes"), LOCTEXT("AddShapeSection", "添加基础形状"));
	const auto AddEntry = [this, &Menu](const FText& Label, const TCHAR* Path)
	{
		Menu.AddMenuEntry(Label, LOCTEXT("ShapeTooltip", "添加静态网格物体；立方体保留六个四边面，可直接环切。"), FSlateIcon(), FUIAction(FExecuteAction::CreateSPLambda(this, [this, MeshPath = FString(Path)]()
		{
			if (bHasPlacement && PlacementWorld.IsValid())
			{
				AddPrimitive(MeshPath);
			}
		}), FCanExecuteAction::CreateSPLambda(this, [this]()
		{
			return bHasPlacement && PlacementWorld.IsValid() && GEditor && !GEditor->PlayWorld;
		})));
	};
	AddEntry(LOCTEXT("Cube", "立方体 Cube"), TEXT("/Engine/BasicShapes/Cube.Cube"));
	AddEntry(LOCTEXT("Plane", "平面 Plane"), TEXT("/Engine/BasicShapes/Plane.Plane"));
	AddEntry(LOCTEXT("Sphere", "球体 Sphere"), TEXT("/Engine/BasicShapes/Sphere.Sphere"));
	AddEntry(LOCTEXT("Cylinder", "圆柱 Cylinder"), TEXT("/Engine/BasicShapes/Cylinder.Cylinder"));
	AddEntry(LOCTEXT("Cone", "圆锥 Cone"), TEXT("/Engine/BasicShapes/Cone.Cone"));
	Menu.EndSection();
	FSlateApplication::Get().PushMenu(Widget.ToSharedRef(), FWidgetPath(), Menu.MakeWidget(), FSlateApplication::Get().GetCursorPos(), FPopupTransitionEffect(FPopupTransitionEffect::ContextMenu));
}

void FBlenderStyleEdMode::AddPrimitive(const FString& EngineMeshPath)
{
	if (!GEditor || GEditor->PlayWorld || IsTransforming() || IsMeshEditing())
	{
		return;
	}
	UWorld* World = bHasPlacement ? PlacementWorld.Get() : GEditor->GetEditorWorldContext().World();
	if (!World || World->IsGameWorld() || World != GEditor->GetEditorWorldContext().World() || !World->GetCurrentLevel() || FLevelUtils::IsLevelLocked(World->GetCurrentLevel()))
	{
		return;
	}
	UStaticMesh* Mesh = EngineMeshPath == TEXT("/Engine/BasicShapes/Cube.Cube")
		? CreateBlenderQuadCube() : LoadObject<UStaticMesh>(nullptr, *EngineMeshPath);
	if (!Mesh)
	{
		return;
	}
	FVector Location = bHasPlacement ? PlacementLocation : FVector::ZeroVector;
	if (bHasPlacement && bPlacementHit)
	{
		const FBoxSphereBounds Bounds = Mesh->GetBounds();
		const FVector Extent = Bounds.BoxExtent;
		Location += PlacementNormal * FVector::DotProduct(PlacementNormal.GetAbs(), Extent) - Bounds.Origin;
	}
	const FScopedTransaction AddTransaction(LOCTEXT("AddTransaction", "Blender 添加基础形状"));
	GEditor->GetSelectedActors()->Modify();
	World->GetCurrentLevel()->Modify();
	AStaticMeshActor* Actor = Cast<AStaticMeshActor>(GEditor->AddActor(World->GetCurrentLevel(), AStaticMeshActor::StaticClass(), FTransform(Location), false, RF_Transactional, false));
	if (!Actor)
	{
		return;
	}
	Actor->Modify();
	Actor->GetStaticMeshComponent()->Modify();
	Actor->GetStaticMeshComponent()->SetStaticMesh(Mesh);
	Actor->SetActorLabel(TEXT("Blender_") + Mesh->GetName());
	Actor->PostEditChange();
	Actor->PostEditMove(true);
	GEditor->SelectNone(false, true, false);
	GEditor->SelectActor(Actor, true, true);
	GEditor->RedrawLevelEditingViewports();
}

#undef LOCTEXT_NAMESPACE
