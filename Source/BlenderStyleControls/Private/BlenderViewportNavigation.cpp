#include "BlenderViewportNavigation.h"

#include "Editor.h"
#include "EditorViewportClient.h"
#include "LevelEditorViewport.h"
#include "Settings/LevelEditorViewportSettings.h"
#include "UnrealClient.h"

namespace BlenderNavigation
{
constexpr double DegreesPerPixel = 0.25;
constexpr double WheelLogScale = 0.1823215567939546; // ln(1.2), reciprocal wheel steps.
constexpr double MinDistance = 1.0;
constexpr double MaxDistance = 1.0e12;

bool IsModifier(FKey Key)
{
    return Key == EKeys::LeftShift || Key == EKeys::RightShift || Key == EKeys::LeftControl
        || Key == EKeys::RightControl || Key == EKeys::LeftAlt || Key == EKeys::RightAlt;
}

bool IsNavigationKey(FKey Key)
{
    return Key == EKeys::MiddleMouseButton || Key == EKeys::RightMouseButton
        || Key == EKeys::MouseScrollUp || Key == EKeys::MouseScrollDown
        || Key == EKeys::NumPadOne || Key == EKeys::NumPadThree || Key == EKeys::NumPadSeven
        || Key == EKeys::NumPadFive || Key == EKeys::NumPadTwo || Key == EKeys::NumPadFour
        || Key == EKeys::NumPadSix || Key == EKeys::NumPadEight || Key == EKeys::Decimal || Key == EKeys::Home;
}

FRotator AxisRotation(ELevelViewportType Type)
{
    // Match UE's actual orthographic matrices, including the screen up direction at both poles.
    switch (Type)
    {
    case LVT_OrthoXY: return FRotator(-90, -90, 0);
    case LVT_OrthoNegativeXY: return FRotator(90, 90, 0);
    case LVT_OrthoXZ: return FRotator(0, -90, 0);
    case LVT_OrthoNegativeXZ: return FRotator(0, 90, 0);
    case LVT_OrthoNegativeYZ: return FRotator(0, 180, 0);
    default: return FRotator::ZeroRotator;
    }
}

ELevelViewportType NearestAxis(const FVector& Forward)
{
    const FVector Abs = Forward.GetAbs();
    if (Abs.Z >= Abs.X && Abs.Z >= Abs.Y) return Forward.Z < 0 ? LVT_OrthoXY : LVT_OrthoNegativeXY;
    if (Abs.X >= Abs.Y) return Forward.X < 0 ? LVT_OrthoNegativeYZ : LVT_OrthoYZ;
    return Forward.Y < 0 ? LVT_OrthoXZ : LVT_OrthoNegativeXZ;
}

FRotator ViewRotation(FEditorViewportClient* Client)
{
    return Client->IsPerspective() ? Client->GetViewRotation() : AxisRotation(Client->GetViewportType());
}

double PerspectiveUnitsPerPixel(FEditorViewportClient* Client, FViewport* Viewport, double Distance)
{
    const int32 Width = FMath::Max(Viewport->GetSizeXY().X, 1);
    const int32 Height = FMath::Max(Viewport->GetSizeXY().Y, 1);
    const EAspectRatioAxisConstraint Constraint = GetDefault<ULevelEditorViewportSettings>()->AspectRatioAxisConstraint;
    const bool bHorizontalFOV = Constraint == AspectRatio_MaintainXFOV
        || (Constraint == AspectRatio_MajorAxisFOV && Width > Height);
    const double HalfFOV = FMath::DegreesToRadians(FMath::Clamp(double(Client->ViewFOV), 1.0, 170.0) * 0.5);
    return 2.0 * Distance * FMath::Tan(HalfFOV) / (bHorizontalFOV ? Width : Height);
}

void SetOrthoUnitsPerPixel(FEditorViewportClient* Client, FViewport* Viewport, double Units)
{
    // Calibrate against the engine getter so r.Editor.AlignedOrthoZoom and viewport width stay correct.
    const double CurrentUnits = FMath::Max(double(Client->GetOrthoUnitsPerPixel(Viewport)), UE_SMALL_NUMBER);
    const double Requested = Client->GetOrthoZoom() * Units / CurrentUnits;
    const double Minimum = FMath::Max(double(GetDefault<ULevelEditorViewportSettings>()->MinimumOrthographicZoom), MIN_ORTHOZOOM);
    Client->SetOrthoZoom(float(FMath::Clamp(Requested, Minimum, double(MAX_ORTHOZOOM))));
}

bool HasActorLock(FEditorViewportClient* Client)
{
    return Client->IsLevelEditorClient() && static_cast<FLevelEditorViewportClient*>(Client)->IsAnyActorLocked();
}
}

FBlenderViewportNavigation::~FBlenderViewportNavigation()
{
    ForgetViewports();
}

void FBlenderViewportNavigation::ActivateViewport(FEditorViewportClient* Client)
{
    if (!Client || !Client->IsLevelEditorClient() || !Client->Viewport) return;

    // Never touch a closed viewport merely because its address remains in this map.
    if (GEditor)
    {
        const TArray<FEditorViewportClient*>& LiveClients = GEditor->GetAllViewportClients();
        for (auto It = Views.CreateIterator(); It; ++It)
        {
            if (!LiveClients.Contains(It.Key()))
            {
                if (ActiveClient == It.Key()) Cancel();
                It.RemoveCurrent();
            }
        }
    }

    FViewState* Existing = Views.Find(Client);
    if (Existing && (Existing->Viewport != Client->Viewport || Existing->CameraController != Client->GetCameraController()))
    {
        // A viewport/layout replacement can reuse a client address; do not restore a stale controller.
        if (ActiveClient == Client) Cancel();
        Views.Remove(Client);
        Existing = nullptr;
    }
    if (!Existing)
    {
        FViewState NewState;
        NewState.Viewport = Client->Viewport;
        NewState.CameraController = Client->GetCameraController();
        NewState.bOriginalNumpadCameraControl = Client->bUseNumpadCameraControl;
        if (NewState.CameraController)
        {
            NewState.OriginalCameraConfig = NewState.CameraController->GetConfig();
            NewState.CameraController->ResetVelocity();
        }
        Views.Add(Client, NewState);
    }

    Client->bUseNumpadCameraControl = false;
    if (FEditorCameraController* Controller = Client->GetCameraController())
    {
        // Tick polls raw key state even when InputKey is consumed. Clamp this viewport's native
        // simulation, including pre-existing velocity, instead of changing global WASD preferences.
        FCameraControllerConfig& Config = Controller->GetConfig();
        Config.MaximumMovementSpeed = 0.0f;
        Config.MaximumRotationSpeed = 0.0f;
        Config.MaximumFOVSpeed = 0.0f;
        // A nonzero FOV acceleration would cache a recoil target even with zero maximum speed.
        Config.FOVAccelerationRate = 0.0f;
        Config.bEnableFOVRecoil = false;
        Config.bLockedPitch = false;
    }
}

FBlenderViewportNavigation::FViewState& FBlenderViewportNavigation::StateFor(FEditorViewportClient* Client)
{
    ActivateViewport(Client);
    return Views.FindChecked(Client);
}

void FBlenderViewportNavigation::SyncPivot(FEditorViewportClient* Client, FViewState& State, const FBox& Bounds)
{
    using namespace BlenderNavigation;
    if (Client->IsOrtho())
    {
        // Native axis-orthographic view locations are their center, not a distant perspective eye.
        if (!State.bHasPivot || !Client->GetViewLocation().Equals(State.Pivot, 0.001))
            State.Pivot = Client->GetViewLocation();
        State.bHasPivot = true;
        Client->SetLookAtLocation(State.Pivot);
        Client->SetViewRotation(AxisRotation(Client->GetViewportType()));
        return;
    }

    const FVector Eye = Client->GetViewLocation();
    const FVector Forward = Client->GetViewRotation().Vector();
    const FVector ExpectedEye = State.Pivot - Forward * State.Distance;
    if (State.bHasPivot && Eye.Equals(ExpectedEye, 0.001) && Client->GetLookAtLocation().Equals(State.Pivot, 0.001)) return;

    double Distance = FVector::DotProduct(Client->GetLookAtLocation() - Eye, Forward);
    if (Distance < MinDistance && Bounds.IsValid)
        Distance = FVector::DotProduct(Bounds.GetCenter() - Eye, Forward);
    if (!FMath::IsFinite(Distance) || Distance < MinDistance) Distance = 1000.0;
    State.Distance = FMath::Clamp(Distance, MinDistance, MaxDistance);
    // Project the old look-at onto the current viewing ray so starting orbit never snaps the image.
    State.Pivot = Eye + Forward * State.Distance;
    State.bHasPivot = true;
    Client->SetLookAtLocation(State.Pivot);
}

bool FBlenderViewportNavigation::IsShiftDown(FViewport* Viewport) const
{
    return HeldModifiers.Contains(EKeys::LeftShift) || HeldModifiers.Contains(EKeys::RightShift)
        || Viewport->KeyState(EKeys::LeftShift) || Viewport->KeyState(EKeys::RightShift);
}

bool FBlenderViewportNavigation::IsControlDown(FViewport* Viewport) const
{
    return HeldModifiers.Contains(EKeys::LeftControl) || HeldModifiers.Contains(EKeys::RightControl)
        || Viewport->KeyState(EKeys::LeftControl) || Viewport->KeyState(EKeys::RightControl);
}

bool FBlenderViewportNavigation::InputKey(FEditorViewportClient* Client, FViewport* Viewport,
    FKey Key, EInputEvent Event, const FBox& SelectionBounds)
{
    using namespace BlenderNavigation;
    if (!Client || !Viewport || Client->Viewport != Viewport || !Client->IsLevelEditorClient()) return false;

    if (IsModifier(Key))
    {
        if (Event == IE_Pressed) HeldModifiers.Add(Key);
        else if (Event == IE_Released) HeldModifiers.Remove(Key);
        // Native UE can start camera tracking from a modifier event while a mouse button is held.
        return IsNavigating() || Viewport->KeyState(EKeys::LeftMouseButton)
            || Viewport->KeyState(EKeys::MiddleMouseButton) || Viewport->KeyState(EKeys::RightMouseButton);
    }

    if (Key == EKeys::Escape && IsNavigating())
    {
        if (Event == IE_Pressed) Cancel();
        return true;
    }
    if (!IsNavigationKey(Key)) return false;
    if (Key == EKeys::RightMouseButton)
    {
        if (Event == IE_Pressed && IsNavigating()) Cancel();
        return true;
    }
    if (Key == EKeys::MiddleMouseButton && Event == IE_Released)
    {
        // Consume even a late release following focus loss or Escape.
        if (ActiveClient == Client) Cancel();
        return true;
    }
    if (Event != IE_Pressed && Event != IE_Repeat) return true;
    // A piloted camera belongs to an actor. Keep navigation from editing it through viewport ticks.
    if (HasActorLock(Client)) return true;

    FViewState& State = StateFor(Client);
    Client->ToggleOrbitCamera(false);
    SyncPivot(Client, State, SelectionBounds);
    if (Key == EKeys::MiddleMouseButton)
    {
        if (Event == IE_Repeat) return true;
        ActiveClient = Client;
        ActiveViewport = Viewport;
        LastMouse = FIntPoint(Viewport->GetMouseX(), Viewport->GetMouseY());
        return true;
    }
    if (IsNavigating()) return true;
    if (Key == EKeys::MouseScrollUp || Key == EKeys::MouseScrollDown)
        Zoom(Client, State, Key == EKeys::MouseScrollUp ? -WheelLogScale : WheelLogScale);
    else if (Key == EKeys::NumPadOne || Key == EKeys::NumPadThree || Key == EKeys::NumPadSeven)
        AxisView(Client, Viewport, State, Key, IsControlDown(Viewport));
    else if (Key == EKeys::NumPadFive)
        ToggleProjection(Client, Viewport, State);
    else if (Key == EKeys::Decimal || Key == EKeys::Home)
        FrameBounds(Client, Viewport, SelectionBounds);
    else
    {
        const double Pixels = 15.0 / DegreesPerPixel;
        const double X = Key == EKeys::NumPadFour ? -Pixels : Key == EKeys::NumPadSix ? Pixels : 0.0;
        const double Y = Key == EKeys::NumPadEight ? Pixels : Key == EKeys::NumPadTwo ? -Pixels : 0.0;
        Orbit(Client, Viewport, State, X, Y);
    }
    return true;
}

bool FBlenderViewportNavigation::InputAxis(FEditorViewportClient* Client, FViewport* Viewport, FKey Key, float Delta)
{
    // FSceneViewport also emits MouseWheelAxis after MouseScrollUp/Down. Applying it again doubles zoom.
    return Client && Client->IsLevelEditorClient() && Viewport && Client->Viewport == Viewport
        && (Key == EKeys::MouseX || Key == EKeys::MouseY || Key == EKeys::MouseWheelAxis);
}

bool FBlenderViewportNavigation::MouseMove(FEditorViewportClient* Client, FViewport* Viewport, int32 X, int32 Y)
{
    if (!IsNavigating() || ActiveClient != Client || ActiveViewport != Viewport) return false;
    FViewState* State = Views.Find(Client);
    if (!State) { Cancel(); return false; }
    const FVector2D Delta(double(X - LastMouse.X), double(Y - LastMouse.Y));
    LastMouse = FIntPoint(X, Y);
    if (Delta.IsNearlyZero()) return true;
    // Blender dolly translates the eye and center together; zoom changes their separation.
    if (IsControlDown(Viewport) && IsShiftDown(Viewport)) Dolly(Client, Viewport, *State, Delta.Y);
    else if (IsControlDown(Viewport)) Zoom(Client, *State, Delta.Y * 0.01);
    else if (IsShiftDown(Viewport)) Pan(Client, Viewport, *State, Delta);
    // Blender's screen-right drag orbits the eye left. Screen Y is already inverted
    // relative to Blender's window coordinates; keep its existing vertical sign.
    // Change the mouse adapter only: numpad orbit directions share Orbit().
    else Orbit(Client, Viewport, *State, -Delta.X, Delta.Y);
    return true;
}

void FBlenderViewportNavigation::Orbit(FEditorViewportClient* Client, FViewport* Viewport, FViewState& State,
    double HorizontalPixels, double VerticalPixels)
{
    using namespace BlenderNavigation;
    FQuat Rotation = ViewRotation(Client).Quaternion();
    if (Client->IsOrtho())
    {
        // Blender's default auto-perspective enters perspective when orbiting an axis view.
        const double Units = Client->GetOrthoUnitsPerPixel(Viewport);
        const double UnitAtDistance = PerspectiveUnitsPerPixel(Client, Viewport, 1.0);
        State.Distance = FMath::Clamp(Units / UnitAtDistance, MinDistance, MaxDistance);
        Client->SetViewportType(LVT_Perspective);
    }
    State.bRestorePerspectiveRotation = false;
    // Quaternions preserve distance and remain defined at/through the poles. No pole interpolation.
    const double Upright = Rotation.GetAxisZ().Z < 0.0 ? -1.0 : 1.0;
    const FQuat Yaw(FVector::UpVector, FMath::DegreesToRadians(-HorizontalPixels * DegreesPerPixel * Upright));
    Rotation = Yaw * Rotation;
    const FQuat Pitch(Rotation.GetAxisY(), FMath::DegreesToRadians(VerticalPixels * DegreesPerPixel));
    Rotation = (Pitch * Rotation).GetNormalized();
    Client->SetViewRotation(Rotation.Rotator());
    Client->SetViewLocation(State.Pivot - Rotation.GetAxisX() * State.Distance);
    Client->SetLookAtLocation(State.Pivot);
    Client->Invalidate();
}

void FBlenderViewportNavigation::Pan(FEditorViewportClient* Client, FViewport* Viewport, FViewState& State,
    const FVector2D& Delta)
{
    const FQuat Rotation = BlenderNavigation::ViewRotation(Client).Quaternion();
    const double Units = Client->IsOrtho() ? Client->GetOrthoUnitsPerPixel(Viewport)
        : BlenderNavigation::PerspectiveUnitsPerPixel(Client, Viewport, State.Distance);
    const FVector Offset = (-Rotation.GetAxisY() * Delta.X + Rotation.GetAxisZ() * Delta.Y) * Units;
    State.Pivot += Offset;
    Client->SetViewLocation(Client->GetViewLocation() + Offset);
    Client->SetLookAtLocation(State.Pivot);
    Client->Invalidate();
}

void FBlenderViewportNavigation::Zoom(FEditorViewportClient* Client, FViewState& State, double LogScale)
{
    using namespace BlenderNavigation;
    const double Factor = FMath::Exp(FMath::Clamp(LogScale, -20.0, 20.0));
    if (Client->IsOrtho())
    {
        const double Minimum = FMath::Max(double(GetDefault<ULevelEditorViewportSettings>()->MinimumOrthographicZoom), MIN_ORTHOZOOM);
        Client->SetOrthoZoom(float(FMath::Clamp(Client->GetOrthoZoom() * Factor, Minimum, double(MAX_ORTHOZOOM))));
    }
    else
    {
        State.Distance = FMath::Clamp(State.Distance * Factor, MinDistance, MaxDistance);
        Client->SetViewLocation(State.Pivot - Client->GetViewRotation().Vector() * State.Distance);
    }
    Client->Invalidate();
}

void FBlenderViewportNavigation::Dolly(FEditorViewportClient* Client, FViewport* Viewport, FViewState& State,
    double VerticalPixels)
{
    using namespace BlenderNavigation;
    if (Client->IsOrtho())
    {
        // Match the orthographic scale at the center before entering perspective, as for orbit.
        const FRotator Rotation = ViewRotation(Client);
        const double Units = Client->GetOrthoUnitsPerPixel(Viewport);
        State.Distance = FMath::Clamp(Units / PerspectiveUnitsPerPixel(Client, Viewport, 1.0), MinDistance, MaxDistance);
        Client->SetViewportType(LVT_Perspective);
        Client->SetViewRotation(Rotation);
        Client->SetViewLocation(State.Pivot - Rotation.Vector() * State.Distance);
    }
    State.bRestorePerspectiveRotation = false;
    const FVector Offset = Client->GetViewRotation().Vector() * (-VerticalPixels * 0.01 * State.Distance);
    State.Pivot += Offset;
    Client->SetViewLocation(Client->GetViewLocation() + Offset);
    Client->SetLookAtLocation(State.Pivot);
    Client->Invalidate();
}

void FBlenderViewportNavigation::AxisView(FEditorViewportClient* Client, FViewport* Viewport, FViewState& State,
    FKey Key, bool bOpposite)
{
    using namespace BlenderNavigation;
    ELevelViewportType Type = bOpposite ? LVT_OrthoYZ : LVT_OrthoNegativeYZ; // UE Front/Back.
    if (Key == EKeys::NumPadThree) Type = bOpposite ? LVT_OrthoNegativeXZ : LVT_OrthoXZ;
    else if (Key == EKeys::NumPadSeven) Type = bOpposite ? LVT_OrthoNegativeXY : LVT_OrthoXY;
    const double Units = Client->IsPerspective() ? PerspectiveUnitsPerPixel(Client, Viewport, State.Distance)
        : Client->GetOrthoUnitsPerPixel(Viewport);
    Client->SetViewportType(Type);
    Client->SetViewLocation(State.Pivot);
    Client->SetLookAtLocation(State.Pivot);
    Client->SetViewRotation(AxisRotation(Type));
    SetOrthoUnitsPerPixel(Client, Viewport, Units);
    State.bRestorePerspectiveRotation = false;
    Client->Invalidate();
}

void FBlenderViewportNavigation::ToggleProjection(FEditorViewportClient* Client, FViewport* Viewport, FViewState& State)
{
    using namespace BlenderNavigation;
    if (Client->IsPerspective())
    {
        const double Units = PerspectiveUnitsPerPixel(Client, Viewport, State.Distance);
        State.PerspectiveRotation = Client->GetViewRotation();
        State.bRestorePerspectiveRotation = true;
        // UE 5.4's free-look orthographic renderer ignores camera rotation. Use its nearest real
        // axis view, retaining the perspective angle so the return toggle does not lose it.
        const ELevelViewportType Type = NearestAxis(Client->GetViewRotation().Vector());
        Client->SetViewportType(Type);
        Client->SetViewLocation(State.Pivot);
        Client->SetViewRotation(AxisRotation(Type));
        Client->SetLookAtLocation(State.Pivot);
        SetOrthoUnitsPerPixel(Client, Viewport, Units);
    }
    else
    {
        const double Units = Client->GetOrthoUnitsPerPixel(Viewport);
        const FRotator Rotation = State.bRestorePerspectiveRotation ? State.PerspectiveRotation : AxisRotation(Client->GetViewportType());
        State.Distance = FMath::Clamp(Units / PerspectiveUnitsPerPixel(Client, Viewport, 1.0), MinDistance, MaxDistance);
        Client->SetViewportType(LVT_Perspective);
        Client->SetViewRotation(Rotation);
        Client->SetViewLocation(State.Pivot - Rotation.Vector() * State.Distance);
        Client->SetLookAtLocation(State.Pivot);
        State.bRestorePerspectiveRotation = false;
    }
    Client->Invalidate();
}

bool FBlenderViewportNavigation::FrameBounds(FEditorViewportClient* Client, FViewport* Viewport, const FBox& Bounds)
{
    using namespace BlenderNavigation;
    if (!Client || !Viewport || !Bounds.IsValid || !Client->IsLevelEditorClient()
        || Client->Viewport != Viewport || Bounds.Min.ContainsNaN() || Bounds.Max.ContainsNaN() || HasActorLock(Client)) return false;
    FViewState& State = StateFor(Client);
    const double Radius = FMath::Max(Bounds.GetExtent().Size(), 1.0);
    State.Pivot = Bounds.GetCenter();
    State.bHasPivot = true;
    const int32 Width = FMath::Max(Viewport->GetSizeXY().X, 1);
    const int32 Height = FMath::Max(Viewport->GetSizeXY().Y, 1);
    if (Client->IsPerspective())
    {
        Client->ToggleOrbitCamera(false);
        const double UnitAtDistance = PerspectiveUnitsPerPixel(Client, Viewport, 1.0);
        const double HorizontalHalf = FMath::Atan(UnitAtDistance * Width * 0.5);
        const double VerticalHalf = FMath::Atan(UnitAtDistance * Height * 0.5);
        State.Distance = FMath::Clamp(1.15 * Radius / FMath::Sin(FMath::Min(HorizontalHalf, VerticalHalf)), MinDistance, MaxDistance);
        Client->SetViewLocation(State.Pivot - Client->GetViewRotation().Vector() * State.Distance);
    }
    else
    {
        Client->SetViewLocation(State.Pivot);
        SetOrthoUnitsPerPixel(Client, Viewport, 2.3 * Radius / FMath::Min(Width, Height));
    }
    Client->SetLookAtLocation(State.Pivot);
    Client->Invalidate();
    return true;
}

void FBlenderViewportNavigation::Cancel()
{
    ActiveClient = nullptr;
    ActiveViewport = nullptr;
    HeldModifiers.Reset();
}

void FBlenderViewportNavigation::ForgetViewports()
{
    Cancel();
    if (GEditor)
    {
        const TArray<FEditorViewportClient*>& LiveClients = GEditor->GetAllViewportClients();
        for (const TPair<FEditorViewportClient*, FViewState>& Entry : Views)
        {
            FEditorViewportClient* Client = Entry.Key;
            const FViewState& State = Entry.Value;
            if (!LiveClients.Contains(Client) || Client->Viewport != State.Viewport
                || Client->GetCameraController() != State.CameraController) continue;
            Client->bUseNumpadCameraControl = State.bOriginalNumpadCameraControl;
            if (State.CameraController)
            {
                State.CameraController->ResetVelocity();
                State.CameraController->SetConfig(State.OriginalCameraConfig);
            }
        }
    }
    Views.Reset();
}
