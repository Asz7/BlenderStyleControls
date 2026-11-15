#include "BlenderStyleControlsTests.h"
#include "BlenderViewportTests.h"
#include "BlenderNavigationTests.h"
#include "BlenderSelectionTests.h"
#include "BlenderTopologyTests.h"
#include "BlenderMeshEditTests.h"

#include "BlenderStyleEdMode.h"
#include "Components/StaticMeshComponent.h"
#include "Dom/JsonObject.h"
#include "Editor.h"
#include "Editor/TransBuffer.h"
#include "EditorModeManager.h"
#include "EditorViewportClient.h"
#include "Engine/Selection.h"
#include "Engine/StaticMesh.h"
#include "Engine/StaticMeshActor.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformMisc.h"
#include "InputCoreTypes.h"
#include "Input/Events.h"
#include "LevelEditorViewport.h"
#include "Misc/CommandLine.h"
#include "Misc/DateTime.h"
#include "Misc/FileHelper.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"
#include "PreviewScene.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "SEditorViewport.h"
#include "Settings/EditorLoadingSavingSettings.h"
#include "Slate/SceneViewport.h"
#include "UObject/Package.h"

#include <initializer_list>

DEFINE_LOG_CATEGORY_STATIC(LogBlenderControlsVerification, Log, All);

namespace BlenderControlsVerification
{
constexpr double PositionTolerance = 0.001;
constexpr double ScaleTolerance = 0.0001;

struct FReport
{
    TArray<TSharedPtr<FJsonValue>> Checks;
    int32 FailureCount = 0;

    bool Check(const FString& Name, bool bPassed, const FString& Details = FString())
    {
        TSharedRef<FJsonObject> Entry = MakeShared<FJsonObject>();
        Entry->SetStringField(TEXT("name"), Name);
        Entry->SetBoolField(TEXT("passed"), bPassed);
        Entry->SetStringField(TEXT("details"), Details);
        Checks.Add(MakeShared<FJsonValueObject>(Entry));
        FailureCount += bPassed ? 0 : 1;
        if (bPassed)
        {
            UE_LOG(LogBlenderControlsVerification, Display, TEXT("PASS %s %s"), *Name, *Details);
        }
        else
        {
            UE_LOG(LogBlenderControlsVerification, Error, TEXT("FAIL %s %s"), *Name, *Details);
        }
        return bPassed;
    }

    bool Write(const FString& Path, const FString& TestWorld)
    {
        TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
        Root->SetStringField(TEXT("suite"), TEXT("BlenderStyleControls.TargetedEditorIntegration"));
        Root->SetStringField(TEXT("utc"), FDateTime::UtcNow().ToIso8601());
        Root->SetStringField(TEXT("world"), TestWorld);
        Root->SetBoolField(TEXT("passed"), FailureCount == 0);
        Root->SetNumberField(TEXT("checks"), Checks.Num());
        Root->SetNumberField(TEXT("failures"), FailureCount);
        Root->SetBoolField(TEXT("saved_user_maps"), false);
        Root->SetStringField(TEXT("scope"), TEXT("Real editor actors and viewport; typed keys call FEdMode::InputKey. Synthetic FSceneViewport mouse events cover held LMB/MMB with native tracking together, not an independent tracking-only branch. Duplicate starts via StartTransform(bDuplicate=true). Toolbar screenshots are requested; physical clicks, Slate focus routing, pointer movement, snapping and physical Shift+D chord require separate UI checks."));
        Root->SetArrayField(TEXT("results"), Checks);
        FString Json;
        const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Json);
        if (!FJsonSerializer::Serialize(Root, Writer))
        {
            return false;
        }
        IFileManager::Get().MakeDirectory(*FPaths::GetPath(Path), true);
        return FFileHelper::SaveStringToFile(Json, *Path, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
    }
};

void Select(const TArray<AActor*>& Actors)
{
    GEditor->SelectNone(false, true, false);
    for (AActor* Actor : Actors)
    {
        GEditor->SelectActor(Actor, true, false, true);
    }
    GEditor->NoteSelectionChange();
}

AStaticMeshActor* SelectedMeshActor()
{
    for (FSelectionIterator It(*GEditor->GetSelectedActors()); It; ++It)
    {
        if (AStaticMeshActor* Actor = Cast<AStaticMeshActor>(*It))
        {
            return Actor;
        }
    }
    return nullptr;
}

int32 MeshActorCount(UWorld* World)
{
    int32 Count = 0;
    for (TActorIterator<AStaticMeshActor> It(World); It; ++It)
    {
        ++Count;
    }
    return Count;
}

bool IsPresent(UWorld* World, const AActor* Candidate)
{
    for (TActorIterator<AActor> It(World); It; ++It)
    {
        if (*It == Candidate)
        {
            return true;
        }
    }
    return false;
}

AStaticMeshActor* SpawnFixture(UWorld* World, UStaticMesh* Mesh, const TCHAR* Label,
    const FVector& Position, const FRotator& Rotation = FRotator::ZeroRotator,
    const FVector& Scale = FVector::OneVector)
{
    FActorSpawnParameters Parameters;
    Parameters.ObjectFlags |= RF_Transactional;
    Parameters.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
    AStaticMeshActor* Actor = World->SpawnActor<AStaticMeshActor>(Position, Rotation, Parameters);
    if (Actor)
    {
        Actor->SetActorLabel(Label, false);
        UStaticMeshComponent* Component = Actor->GetStaticMeshComponent();
        Component->SetFlags(RF_Transactional);
        Component->SetMobility(EComponentMobility::Movable);
        Component->SetStaticMesh(Mesh);
        Actor->SetActorScale3D(Scale);
    }
    return Actor;
}

bool CheckTransform(FReport& Report, const FString& Name, const AActor* Actor,
    const FVector& Position, const FRotator& Rotation, const FVector& Scale)
{
    const bool bMatches = IsValid(Actor)
        && Actor->GetActorLocation().Equals(Position, PositionTolerance)
        && Actor->GetActorQuat().Equals(Rotation.Quaternion(), 0.0001)
        && Actor->GetActorScale3D().Equals(Scale, ScaleTolerance);
    return Report.Check(Name, bMatches, IsValid(Actor)
        ? FString::Printf(TEXT("actual=[%s] expected location=(%s) rotation=(%s) scale=(%s)"),
            *Actor->GetActorTransform().ToHumanReadableString(), *Position.ToString(),
            *Rotation.ToString(), *Scale.ToString())
        : TEXT("Actor missing"));
}

void Key(FBlenderStyleEdMode* Mode, FLevelEditorViewportClient* Client, const FKey& InKey)
{
    Mode->InputKey(Client, Client->Viewport, InKey, IE_Pressed);
    Mode->InputKey(Client, Client->Viewport, InKey, IE_Released);
}

void Keys(FBlenderStyleEdMode* Mode, FLevelEditorViewportClient* Client,
    std::initializer_list<FKey> InKeys)
{
    for (const FKey& InKey : InKeys)
    {
        Key(Mode, Client, InKey);
    }
}

void ResetUndo()
{
    if (FBlenderStyleEdMode* Mode = static_cast<FBlenderStyleEdMode*>(GLevelEditorModeTools().GetActiveMode(FBlenderStyleEdMode::ModeId)))
    {
        Mode->CancelActiveTransform();
    }
    GEditor->ResetTransaction(FText::FromString(TEXT("Blender controls isolated test fixture")));
}

void CheckSingleTransaction(FReport& Report, const FString& Name,
    TFunctionRef<void(const FString&)> CheckInitial,
    TFunctionRef<void(const FString&)> CheckCommitted)
{
    if (FBlenderStyleEdMode* Mode = static_cast<FBlenderStyleEdMode*>(GLevelEditorModeTools().GetActiveMode(FBlenderStyleEdMode::ModeId)))
    {
        const bool bEnded = !Mode->IsTransforming();
        Report.Check(Name + TEXT(".confirm_ends_modal"), bEnded);
        if (!bEnded)
        {
            // Preserve useful failures without undoing inside an open transaction.
            Mode->CancelActiveTransform();
        }
    }
    Report.Check(Name + TEXT(".one_undo_record"), GEditor->Trans && GEditor->Trans->GetQueueLength() == 1,
        FString::Printf(TEXT("queue length=%d"), GEditor->Trans ? GEditor->Trans->GetQueueLength() : -1));
    const bool bUndo = GEditor->UndoTransaction();
    Report.Check(Name + TEXT(".undo_succeeds"), bUndo);
    CheckInitial(Name + TEXT(".undo_state"));
    Report.Check(Name + TEXT(".undo_exhausted"), GEditor->Trans && !GEditor->Trans->CanUndo());
    Report.Check(Name + TEXT(".redo_succeeds"), GEditor->RedoTransaction());
    CheckCommitted(Name + TEXT(".redo_state"));
}

FLevelEditorViewportClient* FindLevelViewport(UWorld* World)
{
    FLevelEditorViewportClient* Current = GCurrentLevelEditingViewportClient;
    if (Current && Current->Viewport && Current->IsPerspective() && Current->GetWorld() == World)
    {
        return Current;
    }
    for (FLevelEditorViewportClient* Client : GEditor->GetLevelViewportClients())
    {
        if (Client && Client->Viewport && Client->IsPerspective() && Client->GetWorld() == World)
        {
            return Client;
        }
    }
    return nullptr;
}

void CheckNativeMouseTracking(FReport& Report, FBlenderStyleEdMode* Mode,
    FLevelEditorViewportClient* Client, AStaticMeshActor* Actor)
{
    const TSharedPtr<SEditorViewport> EditorWidget = Client->GetEditorViewportWidget();
    const TSharedPtr<FSceneViewport> SceneViewport = EditorWidget.IsValid() ? EditorWidget->GetSceneViewport() : nullptr;
    if (!Report.Check(TEXT("native_tracking.real_scene_viewport"), SceneViewport.IsValid()
        && SceneViewport.Get() == Client->Viewport && SceneViewport->GetSizeXY().GetMin() > 0))
    {
        return;
    }
    if (!Report.Check(TEXT("native_tracking.initially_idle"), !Client->IsTracking()
        && !Client->Viewport->KeyState(EKeys::LeftMouseButton)
        && !Client->Viewport->KeyState(EKeys::MiddleMouseButton)
        && !Client->Viewport->KeyState(EKeys::RightMouseButton)))
    {
        return;
    }

    for (const FKey& MouseButton : {EKeys::LeftMouseButton, EKeys::MiddleMouseButton})
    {
        Select({Actor});
        ResetUndo();
        const FString Prefix = MouseButton == EKeys::LeftMouseButton
            ? TEXT("native_tracking.left") : TEXT("native_tracking.middle");
        const FTransform Initial = Actor->GetActorTransform();
        const FGeometry Geometry = SceneViewport->GetCachedGeometry();
        const FVector2D ScreenPosition(Geometry.LocalToAbsolute(Geometry.GetLocalSize() * 0.5f));
        TSet<FKey> PressedButtons;
        PressedButtons.Add(MouseButton);
        const FPointerEvent Down(0, ScreenPosition, ScreenPosition, PressedButtons, MouseButton, 0.0f, FModifierKeysState());
        // Real scene input sets KeyStateMap and calls the actual level client's
        // InputKey. Calling Client->InputKey alone would not set mouse state.
        // We deliberately do not submit the reply to Slate/OS mouse capture.
        SceneViewport->OnMouseButtonDown(Geometry, Down);
        Report.Check(Prefix + TEXT(".button_is_down"), SceneViewport->KeyState(MouseButton));
        Report.Check(Prefix + TEXT(".native_tracking_suppressed_by_Blender_controls"), !Client->IsTracking());
        Report.Check(Prefix + TEXT(".start_transform_rejected"),
            !Mode->StartTransform(EBlenderTransformOperation::Move, Client, Client->Viewport));
        Mode->CancelActiveTransform();
        for (const FKey& Shortcut : {EKeys::G, EKeys::R, EKeys::S})
        {
            Report.Check(Prefix + TEXT(".held_pointer_") + Shortcut.GetFName().ToString() + TEXT("_consumed_without_transform"),
                Mode->InputKey(Client, Client->Viewport, Shortcut, IE_Pressed));
            Report.Check(Prefix + TEXT(".no_modal_") + Shortcut.GetFName().ToString(), !Mode->IsTransforming());
            Mode->CancelActiveTransform();
        }
        Report.Check(Prefix + TEXT(".actor_unchanged"), Actor->GetActorTransform().Equals(Initial, 0.0001));

        // Always release through the same real scene viewport, even after a
        // failed assertion. UE's release path calls protected StopTracking.
        const TSet<FKey> NoButtons;
        const FPointerEvent Up(0, ScreenPosition, ScreenPosition, NoButtons, MouseButton, 0.0f, FModifierKeysState());
        SceneViewport->OnMouseButtonUp(Geometry, Up);
        if (Client->IsTracking())
        {
            Client->AbortTracking();
        }
        Report.Check(Prefix + TEXT(".released_and_stopped"), !SceneViewport->KeyState(MouseButton) && !Client->IsTracking());
        Actor->SetActorTransform(Initial);
        Select({Actor});
        if (Client->IsTracking())
        {
            // Do not run another fixture or reset transactions while tracking.
            break;
        }
        ResetUndo();
    }
}

void RunCases(FReport& Report, UWorld* World, FLevelEditorViewportClient* Client, UStaticMesh* Cube)
{
    FEditorModeTools& Modes = GLevelEditorModeTools();
    Modes.ActivateMode(FBlenderStyleEdMode::ModeId);
    FBlenderStyleEdMode* Mode = static_cast<FBlenderStyleEdMode*>(Modes.GetActiveMode(FBlenderStyleEdMode::ModeId));
    if (!Report.Check(TEXT("mode.enable"), Mode != nullptr))
    {
        return;
    }
    GEditor->Exec(World, TEXT("BlenderControls.CaptureUI"));

    AStaticMeshActor* Actor = SpawnFixture(World, Cube, TEXT("BlenderVerify_Primary"), FVector(10, 20, 30));
    if (!Report.Check(TEXT("fixture.primary"), Actor != nullptr))
    {
        return;
    }
    Select({Actor});
    ResetUndo();

    // A native drag must retain control of G/R/S and reject another modal tool.
    // This covers real held-button + tracking state together, not a fake client.
    CheckNativeMouseTracking(Report, Mode, Client, Actor);
    if (Client->IsTracking())
    {
        return;
    }

    // Catches incorrect cm units, axis leakage, and missing transaction capture.
    Keys(Mode, Client, {EKeys::G, EKeys::X, EKeys::Two, EKeys::Zero, EKeys::Zero, EKeys::Enter});
    Report.Check(TEXT("move.confirm_ends_modal"), !Mode->IsTransforming());
    CheckTransform(Report, TEXT("move.g_x_200"), Actor, FVector(210, 20, 30), FRotator::ZeroRotator, FVector::OneVector);
    CheckSingleTransaction(Report, TEXT("move"),
        [&](const FString& Name) { CheckTransform(Report, Name, Actor, FVector(10, 20, 30), FRotator::ZeroRotator, FVector::OneVector); },
        [&](const FString& Name) { CheckTransform(Report, Name, Actor, FVector(210, 20, 30), FRotator::ZeroRotator, FVector::OneVector); });

    ResetUndo();
    Keys(Mode, Client, {EKeys::G, EKeys::Z, EKeys::Five, EKeys::Zero});
    CheckTransform(Report, TEXT("cancel.preview_applied"), Actor, FVector(210, 20, 80), FRotator::ZeroRotator, FVector::OneVector);
    Key(Mode, Client, EKeys::Escape);
    Report.Check(TEXT("cancel.ends_modal"), !Mode->IsTransforming());
    CheckTransform(Report, TEXT("cancel.restores_exact_initial"), Actor, FVector(210, 20, 30), FRotator::ZeroRotator, FVector::OneVector);
    Report.Check(TEXT("cancel.no_undo_record"), !GEditor->Trans->CanUndo());

    // Switching axes must recompute from the start, without leaving an X delta.
    ResetUndo();
    Keys(Mode, Client, {EKeys::G, EKeys::X, EKeys::Two, EKeys::Zero, EKeys::Zero, EKeys::Y});
    CheckTransform(Report, TEXT("axis_change.recomputes_from_start"), Actor, FVector(210, 220, 30), FRotator::ZeroRotator, FVector::OneVector);
    Key(Mode, Client, EKeys::Escape);

    ResetUndo();
    Keys(Mode, Client, {EKeys::G, EKeys::Y, EKeys::Subtract, EKeys::One, EKeys::Two, EKeys::Decimal, EKeys::Five, EKeys::Enter});
    CheckTransform(Report, TEXT("numeric.negative_decimal"), Actor, FVector(210, 7.5, 30), FRotator::ZeroRotator, FVector::OneVector);
    CheckSingleTransaction(Report, TEXT("numeric"),
        [&](const FString& Name) { CheckTransform(Report, Name, Actor, FVector(210, 20, 30), FRotator::ZeroRotator, FVector::OneVector); },
        [&](const FString& Name) { CheckTransform(Report, Name, Actor, FVector(210, 7.5, 30), FRotator::ZeroRotator, FVector::OneVector); });

    Actor->SetActorTransform(FTransform(FRotator(0, 90, 0), FVector(10, 20, 30), FVector(2, 3, 4)));
    Select({Actor});
    ResetUndo();
    Keys(Mode, Client, {EKeys::S, EKeys::X, EKeys::Two, EKeys::Enter});
    CheckTransform(Report, TEXT("scale.s_x_2_is_local"), Actor, FVector(10, 20, 30), FRotator(0, 90, 0), FVector(4, 3, 4));
    CheckSingleTransaction(Report, TEXT("scale"),
        [&](const FString& Name) { CheckTransform(Report, Name, Actor, FVector(10, 20, 30), FRotator(0, 90, 0), FVector(2, 3, 4)); },
        [&](const FString& Name) { CheckTransform(Report, Name, Actor, FVector(10, 20, 30), FRotator(0, 90, 0), FVector(4, 3, 4)); });

    Actor->SetActorTransform(FTransform(FRotator::ZeroRotator, FVector(10, 20, 30), FVector::OneVector));
    Select({Actor});
    ResetUndo();
    Keys(Mode, Client, {EKeys::R, EKeys::Z, EKeys::Nine, EKeys::Zero, EKeys::Enter});
    CheckTransform(Report, TEXT("rotate.r_z_90_degrees"), Actor, FVector(10, 20, 30), FRotator(0, 90, 0), FVector::OneVector);
    CheckSingleTransaction(Report, TEXT("rotate"),
        [&](const FString& Name) { CheckTransform(Report, Name, Actor, FVector(10, 20, 30), FRotator::ZeroRotator, FVector::OneVector); },
        [&](const FString& Name) { CheckTransform(Report, Name, Actor, FVector(10, 20, 30), FRotator(0, 90, 0), FVector::OneVector); });

    Actor->SetActorTransform(FTransform(FRotator::ZeroRotator, FVector(10, 20, 30), FVector::OneVector));
    Select({Actor});
    ResetUndo();
    const int32 BeforeCopy = MeshActorCount(World);
    Report.Check(TEXT("duplicate.starts"), Mode->StartTransform(EBlenderTransformOperation::Move, Client, Client->Viewport, true));
    AStaticMeshActor* Copy = SelectedMeshActor();
    const bool bCopyValid = Copy && Copy != Actor;
    Report.Check(TEXT("duplicate.new_actor_selected"), bCopyValid && MeshActorCount(World) == BeforeCopy + 1);
    Report.Check(TEXT("duplicate.same_mesh_resource"), bCopyValid && Copy->GetStaticMeshComponent()->GetStaticMesh() == Actor->GetStaticMeshComponent()->GetStaticMesh());
    Keys(Mode, Client, {EKeys::X, EKeys::Two, EKeys::Zero, EKeys::Zero, EKeys::Enter});
    if (bCopyValid)
    {
        CheckTransform(Report, TEXT("duplicate.copy_moved"), Copy, FVector(210, 20, 30), FRotator::ZeroRotator, FVector::OneVector);
    }
    CheckTransform(Report, TEXT("duplicate.original_unchanged"), Actor, FVector(10, 20, 30), FRotator::ZeroRotator, FVector::OneVector);
    CheckSingleTransaction(Report, TEXT("duplicate"),
        [&](const FString& Name) { Report.Check(Name, MeshActorCount(World) == BeforeCopy && !IsPresent(World, Copy)); },
        [&](const FString& Name)
        {
            Report.Check(Name + TEXT(".count"), MeshActorCount(World) == BeforeCopy + 1);
            Report.Check(Name + TEXT(".actor_present"), IsPresent(World, Copy));
            if (IsPresent(World, Copy))
            {
                CheckTransform(Report, Name + TEXT(".transform"), Copy, FVector(210, 20, 30), FRotator::ZeroRotator, FVector::OneVector);
            }
        });

    Select({Actor});
    ResetUndo();
    const int32 BeforeCancelCopy = MeshActorCount(World);
    Report.Check(TEXT("duplicate_cancel.starts"), Mode->StartTransform(EBlenderTransformOperation::Move, Client, Client->Viewport, true));
    AStaticMeshActor* CancelledCopy = SelectedMeshActor();
    Report.Check(TEXT("duplicate_cancel.created_copy"), CancelledCopy && CancelledCopy != Actor && MeshActorCount(World) == BeforeCancelCopy + 1);
    Keys(Mode, Client, {EKeys::Z, EKeys::Five, EKeys::Zero, EKeys::Escape});
    Report.Check(TEXT("duplicate_cancel.removes_only_new_copy"), MeshActorCount(World) == BeforeCancelCopy && !IsPresent(World, CancelledCopy));
    Report.Check(TEXT("duplicate_cancel.restores_selection"), GEditor->GetSelectedActorCount() == 1 && Actor->IsSelected());
    CheckTransform(Report, TEXT("duplicate_cancel.original_restored"), Actor, FVector(10, 20, 30), FRotator::ZeroRotator, FVector::OneVector);
    Report.Check(TEXT("duplicate_cancel.no_undo_record"), !GEditor->Trans->CanUndo());

    AStaticMeshActor* Other = SpawnFixture(World, Cube, TEXT("BlenderVerify_Other"), FVector(50, 70, 90));
    if (Report.Check(TEXT("fixture.other"), Other != nullptr))
    {
        Select({Actor, Other});
        ResetUndo();
        Keys(Mode, Client, {EKeys::G, EKeys::Y, EKeys::One, EKeys::Zero, EKeys::Zero, EKeys::Enter});
        auto CheckInitial = [&](const FString& Name)
        {
            CheckTransform(Report, Name + TEXT(".first"), Actor, FVector(10, 20, 30), FRotator::ZeroRotator, FVector::OneVector);
            CheckTransform(Report, Name + TEXT(".second"), Other, FVector(50, 70, 90), FRotator::ZeroRotator, FVector::OneVector);
        };
        auto CheckMoved = [&](const FString& Name)
        {
            CheckTransform(Report, Name + TEXT(".first"), Actor, FVector(10, 120, 30), FRotator::ZeroRotator, FVector::OneVector);
            CheckTransform(Report, Name + TEXT(".second"), Other, FVector(50, 170, 90), FRotator::ZeroRotator, FVector::OneVector);
        };
        CheckMoved(TEXT("multi.same_delta"));
        CheckSingleTransaction(Report, TEXT("multi"), CheckInitial, CheckMoved);
    }

    AStaticMeshActor* Parent = SpawnFixture(World, Cube, TEXT("BlenderVerify_Parent"), FVector(100, 50, 30));
    AStaticMeshActor* Child = SpawnFixture(World, Cube, TEXT("BlenderVerify_Child"), FVector(120, 70, 80));
    if (Report.Check(TEXT("fixture.hierarchy"), Parent && Child))
    {
        Report.Check(TEXT("hierarchy.attached"), Child->AttachToActor(Parent, FAttachmentTransformRules::KeepWorldTransform));
        Select({Parent, Child});
        ResetUndo();
        Keys(Mode, Client, {EKeys::G, EKeys::X, EKeys::Two, EKeys::Zero, EKeys::Zero, EKeys::Enter});
        auto CheckInitial = [&](const FString& Name)
        {
            CheckTransform(Report, Name + TEXT(".parent"), Parent, FVector(100, 50, 30), FRotator::ZeroRotator, FVector::OneVector);
            CheckTransform(Report, Name + TEXT(".child"), Child, FVector(120, 70, 80), FRotator::ZeroRotator, FVector::OneVector);
        };
        auto CheckMoved = [&](const FString& Name)
        {
            CheckTransform(Report, Name + TEXT(".parent"), Parent, FVector(300, 50, 30), FRotator::ZeroRotator, FVector::OneVector);
            CheckTransform(Report, Name + TEXT(".child_no_double_delta"), Child, FVector(320, 70, 80), FRotator::ZeroRotator, FVector::OneVector);
        };
        CheckMoved(TEXT("hierarchy.move"));
        CheckSingleTransaction(Report, TEXT("hierarchy"), CheckInitial, CheckMoved);
    }

    Select({Actor});
    ResetUndo();
    const FTransform BeforeInvalidViewport = Actor->GetActorTransform();
    {
        // A real asset-preview viewport client, not a forged pointer or mode mock.
        FPreviewScene PreviewScene(FPreviewScene::ConstructionValues().SetCreatePhysicsScene(false));
        FEditorViewportClient AssetClient(nullptr, &PreviewScene);
        Report.Check(TEXT("viewport.non_level_fixture"), !AssetClient.IsLevelEditorClient());
        Report.Check(TEXT("viewport.non_level_key_unhandled"), !Mode->InputKey(&AssetClient, Client->Viewport, EKeys::G, IE_Pressed));
        Report.Check(TEXT("viewport.non_level_start_rejected"), !Mode->StartTransform(EBlenderTransformOperation::Move, &AssetClient, Client->Viewport));
        Report.Check(TEXT("viewport.non_level_no_modal"), !Mode->IsTransforming());
        Report.Check(TEXT("viewport.non_level_no_actor_change"), Actor->GetActorTransform().Equals(BeforeInvalidViewport, 0.0001));
        Mode->CancelActiveTransform();
    }

    // Exiting the mode during a preview must cancel, including its transaction.
    Select({Actor});
    ResetUndo();
    const FTransform BeforeDisable = Actor->GetActorTransform();
    Keys(Mode, Client, {EKeys::G, EKeys::Z, EKeys::Two, EKeys::Zero});
    Report.Check(TEXT("disable.preview_was_active"), Mode->IsTransforming()
        && !Actor->GetActorTransform().Equals(BeforeDisable, 0.0001));
    Modes.DeactivateMode(FBlenderStyleEdMode::ModeId);
    Mode = nullptr;
    Report.Check(TEXT("disable.mode_inactive"), !Modes.IsModeActive(FBlenderStyleEdMode::ModeId));
    Report.Check(TEXT("disable.restores_transform"), Actor->GetActorTransform().Equals(BeforeDisable, 0.0001));
    Report.Check(TEXT("disable.no_undo_record"), !GEditor->Trans->CanUndo());
}
} // namespace BlenderControlsVerification

void RunBlenderStyleControlsVerification()
{
    using namespace BlenderControlsVerification;
    // This is deliberately unavailable in an ordinary user editor session.
    if (!FParse::Param(FCommandLine::Get(), TEXT("BlenderControlsVerify")))
    {
        UE_LOG(LogBlenderControlsVerification, Warning, TEXT("Refused: verification requires a dedicated editor process launched with -BlenderControlsVerify."));
        return;
    }
    static bool bStarted = false;
    if (bStarted)
    {
        return;
    }
    bStarted = true;

    FString ReportPath = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("BlenderStyleControls/verification.json"));
    FParse::Value(FCommandLine::Get(), TEXT("BlenderControlsReport="), ReportPath);
    ReportPath = FPaths::ConvertRelativePathToFull(ReportPath);
    FReport Report;
    FString TestWorldName;
    UWorld* TestWorld = nullptr;
    UEditorLoadingSavingSettings* LoadingSettings = GetMutableDefault<UEditorLoadingSavingSettings>();
    const bool bPreviousAutosave = LoadingSettings->bAutoSaveEnable;
    LoadingSettings->bAutoSaveEnable = false; // In-memory only; no SaveConfig.

    if (Report.Check(TEXT("precondition.editor_available"), GEditor != nullptr)
        && Report.Check(TEXT("precondition.not_pie"), GEditor->PlayWorld == nullptr))
    {
        FEditorModeTools& Modes = GLevelEditorModeTools();
        Report.Check(TEXT("mode.default_off"), !Modes.IsModeActive(FBlenderStyleEdMode::ModeId));
        Modes.DeactivateMode(FBlenderStyleEdMode::ModeId);
        GEditor->Exec(GEditor->GetEditorWorldContext().World(), TEXT("BlenderControls.CaptureUI"));
        // NewMap directly discards the startup map without saving it. Do NOT use
        // CreateNewMapForEditing(false): UE5.4 still calls SaveDirtyPackages there.
        TestWorld = GEditor->NewMap(false);
        if (Report.Check(TEXT("fixture.unsaved_blank_world"), TestWorld && TestWorld->WorldType == EWorldType::Editor
            && TestWorld->GetOutermost()->HasAnyPackageFlags(PKG_NewlyCreated)))
        {
            TestWorldName = TestWorld->GetPathName();
            FLevelEditorViewportClient* Client = FindLevelViewport(TestWorld);
            UStaticMesh* Cube = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cube.Cube"));
            const bool bReady = Report.Check(TEXT("precondition.real_level_viewport"), Client != nullptr,
                TEXT("Use UnrealEditor.exe with Slate initialized and -RenderOffscreen, not a commandlet."));
            if (Report.Check(TEXT("fixture.engine_cube"), Cube != nullptr) && bReady)
            {
                const bool bFocusedFixes = FParse::Param(FCommandLine::Get(), TEXT("BlenderControlsFocusedFixes"));
                const bool bPreviewOnly = FParse::Param(FCommandLine::Get(), TEXT("BlenderControlsPreviewTests"));
                if (!bFocusedFixes && !bPreviewOnly) RunCases(Report, TestWorld, Client, Cube);
                Modes.Tick(Client, 0.016f);
                if (bPreviewOnly)
                {
                    AppendBlenderViewportVerification(Client, Client->Viewport, Report.Checks, Report.FailureCount);
                }
                else
                {
                AppendBlenderMeshEditVerification(Client, Client->Viewport, Report.Checks, Report.FailureCount);
                AppendBlenderNavigationVerification(Client, Client->Viewport, Report.Checks, Report.FailureCount);
                AppendBlenderTopologyVerification(Client, Client->Viewport, Report.Checks, Report.FailureCount);
                if (!bFocusedFixes)
                {
                    AppendBlenderSelectionVerification(Client, Client->Viewport, Report.Checks, Report.FailureCount);
                    AppendBlenderViewportVerification(Client, Client->Viewport, Report.Checks, Report.FailureCount);
                }
                }
            }
        }
        if (FBlenderStyleEdMode* Mode = static_cast<FBlenderStyleEdMode*>(Modes.GetActiveMode(FBlenderStyleEdMode::ModeId)))
        {
            Mode->CancelActiveTransform();
        }
        Modes.DeactivateMode(FBlenderStyleEdMode::ModeId);
        GEditor->SelectNone(false, true, false);
        ResetUndo();
        if (TestWorld && GEditor->GetEditorWorldContext().World() == TestWorld)
        {
            TestWorld->GetOutermost()->SetDirtyFlag(false);
            // Discard only our generated map; leave a fresh empty map for exit.
            UWorld* ClosingWorld = GEditor->NewMap(false);
            if (ClosingWorld)
            {
                ClosingWorld->GetOutermost()->SetDirtyFlag(false);
            }
            Report.Check(TEXT("cleanup.test_world_discarded"), ClosingWorld && MeshActorCount(ClosingWorld) == 0);
        }
    }
    LoadingSettings->bAutoSaveEnable = bPreviousAutosave;
    const bool bWritten = Report.Write(ReportPath, TestWorldName);
    UE_LOG(LogBlenderControlsVerification, Display, TEXT("Verification complete: %d checks, %d failures; report=%s; written=%s"),
        Report.Checks.Num(), Report.FailureCount, *ReportPath, bWritten ? TEXT("true") : TEXT("false"));
    FPlatformMisc::RequestExitWithStatus(false, Report.FailureCount == 0 && bWritten ? 0 : 1);
}
