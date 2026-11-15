#include "BlenderNavigationTests.h"

#include "BlenderViewportNavigation.h"
#include "Dom/JsonObject.h"
#include "Editor.h"
#include "EditorViewportClient.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "UnrealClient.h"

DEFINE_LOG_CATEGORY_STATIC(LogBlenderNavigationVerification, Log, All);

namespace BlenderNavigationVerification
{
struct FReport
{
    TArray<TSharedPtr<FJsonValue>>& Checks;
    int32& Failures;

    void Check(const TCHAR* Name, bool bPassed, const FString& Detail = FString())
    {
        TSharedRef<FJsonObject> Entry = MakeShared<FJsonObject>();
        Entry->SetStringField(TEXT("name"), Name);
        Entry->SetBoolField(TEXT("passed"), bPassed);
        Entry->SetStringField(TEXT("details"), Detail);
        Checks.Add(MakeShared<FJsonValueObject>(Entry));
        Failures += bPassed ? 0 : 1;
        if (bPassed)
        {
            UE_LOG(LogBlenderNavigationVerification, Display, TEXT("PASS %s %s"), Name, *Detail);
        }
        else
        {
            UE_LOG(LogBlenderNavigationVerification, Error, TEXT("FAIL %s %s"), Name, *Detail);
        }
    }
};

struct FCameraSnapshot
{
    FEditorViewportClient* Client;
    ELevelViewportType Type;
    FViewportCameraTransform Perspective;
    FViewportCameraTransform Orthographic;
    FEngineShowFlags ShowFlags;
    float FOV;
    bool bOrbit;

    explicit FCameraSnapshot(FEditorViewportClient* InClient)
        : Client(InClient), Type(Client->GetViewportType()), Perspective(Client->ViewTransformPerspective),
          Orthographic(Client->ViewTransformOrthographic), ShowFlags(Client->EngineShowFlags),
          FOV(Client->ViewFOV), bOrbit(Client->bUsingOrbitCamera) {}

    ~FCameraSnapshot()
    {
        Client->SetViewportType(Type);
        Client->ViewTransformPerspective = Perspective;
        Client->ViewTransformOrthographic = Orthographic;
        Client->EngineShowFlags = ShowFlags;
        Client->ViewFOV = FOV;
        Client->bUsingOrbitCamera = bOrbit;
        Client->Invalidate();
    }
};
}

void AppendBlenderNavigationVerification(FEditorViewportClient* Client, FViewport* Viewport,
    TArray<TSharedPtr<FJsonValue>>& Checks, int32& Failures)
{
    using namespace BlenderNavigationVerification;
    FReport Report{Checks, Failures};
    const bool bReady = FParse::Param(FCommandLine::Get(), TEXT("BlenderControlsVerify"))
        && GEditor && !GEditor->PlayWorld && Client && Client->IsLevelEditorClient() && Viewport
        && Viewport == Client->Viewport && Client->GetWorld()
        && Client->GetWorld()->WorldType == EWorldType::Editor;
    Report.Check(TEXT("navigation.dedicated_editor_camera_precondition"), bReady);
    if (!bReady) return;

    FCameraSnapshot CameraRestore(Client);
    TMap<TWeakObjectPtr<AActor>, FTransform> ActorTransforms;
    for (TActorIterator<AActor> It(Client->GetWorld()); It; ++It) ActorTransforms.Add(*It, It->GetActorTransform());

    FBlenderViewportNavigation Navigation;
    const FBox Bounds(FVector(-100, -100, -100), FVector(100, 100, 100));
    const FBox EmptyBounds(ForceInit);
    auto Key = [&](FKey InKey, EInputEvent Event = IE_Pressed)
    {
        return Navigation.InputKey(Client, Viewport, InKey, Event, Bounds);
    };
    auto Reset = [&]()
    {
        Navigation.ForgetViewports();
        Client->SetViewportType(LVT_Perspective);
        Client->bUsingOrbitCamera = false;
        Client->SetViewLocation(FVector(-1000, 0, 0));
        Client->SetViewRotation(FRotator::ZeroRotator);
        Client->SetLookAtLocation(FVector::ZeroVector);
        Client->ViewFOV = 90.0f;
        Navigation.ActivateViewport(Client);
    };
    auto Drag = [&](int32 DX, int32 DY)
    {
        const int32 X = Viewport->GetMouseX(), Y = Viewport->GetMouseY();
        Key(EKeys::MiddleMouseButton);
        const bool bMoved = Navigation.MouseMove(Client, Viewport, X + DX, Y + DY);
        Key(EKeys::MiddleMouseButton, IE_Released);
        return bMoved;
    };

    Reset();
    Report.Check(TEXT("navigation.mouse_right_orbits_eye_left_like_Blender"), Drag(100, 0)
        && Client->GetViewLocation().Y < -100.0
        && FMath::IsNearlyEqual(Client->GetViewLocation().Size(), 1000.0, 0.001)
        && Client->GetLookAtLocation().IsNearlyZero()
        && FVector::DotProduct(Client->GetViewRotation().Vector(), -Client->GetViewLocation().GetSafeNormal()) > 0.999999);
    Reset();
    Report.Check(TEXT("navigation.orbit_down_moves_eye_up"), Drag(0, 100) && Client->GetViewLocation().Z > 100.0
        && FMath::IsNearlyEqual(Client->GetViewLocation().Size(), 1000.0, 0.001));

    Reset();
    Key(EKeys::LeftShift);
    const bool bPanMoved = Drag(100, 50);
    Key(EKeys::LeftShift, IE_Released);
    const FVector PanOffset = Client->GetViewLocation() - FVector(-1000, 0, 0);
    Report.Check(TEXT("navigation.pan_drag_scene_with_pointer"), bPanMoved && PanOffset.Y < -10.0 && PanOffset.Z > 10.0
        && FMath::IsNearlyZero(PanOffset.X) && Client->GetViewRotation().IsNearlyZero()
        && Client->GetLookAtLocation().Equals(PanOffset, 0.001));

    Reset();
    Key(EKeys::LeftControl);
    const bool bZoomMoved = Drag(0, -100);
    Key(EKeys::LeftControl, IE_Released);
    Report.Check(TEXT("navigation.ctrl_mmb_zoom_changes_distance_keeps_center"), bZoomMoved
        && Client->GetViewLocation().Size() < 900.0 && Client->GetViewLocation().Size() > 1.0
        && Client->GetViewRotation().IsNearlyZero() && Client->GetLookAtLocation().IsNearlyZero());

    Reset();
    const FVector BeforeDollyEye = Client->GetViewLocation();
    const FVector BeforeDollyCenter = Client->GetLookAtLocation();
    const double BeforeDollyDistance = FVector::Distance(BeforeDollyEye, BeforeDollyCenter);
    Key(EKeys::LeftShift);
    Key(EKeys::LeftControl);
    const bool bCombinedDollyMoved = Drag(50, -40);
    Key(EKeys::LeftControl, IE_Released);
    Key(EKeys::LeftShift, IE_Released);
    const FVector DollyEyeDelta = Client->GetViewLocation() - BeforeDollyEye;
    const FVector DollyCenterDelta = Client->GetLookAtLocation() - BeforeDollyCenter;
    Report.Check(TEXT("navigation.ctrl_shift_mmb_dolly_moves_eye_and_center_together"), bCombinedDollyMoved
        && DollyEyeDelta.X > 0.0 && FMath::IsNearlyZero(DollyEyeDelta.Y) && FMath::IsNearlyZero(DollyEyeDelta.Z)
        && DollyEyeDelta.Equals(DollyCenterDelta, 0.001) && Client->GetViewRotation().IsNearlyZero()
        && FMath::IsNearlyEqual(FVector::Distance(Client->GetViewLocation(), Client->GetLookAtLocation()), BeforeDollyDistance, 0.001));

    Reset();
    Key(EKeys::NumPadSeven);
    const FVector BeforeOrthoDollyCenter = Client->GetLookAtLocation();
    const FRotator BeforeOrthoDollyRotation = Client->GetViewRotation();
    const int32 DollyStartX = Viewport->GetMouseX(), DollyStartY = Viewport->GetMouseY();
    Key(EKeys::LeftControl); Key(EKeys::LeftShift); Key(EKeys::MiddleMouseButton);
    // Horizontal-only input changes projection without translating, retaining the scale at the center.
    Navigation.MouseMove(Client, Viewport, DollyStartX + 20, DollyStartY);
    const FVector ConvertedEye = Client->GetViewLocation();
    const bool bOrthoConverted = Client->IsPerspective()
        && Client->GetViewRotation().Equals(BeforeOrthoDollyRotation, 0.001)
        && Client->GetLookAtLocation().Equals(BeforeOrthoDollyCenter, 0.001)
        && FMath::IsNearlyEqual(FVector::Distance(ConvertedEye, BeforeOrthoDollyCenter), 1000.0, 0.01);
    Navigation.MouseMove(Client, Viewport, DollyStartX + 20, DollyStartY - 40);
    Key(EKeys::MiddleMouseButton, IE_Released);
    Key(EKeys::LeftShift, IE_Released); Key(EKeys::LeftControl, IE_Released);
    const FVector OrthoDollyEyeDelta = Client->GetViewLocation() - ConvertedEye;
    Report.Check(TEXT("navigation.ortho_dolly_preserves_framing_then_translates_eye_and_center"), bOrthoConverted
        && FVector::DotProduct(OrthoDollyEyeDelta, BeforeOrthoDollyRotation.Vector()) > 0.0
        && OrthoDollyEyeDelta.Equals(Client->GetLookAtLocation() - BeforeOrthoDollyCenter, 0.001)
        && FMath::IsNearlyEqual(FVector::Distance(Client->GetViewLocation(), Client->GetLookAtLocation()), 1000.0, 0.01));

    Reset();
    Key(EKeys::MouseScrollUp);
    const double ZoomedDistance = Client->GetViewLocation().Size();
    Navigation.InputAxis(Client, Viewport, EKeys::MouseWheelAxis, 1.0f);
    const bool bNoDoubleWheel = FMath::IsNearlyEqual(Client->GetViewLocation().Size(), ZoomedDistance, 0.001);
    Key(EKeys::MouseScrollDown);
    Report.Check(TEXT("navigation.wheel_in_out_reversible_no_duplicate_axis"), ZoomedDistance < 1000.0
        && bNoDoubleWheel && FMath::IsNearlyEqual(Client->GetViewLocation().Size(), 1000.0, 0.001));

    Reset();
    Key(EKeys::MiddleMouseButton);
    Report.Check(TEXT("navigation.mmb_starts_modal_navigation"), Navigation.IsNavigating());
    const FVector BeforeAxis = Client->GetViewLocation();
    Report.Check(TEXT("navigation.axes_consumed_without_double_movement"),
        Navigation.InputAxis(Client, Viewport, EKeys::MouseX, 50.0f)
        && Navigation.InputAxis(Client, Viewport, EKeys::MouseY, 30.0f)
        && BeforeAxis.Equals(Client->GetViewLocation()));
    Key(EKeys::Escape);
    const FVector AfterCancel = Client->GetViewLocation();
    Report.Check(TEXT("navigation.escape_stops_capture_and_late_move"), !Navigation.IsNavigating()
        && !Navigation.MouseMove(Client, Viewport, 15, 15) && AfterCancel.Equals(Client->GetViewLocation()));
    Report.Check(TEXT("navigation.late_mmb_release_consumed"), Key(EKeys::MiddleMouseButton, IE_Released));
    Key(EKeys::MiddleMouseButton);
    Navigation.Cancel();
    Report.Check(TEXT("navigation.focus_cancel_stops_capture"), !Navigation.IsNavigating());
    Report.Check(TEXT("navigation.ordinary_keys_unhandled"), !Key(EKeys::G) && !Key(EKeys::A)
        && !Navigation.InputAxis(Client, Viewport, EKeys::Gamepad_LeftX, 1.0f));
    Report.Check(TEXT("navigation.right_mouse_native_fly_blocked"), Key(EKeys::RightMouseButton)
        && Key(EKeys::RightMouseButton, IE_Released));

    Reset();
    const FKey AxisKeys[] = {EKeys::NumPadOne, EKeys::NumPadThree, EKeys::NumPadSeven};
    const ELevelViewportType ExpectedTypes[] = {LVT_OrthoNegativeYZ, LVT_OrthoXZ, LVT_OrthoXY};
    const ELevelViewportType OppositeTypes[] = {LVT_OrthoYZ, LVT_OrthoNegativeXZ, LVT_OrthoNegativeXY};
    bool bAxisViews = true, bOppositeViews = true, bOrthoPan = true;
    for (int32 Index = 0; Index < 3; ++Index)
    {
        Key(AxisKeys[Index]);
        bAxisViews &= Client->GetViewportType() == ExpectedTypes[Index];
        const FVector BeforePan = Client->GetViewLocation();
        const FVector ScreenRight = FRotationMatrix(Client->GetViewRotation()).GetScaledAxis(EAxis::Y);
        Key(EKeys::LeftShift);
        bOrthoPan &= Drag(50, 0) && FVector::DotProduct(Client->GetViewLocation() - BeforePan, ScreenRight) < 0.0;
        Key(EKeys::LeftShift, IE_Released);
        Key(EKeys::LeftControl);
        Key(AxisKeys[Index]);
        Key(EKeys::LeftControl, IE_Released);
        bOppositeViews &= Client->GetViewportType() == OppositeTypes[Index];
    }
    Report.Check(TEXT("navigation.numpad_front_right_top"), bAxisViews);
    Report.Check(TEXT("navigation.control_numpad_opposite_views"), bOppositeViews);
    Report.Check(TEXT("navigation.axis_orthographic_pan_screen_direction"), bOrthoPan);
    const float BeforeOrthoWheel = Client->GetOrthoZoom();
    Key(EKeys::MouseScrollUp);
    Report.Check(TEXT("navigation.orthographic_wheel_changes_scale_only"), Client->IsOrtho()
        && Client->GetOrthoZoom() < BeforeOrthoWheel);
    Key(EKeys::NumPadFive);
    Report.Check(TEXT("navigation.axis_projection_toggle_to_perspective"), Client->IsPerspective());
    Key(EKeys::NumPadFive);
    Report.Check(TEXT("navigation.axis_projection_toggle_to_orthographic"), Client->IsOrtho());

    Reset();
    Client->SetViewRotation(FRotator(-25, 35, 0));
    Client->SetViewLocation(-Client->GetViewRotation().Vector() * 1000.0);
    const FRotator ArbitraryRotation = Client->GetViewRotation();
    Key(EKeys::NumPadFive);
    const bool bArbitraryBecomesOrtho = Client->IsOrtho();
    Key(EKeys::NumPadFive);
    Report.Check(TEXT("navigation.projection_toggle_restores_arbitrary_perspective_angle"), bArbitraryBecomesOrtho
        && Client->IsPerspective() && Client->GetViewRotation().Equals(ArbitraryRotation, 0.001));

    Reset();
    Key(EKeys::NumPadSeven);
    Report.Check(TEXT("navigation.orbit_from_top_enters_perspective_without_pole_nan"), Drag(20, 30)
        && Client->IsPerspective() && !Client->GetViewLocation().ContainsNaN()
        && !Client->GetViewRotation().ContainsNaN()
        && FMath::IsNearlyEqual(FVector::Distance(Client->GetViewLocation(), Client->GetLookAtLocation()), 1000.0, 0.01));
    Reset();
    for (int32 Index = 0; Index < 30; ++Index) Key(EKeys::NumPadEight);
    Report.Check(TEXT("navigation.numpad_orbit_crosses_poles_without_distance_drift"),
        !Client->GetViewLocation().ContainsNaN() && !Client->GetViewRotation().ContainsNaN()
        && FMath::IsNearlyEqual(Client->GetViewLocation().Size(), 1000.0, 0.01));

    Reset();
    const FVector FrameCenter(450, -200, 100);
    const FBox FrameBox(FrameCenter - FVector(100), FrameCenter + FVector(100));
    Report.Check(TEXT("navigation.frame_bounds_centers_camera"), Navigation.FrameBounds(Client, Viewport, FrameBox)
        && Client->GetLookAtLocation().Equals(FrameCenter, 0.001)
        && FVector::DotProduct(Client->GetViewRotation().Vector(), (FrameCenter - Client->GetViewLocation()).GetSafeNormal()) > 0.999999);
    const FVector BeforeEmptyFrame = Client->GetViewLocation();
    Report.Check(TEXT("navigation.empty_frame_bounds_no_camera_jump"), !Navigation.FrameBounds(Client, Viewport, EmptyBounds)
        && Client->GetViewLocation().Equals(BeforeEmptyFrame));
    Report.Check(TEXT("navigation.numpad_decimal_frames_selection"), Key(EKeys::Decimal)
        && Client->GetLookAtLocation().IsNearlyZero(0.001));
    Report.Check(TEXT("navigation.home_frames_supplied_bounds"),
        Navigation.InputKey(Client, Viewport, EKeys::Home, IE_Pressed, FrameBox)
        && Client->GetLookAtLocation().Equals(FrameCenter, 0.001));

    Navigation.ForgetViewports();
    FEditorCameraController* Controller = Client->GetCameraController();
    if (Controller)
    {
        const FCameraControllerConfig Original = Controller->GetConfig();
        const bool bOriginalNumpad = Client->bUseNumpadCameraControl;
        Navigation.ActivateViewport(Client);
        FCameraControllerUserImpulseData Impulse;
        Impulse.MoveForwardBackwardImpulse = Impulse.MoveRightLeftImpulse = Impulse.MoveUpDownImpulse = 1.0f;
        Impulse.RotateYawImpulse = Impulse.RotatePitchImpulse = Impulse.ZoomOutInImpulse = 1.0f;
        Impulse.RotateYawVelocityModifier = 2.0f;
        FVector Position(1, 2, 3), Euler(0, -120, 30);
        float FOV = 90.0f;
        Controller->UpdateSimulation(Impulse, 1.0f / 30.0f, true, 1.0f, Position, Euler, FOV);
        Report.Check(TEXT("navigation.native_flight_simulation_cannot_move_camera"),
            Position.Equals(FVector(1, 2, 3)) && Euler.Equals(FVector(0, -120, 30)) && FOV == 90.0f
            && !Client->bUseNumpadCameraControl);
        Navigation.ForgetViewports();
        const FCameraControllerConfig Restored = Controller->GetConfig();
        Report.Check(TEXT("navigation.disable_restores_native_controller_config"),
            Restored.MaximumMovementSpeed == Original.MaximumMovementSpeed
            && Restored.MaximumRotationSpeed == Original.MaximumRotationSpeed
            && Restored.MaximumFOVSpeed == Original.MaximumFOVSpeed
            && Restored.FOVAccelerationRate == Original.FOVAccelerationRate
            && Restored.bLockedPitch == Original.bLockedPitch
            && Restored.bEnableFOVRecoil == Original.bEnableFOVRecoil
            && Client->bUseNumpadCameraControl == bOriginalNumpad);
    }
    else Report.Check(TEXT("navigation.controller_available"), false);

    bool bActorsUnchanged = true;
    int32 ActorCount = 0;
    for (TActorIterator<AActor> It(Client->GetWorld()); It; ++It)
    {
        ++ActorCount;
        const FTransform* Original = ActorTransforms.Find(*It);
        bActorsUnchanged &= Original && Original->Equals(It->GetActorTransform(), 0.0001);
    }
    Report.Check(TEXT("navigation.all_scene_actors_unchanged"), bActorsUnchanged && ActorCount == ActorTransforms.Num());
}
