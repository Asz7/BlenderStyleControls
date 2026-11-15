#pragma once

#include "CoreMinimal.h"
#include "CameraController.h"
#include "InputCoreTypes.h"

class FEditorViewportClient;
class FViewport;

/** Per-level-viewport Blender navigation. Never moves actors or changes editor preferences. */
class FBlenderViewportNavigation
{
public:
    ~FBlenderViewportNavigation();

    /** Call for every live level viewport on mode entry/tick, including newly opened viewports. */
    void ActivateViewport(FEditorViewportClient* Client);
    bool InputKey(FEditorViewportClient* Client, FViewport* Viewport, FKey Key, EInputEvent Event,
        const FBox& SelectionBounds);
    /** Consumes native mouse axes; MouseMove is the only drag delta source. */
    bool InputAxis(FEditorViewportClient* Client, FViewport* Viewport, FKey Key, float Delta);
    bool MouseMove(FEditorViewportClient* Client, FViewport* Viewport, int32 X, int32 Y);
    bool IsNavigating() const { return ActiveClient != nullptr; }
    /** End the gesture at the current view, without rolling back a camera movement. */
    void Cancel();
    /** Cancel and restore native camera controller settings only on still-live viewports. */
    void ForgetViewports();
    bool FrameBounds(FEditorViewportClient* Client, FViewport* Viewport, const FBox& Bounds);

private:
    struct FViewState
    {
        FViewport* Viewport = nullptr;
        FEditorCameraController* CameraController = nullptr;
        FCameraControllerConfig OriginalCameraConfig;
        bool bOriginalNumpadCameraControl = false;
        FVector Pivot = FVector::ZeroVector;
        double Distance = 1000.0;
        bool bHasPivot = false;
        bool bRestorePerspectiveRotation = false;
        FRotator PerspectiveRotation = FRotator::ZeroRotator;
    };

    TMap<FEditorViewportClient*, FViewState> Views;
    FEditorViewportClient* ActiveClient = nullptr;
    FViewport* ActiveViewport = nullptr;
    FIntPoint LastMouse = FIntPoint::ZeroValue;
    TSet<FKey> HeldModifiers;

    FViewState& StateFor(FEditorViewportClient* Client);
    void SyncPivot(FEditorViewportClient* Client, FViewState& State, const FBox& Bounds);
    void Orbit(FEditorViewportClient* Client, FViewport* Viewport, FViewState& State,
        double HorizontalPixels, double VerticalPixels);
    void Pan(FEditorViewportClient* Client, FViewport* Viewport, FViewState& State, const FVector2D& Delta);
    void Zoom(FEditorViewportClient* Client, FViewState& State, double LogScale);
    void Dolly(FEditorViewportClient* Client, FViewport* Viewport, FViewState& State, double VerticalPixels);
    void AxisView(FEditorViewportClient* Client, FViewport* Viewport, FViewState& State, FKey Key, bool bOpposite);
    void ToggleProjection(FEditorViewportClient* Client, FViewport* Viewport, FViewState& State);
    bool IsShiftDown(FViewport* Viewport) const;
    bool IsControlDown(FViewport* Viewport) const;
};
