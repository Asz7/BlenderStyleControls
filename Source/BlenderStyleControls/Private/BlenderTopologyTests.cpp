#include "BlenderTopologyTests.h"
#include "BlenderMeshEditSession.h"
#include "BlenderStyleEdMode.h"
#include "Algo/Reverse.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Components/StaticMeshComponent.h"
#include "Dom/JsonObject.h"
#include "Editor.h"
#include "EditorModeManager.h"
#include "EditorViewportClient.h"
#include "Engine/Selection.h"
#include "Engine/StaticMesh.h"
#include "Engine/StaticMeshActor.h"
#include "Engine/World.h"
#include "Input/Events.h"
#include "InputCoreTypes.h"
#include "MeshDescription.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "SceneView.h"
#include "SEditorViewport.h"
#include "Slate/SceneViewport.h"
#include "StaticMeshAttributes.h"
#include "StaticMeshCompiler.h"
#include "StaticMeshOperations.h"
#include "UObject/Package.h"
#include "UObject/StrongObjectPtr.h"
#include <initializer_list>

namespace
{
constexpr double PositionTolerance = 0.025;
constexpr float AttributeTolerance = 0.0001f;
const EBlenderMeshSelectionMode VertexMode = EBlenderMeshSelectionMode::Vertex;
const EBlenderMeshSelectionMode EdgeMode = EBlenderMeshSelectionMode::Edge;
const EBlenderMeshSelectionMode FaceMode = EBlenderMeshSelectionMode::Face;

FVector2f FixtureUV(const FVector3f& Position, int32 Group, int32 Channel)
{
    return Channel == 0 ? FVector2f(Position.X * .01f + Group * 10.f, Position.Y * .01f)
        : FVector2f(Position.X * .02f + Group * .125f, Position.Y * .02f + Group * 3.f);
}

FVector4f FixtureColor(const FVector3f& Position, int32 Group)
{
    return FVector4f(.1f + Position.X * .001f, .2f + Position.Y * .001f, .3f + Group * .03f, .75f);
}

struct FTopologyFixture
{
    FMeshDescription Mesh;

    FTopologyFixture()
    {
        FStaticMeshAttributes Attributes(Mesh);
        Attributes.Register();
        Attributes.GetVertexInstanceUVs().SetNumChannels(2);
    }

    int32 Vertex(const FVector3f& Position)
    {
        const FVertexID ID = Mesh.CreateVertex();
        Mesh.GetVertexPositions()[ID] = Position;
        return ID.GetValue();
    }

    void Face(std::initializer_list<int32> Corners)
    {
        FStaticMeshAttributes Attributes(Mesh);
        const FPolygonGroupID Group = Mesh.CreatePolygonGroup();
        Attributes.GetPolygonGroupMaterialSlotNames()[Group] = FName(*FString::Printf(TEXT("TopologySlot_%d"), Group.GetValue()));
        TArray<FVertexInstanceID> Instances;
        for (int32 VertexID : Corners)
        {
            const FVertexInstanceID Instance = Mesh.CreateVertexInstance(FVertexID(VertexID));
            const FVector3f Position = Mesh.GetVertexPositions()[FVertexID(VertexID)];
            for (int32 Channel = 0; Channel < 2; ++Channel)
                Attributes.GetVertexInstanceUVs().Set(Instance, Channel, FixtureUV(Position, Group.GetValue(), Channel));
            Attributes.GetVertexInstanceColors()[Instance] = FixtureColor(Position, Group.GetValue());
            Instances.Add(Instance);
        }
        // Fixture declarations use right-hand mathematical corner order for readability.
        // UE's triangle normals use Cross(edge2, edge1), so reverse the perimeter to
        // produce the intended +Z / +X / -Y outward normals in the actual mesh.
        Algo::Reverse(Instances);
        Mesh.CreatePolygon(Group, Instances);
    }

    void Strip(int32 Faces)
    {
        for (int32 Y = 0; Y < 2; ++Y)
            for (int32 X = 0; X <= Faces; ++X) Vertex(FVector3f(X * 100.f, Y * 100.f, 0));
        for (int32 X = 0; X < Faces; ++X) Face({X, X + 1, Faces + X + 2, Faces + X + 1});
    }

    int32 Edge(int32 A, int32 B) const
    {
        for (FEdgeID ID : Mesh.Edges().GetElementIDs())
        {
            const auto Vertices = Mesh.GetEdgeVertices(ID);
            if ((Vertices[0].GetValue() == A && Vertices[1].GetValue() == B) ||
                (Vertices[0].GetValue() == B && Vertices[1].GetValue() == A)) return ID.GetValue();
        }
        return INDEX_NONE;
    }

    UStaticMesh* Build()
    {
        UStaticMesh* Result = NewObject<UStaticMesh>(GetTransientPackage(), NAME_None, RF_Transient);
        Result->AddSourceModel();
        const FStaticMeshConstAttributes Attributes(Mesh);
        for (FPolygonGroupID ID : Mesh.PolygonGroups().GetElementIDs())
        {
            FStaticMaterial Material;
            Material.MaterialSlotName = Attributes.GetPolygonGroupMaterialSlotNames()[ID];
            Material.ImportedMaterialSlotName = Material.MaterialSlotName;
            Result->GetStaticMaterials().Add(Material);
        }
        Result->GetSourceModel(0).BuildSettings.bGenerateLightmapUVs = false;
        FStaticMeshOperations::ComputeTriangleTangentsAndNormals(Mesh);
        FStaticMeshOperations::ComputeTangentsAndNormals(Mesh, EComputeNTBsFlags::Normals | EComputeNTBsFlags::Tangents);
        Result->CreateMeshDescription(0, FMeshDescription(Mesh));
        Result->CommitMeshDescription(0);
        Result->Build(false);
        TArray<UStaticMesh*> Finish{Result};
        FStaticMeshCompilingManager::Get().FinishCompilation(Finish);
        return Result;
    }
};

bool AllQuads(const FMeshDescription& Mesh, int32 ExpectedFaces, int32 ExpectedVertices)
{
    if (Mesh.Polygons().Num() != ExpectedFaces || Mesh.Vertices().Num() != ExpectedVertices) return false;
    for (FPolygonID Face : Mesh.Polygons().GetElementIDs())
        if (Mesh.GetPolygonVertexInstances(Face).Num() != 4) return false;
    return true;
}

bool SameCornerAttributes(const FMeshDescription& A, FPolygonID FaceA, const FMeshDescription& B, FPolygonID FaceB)
{
    if (!A.Polygons().IsValid(FaceA) || !B.Polygons().IsValid(FaceB) ||
        A.GetPolygonPolygonGroup(FaceA) != B.GetPolygonPolygonGroup(FaceB)) return false;
    const FStaticMeshConstAttributes AttrA(A), AttrB(B);
    const auto UVA = AttrA.GetVertexInstanceUVs(), UVB = AttrB.GetVertexInstanceUVs();
    const auto CornersA = A.GetPolygonVertexInstances(FaceA), CornersB = B.GetPolygonVertexInstances(FaceB);
    if (CornersA.Num() != CornersB.Num() || UVA.GetNumChannels() != UVB.GetNumChannels()) return false;
    for (int32 I = 0; I < CornersA.Num(); ++I)
    {
        if (!AttrA.GetVertexInstanceColors()[CornersA[I]].Equals(AttrB.GetVertexInstanceColors()[CornersB[I]], AttributeTolerance)) return false;
        for (int32 Channel = 0; Channel < UVA.GetNumChannels(); ++Channel)
            if (!UVA.Get(CornersA[I], Channel).Equals(UVB.Get(CornersB[I], Channel), AttributeTolerance)) return false;
    }
    return true;
}

bool ExtrudedCapAttributesMatch(const FMeshDescription& Source, FPolygonID SourceFace,
    const FMeshDescription& Edited, FPolygonID CapFace, const FVector3f& LocalDisplacement, FString& Details)
{
    if (!Source.Polygons().IsValid(SourceFace) || !Edited.Polygons().IsValid(CapFace))
    {
        Details = TEXT("Source or cap polygon is invalid.");
        return false;
    }
    const FStaticMeshConstAttributes SourceAttributes(Source), CapAttributes(Edited);
    const auto SourceUVs = SourceAttributes.GetVertexInstanceUVs(), CapUVs = CapAttributes.GetVertexInstanceUVs();
    const auto SourceCorners = Source.GetPolygonVertexInstances(SourceFace), CapCorners = Edited.GetPolygonVertexInstances(CapFace);
    const FPolygonGroupID SourceGroup = Source.GetPolygonPolygonGroup(SourceFace), CapGroup = Edited.GetPolygonPolygonGroup(CapFace);
    const bool SameSlot = SourceAttributes.GetPolygonGroupMaterialSlotNames()[SourceGroup] == CapAttributes.GetPolygonGroupMaterialSlotNames()[CapGroup];
    int32 Rotation = INDEX_NONE;
    if (SourceCorners.Num() == CapCorners.Num() && !SourceCorners.IsEmpty())
    {
        // Polygon creation/retriangulation may rotate the perimeter's first corner.
        // Derive the correspondence from geometry, not matching attribute values:
        // every source corner must move by the exact displacement in the same winding.
        for (int32 Offset = 0; Offset < CapCorners.Num(); ++Offset)
        {
            bool GeometryMatches = true;
            for (int32 I = 0; I < SourceCorners.Num(); ++I)
            {
                const FVector3f SourcePosition = Source.GetVertexPositions()[Source.GetVertexInstanceVertex(SourceCorners[I])];
                const FVector3f CapPosition = Edited.GetVertexPositions()[Edited.GetVertexInstanceVertex(CapCorners[(I + Offset) % CapCorners.Num()])];
                GeometryMatches &= (SourcePosition + LocalDisplacement).Equals(CapPosition, float(PositionTolerance));
            }
            if (GeometryMatches) { Rotation = Offset; break; }
        }
    }
    int32 UVMismatches = INDEX_NONE, ColorMismatches = INDEX_NONE;
    if (Rotation != INDEX_NONE)
    {
        UVMismatches = ColorMismatches = 0;
        for (int32 I = 0; I < SourceCorners.Num(); ++I)
        {
            const FVertexInstanceID A = SourceCorners[I], B = CapCorners[(I + Rotation) % CapCorners.Num()];
            if (!SourceAttributes.GetVertexInstanceColors()[A].Equals(CapAttributes.GetVertexInstanceColors()[B], AttributeTolerance)) ++ColorMismatches;
            for (int32 Channel = 0; Channel < FMath::Min(SourceUVs.GetNumChannels(), CapUVs.GetNumChannels()); ++Channel)
                if (!SourceUVs.Get(A, Channel).Equals(CapUVs.Get(B, Channel), AttributeTolerance)) ++UVMismatches;
        }
    }
    Details = FString::Printf(TEXT("source/cap corners=%d/%d UV_channels=%d/%d material_groups=%d/%d same_slot=%d geometry_winding_rotation=%d UV_mismatches=%d color_mismatches=%d"),
        SourceCorners.Num(), CapCorners.Num(), SourceUVs.GetNumChannels(), CapUVs.GetNumChannels(),
        SourceGroup.GetValue(), CapGroup.GetValue(), SameSlot, Rotation, UVMismatches, ColorMismatches);
    return Rotation != INDEX_NONE && SourceGroup == CapGroup && SameSlot &&
        SourceUVs.GetNumChannels() == CapUVs.GetNumChannels() && UVMismatches == 0 && ColorMismatches == 0;
}

// Snapshot comparisons deliberately include stable IDs, topology, material slots and corner
// attributes. Position-only comparisons would miss a failed topology undo or a lost UV seam.
bool SameMesh(const FMeshDescription& A, const FMeshDescription& B)
{
    if (A.Vertices().Num() != B.Vertices().Num() || A.VertexInstances().Num() != B.VertexInstances().Num() ||
        A.Edges().Num() != B.Edges().Num() || A.Polygons().Num() != B.Polygons().Num() ||
        A.Triangles().Num() != B.Triangles().Num() || A.PolygonGroups().Num() != B.PolygonGroups().Num()) return false;
    for (FVertexID Vertex : A.Vertices().GetElementIDs())
        if (!B.Vertices().IsValid(Vertex) || !A.GetVertexPositions()[Vertex].Equals(B.GetVertexPositions()[Vertex], AttributeTolerance)) return false;
    for (FEdgeID Edge : A.Edges().GetElementIDs())
    {
        if (!B.Edges().IsValid(Edge)) return false;
        const auto VA = A.GetEdgeVertices(Edge), VB = B.GetEdgeVertices(Edge);
        if (VA[0] != VB[0] || VA[1] != VB[1]) return false;
    }
    for (FPolygonID Face : A.Polygons().GetElementIDs())
    {
        if (!SameCornerAttributes(A, Face, B, Face)) return false;
        const auto CA = A.GetPolygonVertexInstances(Face), CB = B.GetPolygonVertexInstances(Face);
        for (int32 I = 0; I < CA.Num(); ++I)
            if (CA[I] != CB[I] || A.GetVertexInstanceVertex(CA[I]) != B.GetVertexInstanceVertex(CB[I])) return false;
    }
    const FStaticMeshConstAttributes AttrA(A), AttrB(B);
    for (FPolygonGroupID Group : A.PolygonGroups().GetElementIDs())
        if (!B.PolygonGroups().IsValid(Group) || AttrA.GetPolygonGroupMaterialSlotNames()[Group] != AttrB.GetPolygonGroupMaterialSlotNames()[Group]) return false;
    return true;
}

bool SameTriangleIndices(const FMeshDescription& A, const FMeshDescription& B)
{
    if (A.Triangles().Num() != B.Triangles().Num()) return false;
    for (FTriangleID Triangle : A.Triangles().GetElementIDs())
    {
        if (!B.Triangles().IsValid(Triangle) || A.GetTrianglePolygon(Triangle) != B.GetTrianglePolygon(Triangle)) return false;
        const auto VerticesA = A.GetTriangleVertices(Triangle), VerticesB = B.GetTriangleVertices(Triangle);
        const auto InstancesA = A.GetTriangleVertexInstances(Triangle), InstancesB = B.GetTriangleVertexInstances(Triangle);
        for (int32 I = 0; I < 3; ++I)
            if (VerticesA[I] != VerticesB[I] || InstancesA[I] != InstancesB[I]) return false;
    }
    return true;
}

struct FSelectionSnapshot
{
    EBlenderMeshSelectionMode Mode;
    TArray<int32> Selected;
    int32 ActiveObject = INDEX_NONE;
    int32 ActiveID = INDEX_NONE;
    EBlenderMeshSelectionMode ActiveMode = VertexMode;
    bool bHasActive;

    explicit FSelectionSnapshot(const FBlenderMeshEditSession& Session)
        : Mode(Session.GetSelectionMode()), Selected(Session.GetSelectedElementIDs(0, Mode)),
          bHasActive(Session.GetActiveElement(ActiveObject, ActiveMode, ActiveID)) {}

    bool Matches(const FBlenderMeshEditSession& Session) const
    {
        int32 Object = INDEX_NONE, ID = INDEX_NONE;
        EBlenderMeshSelectionMode CurrentMode = VertexMode;
        const bool HasActive = Session.GetActiveElement(Object, CurrentMode, ID);
        return Session.GetSelectionMode() == Mode && Session.GetSelectedElementIDs(0, Mode) == Selected &&
            HasActive == bHasActive && (!HasActive || (Object == ActiveObject && ID == ActiveID && CurrentMode == ActiveMode));
    }
};

bool PositionsEqual(const TArray<FVector>& Actual, const TArray<FVector>& Expected)
{
    if (Actual.Num() != Expected.Num()) return false;
    TSet<int32> Used;
    for (const FVector& Point : Actual)
    {
        if (Point.ContainsNaN()) return false;
        bool Found = false;
        for (int32 I = 0; I < Expected.Num(); ++I)
            if (!Used.Contains(I) && Point.Equals(Expected[I], PositionTolerance)) { Used.Add(I); Found = true; break; }
        if (!Found) return false;
    }
    return true;
}

TArray<FVector> SelectedPositions(const FBlenderMeshEditSession& Session)
{
    TArray<FVector> Result;
    for (const FBlenderMeshVertexRef& Vertex : Session.GetSelectedVertices()) Result.Add(Vertex.WorldPosition);
    return Result;
}

TArray<FVector> InsertedPositions(const FMeshDescription& Original, const FMeshDescription& Edited)
{
    TArray<FVector> Result;
    for (FVertexID ID : Edited.Vertices().GetElementIDs())
    {
        const FVector Point(Edited.GetVertexPositions()[ID]);
        bool Found = false;
        for (FVertexID OldID : Original.Vertices().GetElementIDs())
            if (Point.Equals(FVector(Original.GetVertexPositions()[OldID]), PositionTolerance)) { Found = true; break; }
        if (!Found) Result.Add(Point);
    }
    return Result;
}

bool InterpolatedFixtureAttributes(const FMeshDescription& Mesh)
{
    const FStaticMeshConstAttributes Attributes(Mesh);
    const auto UVs = Attributes.GetVertexInstanceUVs();
    if (UVs.GetNumChannels() != 2) return false;
    for (FPolygonID Face : Mesh.Polygons().GetElementIDs())
    {
        const int32 Group = Mesh.GetPolygonPolygonGroup(Face).GetValue();
        for (FVertexInstanceID Corner : Mesh.GetPolygonVertexInstances(Face))
        {
            const FVector3f Position = Mesh.GetVertexPositions()[Mesh.GetVertexInstanceVertex(Corner)];
            if (!Attributes.GetVertexInstanceColors()[Corner].Equals(FixtureColor(Position, Group), AttributeTolerance)) return false;
            for (int32 Channel = 0; Channel < 2; ++Channel)
                if (!UVs.Get(Corner, Channel).Equals(FixtureUV(Position, Group, Channel), AttributeTolerance)) return false;
        }
    }
    return true;
}

bool HasUsableLoop(FBlenderMeshEditSession& Session)
{
    FText Error;
    for (int32 Edge : Session.GetElementIDs(0, EdgeMode))
    {
        TArray<TPair<FVector, FVector>> Segments;
        if (Session.GetLoopCutPreview(0, Edge, Segments, Error) && !Segments.IsEmpty()) return true;
    }
    return false;
}
}

void AppendBlenderTopologyVerification(FEditorViewportClient* Client, FViewport* Viewport,
    TArray<TSharedPtr<FJsonValue>>& Checks, int32& Failures)
{
    if (!FParse::Param(FCommandLine::Get(), TEXT("BlenderControlsVerify")) || !Client || !Viewport || !GEditor) return;
    auto Check = [&](const TCHAR* Name, bool Passed, const FString& Details = FString())
    {
        TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
        Result->SetStringField(TEXT("name"), Name); Result->SetBoolField(TEXT("passed"), Passed);
        Result->SetStringField(TEXT("details"), Details); Checks.Add(MakeShared<FJsonValueObject>(Result));
        if (!Passed) ++Failures;
    };
    UWorld* World = GEditor->GetEditorWorldContext().World();
    if (!World || World->IsGameWorld()) { Check(TEXT("topology.fixture_world"), false); return; }
    const bool WorldWasDirty = World->GetOutermost()->IsDirty();
    const bool TransientWasDirty = GetTransientPackage()->IsDirty();
    const FVector Offset(42000, 42000, 42000);
    TArray<AStaticMeshActor*> Actors;
    TArray<UStaticMesh*> CreatedAssets;
    auto Spawn = [&](UStaticMesh* Mesh)
    {
        if (!Mesh) return static_cast<AStaticMeshActor*>(nullptr);
        FActorSpawnParameters Parameters; Parameters.ObjectFlags |= RF_Transient;
        Parameters.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
        AStaticMeshActor* Actor = World->SpawnActor<AStaticMeshActor>(Offset + FVector(Actors.Num() * 800, 0, 0), FRotator::ZeroRotator, Parameters);
        if (Actor)
        {
            Actor->GetStaticMeshComponent()->SetMobility(EComponentMobility::Movable);
            Actor->GetStaticMeshComponent()->SetStaticMesh(Mesh); Actors.Add(Actor);
        }
        return Actor;
    };
    FBlenderMeshEditSession Session;
    FText Error;
    FMeshDescription Preview;
    FTopologyFixture Quad; Quad.Strip(1);
    TStrongObjectPtr<UStaticMesh> QuadMesh(Quad.Build());
    const FStaticMeshConstAttributes QuadAttributes(Quad.Mesh);
    bool QuadEngineNormalsPointUp = Quad.Mesh.Triangles().Num() == 2;
    for (FTriangleID TriangleID : Quad.Mesh.Triangles().GetElementIDs())
        QuadEngineNormalsPointUp &= QuadAttributes.GetTriangleNormals()[TriangleID].Equals(FVector3f(0, 0, 1), AttributeTolerance);
    AStaticMeshActor* QuadActor = Spawn(QuadMesh.Get());
    if (QuadActor && Session.Begin(World, {QuadActor}, Error))
    {
        Session.SelectElement(0, FaceMode, 0, false);
        const FSelectionSnapshot OriginalSelection(Session);
        // One sheet cap becomes one displaced cap and exactly four boundary walls.
        const bool Began = Session.BeginExtrude(false, Error);
        const bool Updated = Began && Session.UpdateExtrude(25);
        const bool Copied = Session.CopyMeshDescription(0, Preview);
        TArray<FVector> Expected;
        for (FVertexID Vertex : Quad.Mesh.Vertices().GetElementIDs())
            Expected.Add(QuadActor->GetActorTransform().TransformPosition(FVector(Quad.Mesh.GetVertexPositions()[Vertex])) + FVector(0, 0, 25));
        const TArray<int32> Caps = Session.GetSelectedElementIDs(0, FaceMode);
        Check(TEXT("topology.extrude_one_quad_has_cap_and_four_walls"), QuadEngineNormalsPointUp && Updated && Copied && AllQuads(Preview, 5, 8) &&
            Caps.Num() == 1 && PositionsEqual(SelectedPositions(Session), Expected),
            FString::Printf(TEXT("fixture_engine_triangle_normals_are_positive_Z=%d"), QuadEngineNormalsPointUp));
        FString CapDetails = FString::Printf(TEXT("copied=%d cap_count=%d"), Copied, Caps.Num());
        const bool CapAttributesPreserved = Copied && Caps.Num() == 1 &&
            ExtrudedCapAttributesMatch(Quad.Mesh, FPolygonID(0), Preview, FPolygonID(Caps[0]), FVector3f(0, 0, 25), CapDetails);
        Check(TEXT("topology.extruded_cap_preserves_uv_channels_color_material"), CapAttributesPreserved, CapDetails);
        Session.CancelTopologyEdit();
        Check(TEXT("topology.extrude_cancel_restores_full_snapshot_and_active_selection"), Session.CopyMeshDescription(0, Preview) &&
            SameMesh(Quad.Mesh, Preview) && OriginalSelection.Matches(Session) && !Session.HasChanges() && !Session.IsTopologyEditing());

        // This is the E / RMB contract: accept zero displacement, then undo removes the extrusion.
        const bool ZeroBegan = Session.BeginExtrude(false, Error);
        if (ZeroBegan) { Session.UpdateExtrude(0); Session.AcceptTopologyEdit(); }
        FMeshDescription ZeroSnapshot;
        const bool ZeroCopied = Session.CopyMeshDescription(0, ZeroSnapshot);
        const FSelectionSnapshot ZeroSelection(Session);
        Check(TEXT("topology.zero_offset_extrusion_is_a_real_edit"), ZeroBegan && ZeroCopied &&
            AllQuads(ZeroSnapshot, 5, 8) && Session.HasChanges() && !Session.IsTopologyEditing());
        const bool Undone = Session.Undo();
        Check(TEXT("topology.extrude_undo_restores_topology_attributes_selection"), Undone && Session.CopyMeshDescription(0, Preview) &&
            SameMesh(Quad.Mesh, Preview) && OriginalSelection.Matches(Session) && !Session.HasChanges());
        const bool Redone = Session.Redo();
        Check(TEXT("topology.extrude_redo_restores_zero_offset_caps_and_selection"), Redone && Session.CopyMeshDescription(0, Preview) &&
            SameMesh(ZeroSnapshot, Preview) && ZeroSelection.Matches(Session) && Session.HasChanges());
        Session.Cancel();
    }
    else Check(TEXT("topology.quad_fixture_begin"), false, Error.ToString());

    FTopologyFixture Pair; Pair.Strip(2);
    TStrongObjectPtr<UStaticMesh> PairMesh(Pair.Build());
    AStaticMeshActor* PairActor = Spawn(PairMesh.Get());
    if (PairActor && Session.Begin(World, {PairActor}, Error))
    {
        Session.SelectElement(0, FaceMode, 0, false); Session.SelectElement(0, FaceMode, 1, true);
        const bool Began = Session.BeginExtrude(false, Error);
        if (Began) Session.UpdateExtrude(40);
        const bool Copied = Session.CopyMeshDescription(0, Preview);
        int32 Walls = 0, InternalWalls = 0; bool SideMaterials = true;
        if (Copied) for (FPolygonID Face : Preview.Polygons().GetElementIDs())
        {
            bool HasBase = false, HasTop = false, OnSharedPlane = true;
            float CenterX = 0;
            const auto Corners = Preview.GetPolygonVertexInstances(Face);
            for (FVertexInstanceID Corner : Corners)
            {
                const FVector3f Point = Preview.GetVertexPositions()[Preview.GetVertexInstanceVertex(Corner)];
                HasBase |= FMath::IsNearlyEqual(Point.Z, 0.f); HasTop |= FMath::IsNearlyEqual(Point.Z, 40.f);
                OnSharedPlane &= FMath::IsNearlyEqual(Point.X, 100.f); CenterX += Point.X;
            }
            if (HasBase && HasTop)
            {
                ++Walls; if (OnSharedPlane) ++InternalWalls;
                const int32 ExpectedGroup = CenterX / Corners.Num() < 100.f ? 0 : 1;
                SideMaterials &= Preview.GetPolygonPolygonGroup(Face).GetValue() == ExpectedGroup;
            }
        }
        Check(TEXT("topology.adjacent_region_has_six_boundary_walls_no_internal_wall"), Began && Copied &&
            AllQuads(Preview, 8, 12) && Walls == 6 && InternalWalls == 0 && SideMaterials && Session.GetSelectedElementIDs(0, FaceMode).Num() == 2);
        Session.CancelTopologyEdit();
        Session.SelectElement(0, FaceMode, 0, false);
        const bool SingleBegan = Session.BeginExtrude(false, Error);
        if (SingleBegan) Session.UpdateExtrude(20);
        Check(TEXT("topology.unselected_polygon_and_shared_source_attributes_unchanged"), SingleBegan && Session.CopyMeshDescription(0, Preview) &&
            SameCornerAttributes(Pair.Mesh, FPolygonID(1), Preview, FPolygonID(1)) &&
            PairMesh->GetMeshDescription(0) && SameMesh(Pair.Mesh, *PairMesh->GetMeshDescription(0)));
        Session.Cancel();
    }
    else Check(TEXT("topology.adjacent_fixture_begin"), false, Error.ToString());

    FTopologyFixture Bent;
    Bent.Vertex({0, 0, 0}); Bent.Vertex({100, 0, 0}); Bent.Vertex({100, 100, 0}); Bent.Vertex({0, 100, 0});
    Bent.Vertex({100, 0, -100}); Bent.Vertex({100, 100, -100});
    Bent.Face({0, 1, 2, 3}); Bent.Face({1, 4, 5, 2});
    TStrongObjectPtr<UStaticMesh> BentMesh(Bent.Build());
    AStaticMeshActor* BentActor = Spawn(BentMesh.Get());
    if (BentActor && Session.Begin(World, {BentActor}, Error))
    {
        Session.SelectElement(0, FaceMode, 0, false); Session.SelectElement(0, FaceMode, 1, true);
        const FVector Diagonal = FVector(1, 0, 1).GetSafeNormal();
        TArray<FVector> RegionExpected, NormalsExpected;
        for (FVertexID ID : Bent.Mesh.Vertices().GetElementIDs())
        {
            const FVector Base = BentActor->GetActorTransform().TransformPosition(FVector(Bent.Mesh.GetVertexPositions()[ID]));
            const FVector Normal = ID.GetValue() == 0 || ID.GetValue() == 3 ? FVector::UpVector :
                (ID.GetValue() >= 4 ? FVector::ForwardVector : Diagonal);
            RegionExpected.Add(Base + Diagonal * 30); NormalsExpected.Add(Base + Normal * 30);
        }
        const bool RegionBegan = Session.BeginExtrude(false, Error);
        if (RegionBegan) Session.UpdateExtrude(30);
        Check(TEXT("topology.region_extrudes_bent_faces_along_one_average_normal"), RegionBegan && PositionsEqual(SelectedPositions(Session), RegionExpected));
        Session.CancelTopologyEdit();
        const bool NormalsBegan = Session.BeginExtrude(true, Error);
        if (NormalsBegan) Session.UpdateExtrude(30);
        Check(TEXT("topology.along_normals_uses_adjoining_selected_face_normals"), NormalsBegan && PositionsEqual(SelectedPositions(Session), NormalsExpected) &&
            !PositionsEqual(SelectedPositions(Session), RegionExpected));
        Session.Cancel();
    }
    else Check(TEXT("topology.bent_fixture_begin"), false, Error.ToString());

    // Three adjoining faces give the top cap four different averaged normals.
    // Its displaced vertices are noncoplanar, so alternating a triangulation diagonal
    // changes the actual surface, rather than only an equivalent planar tessellation.
    FTopologyFixture Corner;
    Corner.Vertex({0, 0, 0}); Corner.Vertex({100, 0, 0}); Corner.Vertex({100, 100, 0}); Corner.Vertex({0, 100, 0});
    Corner.Vertex({100, 0, -100}); Corner.Vertex({100, 100, -100}); Corner.Vertex({0, 0, -100});
    Corner.Face({0, 1, 2, 3}); Corner.Face({1, 4, 5, 2}); Corner.Face({0, 6, 4, 1});
    TStrongObjectPtr<UStaticMesh> CornerMesh(Corner.Build());
    AStaticMeshActor* CornerActor = Spawn(CornerMesh.Get());
    if (CornerActor && Session.Begin(World, {CornerActor}, Error))
    {
        Session.SelectElement(0, FaceMode, 0, false); Session.SelectElement(0, FaceMode, 1, true); Session.SelectElement(0, FaceMode, 2, true);
        const bool Began = Session.BeginExtrude(true, Error);
        FMeshDescription First, Second, Third;
        const bool FirstCopied = Began && Session.UpdateExtrude(30) && Session.CopyMeshDescription(0, First);
        double NonPlanarDeviation = 0;
        if (FirstCopied) for (int32 FaceID : Session.GetSelectedElementIDs(0, FaceMode))
        {
            const auto Corners = First.GetPolygonVertexInstances(FPolygonID(FaceID));
            if (Corners.Num() != 4) continue;
            FVector Points[4];
            for (int32 I = 0; I < 4; ++I) Points[I] = FVector(First.GetVertexPositions()[First.GetVertexInstanceVertex(Corners[I])]);
            const FVector Normal = FVector::CrossProduct(Points[1] - Points[0], Points[2] - Points[0]).GetSafeNormal();
            NonPlanarDeviation = FMath::Max(NonPlanarDeviation, FMath::Abs(FVector::DotProduct(Normal, Points[3] - Points[0])));
        }
        const bool SecondCopied = Began && Session.UpdateExtrude(30) && Session.CopyMeshDescription(0, Second);
        const bool ThirdCopied = Began && Session.UpdateExtrude(30) && Session.CopyMeshDescription(0, Third);
        const bool SameSecond = FirstCopied && SecondCopied && SameMesh(First, Second) && SameTriangleIndices(First, Second);
        const bool SameThird = FirstCopied && ThirdCopied && SameMesh(First, Third) && SameTriangleIndices(First, Third);
        Check(TEXT("topology.repeated_along_normals_distance_preserves_nonplanar_cap_triangulation"),
            NonPlanarDeviation > .01 && SameSecond && SameThird,
            FString::Printf(TEXT("nonplanar_deviation_cm=%.6f repeat_2_exact=%d repeat_3_exact=%d; compares triangle IDs, polygon owners, ordered vertex and corner-instance IDs"),
                NonPlanarDeviation, SameSecond, SameThird));
        Session.Cancel();
    }
    else Check(TEXT("topology.nonplanar_fixture_begin"), false, Error.ToString());

    AStaticMeshActor* ScaledActor = Spawn(QuadMesh.Get());
    if (ScaledActor)
    {
        ScaledActor->SetActorRotation(FRotator(27, 38, 11)); ScaledActor->SetActorScale3D(FVector(2, .5, 3));
    }
    if (ScaledActor && Session.Begin(World, {ScaledActor}, Error))
    {
        Session.SelectElement(0, FaceMode, 0, false);
        const FTransform Transform = ScaledActor->GetActorTransform();
        const FVector P0 = Transform.TransformPosition(FVector(Quad.Mesh.GetVertexPositions()[FVertexID(0)]));
        const FVector P1 = Transform.TransformPosition(FVector(Quad.Mesh.GetVertexPositions()[FVertexID(1)]));
        const FVector P2 = Transform.TransformPosition(FVector(Quad.Mesh.GetVertexPositions()[FVertexID(2)]));
        const FVector WorldNormal = FVector::CrossProduct(P1 - P0, P2 - P0).GetSafeNormal();
        TArray<FVector> PositiveExpected, NegativeExpected;
        for (FVertexID ID : Quad.Mesh.Vertices().GetElementIDs())
        {
            const FVector Base = Transform.TransformPosition(FVector(Quad.Mesh.GetVertexPositions()[ID]));
            PositiveExpected.Add(Base + WorldNormal * 37); NegativeExpected.Add(Base - WorldNormal * 12);
        }
        const bool Began = Session.BeginExtrude(false, Error);
        if (Began) Session.UpdateExtrude(37);
        Check(TEXT("topology.extrusion_distance_is_world_centimeters_under_nonuniform_scale"), Began && PositionsEqual(SelectedPositions(Session), PositiveExpected));
        const bool Updated = Began && Session.UpdateExtrude(-12);
        Check(TEXT("topology.negative_distance_recomputes_from_start_without_accumulation"), Updated && PositionsEqual(SelectedPositions(Session), NegativeExpected));
        Session.Cancel();
    }
    else Check(TEXT("topology.scaled_fixture_begin"), false, Error.ToString());

    if (ScaledActor)
    {
        ScaledActor->SetActorRotation(FRotator::ZeroRotator);
        ScaledActor->SetActorScale3D(FVector(-2, 1, .5));
    }
    if (ScaledActor && Session.Begin(World, {ScaledActor}, Error))
    {
        Session.SelectElement(0, FaceMode, 0, false);
        TArray<FVector> Expected;
        for (FVertexID ID : Quad.Mesh.Vertices().GetElementIDs())
            Expected.Add(ScaledActor->GetActorTransform().TransformPosition(FVector(Quad.Mesh.GetVertexPositions()[ID])) + FVector(0, 0, 25));
        const bool Began = Session.BeginExtrude(false, Error);
        if (Began) Session.UpdateExtrude(25);
        // A mirrored X scale does not turn the original +Z exterior into the interior.
        // Using only the cross product of transformed edges would move this cap inward.
        Check(TEXT("topology.mirrored_scale_preserves_outward_positive_extrusion"), Began && PositionsEqual(SelectedPositions(Session), Expected));
        Session.Cancel();
    }
    else Check(TEXT("topology.mirrored_fixture_begin"), false, Error.ToString());

    FTopologyFixture Strip; Strip.Strip(3);
    TStrongObjectPtr<UStaticMesh> StripMesh(Strip.Build());
    AStaticMeshActor* StripActor = Spawn(StripMesh.Get());
    AStaticMeshActor* SharedActor = Spawn(StripMesh.Get());
    if (StripActor && SharedActor && Session.Begin(World, {StripActor, SharedActor}, Error))
    {
        const int32 SeedEdge = Strip.Edge(0, 4);
        Session.SelectElement(0, EdgeMode, SeedEdge, false);
        const FSelectionSnapshot OriginalSelection(Session);
        TArray<TPair<FVector, FVector>> Segments;
        const bool Previewed = Session.GetLoopCutPreview(0, SeedEdge, Segments, Error);
        const bool PreviewUnchanged = Session.CopyMeshDescription(0, Preview) && SameMesh(Strip.Mesh, Preview);
        const bool Began = Session.BeginLoopCut(0, SeedEdge, Error);
        const bool Copied = Session.CopyMeshDescription(0, Preview);
        const TArray<FVector> Centered{{0, 50, 0}, {100, 50, 0}, {200, 50, 0}, {300, 50, 0}};
        Check(TEXT("topology.quad_strip_preview_and_centered_cut_have_exact_topology"), Previewed && Segments.Num() == 3 && PreviewUnchanged &&
            Began && Copied && AllQuads(Preview, 6, 12) && PositionsEqual(InsertedPositions(Strip.Mesh, Preview), Centered) && Session.GetSelectedElementIDs(0, EdgeMode).Num() == 3);

        bool SlideUpdated = Began && Session.UpdateLoopCut(.25);
        FMeshDescription Quarter, ThreeQuarter, QuarterAgain;
        SlideUpdated &= Session.CopyMeshDescription(0, Quarter);
        SlideUpdated &= Session.UpdateLoopCut(.75) && Session.CopyMeshDescription(0, ThreeQuarter);
        SlideUpdated &= Session.UpdateLoopCut(.25) && Session.CopyMeshDescription(0, QuarterAgain);
        const TArray<FVector> At25{{0, 25, 0}, {100, 25, 0}, {200, 25, 0}, {300, 25, 0}};
        const TArray<FVector> At75{{0, 75, 0}, {100, 75, 0}, {200, 75, 0}, {300, 75, 0}};
        const TArray<FVector> First = InsertedPositions(Strip.Mesh, Quarter), Second = InsertedPositions(Strip.Mesh, ThreeQuarter);
        Check(TEXT("topology.loop_slide_moves_a_continuous_strip_from_fixed_endpoints"), SlideUpdated && SameMesh(Quarter, QuarterAgain) &&
            ((PositionsEqual(First, At25) && PositionsEqual(Second, At75)) || (PositionsEqual(First, At75) && PositionsEqual(Second, At25))));
        Check(TEXT("topology.loop_interpolates_both_uv_channels_colors_and_material_seams"), SlideUpdated &&
            InterpolatedFixtureAttributes(Quarter) && InterpolatedFixtureAttributes(ThreeQuarter));
        Session.CancelTopologyEdit();
        Check(TEXT("topology.loop_cancel_restores_topology_attributes_and_active_edge"), Session.CopyMeshDescription(0, Preview) &&
            SameMesh(Strip.Mesh, Preview) && OriginalSelection.Matches(Session) && !Session.HasChanges());

        const bool Rebegan = Session.BeginLoopCut(0, SeedEdge, Error);
        if (Rebegan) { Session.UpdateLoopCut(.2); Session.UpdateLoopCut(.5); Session.AcceptTopologyEdit(); }
        FMeshDescription CenterSnapshot;
        const bool CenterCopied = Session.CopyMeshDescription(0, CenterSnapshot);
        const FSelectionSnapshot CenterSelection(Session);
        Check(TEXT("topology.slide_escape_contract_retains_centered_cut"), Rebegan && CenterCopied && AllQuads(CenterSnapshot, 6, 12) &&
            PositionsEqual(InsertedPositions(Strip.Mesh, CenterSnapshot), Centered) && Session.HasChanges());
        const bool Undone = Session.Undo();
        const bool UndoExact = Undone && Session.CopyMeshDescription(0, Preview) && SameMesh(Strip.Mesh, Preview) && OriginalSelection.Matches(Session);
        const bool Redone = Session.Redo();
        Check(TEXT("topology.loop_undo_redo_restores_attributes_and_selected_cut"), UndoExact && Redone && Session.CopyMeshDescription(0, Preview) &&
            SameMesh(CenterSnapshot, Preview) && CenterSelection.Matches(Session));

        const bool Committed = Session.Commit(CreatedAssets, Error);
        UStaticMesh* EditedMesh = StripActor->GetStaticMeshComponent()->GetStaticMesh();
        Check(TEXT("topology.commit_creates_only_one_independent_mesh_and_leaves_source_unchanged"), Committed && CreatedAssets.Num() == 1 &&
            EditedMesh == CreatedAssets[0] && EditedMesh != StripMesh.Get() && SharedActor->GetStaticMeshComponent()->GetStaticMesh() == StripMesh.Get() &&
            StripMesh->GetMeshDescription(0) && SameMesh(Strip.Mesh, *StripMesh->GetMeshDescription(0)));
        if (Committed && Session.Begin(World, {StripActor}, Error))
        {
            Check(TEXT("topology.commit_reentry_preserves_logical_quads_attributes_and_next_loop"), Session.CopyMeshDescription(0, Preview) &&
                AllQuads(Preview, 6, 12) && InterpolatedFixtureAttributes(Preview) && HasUsableLoop(Session) && !Session.HasChanges());
            Session.Cancel();
        }
        else { Check(TEXT("topology.commit_reentry_preserves_logical_quads_attributes_and_next_loop"), false, Error.ToString()); Session.Cancel(); }
    }
    else Check(TEXT("topology.strip_fixture_begin"), false, Error.ToString());

    FTopologyFixture Triangle;
    Triangle.Vertex({0, 0, 0}); Triangle.Vertex({100, 0, 0}); Triangle.Vertex({0, 100, 0}); Triangle.Face({0, 1, 2});
    TStrongObjectPtr<UStaticMesh> TriangleMesh(Triangle.Build());
    AStaticMeshActor* TriangleActor = Spawn(TriangleMesh.Get());
    if (QuadActor && TriangleActor && Session.Begin(World, {QuadActor, TriangleActor}, Error))
    {
        Session.SelectElement(0, VertexMode, 0, false);
        const FSelectionSnapshot VertexSelection(Session);
        const bool ExtrudeRejected = !Session.BeginExtrude(false, Error);
        FMeshDescription OtherPreview;
        Check(TEXT("topology.non_face_extrusion_rejects_without_mutation"), ExtrudeRejected && !Error.IsEmpty() &&
            Session.CopyMeshDescription(0, Preview) && Session.CopyMeshDescription(1, OtherPreview) &&
            SameMesh(Quad.Mesh, Preview) && SameMesh(Triangle.Mesh, OtherPreview) && VertexSelection.Matches(Session) &&
            !Session.IsTopologyEditing() && !Session.HasChanges());
        Session.SelectElement(0, FaceMode, 0, false);
        const FSelectionSnapshot OriginalSelection(Session);
        TArray<TPair<FVector, FVector>> Segments;
        const bool PreviewRejected = !Session.GetLoopCutPreview(1, Triangle.Edge(0, 1), Segments, Error);
        const bool BeginRejected = !Session.BeginLoopCut(1, Triangle.Edge(0, 1), Error);
        Check(TEXT("topology.triangle_loop_rejection_is_atomic_across_session_objects"), PreviewRejected && BeginRejected && !Error.IsEmpty() &&
            Session.CopyMeshDescription(0, Preview) && Session.CopyMeshDescription(1, OtherPreview) &&
            SameMesh(Quad.Mesh, Preview) && SameMesh(Triangle.Mesh, OtherPreview) && OriginalSelection.Matches(Session) &&
            !Session.IsTopologyEditing() && !Session.HasChanges());
        Session.Cancel();
    }
    else Check(TEXT("topology.triangle_fixture_begin"), false, Error.ToString());

    TStrongObjectPtr<UStaticMesh> CubeMesh(CreateBlenderQuadCube());
    if (CubeMesh.IsValid()) CreatedAssets.Add(CubeMesh.Get());
    AStaticMeshActor* CubeActor = Spawn(CubeMesh.Get());
    if (CubeActor && Session.Begin(World, {CubeActor}, Error))
    {
        Check(TEXT("topology.shift_a_cube_helper_has_six_quads_and_usable_loop"), Session.CopyMeshDescription(0, Preview) && AllQuads(Preview, 6, 8) && HasUsableLoop(Session));
        // Use the engine's real normal calculation on the generated source asset.
        // A self-consistent but reversed mathematical cross product would otherwise
        // let both the cube and extrusion tests pass while producing an inside-out cube.
        const FMeshDescription* CubeSource = CubeMesh->GetMeshDescription(0);
        bool AllNormalsOutward = CubeSource && CubeSource->Triangles().Num() == 12;
        double MinOutwardDot = TNumericLimits<double>::Max();
        if (CubeSource)
        {
            FMeshDescription EngineNormals(*CubeSource);
            FStaticMeshOperations::ComputeTriangleTangentsAndNormals(EngineNormals);
            const FStaticMeshConstAttributes Attributes(EngineNormals);
            FVector CubeCenter = FVector::ZeroVector;
            for (FVertexID ID : EngineNormals.Vertices().GetElementIDs()) CubeCenter += FVector(EngineNormals.GetVertexPositions()[ID]);
            if (EngineNormals.Vertices().Num() > 0) CubeCenter /= EngineNormals.Vertices().Num();
            for (FTriangleID ID : EngineNormals.Triangles().GetElementIDs())
            {
                FVector TriangleCenter = FVector::ZeroVector;
                for (FVertexID Vertex : EngineNormals.GetTriangleVertices(ID)) TriangleCenter += FVector(EngineNormals.GetVertexPositions()[Vertex]);
                TriangleCenter /= 3.0;
                const FVector Normal(Attributes.GetTriangleNormals()[ID]);
                const double OutwardDot = FVector::DotProduct(Normal, TriangleCenter - CubeCenter);
                MinOutwardDot = FMath::Min(MinOutwardDot, OutwardDot);
                AllNormalsOutward &= !Normal.ContainsNaN() && Normal.SizeSquared() > .99 && OutwardDot > .01;
            }
        }
        Check(TEXT("topology.generated_cube_engine_triangle_normals_point_outward"), AllNormalsOutward,
            FString::Printf(TEXT("all_12_engine_normals_outward=%d minimum_normal_dot_from_cube_center=%.6f"), AllNormalsOutward, MinOutwardDot));
        int32 SeedEdge = INDEX_NONE;
        TArray<TPair<FVector, FVector>> Segments;
        for (int32 Edge : Session.GetElementIDs(0, EdgeMode))
            if (Session.GetLoopCutPreview(0, Edge, Segments, Error) && Segments.Num() == 4) { SeedEdge = Edge; break; }
        const bool Began = SeedEdge != INDEX_NONE && Session.BeginLoopCut(0, SeedEdge, Error);
        Check(TEXT("topology.closed_cube_loop_terminates_once_and_splits_four_quads"), Began && Session.CopyMeshDescription(0, Preview) &&
            AllQuads(Preview, 10, 12) && Session.GetSelectedElementIDs(0, EdgeMode).Num() == 4);
        Session.Cancel();
    }
    else Check(TEXT("topology.cube_fixture_begin"), false, Error.ToString());

    // Exercise the real mode entrypoints with the scene viewport's modifier state.
    // Only modifier/pointer cache events go to the scene viewport; command keys and
    // clicks go directly to the mode, avoiding OS input and native editor shortcuts.
    const TSharedPtr<SEditorViewport> Widget = Client->GetEditorViewportWidget();
    const TSharedPtr<FSceneViewport> SceneViewport = Widget.IsValid() ? Widget->GetSceneViewport() : nullptr;
    FEditorModeTools& Modes = GLevelEditorModeTools();
    const bool ModeWasActive = Modes.IsModeActive(FBlenderStyleEdMode::ModeId);
    FBlenderStyleEdMode* Mode = static_cast<FBlenderStyleEdMode*>(Modes.GetActiveMode(FBlenderStyleEdMode::ModeId));
    const bool ExistingModeIdle = !Mode || (!Mode->IsMeshEditing() && !Mode->IsTransforming() && !Mode->IsTopologyModal() && !Mode->IsSelecting());
    const bool ViewportIdle = SceneViewport.IsValid() && SceneViewport.Get() == Viewport && !SceneViewport->HasMouseCapture() &&
        !Client->IsTracking() && !Viewport->KeyState(EKeys::LeftControl) && !Viewport->KeyState(EKeys::RightControl) &&
        !Viewport->KeyState(EKeys::LeftShift) && !Viewport->KeyState(EKeys::RightShift) &&
        !Viewport->KeyState(EKeys::LeftAlt) && !Viewport->KeyState(EKeys::RightAlt) &&
        !Viewport->KeyState(EKeys::LeftMouseButton) && !Viewport->KeyState(EKeys::MiddleMouseButton) && !Viewport->KeyState(EKeys::RightMouseButton);
    AStaticMeshActor* ModeActor = Spawn(QuadMesh.Get());
    if (ModeActor && ExistingModeIdle && ViewportIdle && !GEditor->PlayWorld)
    {
        TArray<AActor*> PreviousSelection;
        for (FSelectionIterator It(*GEditor->GetSelectedActors()); It; ++It)
            if (AActor* Actor = Cast<AActor>(*It)) PreviousSelection.Add(Actor);
        const FVector OldLocation = Client->GetViewLocation();
        const FRotator OldRotation = Client->GetViewRotation();
        const ELevelViewportType OldType = Client->GetViewportType();
        const FVector2D OldMouse(SceneViewport->GetMouseX(), SceneViewport->GetMouseY());
        const FGeometry Geometry = SceneViewport->GetCachedGeometry();
        const FModifierKeysState ControlModifiers(false, false, true, false, false, false, false, false, false);
        auto SetControl = [&](bool Down)
        {
            if (Down) SceneViewport->OnKeyDown(Geometry, FKeyEvent(EKeys::LeftControl, ControlModifiers, 0, false, 0, 0));
            else SceneViewport->OnKeyUp(Geometry, FKeyEvent(EKeys::LeftControl, FModifierKeysState(), 0, false, 0, 0));
        };
        auto MovePointer = [&](const FVector2D& Pixel)
        {
            const FVector2D Screen = Geometry.LocalToAbsolute(Pixel / Geometry.Scale);
            const TSet<FKey> NoButtons;
            SceneViewport->OnMouseMove(Geometry, FPointerEvent(0, Screen, Screen, NoButtons, EKeys::Invalid, 0, FModifierKeysState()));
        };
        if (!ModeWasActive) Modes.ActivateMode(FBlenderStyleEdMode::ModeId);
        Mode = static_cast<FBlenderStyleEdMode*>(Modes.GetActiveMode(FBlenderStyleEdMode::ModeId));
        GEditor->SelectNone(false, true, false);
        GEditor->SelectActor(ModeActor, true, true, true);
        const FVector FaceCenter = ModeActor->GetActorLocation() + FVector(50, 50, 0);
        Client->SetViewportType(LVT_Perspective);
        Client->SetViewLocation(FaceCenter + FVector(0, 0, 600));
        Client->SetViewRotation(FRotator(-90, 0, 0));
        auto Key = [&](FKey Pressed)
        {
            if (!Mode) return false;
            const bool Consumed = Mode->InputKey(Client, Viewport, Pressed, IE_Pressed);
            Mode->InputKey(Client, Viewport, Pressed, IE_Released);
            return Consumed;
        };
        const bool Began = Mode && Mode->BeginMeshEdit(Client, Viewport);
        if (Began)
        {
            FBlenderMeshEditSession* ModeSession = Mode->GetMeshEditSession();
            ModeSession->SelectElement(0, FaceMode, 0, false);
            const bool ExtrudeKey = Key(EKeys::E);
            const bool ExtrudeStarted = Mode->IsTopologyModal() && ModeSession->IsTopologyEditing();
            Key(EKeys::Two); Key(EKeys::Five); Key(EKeys::Enter);
            TArray<FVector> Expected;
            for (FVertexID ID : Quad.Mesh.Vertices().GetElementIDs())
                Expected.Add(ModeActor->GetActorTransform().TransformPosition(FVector(Quad.Mesh.GetVertexPositions()[ID])) + FVector(0, 0, 25));
            Check(TEXT("topology.mode_e_numeric_enter_confirms_world_distance"), ExtrudeKey && ExtrudeStarted && Mode->IsMeshEditing() &&
                !Mode->IsTopologyModal() && ModeSession->CopyMeshDescription(0, Preview) && AllQuads(Preview, 5, 8) &&
                PositionsEqual(SelectedPositions(*ModeSession), Expected));
            Mode->EndMeshEdit(false);

            const bool Restarted = Mode->BeginMeshEdit(Client, Viewport);
            ModeSession = Mode->GetMeshEditSession();
            if (Restarted && ModeSession)
            {
                auto SessionAlive = [&]() { return Mode->IsMeshEditing() && Mode->GetMeshEditSession() == ModeSession; };
                ModeSession->SelectElement(0, FaceMode, 0, false);
                const FSelectionSnapshot OriginalSelection(*ModeSession);
                Key(EKeys::E); Key(EKeys::Two); Key(EKeys::Five); Key(EKeys::Escape);
                Expected.Reset();
                for (FVertexID ID : Quad.Mesh.Vertices().GetElementIDs())
                    Expected.Add(ModeActor->GetActorTransform().TransformPosition(FVector(Quad.Mesh.GetVertexPositions()[ID])));
                Check(TEXT("topology.mode_escape_retains_zero_extrusion_without_leaving_edit_mode"), SessionAlive() && !Mode->IsTopologyModal() &&
                    ModeSession->HasChanges() && ModeSession->CopyMeshDescription(0, Preview) && AllQuads(Preview, 5, 8) &&
                    PositionsEqual(SelectedPositions(*ModeSession), Expected));
                SetControl(true);
                const bool ControlDown = SceneViewport->KeyState(EKeys::LeftControl);
                const bool UndoConsumed = Key(EKeys::Z);
                SetControl(false);
                Check(TEXT("topology.mode_ctrl_z_removes_zero_extrusion_and_restores_selection"), ControlDown && UndoConsumed &&
                    !SceneViewport->KeyState(EKeys::LeftControl) && !Mode->IsTopologyModal() && SessionAlive() &&
                    ModeSession->CopyMeshDescription(0, Preview) && SameMesh(Quad.Mesh, Preview) && OriginalSelection.Matches(*ModeSession) && !ModeSession->HasChanges());

                FSceneViewFamilyContext Family(FSceneViewFamily::ConstructionValues(Viewport, Client->GetScene(), Client->EngineShowFlags));
                FSceneView* View = Client->CalcSceneView(&Family);
                FVector2D Pixel;
                const bool Projected = View && View->WorldToPixel(FaceCenter, Pixel) && Pixel.X > 2 && Pixel.Y > 2 &&
                    Pixel.X < Viewport->GetSizeXY().X - 2 && Pixel.Y < Viewport->GetSizeXY().Y - 2;
                if (Projected) MovePointer(Pixel);
                SetControl(true); Key(EKeys::R); SetControl(false);
                const bool HoverStarted = SessionAlive() && Mode->IsTopologyModal() && !ModeSession->IsTopologyEditing();
                Key(EKeys::Escape);
                Check(TEXT("topology.mode_loop_hover_escape_does_not_insert_geometry"), Projected && HoverStarted && SessionAlive() &&
                    !Mode->IsTopologyModal() && ModeSession->CopyMeshDescription(0, Preview) && SameMesh(Quad.Mesh, Preview) && !ModeSession->HasChanges());

                if (Projected) MovePointer(Pixel);
                SetControl(true); Key(EKeys::R); SetControl(false);
                Key(EKeys::LeftMouseButton);
                const bool SlideStarted = SessionAlive() && Mode->IsTopologyModal() && ModeSession->IsTopologyEditing();
                Key(EKeys::Zero); Key(EKeys::Enter);
                const bool Copied = SessionAlive() && ModeSession->CopyMeshDescription(0, Preview);
                const TArray<FVector> Horizontal{{0, 50, 0}, {100, 50, 0}};
                const TArray<FVector> Vertical{{50, 0, 0}, {50, 100, 0}};
                const TArray<FVector> Inserted = Copied ? InsertedPositions(Quad.Mesh, Preview) : TArray<FVector>();
                Check(TEXT("topology.mode_ctrl_r_face_hover_lmb_numeric_zero_centers_loop"), Projected && SlideStarted && SessionAlive() &&
                    !Mode->IsTopologyModal() && Copied && AllQuads(Preview, 2, 6) &&
                    (PositionsEqual(Inserted, Horizontal) || PositionsEqual(Inserted, Vertical)),
                    FString::Printf(TEXT("projected=%d slide_started=%d faces=%d vertices=%d; real scene Ctrl state, face-center hover, no OS input"),
                        Projected, SlideStarted, Preview.Polygons().Num(), Preview.Vertices().Num()));
            }
            else Check(TEXT("topology.mode_fixture_reentry"), false);
        }
        else Check(TEXT("topology.mode_fixture_begin"), false);
        // Always release synthetic state and only close the fixture session we started.
        SetControl(false);
        if (Began && Mode)
        {
            Mode->CancelActiveTransform();
            if (Mode->IsMeshEditing()) Mode->EndMeshEdit(false);
        }
        if (!ModeWasActive) Modes.DeactivateMode(FBlenderStyleEdMode::ModeId);
        Client->SetViewportType(OldType); Client->SetViewLocation(OldLocation); Client->SetViewRotation(OldRotation);
        MovePointer(OldMouse); Client->Invalidate();
        GEditor->SelectNone(false, true, false);
        for (AActor* Actor : PreviousSelection) if (IsValid(Actor)) GEditor->SelectActor(Actor, true, false, true);
        GEditor->NoteSelectionChange();
    }
    else Check(TEXT("topology.mode_fixture_idle_viewport"), false);

    Session.Cancel();
    for (AStaticMeshActor* Actor : Actors) if (IsValid(Actor)) World->DestroyActor(Actor);
    // Only assets created by this invocation's commit and cube helper are discarded.
    // Nothing is saved, and pre-existing user assets are never enumerated or touched.
    for (UStaticMesh* Asset : CreatedAssets) if (IsValid(Asset))
    {
        UPackage* Package = Asset->GetOutermost();
        FAssetRegistryModule::AssetDeleted(Asset);
        Asset->ClearFlags(RF_Public | RF_Standalone);
        Asset->Rename(nullptr, GetTransientPackage(), REN_DontCreateRedirectors | REN_NonTransactional);
        Asset->MarkAsGarbage(); Package->SetDirtyFlag(false);
    }
    World->GetOutermost()->SetDirtyFlag(WorldWasDirty);
    GetTransientPackage()->SetDirtyFlag(TransientWasDirty);
}
