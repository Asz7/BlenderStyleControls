#include "BlenderStyleEdMode.h"
#include "BlenderMeshEditSession.h"
#include "BlenderViewportNavigation.h"
#include "BlenderViewportPresentation.h"
#include "CanvasItem.h"
#include "CanvasTypes.h"
#include "Editor.h"
#include "EditorViewportClient.h"
#include "Engine/Selection.h"
#include "EngineUtils.h"
#include "Framework/Application/SlateApplication.h"
#include "Framework/MultiBox/MultiBoxBuilder.h"
#include "SceneView.h"
#include "SEditorViewport.h"
#include "UnrealClient.h"

void FBlenderStyleEdMode::SyncPresentation()
{
    if (!Presentation) return;
    if (Presentation->GetShading() != EBlenderViewportShading::Wireframe)
        PreviousShading = static_cast<int32>(Presentation->GetShading());
    Presentation->SetMeshEditActive(IsMeshEditing());
    if (IsMeshEditing())
    {
        MeshSession->SetXRay(Presentation->IsXRay());
        MeshSession->SetPreviewMaterial(Presentation->GetPreviewMaterial());
    }
    if (GEditor) GEditor->RedrawLevelEditingViewports();
}

void FBlenderStyleEdMode::RestoreViewportControls()
{
    bViewportControlsActive = false;
    AbortTopologyEdit();
    CancelSelectionGesture();
    if (Navigation) Navigation->ForgetViewports();
    if (Presentation) Presentation->Restore();
}

FBox FBlenderStyleEdMode::GetNavigationBounds(FEditorViewportClient* Client, bool bAll) const
{
    FBox Bounds(ForceInit);
    if (!Client || !GEditor) return Bounds;
    if (IsMeshEditing())
    {
        if (!bAll)
        {
            for (const auto& Vertex : MeshSession->GetSelectedVertices()) Bounds += Vertex.WorldPosition;
        }
        else for (int32 Object = 0; Object < MeshSession->GetObjectCount(); ++Object)
        {
            for (int32 ID : MeshSession->GetElementIDs(Object, EBlenderMeshSelectionMode::Vertex))
            {
                FVector Point;
                if (MeshSession->GetVertexWorldPosition(Object, ID, Point)) Bounds += Point;
            }
        }
        return Bounds;
    }
    for (TActorIterator<AActor> It(Client->GetWorld()); It; ++It)
    {
        AActor* Actor = *It;
        if (!IsUsableActor(Actor, Client->GetWorld()) || Actor->IsHiddenEd() || (!bAll && !Actor->IsSelected())) continue;
        const FBox ActorBounds = Actor->GetComponentsBoundingBox(true);
        if (ActorBounds.IsValid) Bounds += ActorBounds;
        else Bounds += Actor->GetActorLocation();
    }
    return Bounds;
}

void FBlenderStyleEdMode::CancelSelectionGesture()
{
    bBoxArmed = bPointerPending = bBoxDragging = false;
    bGestureShift = bGestureControl = bGestureAlt = false;
    GestureClient = nullptr; GestureViewport = nullptr; GestureButton = EKeys::Invalid;
    if (GEditor) GEditor->RedrawLevelEditingViewports();
}

bool FBlenderStyleEdMode::HandleSelectionInput(FEditorViewportClient* Client, FViewport* Viewport, FKey Key, EInputEvent Event)
{
    if (IsTopologyModal()) return true;
    if (IsSelecting() && (Client != GestureClient || Viewport != GestureViewport)) CancelSelectionGesture();
    if (IsSelecting())
    {
        if ((Key == EKeys::Escape || Key == EKeys::RightMouseButton) && Event == IE_Pressed)
        {
            bSuppressRightRelease = Key == EKeys::RightMouseButton;
            CancelSelectionGesture(); return true;
        }
        if (bBoxArmed && !bPointerPending && (Key == EKeys::LeftMouseButton || Key == EKeys::MiddleMouseButton) && Event == IE_Pressed)
        {
            bPointerPending = true; bBoxDragging = true; GestureButton = Key;
            GestureStart = GestureEnd = FIntPoint(Viewport->GetMouseX(), Viewport->GetMouseY());
            bGestureShift = IsShiftDown(Viewport); bGestureControl = IsControlDown(Viewport);
            return true;
        }
        if (bPointerPending && Key == GestureButton && Event == IE_Released)
        {
            ApplySelectionGesture(); CancelSelectionGesture(); return true;
        }
        // B is a modal gesture, and pending click/drag must not leak into UE tracking.
        return true;
    }
    const bool bShift = IsShiftDown(Viewport), bControl = IsControlDown(Viewport);
    const bool bAlt = Viewport->KeyState(EKeys::LeftAlt) || Viewport->KeyState(EKeys::RightAlt);
    if (Key == EKeys::B && Event == IE_Pressed && !bControl && !bAlt && !bShift
        && !Viewport->KeyState(EKeys::LeftMouseButton) && !Viewport->KeyState(EKeys::MiddleMouseButton)
        && !Viewport->KeyState(EKeys::RightMouseButton) && (!Navigation || !Navigation->IsNavigating()))
    {
        bBoxArmed = true; GestureClient = Client; GestureViewport = Viewport;
        GestureStart = GestureEnd = FIntPoint(Viewport->GetMouseX(), Viewport->GetMouseY());
        LastNotice = FText::GetEmpty(); Client->Invalidate(); return true;
    }
    if (Key == EKeys::LeftMouseButton)
    {
        if ((Event == IE_Pressed || Event == IE_DoubleClick) && !Viewport->KeyState(EKeys::MiddleMouseButton)
            && !Viewport->KeyState(EKeys::RightMouseButton))
        {
            bPointerPending = true; GestureClient = Client; GestureViewport = Viewport; GestureButton = Key;
            GestureStart = GestureEnd = FIntPoint(Viewport->GetMouseX(), Viewport->GetMouseY());
            bGestureShift = bShift; bGestureControl = bControl; bGestureAlt = bAlt;
        }
        return true;
    }
    return false;
}

bool FBlenderStyleEdMode::UpdateSelectionPointer(FEditorViewportClient* Client, FViewport* Viewport, int32 X, int32 Y)
{
    if (!IsSelecting()) return false;
    if (Client != GestureClient || Viewport != GestureViewport) { CancelSelectionGesture(); return false; }
    GestureEnd = FIntPoint(X, Y);
    if (bPointerPending && !bGestureAlt && (GestureEnd - GestureStart).SizeSquared() >= 16) bBoxDragging = true;
    Client->Invalidate(); return true;
}

void FBlenderStyleEdMode::ApplySelectionGesture()
{
    if (!GestureClient || !GestureViewport || !GEditor) return;
    SyncPresentation();
    if (bBoxDragging || bBoxArmed)
    {
        const FIntPoint Size = GestureViewport->GetSizeXY();
        const FIntRect Rect(
            FMath::Clamp(FMath::Min(GestureStart.X, GestureEnd.X), 0, Size.X - 1),
            FMath::Clamp(FMath::Min(GestureStart.Y, GestureEnd.Y), 0, Size.Y - 1),
            FMath::Clamp(FMath::Max(GestureStart.X, GestureEnd.X) + 1, 1, Size.X),
            FMath::Clamp(FMath::Max(GestureStart.Y, GestureEnd.Y) + 1, 1, Size.Y));
        EBlenderSelectionOperation Op = EBlenderSelectionOperation::Replace;
        if (bBoxArmed) Op = GestureButton == EKeys::MiddleMouseButton || bGestureControl ? EBlenderSelectionOperation::Subtract : EBlenderSelectionOperation::Add;
        else if (bGestureControl && bGestureShift) Op = EBlenderSelectionOperation::Intersect;
        else if (bGestureControl) Op = EBlenderSelectionOperation::Subtract;
        else if (bGestureShift) Op = EBlenderSelectionOperation::Add;
        if (IsMeshEditing()) MeshSession->BoxSelect(GestureClient, GestureViewport, Rect, Op);
        else
        {
            TSet<AActor*> Candidates;
            if (Presentation && Presentation->IsXRay())
            {
                FSceneViewFamilyContext Family(FSceneViewFamily::ConstructionValues(GestureViewport, GestureClient->GetScene(), GestureClient->EngineShowFlags));
                const FSceneView* View = GestureClient->CalcSceneView(&Family);
                if (View) for (TActorIterator<AActor> It(GestureClient->GetWorld()); It; ++It)
                {
                    AActor* Actor = *It; FVector2D Pixel;
                    if (Actor->IsHiddenEd() || !IsUsableActor(Actor, GestureClient->GetWorld())) continue;
                    if (View->IsPerspectiveProjection() && View->WorldToScreen(Actor->GetActorLocation()).W <= 0) continue;
                    if (View->WorldToPixel(Actor->GetActorLocation(), Pixel) && Rect.Contains(FIntPoint(FMath::RoundToInt(Pixel.X), FMath::RoundToInt(Pixel.Y)))) Candidates.Add(Actor);
                }
            }
            else
            {
                TArray<HHitProxy*> HitProxies;
                GestureViewport->GetHitProxyMap(Rect, HitProxies);
                for (HHitProxy* Proxy : HitProxies)
                    if (Proxy && Proxy->IsA(HActor::StaticGetType()))
                        if (AActor* Actor = static_cast<HActor*>(Proxy)->Actor; IsUsableActor(Actor, GestureClient->GetWorld()) && !Actor->IsHiddenEd()) Candidates.Add(Actor);
            }
            GEditor->GetSelectedActors()->BeginBatchSelectOperation();
            if (Op == EBlenderSelectionOperation::Replace) GEditor->SelectNone(false, true, false);
            if (Op == EBlenderSelectionOperation::Intersect)
            {
                TArray<AActor*> ToDeselect;
                for (FSelectionIterator It(*GEditor->GetSelectedActors()); It; ++It)
                    if (AActor* Actor = Cast<AActor>(*It); Actor && !Candidates.Contains(Actor)) ToDeselect.Add(Actor);
                for (AActor* Actor : ToDeselect) GEditor->SelectActor(Actor, false, false, true);
            }
            else for (AActor* Actor : Candidates) GEditor->SelectActor(Actor, Op != EBlenderSelectionOperation::Subtract, false, true);
            GEditor->GetSelectedActors()->EndBatchSelectOperation();
            GEditor->NoteSelectionChange();
        }
    }
    else if (IsMeshEditing())
    {
        EBlenderPickAction Action = EBlenderPickAction::Normal;
        if (bGestureAlt) Action = bGestureControl ? EBlenderPickAction::Ring : EBlenderPickAction::Loop;
        else if (bGestureControl) Action = EBlenderPickAction::ShortestPath;
        MeshSession->Pick(GestureClient, GestureViewport, Action, bGestureShift, false, bGestureControl && bGestureShift && !bGestureAlt);
    }
    else
    {
        HHitProxy* Proxy = GestureViewport->GetHitProxy(GestureEnd.X, GestureEnd.Y);
        AActor* Actor = Proxy && Proxy->IsA(HActor::StaticGetType()) ? static_cast<HActor*>(Proxy)->Actor : nullptr;
        if (!IsUsableActor(Actor, GestureClient->GetWorld()) || (Actor && Actor->IsHiddenEd())) Actor = nullptr;
        const bool bSelected = Actor && Actor->IsSelected();
        if (!bGestureShift) GEditor->SelectNone(false, true, false);
        if (Actor) GEditor->SelectActor(Actor, bGestureShift ? !bSelected : true, false, true);
        GEditor->NoteSelectionChange();
    }
    LastNotice = FText::GetEmpty();
    GEditor->RedrawLevelEditingViewports();
}

void FBlenderStyleEdMode::DrawSelectionHUD(FViewport* Viewport, FCanvas* Canvas)
{
    if (!IsSelecting() || GestureViewport != Viewport || !Canvas) return;
    const FLinearColor Color(.72f, .83f, 1.f, .95f);
    if (bPointerPending && (bBoxDragging || bBoxArmed))
    {
        const FVector2D Min(FMath::Min(GestureStart.X, GestureEnd.X), FMath::Min(GestureStart.Y, GestureEnd.Y));
        const FVector2D Size(FMath::Abs(GestureEnd.X - GestureStart.X), FMath::Abs(GestureEnd.Y - GestureStart.Y));
        FCanvasTileItem Fill(Min, Size, FLinearColor(.22f, .42f, .8f, .12f)); Fill.BlendMode = SE_BLEND_Translucent; Canvas->DrawItem(Fill);
        FCanvasBoxItem Box(Min, Size); Box.SetColor(Color); Box.LineThickness = 1.f; Canvas->DrawItem(Box);
    }
    else if (bBoxArmed)
    {
        FCanvasLineItem H(FVector2D(0, GestureEnd.Y), FVector2D(Viewport->GetSizeXY().X, GestureEnd.Y)); H.SetColor(Color); Canvas->DrawItem(H);
        FCanvasLineItem V(FVector2D(GestureEnd.X, 0), FVector2D(GestureEnd.X, Viewport->GetSizeXY().Y)); V.SetColor(Color); Canvas->DrawItem(V);
    }
}

void FBlenderStyleEdMode::OpenShadingMenu(FEditorViewportClient* Client)
{
    if (!Presentation || !Client || !FSlateApplication::IsInitialized()) return;
    auto Widget = Client->GetEditorViewportWidget();
    if (!Widget.IsValid()) return;
    FMenuBuilder Menu(true, nullptr);
    const TWeakPtr<FEdMode> WeakMode = AsShared();
    auto Add = [&](const TCHAR* Name, const TCHAR* Tooltip, EBlenderViewportShading Value)
    {
        Menu.AddMenuEntry(FText::FromString(Name), FText::FromString(Tooltip), FSlateIcon(),
            FUIAction(FExecuteAction::CreateLambda([WeakMode, Value]
            {
                if (auto Mode = WeakMode.Pin()) static_cast<FBlenderStyleEdMode*>(Mode.Get())->GetPresentation()->SetShading(Value);
            })));
    };
    Add(TEXT("线框"), TEXT("Shift+Z 切换线框"), EBlenderViewportShading::Wireframe);
    Add(TEXT("实体"), TEXT("中性表面，查看模型形状"), EBlenderViewportShading::Solid);
    Add(TEXT("材质预览"), TEXT("显示原材质，不受场景灯光影响"), EBlenderViewportShading::Material);
    Add(TEXT("渲染"), TEXT("使用 UE 场景灯光与材质"), EBlenderViewportShading::Rendered);
    FSlateApplication::Get().PushMenu(Widget.ToSharedRef(), FWidgetPath(), Menu.MakeWidget(), FSlateApplication::Get().GetCursorPos(), FPopupTransitionEffect::ContextMenu);
}
