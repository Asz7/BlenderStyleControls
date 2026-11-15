#include "BlenderMeshEditTests.h"

#include "BlenderMeshEditSession.h"
#include "BlenderStyleEdMode.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "CollisionQueryParams.h"
#include "Components/StaticMeshComponent.h"
#include "Dom/JsonObject.h"
#include "Editor.h"
#include "EditorModeManager.h"
#include "EditorViewportClient.h"
#include "Engine/Selection.h"
#include "Engine/StaticMesh.h"
#include "Engine/StaticMeshActor.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "InputCoreTypes.h"
#include "Input/Events.h"
#include "MeshDescription.h"
#include "Misc/CommandLine.h"
#include "Misc/PackageName.h"
#include "Misc/Parse.h"
#include "PhysicsEngine/BodySetup.h"
#include "SceneView.h"
#include "SEditorViewport.h"
#include "Slate/SceneViewport.h"
#include "StaticMeshAttributes.h"
#include "StaticMeshCompiler.h"
#include "StaticMeshResources.h"
#include "UObject/Package.h"
#include "UObject/StrongObjectPtr.h"
#include "UObject/UObjectIterator.h"

#include <initializer_list>

DEFINE_LOG_CATEGORY_STATIC(LogBlenderMeshVerification, Log, All);

namespace BlenderMeshVerification
{
constexpr double PositionTolerance = 0.002;

struct FReport
{
    TArray<TSharedPtr<FJsonValue>>& Checks;
    int32& Failures;

    bool Check(const TCHAR* Name, bool bPassed, const FString& Details = FString())
    {
        TSharedRef<FJsonObject> Entry = MakeShared<FJsonObject>();
        Entry->SetStringField(TEXT("name"), Name);
        Entry->SetBoolField(TEXT("passed"), bPassed);
        Entry->SetStringField(TEXT("details"), Details);
        Checks.Add(MakeShared<FJsonValueObject>(Entry));
        Failures += bPassed ? 0 : 1;
        if (bPassed)
        {
            UE_LOG(LogBlenderMeshVerification, Display, TEXT("PASS %s %s"), Name, *Details);
        }
        else
        {
            UE_LOG(LogBlenderMeshVerification, Error, TEXT("FAIL %s %s"), Name, *Details);
        }
        return bPassed;
    }
};

// Values and element IDs are captured explicitly: counts alone cannot detect mutation.
struct FMeshSnapshot
{
    bool bValid = false;
    int32 EdgeCount = 0;
    int32 TriangleCount = 0;
    int32 UVChannels = 0;
    TMap<int32, FVector3f> Positions;
    TMap<int32, FVector4f> Colors;
    TMap<int32, TArray<FVector2f>> UVs;
    TMap<int32, TArray<int32>> PolygonCorners;
    TMap<int32, int32> InstanceVertices;
    TMap<int32, int32> PolygonGroups;
    TMap<int32, FIntPoint> EdgeVertices;

    explicit FMeshSnapshot(UStaticMesh* Mesh)
    {
        const FMeshDescription* Description = Mesh ? Mesh->GetMeshDescription(0) : nullptr;
        if (!Description)
        {
            return;
        }
        bValid = true;
        EdgeCount = Description->Edges().Num();
        TriangleCount = Description->Triangles().Num();
        const FStaticMeshConstAttributes Attributes(*Description);
        const auto VertexPositions = Attributes.GetVertexPositions();
        const auto InstanceColors = Attributes.GetVertexInstanceColors();
        const auto InstanceUVs = Attributes.GetVertexInstanceUVs();
        UVChannels = InstanceUVs.GetNumChannels();
        for (const FVertexID ID : Description->Vertices().GetElementIDs())
        {
            Positions.Add(ID.GetValue(), VertexPositions[ID]);
        }
        for (const FVertexInstanceID ID : Description->VertexInstances().GetElementIDs())
        {
            InstanceVertices.Add(ID.GetValue(), Description->GetVertexInstanceVertex(ID).GetValue());
            Colors.Add(ID.GetValue(), InstanceColors[ID]);
            TArray<FVector2f>& Values = UVs.Add(ID.GetValue());
            for (int32 Channel = 0; Channel < UVChannels; ++Channel)
            {
                Values.Add(InstanceUVs.Get(ID, Channel));
            }
        }
        for (const FPolygonID ID : Description->Polygons().GetElementIDs())
        {
            PolygonGroups.Add(ID.GetValue(), Description->GetPolygonPolygonGroup(ID).GetValue());
            TArray<int32>& Corners = PolygonCorners.Add(ID.GetValue());
            for (const FVertexInstanceID Corner : Description->GetPolygonVertexInstances(ID))
            {
                Corners.Add(Corner.GetValue());
            }
        }
        for (const FEdgeID ID : Description->Edges().GetElementIDs())
        {
            EdgeVertices.Add(ID.GetValue(), FIntPoint(Description->GetEdgeVertex(ID, 0).GetValue(),
                Description->GetEdgeVertex(ID, 1).GetValue()));
        }
    }

    bool SameAttributesAndTopology(const FMeshSnapshot& Other) const
    {
        if (!bValid || !Other.bValid || EdgeCount != Other.EdgeCount
            || TriangleCount != Other.TriangleCount || UVChannels != Other.UVChannels
            || Positions.Num() != Other.Positions.Num() || Colors.Num() != Other.Colors.Num()
            || UVs.Num() != Other.UVs.Num() || PolygonCorners.Num() != Other.PolygonCorners.Num())
        {
            return false;
        }
        for (const auto& Pair : Colors)
        {
            const FVector4f* Value = Other.Colors.Find(Pair.Key);
            if (!Value || *Value != Pair.Value) return false;
        }
        for (const auto& Pair : UVs)
        {
            const TArray<FVector2f>* Value = Other.UVs.Find(Pair.Key);
            if (!Value || *Value != Pair.Value) return false;
        }
        for (const auto& Pair : PolygonCorners)
        {
            const TArray<int32>* Value = Other.PolygonCorners.Find(Pair.Key);
            if (!Value || *Value != Pair.Value) return false;
        }
        for (const auto& Pair : InstanceVertices)
        {
            const int32* Value = Other.InstanceVertices.Find(Pair.Key);
            if (!Value || *Value != Pair.Value) return false;
        }
        for (const auto& Pair : PolygonGroups)
        {
            const int32* Value = Other.PolygonGroups.Find(Pair.Key);
            if (!Value || *Value != Pair.Value) return false;
        }
        for (const auto& Pair : EdgeVertices)
        {
            const FIntPoint* Value = Other.EdgeVertices.Find(Pair.Key);
            if (!Value || *Value != Pair.Value) return false;
        }
        return true;
    }

    bool SamePositions(const FMeshSnapshot& Other) const
    {
        if (!bValid || !Other.bValid || Positions.Num() != Other.Positions.Num()) return false;
        for (const auto& Pair : Positions)
        {
            const FVector3f* Value = Other.Positions.Find(Pair.Key);
            if (!Value || *Value != Pair.Value) return false;
        }
        return true;
    }
};

void Select(const TArray<AActor*>& Actors)
{
    GEditor->SelectNone(false, true, false);
    for (AActor* Actor : Actors)
    {
        if (IsValid(Actor)) GEditor->SelectActor(Actor, true, false, true);
    }
    GEditor->NoteSelectionChange();
}

void Keys(FBlenderStyleEdMode* Mode, FEditorViewportClient* Client, FViewport* Viewport,
    std::initializer_list<FKey> InKeys)
{
    for (const FKey& Key : InKeys)
    {
        Mode->InputKey(Client, Viewport, Key, IE_Pressed);
        Mode->InputKey(Client, Viewport, Key, IE_Released);
    }
}

AStaticMeshActor* SpawnFixture(UWorld* World, UStaticMesh* Mesh, const TCHAR* Label,
    const FVector& Position, const FRotator& Rotation, const FVector& Scale)
{
    FActorSpawnParameters Parameters;
    Parameters.ObjectFlags |= RF_Transactional;
    Parameters.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
    AStaticMeshActor* Actor = World->SpawnActor<AStaticMeshActor>(Position, Rotation, Parameters);
    if (Actor)
    {
        Actor->SetActorLabel(Label, false);
        Actor->GetStaticMeshComponent()->SetFlags(RF_Transactional);
        Actor->GetStaticMeshComponent()->SetMobility(EComponentMobility::Movable);
        Actor->GetStaticMeshComponent()->SetStaticMesh(Mesh);
        Actor->SetActorScale3D(Scale);
    }
    return Actor;
}

TSet<UStaticMesh*> EditAssets()
{
    TSet<UStaticMesh*> Assets;
    for (TObjectIterator<UStaticMesh> It; It; ++It)
    {
        if (IsValid(*It) && It->GetOutermost()->GetName().StartsWith(TEXT("/Game/BlenderEdits/")))
        {
            Assets.Add(*It);
        }
    }
    return Assets;
}

int32 ActorCount(UWorld* World)
{
    int32 Count = 0;
    for (TActorIterator<AActor> It(World); It; ++It) ++Count;
    return Count;
}

int32 MeshComponentCount(UWorld* World)
{
    int32 Count = 0;
    for (TObjectIterator<UStaticMeshComponent> It; It; ++It)
    {
        if (IsValid(*It) && !It->IsTemplate() && It->GetWorld() == World
            && IsValid(It->GetOwner()) && !It->GetOwner()->IsActorBeingDestroyed()) ++Count;
    }
    return Count;
}

uint64 VertexKey(int32 ObjectIndex, int32 VertexID)
{
    return (static_cast<uint64>(static_cast<uint32>(ObjectIndex)) << 32)
        | static_cast<uint32>(VertexID);
}

TMap<uint64, FVector> Positions(FBlenderMeshEditSession& Session)
{
    TMap<uint64, FVector> Result;
    for (int32 Object = 0; Object < Session.GetObjectCount(); ++Object)
    {
        for (const int32 ID : Session.GetElementIDs(Object, EBlenderMeshSelectionMode::Vertex))
        {
            FVector Position;
            if (Session.GetVertexWorldPosition(Object, ID, Position))
                Result.Add(VertexKey(Object, ID), Position);
        }
    }
    return Result;
}

bool CheckMappedPositions(FBlenderMeshEditSession& Session, const TMap<uint64, FVector>& Before,
    const TSet<uint64>& Selected, TFunctionRef<FVector(const FVector&)> Map)
{
    const TMap<uint64, FVector> After = Positions(Session);
    if (After.Num() != Before.Num()) return false;
    for (const auto& Pair : Before)
    {
        const FVector* Position = After.Find(Pair.Key);
        const FVector Expected = Selected.Contains(Pair.Key) ? Map(Pair.Value) : Pair.Value;
        if (!Position || !Position->Equals(Expected, PositionTolerance)) return false;
    }
    return true;
}

bool Guide(FBlenderStyleEdMode* Mode, const FVector& Center, const FVector& Direction,
    const FLinearColor& Color)
{
    FVector ActualCenter, ActualDirection;
    FLinearColor ActualColor;
    if (!Mode->GetActiveAxisGuide(ActualCenter, ActualDirection, ActualColor)) return false;
    // Check red/green/blue identity without coupling tests to an artist's exact tint.
    const bool bColor = Color.R > 0.5f
        ? ActualColor.R > 0.6f && ActualColor.R > 2 * ActualColor.G && ActualColor.R > 2 * ActualColor.B
        : Color.G > 0.5f
        ? ActualColor.G > 0.6f && ActualColor.G > 2 * ActualColor.R && ActualColor.G > 2 * ActualColor.B
        : ActualColor.B > 0.6f && ActualColor.B > 2 * ActualColor.R && ActualColor.B > 2 * ActualColor.G;
    return ActualCenter.Equals(Center, PositionTolerance)
        && ActualDirection.Equals(Direction, 0.0001) && bColor;
}

void CheckViewportFacePicking(FReport& Report, FBlenderStyleEdMode* Mode,
    FEditorViewportClient* Client, FViewport* Viewport, FBlenderMeshEditSession& Session)
{
    const TSharedPtr<SEditorViewport> Widget = Client->GetEditorViewportWidget();
    const TSharedPtr<FSceneViewport> SceneViewport = Widget.IsValid() ? Widget->GetSceneViewport() : nullptr;
    if (!Report.Check(TEXT("mesh.pick_scene_viewport_idle_without_capture"), SceneViewport.IsValid()
        && SceneViewport.Get() == Viewport && SceneViewport->GetSizeXY().GetMin() > 0
        && !SceneViewport->HasMouseCapture() && SceneViewport->IsCursorVisible() && !Client->IsTracking())) return;

    const FVector PreviousViewLocation = Client->GetViewLocation();
    const FRotator PreviousViewRotation = Client->GetViewRotation();
    const FVector LookAt(180, 35, 30);
    const FVector ViewLocation = LookAt + FVector(0, -850, 400);
    Client->SetViewLocation(ViewLocation);
    Client->SetViewRotation((LookAt - ViewLocation).Rotation());
    Session.SetSelectionMode(EBlenderMeshSelectionMode::Face);
    Session.SelectAll(false);
    FSceneViewFamilyContext Family(FSceneViewFamily::ConstructionValues(Viewport,
        Client->GetScene(), Client->EngineShowFlags).SetRealtimeUpdate(Client->IsRealtime()));
    FSceneView* View = Client->CalcSceneView(&Family);
    FVector2D FacePixels[2];
    FVector FaceCenters[2];
    int32 FaceIDs[] = {INDEX_NONE, INDEX_NONE};
    bool bProjected = View != nullptr;
    for (int32 Object = 0; Object < 2 && bProjected; ++Object)
    {
        const FMeshDescription* Description = Session.GetSourceMesh(Object)->GetMeshDescription(0);
        double NearestDistance = TNumericLimits<double>::Max();
        for (const int32 ID : Session.GetElementIDs(Object, EBlenderMeshSelectionMode::Face))
        {
            const TArray<FVertexInstanceID> Corners = Description->GetPolygonVertexInstances(FPolygonID(ID));
            FVector Center = FVector::ZeroVector;
            bool bValid = !Corners.IsEmpty();
            for (const FVertexInstanceID Corner : Corners)
            {
                FVector Position;
                bValid &= Session.GetVertexWorldPosition(Object, Description->GetVertexInstanceVertex(Corner).GetValue(), Position);
                Center += Position;
            }
            if (!bValid) continue;
            Center /= Corners.Num();
            FVector2D Pixel;
            const double Distance = FVector::DistSquared(Center, ViewLocation);
            if (Distance < NearestDistance && View->WorldToPixel(Center, Pixel)
                && Pixel.X > 2 && Pixel.Y > 2 && Pixel.X < Viewport->GetSizeXY().X - 2 && Pixel.Y < Viewport->GetSizeXY().Y - 2)
            {
                NearestDistance = Distance;
                FaceIDs[Object] = ID;
                FacePixels[Object] = Pixel;
                FaceCenters[Object] = Center;
            }
        }
        bProjected &= FaceIDs[Object] != INDEX_NONE;
    }

    const FGeometry Geometry = SceneViewport->GetCachedGeometry();
    auto MoveAndClick = [&](int32 Object, const FModifierKeysState& Modifiers)
    {
        // WorldToPixel returns physical viewport pixels; the scene geometry is in Slate units.
        const FVector2D Screen = Geometry.LocalToAbsolute(FacePixels[Object] / Geometry.Scale);
        const TSet<FKey> NoButtons;
        const FPointerEvent Move(0, Screen, Screen, NoButtons, EKeys::Invalid, 0.0f, Modifiers);
        SceneViewport->OnMouseMove(Geometry, Move);
        const bool bCachedPosition = FMath::Abs(SceneViewport->GetMouseX() - FacePixels[Object].X) <= 2.0
            && FMath::Abs(SceneViewport->GetMouseY() - FacePixels[Object].Y) <= 2.0;
        // Route the click through the mode; do not submit mouse-capture replies to Slate or the OS.
        Keys(Mode, Client, Viewport, {EKeys::LeftMouseButton});
        return bCachedPosition && !Client->IsTracking();
    };
    bool bFirst = false, bAdd = false, bRemove = false, bShiftState = false;
    if (bProjected)
    {
        bFirst = MoveAndClick(0, FModifierKeysState())
            && Session.GetSelectedElementIDs(0, EBlenderMeshSelectionMode::Face) == TArray<int32>{FaceIDs[0]}
            && Session.GetSelectedElementIDs(1, EBlenderMeshSelectionMode::Face).IsEmpty();
        const FModifierKeysState ShiftModifiers(true, false, false, false, false, false, false, false, false);
        SceneViewport->OnKeyDown(Geometry, FKeyEvent(EKeys::LeftShift, ShiftModifiers, 0, false, 0, 0));
        bShiftState = SceneViewport->KeyState(EKeys::LeftShift);
        bAdd = MoveAndClick(1, ShiftModifiers)
            && Session.GetSelectedElementIDs(0, EBlenderMeshSelectionMode::Face) == TArray<int32>{FaceIDs[0]}
            && Session.GetSelectedElementIDs(1, EBlenderMeshSelectionMode::Face) == TArray<int32>{FaceIDs[1]};
        bRemove = MoveAndClick(1, ShiftModifiers)
            && Session.GetSelectedElementIDs(0, EBlenderMeshSelectionMode::Face) == TArray<int32>{FaceIDs[0]}
            && Session.GetSelectedElementIDs(1, EBlenderMeshSelectionMode::Face).IsEmpty();
        SceneViewport->OnKeyUp(Geometry, FKeyEvent(EKeys::LeftShift, FModifierKeysState(), 0, false, 0, 0));
    }
    Report.Check(TEXT("mesh.projected_face_pick_then_shift_add_remove_across_objects"),
        bProjected && bFirst && bShiftState && bAdd && bRemove && !SceneViewport->KeyState(EKeys::LeftShift),
        FString::Printf(TEXT("projected=%d first=%d shift=%d add=%d remove=%d; synthetic scene pointer and modifier, mode LMB routing, no OS input"),
            bProjected, bFirst, bShiftState, bAdd, bRemove));
    if (bProjected)
    {
        AStaticMeshActor* Occluder = SpawnFixture(Client->GetWorld(), Session.GetSourceMesh(0),
            TEXT("BlenderMeshVerify_ExternalOccluder"), (ViewLocation + FaceCenters[0]) * 0.5,
            FRotator::ZeroRotator, FVector(0.6));
        bool bHiddenCollider = false, bHiddenPick = false, bVisibleBlocked = false;
        if (Occluder)
        {
            Occluder->GetStaticMeshComponent()->SetCollisionEnabled(ECollisionEnabled::QueryOnly);
            Occluder->GetStaticMeshComponent()->SetCollisionResponseToAllChannels(ECR_Block);
            Occluder->SetIsTemporarilyHiddenInEditor(true);
            FHitResult Hit;
            FCollisionQueryParams Params(SCENE_QUERY_STAT(BlenderMeshHiddenOccluderVerification), true);
            bHiddenCollider = Client->GetWorld()->LineTraceSingleByChannel(Hit, ViewLocation,
                FaceCenters[0], ECC_Visibility, Params) && Hit.GetActor() == Occluder;
            Session.SelectAll(false);
            bHiddenPick = MoveAndClick(0, FModifierKeysState())
                && Session.GetSelectedElementIDs(0, EBlenderMeshSelectionMode::Face) == TArray<int32>{FaceIDs[0]};
            Occluder->SetIsTemporarilyHiddenInEditor(false);
            Session.SelectAll(false);
            bVisibleBlocked = MoveAndClick(0, FModifierKeysState())
                && Session.GetSelectedElementIDs(0, EBlenderMeshSelectionMode::Face).IsEmpty()
                && Session.GetSelectedElementIDs(1, EBlenderMeshSelectionMode::Face).IsEmpty();
            Client->GetWorld()->DestroyActor(Occluder);
        }
        Report.Check(TEXT("mesh.pick_ignores_hidden_external_collider_but_respects_visible_occluder"),
            bHiddenCollider && bHiddenPick && bVisibleBlocked,
            FString::Printf(TEXT("Hidden actor still physically blocks=%d hidden pick=%d visible blocks pick=%d"),
                bHiddenCollider, bHiddenPick, bVisibleBlocked));
    }
    Client->SetViewLocation(PreviousViewLocation);
    Client->SetViewRotation(PreviousViewRotation);
    Client->Invalidate();
}

void CheckGeneratedLightmapUVs(FReport& Report, FBlenderStyleEdMode* Mode,
    FEditorViewportClient* Client, FViewport* Viewport, AStaticMeshActor* Actor, UStaticMesh* Cube)
{
    if (Mode->IsMeshEditing()) Mode->EndMeshEdit(false);
    const bool bTransientWasDirty = GetTransientPackage()->IsDirty();
    const FName SourceName = MakeUniqueObjectName(GetTransientPackage(), UStaticMesh::StaticClass(), TEXT("BlenderMeshVerify_GeneratedUVSource"));
    TStrongObjectPtr<UStaticMesh> Source(DuplicateObject<UStaticMesh>(Cube, GetTransientPackage(), SourceName));
    if (!Report.Check(TEXT("mesh.generated_uv_transient_source_created"), Source.IsValid())) return;
    Source->ClearFlags(RF_Public | RF_Standalone);
    Source->SetFlags(RF_Transient);
    FMeshDescription Description(*Cube->GetMeshDescription(0));
    FStaticMeshAttributes Attributes(Description);
    Attributes.GetVertexInstanceUVs().SetNumChannels(1);
    // Non-white, nonuniform source colors make preservation checks meaningful as well.
    auto Colors = Attributes.GetVertexInstanceColors();
    for (const FVertexInstanceID ID : Description.VertexInstances().GetElementIDs())
    {
        const float Value = float((ID.GetValue() % 7) + 1) / 8.0f;
        Colors[ID] = FVector4f(Value, 1.0f - Value, 0.25f, 0.75f);
    }
    Source->CreateMeshDescription(0, MoveTemp(Description));
    Source->CommitMeshDescription(0);
    FMeshBuildSettings& Build = Source->GetSourceModel(0).BuildSettings;
    Build.bGenerateLightmapUVs = true;
    Build.SrcLightmapIndex = 0;
    Build.DstLightmapIndex = 1;
    Build.MinLightmapResolution = 64;
    const FMeshBuildSettings ExpectedBuild = Build;
    Source->SetLightMapCoordinateIndex(1);
    TArray<FText> BuildErrors;
    Source->Build(true, &BuildErrors);
    TArray<UStaticMesh*> MeshesToFinish{Source.Get()};
    FStaticMeshCompilingManager::Get().FinishCompilation(MeshesToFinish);
    const FMeshSnapshot Before(Source.Get());
    const FStaticMeshRenderData* SourceRenderData = Source->GetRenderData();
    const bool bSourceReady = BuildErrors.IsEmpty() && Before.UVChannels == 1
        && SourceRenderData && SourceRenderData->LODResources.Num() > 0
        && SourceRenderData->LODResources[0].VertexBuffers.StaticMeshVertexBuffer.GetNumTexCoords() >= 2;
    Report.Check(TEXT("mesh.generated_uv_fixture_one_source_channel_two_render_channels"), bSourceReady);
    if (bSourceReady)
    {
        Actor->GetStaticMeshComponent()->SetStaticMesh(Source.Get());
        Select({Actor});
        Keys(Mode, Client, Viewport, {EKeys::Tab});
        FBlenderMeshEditSession* Session = Mode->GetMeshEditSession();
        bool bEdited = Mode->IsMeshEditing() && Session;
        if (bEdited)
        {
            Session->SelectAll(true);
            Keys(Mode, Client, Viewport, {EKeys::G, EKeys::Z, EKeys::Two, EKeys::Five, EKeys::Enter, EKeys::Tab});
            bEdited = !Mode->IsMeshEditing() && Actor->GetStaticMeshComponent()->GetStaticMesh() != Source.Get();
        }
        UStaticMesh* Copy = Actor->GetStaticMeshComponent()->GetStaticMesh();
        const FStaticMeshRenderData* RenderData = Copy ? Copy->GetRenderData() : nullptr;
        const FMeshBuildSettings* CopyBuild = Copy && Copy->GetNumSourceModels() > 0 ? &Copy->GetSourceModel(0).BuildSettings : nullptr;
        const FMeshSnapshot After(Source.Get());
        Report.Check(TEXT("mesh.commit_preserves_generated_lightmap_channel_and_build_settings"), bEdited
            && RenderData && RenderData->LODResources.Num() > 0
            && RenderData->LODResources[0].VertexBuffers.StaticMeshVertexBuffer.GetNumTexCoords() >= 2
            && CopyBuild && CopyBuild->bGenerateLightmapUVs == ExpectedBuild.bGenerateLightmapUVs
            && CopyBuild->SrcLightmapIndex == ExpectedBuild.SrcLightmapIndex && CopyBuild->DstLightmapIndex == ExpectedBuild.DstLightmapIndex
            && CopyBuild->MinLightmapResolution == ExpectedBuild.MinLightmapResolution
            && Copy->GetLightMapCoordinateIndex() == Source->GetLightMapCoordinateIndex()
            && !FPackageName::DoesPackageExist(Copy->GetOutermost()->GetName())
            && Before.SamePositions(After) && Before.SameAttributesAndTopology(After)
            && Before.SameAttributesAndTopology(FMeshSnapshot(Copy)),
            TEXT("One source UV channel, generated render UV 1; nonuniform source vertex colors preserved."));
    }
    if (Mode->IsMeshEditing()) Mode->EndMeshEdit(false);
    Actor->GetStaticMeshComponent()->SetStaticMesh(Cube);
    GEditor->ResetTransaction(FText::FromString(TEXT("Blender generated UV fixture cleanup")));
    Source->ClearFlags(RF_Public | RF_Standalone);
    Source->MarkAsGarbage();
    GetTransientPackage()->SetDirtyFlag(bTransientWasDirty);
}

void RunCases(FReport& Report, FBlenderStyleEdMode* Mode, FEditorViewportClient* Client,
    FViewport* Viewport, UStaticMesh* Cube, const TArray<AStaticMeshActor*>& Actors)
{
    const FMeshSnapshot Original(Cube);
    const bool bOriginalDirty = Cube->GetOutermost()->IsDirty();
    const TArray<FTransform> ActorTransforms = {Actors[0]->GetActorTransform(),
        Actors[1]->GetActorTransform(), Actors[2]->GetActorTransform()};
    const int32 BaselineActorCount = ActorCount(Client->GetWorld());
    const int32 BaselineComponentCount = MeshComponentCount(Client->GetWorld());
    auto NoPreviewLeaks = [&]()
    {
        return ActorCount(Client->GetWorld()) == BaselineActorCount
            && MeshComponentCount(Client->GetWorld()) == BaselineComponentCount;
    };
    auto OriginalUnchanged = [&]()
    {
        const FMeshSnapshot Current(Cube);
        return Original.SamePositions(Current) && Original.SameAttributesAndTopology(Current)
            && Cube->GetOutermost()->IsDirty() == bOriginalDirty;
    };
    auto ActorTransformsUnchanged = [&]()
    {
        for (int32 Index = 0; Index < Actors.Num(); ++Index)
        {
            if (!Actors[Index]->GetActorTransform().Equals(ActorTransforms[Index], 0.00001)) return false;
        }
        return true;
    };

    Select({Actors[0]});
    bool bObjectGuides = true;
    FVector UnusedCenter, UnusedDirection;
    FLinearColor UnusedColor;
    bObjectGuides &= !Mode->GetActiveAxisGuide(UnusedCenter, UnusedDirection, UnusedColor);
    const FKey AxisKeys[] = {EKeys::X, EKeys::Y, EKeys::Z};
    const FVector AxisDirections[] = {FVector::ForwardVector, FVector::RightVector, FVector::UpVector};
    const FLinearColor AxisColors[] = {FLinearColor::Red, FLinearColor::Green, FLinearColor::Blue};
    for (int32 Axis = 0; Axis < 3; ++Axis)
    {
        Keys(Mode, Client, Viewport, {EKeys::G});
        bObjectGuides &= !Mode->GetActiveAxisGuide(UnusedCenter, UnusedDirection, UnusedColor);
        Keys(Mode, Client, Viewport, {AxisKeys[Axis]});
        bObjectGuides &= Guide(Mode, Actors[0]->GetActorLocation(), AxisDirections[Axis], AxisColors[Axis]);
        Keys(Mode, Client, Viewport, {EKeys::Escape});
    }
    Report.Check(TEXT("mesh.axis_object_world_rgb_and_inactive"), bObjectGuides,
        TEXT("Checks guide geometry and color only; not physical screen pixels."));

    Select({Actors[0], Actors[1]});
    Keys(Mode, Client, Viewport, {EKeys::Tab});
    FBlenderMeshEditSession* Session = Mode->GetMeshEditSession();
    if (!Report.Check(TEXT("mesh.tab_enters_two_objects"), Mode->IsMeshEditing() && Session
        && Session->IsActive() && Session->GetObjectCount() == 2)) return;
    Report.Check(TEXT("mesh.tab_starts_without_forced_full_selection"), Session->GetSelectedVertices().IsEmpty());
    bool bEmptyAcrossModes = true;
    for (const auto SelectionMode : {EBlenderMeshSelectionMode::Vertex, EBlenderMeshSelectionMode::Edge, EBlenderMeshSelectionMode::Face})
    {
        Session->SetSelectionMode(SelectionMode);
        bEmptyAcrossModes &= Session->GetSelectedElementIDs(0, SelectionMode).IsEmpty()
            && Session->GetSelectedElementIDs(1, SelectionMode).IsEmpty();
    }
    Report.Check(TEXT("mesh.unselected_entry_stays_unselected_across_point_edge_face"), bEmptyAcrossModes && !Session->HasChanges());
    Session->SetSelectionMode(EBlenderMeshSelectionMode::Vertex);
    Keys(Mode, Client, Viewport, {EKeys::A});
    int32 TotalVertices = 0;
    for (int32 Object = 0; Object < Session->GetObjectCount(); ++Object)
        TotalVertices += Session->GetElementIDs(Object, EBlenderMeshSelectionMode::Vertex).Num();
    Report.Check(TEXT("mesh.A_explicitly_selects_all_after_unselected_entry"), TotalVertices > 0
        && Session->GetSelectedVertices().Num() == TotalVertices && !Session->HasChanges());
    Session->SelectAll(false);
    Report.Check(TEXT("mesh.preview_preserves_original_asset_references"),
        Actors[0]->GetStaticMeshComponent()->GetStaticMesh() == Cube
        && Actors[1]->GetStaticMeshComponent()->GetStaticMesh() == Cube
        && Actors[2]->GetStaticMeshComponent()->GetStaticMesh() == Cube && OriginalUnchanged());

    CheckViewportFacePicking(Report, Mode, Client, Viewport, *Session);

    bool bSelectionModes = true;
    Keys(Mode, Client, Viewport, {EKeys::One});
    bSelectionModes &= Session->GetSelectionMode() == EBlenderMeshSelectionMode::Vertex;
    Keys(Mode, Client, Viewport, {EKeys::Two});
    bSelectionModes &= Session->GetSelectionMode() == EBlenderMeshSelectionMode::Edge;
    Keys(Mode, Client, Viewport, {EKeys::Three});
    bSelectionModes &= Session->GetSelectionMode() == EBlenderMeshSelectionMode::Face;
    Report.Check(TEXT("mesh.keys_123_select_vertex_edge_face"), bSelectionModes);

    bool bUniqueUnion = true;
    for (const EBlenderMeshSelectionMode SelectionMode : {EBlenderMeshSelectionMode::Face, EBlenderMeshSelectionMode::Edge})
    {
        Session->SetSelectionMode(SelectionMode);
        Session->SelectAll(false);
        int32 ExpectedVertices = 0;
        for (int32 Object = 0; Object < 2; ++Object)
        {
            ExpectedVertices += Session->GetElementIDs(Object, EBlenderMeshSelectionMode::Vertex).Num();
            const TArray<int32> IDs = Session->GetElementIDs(Object, SelectionMode);
            bUniqueUnion &= IDs.Num() > 1;
            for (const int32 ID : IDs) bUniqueUnion &= Session->SelectElement(Object, SelectionMode, ID, true);
        }
        TSet<uint64> Unique;
        for (const FBlenderMeshVertexRef& Vertex : Session->GetSelectedVertices())
            Unique.Add(VertexKey(Vertex.ObjectIndex, Vertex.VertexId));
        bUniqueUnion &= Unique.Num() == ExpectedVertices && Unique.Num() == Session->GetSelectedVertices().Num();
    }
    Report.Check(TEXT("mesh.face_edge_selection_union_deduplicates_shared_vertices"), bUniqueUnion);

    Session->SetSelectionMode(EBlenderMeshSelectionMode::Vertex);
    Session->SelectAll(false);
    TSet<uint64> Selected;
    for (int32 Object = 0; Object < 2; ++Object)
    {
        const TArray<int32> IDs = Session->GetElementIDs(Object, EBlenderMeshSelectionMode::Vertex);
        if (!Report.Check(Object == 0 ? TEXT("mesh.fixture_first_has_vertices") : TEXT("mesh.fixture_second_has_vertices"), IDs.Num() > 1)) return;
        Session->SelectElement(Object, EBlenderMeshSelectionMode::Vertex, IDs[0], true);
        Selected.Add(VertexKey(Object, IDs[0]));
    }
    const TMap<uint64, FVector> Before = Positions(*Session);
    const FVector Center = Session->GetSelectionCenter();
    Keys(Mode, Client, Viewport, {EKeys::G, EKeys::X, EKeys::One, EKeys::Zero, EKeys::Zero});
    Report.Check(TEXT("mesh.gx100_moves_only_selected_vertices_in_both_world_spaces"),
        Session->GetSelectedVertices().Num() == 2
        && CheckMappedPositions(*Session, Before, Selected, [](const FVector& P) { return P + FVector(100, 0, 0); })
        && Guide(Mode, Center, FVector::ForwardVector, FLinearColor::Red) && ActorTransformsUnchanged());
    Keys(Mode, Client, Viewport, {EKeys::Escape});
    Report.Check(TEXT("mesh.escape_cancels_only_active_transform"), Mode->IsMeshEditing() && !Mode->IsTransforming()
        && !Session->HasChanges() && CheckMappedPositions(*Session, Before, Selected, [](const FVector& P) { return P; }));

    Keys(Mode, Client, Viewport, {EKeys::S, EKeys::X, EKeys::Two});
    bool bScale = Guide(Mode, Center, FVector::ForwardVector, FLinearColor::Red)
        && CheckMappedPositions(*Session, Before, Selected, [&](const FVector& P)
        { return P + FVector::ForwardVector * FVector::DotProduct(P - Center, FVector::ForwardVector); });
    Keys(Mode, Client, Viewport, {EKeys::X});
    const FVector LocalX = Session->GetSourceActor(0)->GetActorQuat().GetAxisX();
    bScale &= Guide(Mode, Center, LocalX, FLinearColor::Red)
        && CheckMappedPositions(*Session, Before, Selected, [&](const FVector& P)
        { return P + LocalX * FVector::DotProduct(P - Center, LocalX); });
    Keys(Mode, Client, Viewport, {EKeys::X});
    bScale &= !Mode->GetActiveAxisGuide(UnusedCenter, UnusedDirection, UnusedColor);
    Keys(Mode, Client, Viewport, {EKeys::Escape});
    Report.Check(TEXT("mesh.sx_world_local_free_uses_common_pivot"), bScale && ActorTransformsUnchanged());

    Keys(Mode, Client, Viewport, {EKeys::R, EKeys::Z, EKeys::Nine, EKeys::Zero});
    const FQuat QuarterTurn(FVector::UpVector, PI / 2.0);
    Report.Check(TEXT("mesh.rz90_rotates_selection_across_objects"),
        CheckMappedPositions(*Session, Before, Selected, [&](const FVector& P) { return Center + QuarterTurn.RotateVector(P - Center); })
        && Guide(Mode, Center, FVector::UpVector, FLinearColor::Blue) && ActorTransformsUnchanged());
    Keys(Mode, Client, Viewport, {EKeys::Escape});

    Keys(Mode, Client, Viewport, {EKeys::G, EKeys::X, EKeys::One, EKeys::Zero, EKeys::Zero, EKeys::Enter});
    bool bHistory = Session->HasChanges() && Session->Undo()
        && CheckMappedPositions(*Session, Before, Selected, [](const FVector& P) { return P; });
    bHistory &= Session->Redo()
        && CheckMappedPositions(*Session, Before, Selected, [](const FVector& P) { return P + FVector(100, 0, 0); });
    Report.Check(TEXT("mesh.local_undo_redo_restores_edit_step"), bHistory && OriginalUnchanged() && ActorTransformsUnchanged(),
        TEXT("Session Undo/Redo API; keyboard modifier routing is not simulated here."));

    // Exercise real scene-viewport modifier state, but route S only to the mode so
    // a regression cannot invoke the editor's Save UI or write any package.
    const TSharedPtr<SEditorViewport> SaveWidget = Client->GetEditorViewportWidget();
    const TSharedPtr<FSceneViewport> SaveViewport = SaveWidget.IsValid() ? SaveWidget->GetSceneViewport() : nullptr;
    bool bSaveShortcutGuard = SaveViewport.IsValid() && SaveViewport.Get() == Viewport;
    if (bSaveShortcutGuard)
    {
        const FGeometry Geometry = SaveViewport->GetCachedGeometry();
        const bool bHadChanges = Session->HasChanges();
        const TMap<uint64, FVector> BeforeSaveKeys = Positions(*Session);
        auto SessionUnchanged = [&]()
        {
            return Mode->IsMeshEditing() && Mode->GetMeshEditSession() == Session && !Mode->IsTransforming()
                && Session->HasChanges() == bHadChanges
                && CheckMappedPositions(*Session, BeforeSaveKeys, Selected, [](const FVector& P) { return P; })
                && Actors[0]->GetStaticMeshComponent()->GetStaticMesh() == Cube
                && Actors[1]->GetStaticMeshComponent()->GetStaticMesh() == Cube
                && Actors[2]->GetStaticMeshComponent()->GetStaticMesh() == Cube;
        };
        const FModifierKeysState ControlModifiers(false, false, true, false, false, false, false, false, false);
        const FModifierKeysState ControlShiftModifiers(true, false, true, false, false, false, false, false, false);
        SaveViewport->OnKeyDown(Geometry, FKeyEvent(EKeys::LeftControl, ControlModifiers, 0, false, 0, 0));
        bSaveShortcutGuard &= SaveViewport->KeyState(EKeys::LeftControl);
        bSaveShortcutGuard &= Mode->InputKey(Client, Viewport, EKeys::S, IE_Pressed);
        Mode->InputKey(Client, Viewport, EKeys::S, IE_Released);
        bSaveShortcutGuard &= SessionUnchanged();
        SaveViewport->OnKeyDown(Geometry, FKeyEvent(EKeys::LeftShift, ControlShiftModifiers, 0, false, 0, 0));
        bSaveShortcutGuard &= SaveViewport->KeyState(EKeys::LeftControl) && SaveViewport->KeyState(EKeys::LeftShift);
        bSaveShortcutGuard &= Mode->InputKey(Client, Viewport, EKeys::S, IE_Pressed);
        Mode->InputKey(Client, Viewport, EKeys::S, IE_Released);
        bSaveShortcutGuard &= SessionUnchanged();
        // Always release both synthetic modifiers, even when an assertion failed.
        SaveViewport->OnKeyUp(Geometry, FKeyEvent(EKeys::LeftShift, ControlModifiers, 0, false, 0, 0));
        SaveViewport->OnKeyUp(Geometry, FKeyEvent(EKeys::LeftControl, FModifierKeysState(), 0, false, 0, 0));
        bSaveShortcutGuard &= !SaveViewport->KeyState(EKeys::LeftControl) && !SaveViewport->KeyState(EKeys::LeftShift);
    }
    Report.Check(TEXT("mesh.ctrl_s_and_ctrl_shift_s_are_consumed_without_changing_session"), bSaveShortcutGuard,
        TEXT("Actual scene modifier state; S sent only to mode. No Save UI or OS input invoked."));
    if (Mode->IsTransforming()) Mode->CancelActiveTransform();

    GEditor->ResetTransaction(FText::FromString(TEXT("Blender mesh isolated commit test")));
    Keys(Mode, Client, Viewport, {EKeys::Tab});
    UStaticMesh* FirstCopy = Actors[0]->GetStaticMeshComponent()->GetStaticMesh();
    UStaticMesh* SecondCopy = Actors[1]->GetStaticMeshComponent()->GetStaticMesh();
    const bool bCopies = !Mode->IsMeshEditing() && FirstCopy && SecondCopy
        && FirstCopy != Cube && SecondCopy != Cube && FirstCopy != SecondCopy;
    Report.Check(TEXT("mesh.commit_creates_independent_unsaved_assets_and_preserves_third_instance"),
        bCopies && FirstCopy->GetOutermost()->GetName().StartsWith(TEXT("/Game/BlenderEdits/"))
        && SecondCopy->GetOutermost()->GetName().StartsWith(TEXT("/Game/BlenderEdits/"))
        && FirstCopy->GetOutermost()->IsDirty() && SecondCopy->GetOutermost()->IsDirty()
        && !FPackageName::DoesPackageExist(FirstCopy->GetOutermost()->GetName())
        && !FPackageName::DoesPackageExist(SecondCopy->GetOutermost()->GetName())
        && Actors[2]->GetStaticMeshComponent()->GetStaticMesh() == Cube && OriginalUnchanged()
        && ActorTransformsUnchanged() && NoPreviewLeaks());
    Report.Check(TEXT("mesh.commit_preserves_uv_color_topology_and_changes_positions"), bCopies
        && Original.SameAttributesAndTopology(FMeshSnapshot(FirstCopy))
        && Original.SameAttributesAndTopology(FMeshSnapshot(SecondCopy))
        && !Original.SamePositions(FMeshSnapshot(FirstCopy))
        && !Original.SamePositions(FMeshSnapshot(SecondCopy)));
    Report.Check(TEXT("mesh.commit_collision_uses_changed_mesh_surface"), bCopies
        && FirstCopy->GetBodySetup() && SecondCopy->GetBodySetup()
        && FirstCopy->GetBodySetup() != Cube->GetBodySetup() && SecondCopy->GetBodySetup() != Cube->GetBodySetup()
        && FirstCopy->GetBodySetup()->CollisionTraceFlag == CTF_UseComplexAsSimple
        && SecondCopy->GetBodySetup()->CollisionTraceFlag == CTF_UseComplexAsSimple,
        TEXT("Verifies isolated body setups and complex-as-simple policy; no physics simulation."));
    if (bCopies)
    {
        const bool bUndo = GEditor->UndoTransaction()
            && Actors[0]->GetStaticMeshComponent()->GetStaticMesh() == Cube
            && Actors[1]->GetStaticMeshComponent()->GetStaticMesh() == Cube
            && !Actors[0]->IsTemporarilyHiddenInEditor() && !Actors[1]->IsTemporarilyHiddenInEditor();
        const bool bRedo = GEditor->RedoTransaction()
            && Actors[0]->GetStaticMeshComponent()->GetStaticMesh() == FirstCopy
            && Actors[1]->GetStaticMeshComponent()->GetStaticMesh() == SecondCopy
            && !Actors[0]->IsTemporarilyHiddenInEditor() && !Actors[1]->IsTemporarilyHiddenInEditor();
        Report.Check(TEXT("mesh.editor_undo_redo_swaps_original_and_committed_references"), bUndo && bRedo
            && Actors[2]->GetStaticMeshComponent()->GetStaticMesh() == Cube && OriginalUnchanged());
    }
    if (Mode->IsMeshEditing()) Mode->EndMeshEdit(false);

    // Use untouched engine instances for the no-op and whole-session cancellation cases.
    for (AStaticMeshActor* Actor : Actors) Actor->GetStaticMeshComponent()->SetStaticMesh(Cube);
    GEditor->ResetTransaction(FText::FromString(TEXT("Blender mesh no-op fixture")));
    Select({Actors[0], Actors[1]});
    const int32 AssetsBeforeNoOp = EditAssets().Num();
    Keys(Mode, Client, Viewport, {EKeys::Tab, EKeys::Tab});
    Report.Check(TEXT("mesh.no_op_tab_creates_no_assets"), !Mode->IsMeshEditing()
        && EditAssets().Num() == AssetsBeforeNoOp && Actors[0]->GetStaticMeshComponent()->GetStaticMesh() == Cube
        && Actors[1]->GetStaticMeshComponent()->GetStaticMesh() == Cube && NoPreviewLeaks());

    const bool bFirstVisibility = Actors[0]->GetStaticMeshComponent()->IsVisible();
    const bool bSecondVisibility = Actors[1]->GetStaticMeshComponent()->IsVisible();
    const bool bFirstHidden = Actors[0]->IsHiddenEd();
    const bool bSecondHidden = Actors[1]->IsHiddenEd();
    Keys(Mode, Client, Viewport, {EKeys::Tab});
    Session = Mode->GetMeshEditSession();
    bool bCanceled = Mode->IsMeshEditing() && Session;
    if (bCanceled)
    {
        Session->SelectAll(true);
        Keys(Mode, Client, Viewport, {EKeys::G, EKeys::Y, EKeys::Five, EKeys::Zero, EKeys::Enter, EKeys::Escape});
        bCanceled = !Mode->IsMeshEditing();
    }
    Report.Check(TEXT("mesh.whole_session_cancel_restores_visibility_references_and_removes_previews"), bCanceled
        && Actors[0]->GetStaticMeshComponent()->GetStaticMesh() == Cube
        && Actors[1]->GetStaticMeshComponent()->GetStaticMesh() == Cube
        && Actors[0]->GetStaticMeshComponent()->IsVisible() == bFirstVisibility
        && Actors[1]->GetStaticMeshComponent()->IsVisible() == bSecondVisibility
        && Actors[0]->IsHiddenEd() == bFirstHidden && Actors[1]->IsHiddenEd() == bSecondHidden
        && EditAssets().Num() == AssetsBeforeNoOp && NoPreviewLeaks() && OriginalUnchanged());

    Actors[1]->SetActorScale3D(FVector(0, 1, 1));
    Select({Actors[0], Actors[1]});
    Keys(Mode, Client, Viewport, {EKeys::Tab});
    Report.Check(TEXT("mesh.invalid_second_actor_rejects_atomically"), !Mode->IsMeshEditing()
        && Actors[0]->GetStaticMeshComponent()->IsVisible() == bFirstVisibility
        && Actors[1]->GetStaticMeshComponent()->IsVisible() == bSecondVisibility
        && Actors[0]->GetStaticMeshComponent()->GetStaticMesh() == Cube
        && Actors[1]->GetStaticMeshComponent()->GetStaticMesh() == Cube && NoPreviewLeaks());
    Actors[1]->SetActorTransform(ActorTransforms[1]);

    // A short actual world query proves collision follows changed vertices. Keep the
    // entire ray above the original shape, so a stale source collision cannot pass.
    if (Mode->IsMeshEditing()) Mode->EndMeshEdit(false);
    Actors[0]->SetActorTransform(FTransform(FQuat::Identity, FVector(12000, 12000, 1000), FVector::OneVector));
    UStaticMeshComponent* TraceComponent = Actors[0]->GetStaticMeshComponent();
    const ECollisionEnabled::Type PreviousCollisionEnabled = TraceComponent->GetCollisionEnabled();
    const ECollisionResponse PreviousVisibilityResponse = TraceComponent->GetCollisionResponseToChannel(ECC_Visibility);
    TraceComponent->SetCollisionEnabled(ECollisionEnabled::QueryOnly);
    TraceComponent->SetCollisionResponseToChannel(ECC_Visibility, ECR_Block);
    FBox SourceBounds(ForceInit);
    const FVector BuildScale = Cube->GetSourceModel(0).BuildSettings.BuildScale3D;
    for (const auto& Pair : Original.Positions)
        SourceBounds += Actors[0]->GetActorTransform().TransformPosition(FVector(Pair.Value) * BuildScale);
    const FVector ExpectedImpact(SourceBounds.GetCenter().X, SourceBounds.GetCenter().Y, SourceBounds.Max.Z + 200.0);
    const FVector TraceStart = ExpectedImpact + FVector(0, 0, 100);
    const FVector TraceEnd = ExpectedImpact - FVector(0, 0, 100);
    FCollisionQueryParams QueryParams(SCENE_QUERY_STAT(BlenderMeshEditVerification), false);
    FHitResult Hit;
    const bool bOriginalGapEmpty = !Client->GetWorld()->LineTraceSingleByChannel(Hit, TraceStart, TraceEnd, ECC_Visibility, QueryParams);
    Select({Actors[0]});
    Keys(Mode, Client, Viewport, {EKeys::Tab});
    Session = Mode->GetMeshEditSession();
    bool bTraceEdit = Mode->IsMeshEditing() && Session;
    if (bTraceEdit)
    {
        Session->SelectAll(true);
        Keys(Mode, Client, Viewport, {EKeys::G, EKeys::Z, EKeys::Two, EKeys::Zero, EKeys::Zero, EKeys::Enter, EKeys::Tab});
        bTraceEdit = !Mode->IsMeshEditing() && TraceComponent->GetStaticMesh() != Cube;
    }
    Hit = FHitResult();
    const bool bSurfaceHit = bTraceEdit
        && Client->GetWorld()->LineTraceSingleByChannel(Hit, TraceStart, TraceEnd, ECC_Visibility, QueryParams);
    Report.Check(TEXT("mesh.commit_world_trace_hits_moved_surface_above_original"),
        bOriginalGapEmpty && bSurfaceHit && Hit.GetComponent() == TraceComponent
        && Hit.ImpactPoint.Equals(ExpectedImpact, 0.1) && OriginalUnchanged() && NoPreviewLeaks(),
        FString::Printf(TEXT("Simple visibility trace; hit=%s expected=%s original gap empty=%s"),
            *Hit.ImpactPoint.ToString(), *ExpectedImpact.ToString(), bOriginalGapEmpty ? TEXT("true") : TEXT("false")));
    if (Mode->IsMeshEditing()) Mode->EndMeshEdit(false);
    TraceComponent->SetStaticMesh(Cube);
    TraceComponent->SetCollisionEnabled(PreviousCollisionEnabled);
    TraceComponent->SetCollisionResponseToChannel(ECC_Visibility, PreviousVisibilityResponse);
    Actors[0]->SetActorTransform(ActorTransforms[0]);
    CheckGeneratedLightmapUVs(Report, Mode, Client, Viewport, Actors[0], Cube);
    Report.Check(TEXT("mesh.engine_cube_still_unchanged_after_extra_fixtures"), OriginalUnchanged() && NoPreviewLeaks());
}
} // namespace BlenderMeshVerification

void AppendBlenderMeshEditVerification(FEditorViewportClient* Client, FViewport* Viewport,
    TArray<TSharedPtr<FJsonValue>>& Checks, int32& Failures)
{
    using namespace BlenderMeshVerification;
    FReport Report{Checks, Failures};
    UWorld* World = Client ? Client->GetWorld() : nullptr;
    if (!Report.Check(TEXT("mesh.precondition_dedicated_unsaved_editor_world"),
        FParse::Param(FCommandLine::Get(), TEXT("BlenderControlsVerify")) && GEditor
        && !GEditor->PlayWorld && World && Viewport && World->WorldType == EWorldType::Editor
        && World == GEditor->GetEditorWorldContext().World()
        && World->GetOutermost()->HasAnyPackageFlags(PKG_NewlyCreated))) return;

    TArray<AActor*> PreviousSelection;
    for (FSelectionIterator It(*GEditor->GetSelectedActors()); It; ++It)
        if (AActor* Actor = Cast<AActor>(*It)) PreviousSelection.Add(Actor);
    const TSet<UStaticMesh*> PreviousAssets = EditAssets();
    UStaticMesh* Cube = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cube.Cube"));
    if (!Report.Check(TEXT("mesh.fixture_engine_cube_source_description"), Cube && Cube->GetMeshDescription(0))) return;
    TArray<AStaticMeshActor*> Actors;
    Actors.Add(SpawnFixture(World, Cube, TEXT("BlenderMeshVerify_First"), FVector(10, 20, 30), FRotator(0, 30, 0), FVector::OneVector));
    Actors.Add(SpawnFixture(World, Cube, TEXT("BlenderMeshVerify_RotatedScaled"), FVector(350, 50, 20), FRotator(20, 70, 10), FVector(2, 0.5, 1.5)));
    Actors.Add(SpawnFixture(World, Cube, TEXT("BlenderMeshVerify_UntouchedShared"), FVector(700, 0, 0), FRotator::ZeroRotator, FVector::OneVector));
    FEditorModeTools& Modes = GLevelEditorModeTools();
    Modes.ActivateMode(FBlenderStyleEdMode::ModeId);
    FBlenderStyleEdMode* Mode = static_cast<FBlenderStyleEdMode*>(Modes.GetActiveMode(FBlenderStyleEdMode::ModeId));
    if (Report.Check(TEXT("mesh.fixture_three_shared_mesh_actors"), Mode && Actors[0] && Actors[1] && Actors[2]))
    {
        RunCases(Report, Mode, Client, Viewport, Cube, Actors);
    }

    if (Mode)
    {
        Mode->CancelActiveTransform();
        if (Mode->IsMeshEditing()) Mode->EndMeshEdit(false);
    }
    Modes.DeactivateMode(FBlenderStyleEdMode::ModeId);
    GEditor->SelectNone(false, true, false);
    GEditor->ResetTransaction(FText::FromString(TEXT("Blender mesh verification cleanup")));
    for (AStaticMeshActor* Actor : Actors)
    {
        if (IsValid(Actor)) World->DestroyActor(Actor);
    }
    int32 CleanedAssets = 0;
    bool bClean = true;
    for (UStaticMesh* Asset : EditAssets())
    {
        if (PreviousAssets.Contains(Asset)) continue;
        // Only this invocation's new /Game/BlenderEdits assets are removed. No delete/save dialogs.
        UPackage* Package = Asset->GetOutermost();
        FAssetRegistryModule::AssetDeleted(Asset);
        Asset->ClearFlags(RF_Public | RF_Standalone);
        bClean &= Asset->Rename(nullptr, GetTransientPackage(), REN_DontCreateRedirectors | REN_NonTransactional);
        Asset->MarkAsGarbage();
        Package->SetDirtyFlag(false);
        ++CleanedAssets;
    }
    Select(PreviousSelection);
    Report.Check(TEXT("mesh.cleanup_removes_only_generated_test_assets"), bClean && EditAssets().Num() == PreviousAssets.Num(),
        FString::Printf(TEXT("Removed %d unsaved test mesh assets; no packages saved."), CleanedAssets));
}
