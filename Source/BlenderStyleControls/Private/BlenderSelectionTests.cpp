#include "BlenderSelectionTests.h"
#include "BlenderMeshEditSession.h"
#include "Components/StaticMeshComponent.h"
#include "Dom/JsonObject.h"
#include "Editor.h"
#include "EditorViewportClient.h"
#include "Engine/StaticMesh.h"
#include "Engine/StaticMeshActor.h"
#include "Engine/World.h"
#include "MeshDescription.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "SceneView.h"
#include "SEditorViewport.h"
#include "Slate/SceneViewport.h"
#include "Input/Events.h"
#include "InputCoreTypes.h"
#include "StaticMeshAttributes.h"
#include "StaticMeshCompiler.h"
#include "StaticMeshOperations.h"
#include "UObject/Package.h"
#include "UObject/StrongObjectPtr.h"
#include "UnrealClient.h"
#include <initializer_list>

namespace
{
struct FSelectionFixture
{
    FMeshDescription Mesh;
    TArray<FVertexID> Vertices;
    FPolygonGroupID Group;
    FSelectionFixture()
    {
        FStaticMeshAttributes Attributes(Mesh); Attributes.Register();
        Attributes.GetVertexInstanceUVs().SetNumChannels(1);
        Group = Mesh.CreatePolygonGroup();
    }
    int32 Vertex(const FVector3f& P)
    {
        const FVertexID ID = Mesh.CreateVertex();
        Mesh.GetVertexPositions()[ID] = P; Vertices.Add(ID); return ID.GetValue();
    }
    void Face(std::initializer_list<int32> Corners)
    {
        TArray<FVertexInstanceID> Instances;
        for (int32 ID : Corners) Instances.Add(Mesh.CreateVertexInstance(FVertexID(ID)));
        Mesh.CreatePolygon(Group, Instances);
    }
    void Grid(int32 NX, int32 NY, float Spacing, float Depth = 0)
    {
        const int32 Base = Vertices.Num();
        for (int32 Y=0; Y<=NY; ++Y) for (int32 X=0; X<=NX; ++X)
            Vertex(FVector3f(Depth, X*Spacing, Y*Spacing));
        for (int32 Y=0; Y<NY; ++Y) for (int32 X=0; X<NX; ++X)
        {
            const int32 V = Base+Y*(NX+1)+X;
            Face({V,V+1,V+NX+2,V+NX+1});
        }
    }
    UStaticMesh* Build()
    {
        UStaticMesh* Result = NewObject<UStaticMesh>(GetTransientPackage(), NAME_None, RF_Transient);
        Result->AddSourceModel();
        Result->GetStaticMaterials().Add(FStaticMaterial());
        Result->GetSourceModel(0).BuildSettings.bGenerateLightmapUVs = false;
        FStaticMeshOperations::ComputeTriangleTangentsAndNormals(Mesh);
        FStaticMeshOperations::ComputeTangentsAndNormals(Mesh, EComputeNTBsFlags::Normals | EComputeNTBsFlags::Tangents);
        Result->CreateMeshDescription(0, FMeshDescription(Mesh));
        Result->CommitMeshDescription(0);
        Result->Build(false);
        TArray<UStaticMesh*> Finish{Result}; FStaticMeshCompilingManager::Get().FinishCompilation(Finish);
        return Result;
    }
    int32 Edge(int32 A, int32 B) const
    {
        for (FEdgeID E : Mesh.Edges().GetElementIDs())
        {
            const auto V = Mesh.GetEdgeVertices(E);
            if ((V[0].GetValue()==A && V[1].GetValue()==B) || (V[0].GetValue()==B && V[1].GetValue()==A)) return E.GetValue();
        }
        return INDEX_NONE;
    }
};
}

void AppendBlenderSelectionVerification(FEditorViewportClient* Client, FViewport* Viewport,
    TArray<TSharedPtr<FJsonValue>>& Checks, int32& Failures)
{
    // This entrypoint is deliberately inert in an ordinary editor process.
    if (!FParse::Param(FCommandLine::Get(), TEXT("BlenderControlsVerify")) || !Client || !Viewport || !GEditor) return;
    auto Check = [&](const TCHAR* Name, bool Passed, const FString& Detail=FString())
    {
        TSharedRef<FJsonObject> J=MakeShared<FJsonObject>(); J->SetStringField(TEXT("name"),Name);
        J->SetBoolField(TEXT("passed"),Passed); J->SetStringField(TEXT("details"),Detail);
        Checks.Add(MakeShared<FJsonValueObject>(J)); if(!Passed) ++Failures;
    };
    UWorld* World=GEditor->GetEditorWorldContext().World();
    if (!World || World->IsGameWorld()) {Check(TEXT("selection.fixture_world"),false); return;}
    const bool WasDirty=World->GetOutermost()->IsDirty();
    const bool TransientDirty=GetTransientPackage()->IsDirty();
    const FVector OldLocation=Client->GetViewLocation(); const FRotator OldRotation=Client->GetViewRotation();
    const ELevelViewportType OldType=Client->GetViewportType();
    TArray<AStaticMeshActor*> Actors;
    auto Spawn=[&](UStaticMesh* Mesh,const FVector& Location)
    {
        FActorSpawnParameters P; P.ObjectFlags|=RF_Transient;
        P.SpawnCollisionHandlingOverride=ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
        AStaticMeshActor* A=World->SpawnActor<AStaticMeshActor>(Location,FRotator::ZeroRotator,P);
        if(A) {A->GetStaticMeshComponent()->SetMobility(EComponentMobility::Movable); A->GetStaticMeshComponent()->SetStaticMesh(Mesh);Actors.Add(A);} return A;
    };
    FSelectionFixture Grid; Grid.Grid(4,4,70); Grid.Grid(1,1,70,400);
    TStrongObjectPtr<UStaticMesh> GridMesh(Grid.Build());
    const FVector Offset(25000,25000,25000);
    AStaticMeshActor* Actor=Spawn(GridMesh.Get(),Offset);
    FBlenderMeshEditSession Session; FText Error;
    if (Actor && Session.Begin(World,{Actor},Error))
    {
        const auto V=EBlenderMeshSelectionMode::Vertex, E=EBlenderMeshSelectionMode::Edge, F=EBlenderMeshSelectionMode::Face;
        Session.SelectElement(0,E,Grid.Edge(11,12),false);
        Session.SelectLoop(0,Grid.Edge(11,12),false,false);
        Check(TEXT("selection.loop_follows_quad_topology_to_boundary"),Session.GetSelectedElementIDs(0,E).Num()==4);
        Session.SelectLoop(0,Grid.Edge(11,12),false,true);
        Check(TEXT("selection.shift_loop_toggles_off"),Session.GetSelectedElementIDs(0,E).IsEmpty());
        Session.SelectLoop(0,Grid.Edge(11,12),true,false);
        Check(TEXT("selection.ring_crosses_opposite_quad_edges"),Session.GetSelectedElementIDs(0,E).Num()==5);
        Session.SelectLoop(0,Grid.Edge(1,2),false,false);
        Check(TEXT("selection.boundary_loop_stops_at_grid_corner"),Session.GetSelectedElementIDs(0,E).Num()==4);
        Session.SelectElement(0,F,5,false); Session.GrowSelection(true);
        Check(TEXT("selection.grow_faces_includes_diagonal_neighbors"),Session.GetSelectedElementIDs(0,F).Num()==9);
        Session.GrowSelection(false);
        Check(TEXT("selection.shrink_faces_keeps_interior"),Session.GetSelectedElementIDs(0,F).Contains(5) && Session.GetSelectedElementIDs(0,F).Num()<9);
        Session.SelectElement(0,V,0,false); Session.SelectLinked(true);
        Check(TEXT("selection.linked_stays_in_connected_component"),Session.GetSelectedElementIDs(0,V).Num()==25);
        Session.SelectLinked(false); Check(TEXT("selection.linked_deselect"),Session.GetSelectedElementIDs(0,V).IsEmpty());
        Session.SelectElement(0,V,0,false); Session.SelectShortestPath(0,V,24,false);
        Check(TEXT("selection.path_edges_only_without_face_stepping"),Session.GetSelectedElementIDs(0,V).Num()==9);
        int32 ActiveObject,ActiveID; EBlenderMeshSelectionMode ActiveMode;
        Check(TEXT("selection.path_destination_becomes_active"),Session.GetActiveElement(ActiveObject,ActiveMode,ActiveID) && ActiveObject==0 && ActiveID==24 && ActiveMode==V);
        Session.SelectElement(0,V,0,false); Session.SelectShortestPath(0,V,24,true);
        Check(TEXT("selection.fill_region_all_topological_paths"),Session.GetSelectedElementIDs(0,V).Num()==25);
        Session.SelectElement(0,V,0,false); Session.SelectShortestPath(0,V,25,false);
        Check(TEXT("selection.disconnected_path_only_toggles_destination"),Session.GetSelectedElementIDs(0,V).Num()==2);
        Session.SelectElement(0,V,25,false);Session.SelectElement(0,V,0,true);
        Session.SelectShortestPath(0,V,25,false);
        Check(TEXT("selection.disconnected_deselect_preserves_other_active"),Session.GetActiveElement(ActiveObject,ActiveMode,ActiveID) && ActiveObject==0 && ActiveID==0);
        Session.SelectShortestPath(0,V,4,false);
        Check(TEXT("selection.path_chains_after_disconnected_deselect"),Session.GetSelectedElementIDs(0,V)==TArray<int32>({0,1,2,3,4}));
        Session.Cancel();
    }
    else Check(TEXT("selection.fixture_begin"),false,Error.ToString());

    // A single five-sided face has a two-edge 200-unit detour and a three-edge 3-unit route.
    // Hop-count implementations pick vertices 0,1,2 and fail this test.
    FSelectionFixture Weighted;
    Weighted.Vertex({0,0,0}); Weighted.Vertex({0,1.5f,100}); Weighted.Vertex({0,3,0});
    Weighted.Vertex({0,2,-0.1f}); Weighted.Vertex({0,1,-0.1f}); Weighted.Face({0,1,2,3,4});
    TStrongObjectPtr<UStaticMesh> WeightedMesh(Weighted.Build());
    AStaticMeshActor* WeightedActor=Spawn(WeightedMesh.Get(),Offset+FVector(0,800,0));
    if(WeightedActor && Session.Begin(World,{WeightedActor},Error))
    {
        Session.SelectElement(0,EBlenderMeshSelectionMode::Vertex,0,false);
        Session.SelectShortestPath(0,EBlenderMeshSelectionMode::Vertex,2);
        Check(TEXT("selection.shortest_path_uses_geometry_not_hops"),Session.GetSelectedElementIDs(0,EBlenderMeshSelectionMode::Vertex)==TArray<int32>({0,2,3,4}));
        Session.SelectShortestPath(0,EBlenderMeshSelectionMode::Vertex,0);
        Check(TEXT("selection.path_toggles_fully_selected_route"),Session.GetSelectedElementIDs(0,EBlenderMeshSelectionMode::Vertex).IsEmpty());
        Session.Cancel();
    }
    else Check(TEXT("selection.weighted_fixture_begin"),false,Error.ToString());

    FSelectionFixture Pole;Pole.Grid(4,4,70);const int32 Extra=Pole.Vertex({60,170,170});Pole.Face({12,13,Extra});
    TStrongObjectPtr<UStaticMesh> PoleMesh(Pole.Build());AStaticMeshActor* PoleActor=Spawn(PoleMesh.Get(),Offset+FVector(0,1200,0));
    if(PoleActor && Session.Begin(World,{PoleActor},Error))
    {
        Session.SetSelectionMode(EBlenderMeshSelectionMode::Edge);Session.SelectLoop(0,Pole.Edge(11,12),false,false);
        Check(TEXT("selection.loop_stops_at_valence_five_pole"),Session.GetSelectedElementIDs(0,EBlenderMeshSelectionMode::Edge).Num()==2);
        Session.Cancel();
    }
    else Check(TEXT("selection.pole_fixture_begin"),false,Error.ToString());

    FSelectionFixture Tube;
    for(int32 Row=0;Row<3;++Row)for(int32 I=0;I<8;++I)
    {const float Angle=I*2.0f*PI/8.0f;Tube.Vertex({Row*70.0f,100.0f*FMath::Cos(Angle),100.0f*FMath::Sin(Angle)});}
    for(int32 Row=0;Row<2;++Row)for(int32 I=0;I<8;++I)
    {const int32 Next=(I+1)%8;Tube.Face({Row*8+I,Row*8+Next,(Row+1)*8+Next,(Row+1)*8+I});}
    TStrongObjectPtr<UStaticMesh> TubeMesh(Tube.Build());AStaticMeshActor* TubeActor=Spawn(TubeMesh.Get(),Offset+FVector(0,1700,0));
    if(TubeActor && Session.Begin(World,{TubeActor},Error))
    {
        Session.SetSelectionMode(EBlenderMeshSelectionMode::Edge);Session.SelectLoop(0,Tube.Edge(8,9),false,false);
        Check(TEXT("selection.loop_closes_on_connected_quad_tube"),Session.GetSelectedElementIDs(0,EBlenderMeshSelectionMode::Edge).Num()==8);
        Session.SelectLoop(0,Tube.Edge(0,8),true,false);
        Check(TEXT("selection.ring_closes_without_duplicate_edges"),Session.GetSelectedElementIDs(0,EBlenderMeshSelectionMode::Edge).Num()==8);
        Session.Cancel();
    }
    else Check(TEXT("selection.tube_fixture_begin"),false,Error.ToString());

    FSelectionFixture Sheet; Sheet.Grid(2,2,100);
    TStrongObjectPtr<UStaticMesh> SheetMesh(Sheet.Build());
    AStaticMeshActor* Front=Spawn(SheetMesh.Get(),Offset+FVector(0,-100,-100));
    AStaticMeshActor* Rear=Spawn(SheetMesh.Get(),Offset+FVector(80,-100,-100));
    UStaticMesh* OccluderMesh=LoadObject<UStaticMesh>(nullptr,TEXT("/Engine/BasicShapes/Cube.Cube"));
    AStaticMeshActor* Occluder=OccluderMesh?Spawn(OccluderMesh,Offset+FVector(-40,0,0)):nullptr;
    if(Occluder)
    {
        Occluder->SetActorScale3D(FVector(0.1,3,3));
        Occluder->GetStaticMeshComponent()->SetCollisionEnabled(ECollisionEnabled::NoCollision);
        Occluder->SetIsTemporarilyHiddenInEditor(true);
    }
    if(Front && Rear && Session.Begin(World,{Front,Rear},Error))
    {
        Client->SetViewportType(LVT_Perspective); Client->SetViewLocation(Offset-FVector(600,0,0)); Client->SetViewRotation(FRotator::ZeroRotator);
        FSceneViewFamilyContext Family(FSceneViewFamily::ConstructionValues(Viewport,Client->GetScene(),Client->EngineShowFlags));
        FSceneView* View=Client->CalcSceneView(&Family); FVector2D Pixel;
        if(View && View->WorldToPixel(Offset,Pixel))
        {
            const FIntRect Box(FMath::FloorToInt(Pixel.X-4),FMath::FloorToInt(Pixel.Y-4),FMath::CeilToInt(Pixel.X+4),FMath::CeilToInt(Pixel.Y+4));
            Session.SelectAll(false); Session.SetXRay(false); Session.BoxSelect(Client,Viewport,Box,EBlenderSelectionOperation::Replace);
            Check(TEXT("selection.box_visible_rejects_rear_vertex"),Session.GetSelectedElementIDs(0,EBlenderMeshSelectionMode::Vertex).Contains(4) && Session.GetSelectedElementIDs(1,EBlenderMeshSelectionMode::Vertex).IsEmpty());
            Session.SetXRay(true); Session.BoxSelect(Client,Viewport,Box,EBlenderSelectionOperation::Add);
            Check(TEXT("selection.box_xray_adds_rear_vertex"),Session.GetSelectedElementIDs(1,EBlenderMeshSelectionMode::Vertex).Contains(4));
            Session.SelectElement(0,EBlenderMeshSelectionMode::Vertex,0,true);
            Session.BoxSelect(Client,Viewport,Box,EBlenderSelectionOperation::Intersect);
            Check(TEXT("selection.box_intersect_keeps_only_prior_inside"),Session.GetSelectedVertices().Num()==2 && !Session.GetSelectedElementIDs(0,EBlenderMeshSelectionMode::Vertex).Contains(0));
            Session.BoxSelect(Client,Viewport,Box,EBlenderSelectionOperation::Subtract);
            Check(TEXT("selection.box_subtract_removes_both_layers"),Session.GetSelectedVertices().IsEmpty());
            FVector2D EdgeMiddle,EdgeEnd;
            View->WorldToPixel(Offset+FVector(0,50,0),EdgeMiddle);View->WorldToPixel(Offset+FVector(0,100,0),EdgeEnd);
            Session.SetSelectionMode(EBlenderMeshSelectionMode::Edge);
            const FIntRect Crossing(FMath::FloorToInt(EdgeMiddle.X-3),FMath::FloorToInt(EdgeMiddle.Y-3),FMath::CeilToInt(EdgeMiddle.X+3),FMath::CeilToInt(EdgeMiddle.Y+3));
            Session.BoxSelect(Client,Viewport,Crossing,EBlenderSelectionOperation::Replace);
            Check(TEXT("selection.box_edges_crossing_fallback"),Session.GetSelectedElementIDs(0,EBlenderMeshSelectionMode::Edge)==TArray<int32>({Sheet.Edge(4,5)}));
            const FIntRect Enclosed(FMath::FloorToInt(FMath::Min(Pixel.X,EdgeEnd.X)-3),FMath::FloorToInt(Pixel.Y-3),FMath::CeilToInt(FMath::Max(Pixel.X,EdgeEnd.X)+3),FMath::CeilToInt(Pixel.Y+3));
            Session.BoxSelect(Client,Viewport,Enclosed,EBlenderSelectionOperation::Replace);
            Check(TEXT("selection.box_edges_enclosed_before_intersections"),Session.GetSelectedElementIDs(0,EBlenderMeshSelectionMode::Edge)==TArray<int32>({Sheet.Edge(4,5)}));
            Session.SetSelectionMode(EBlenderMeshSelectionMode::Face);Session.SetXRay(false);FVector2D FaceCorner;
            View->WorldToPixel(Offset+FVector(0,10,10),FaceCorner);
            const FIntRect Fragment(FMath::FloorToInt(FaceCorner.X-2),FMath::FloorToInt(FaceCorner.Y-2),FMath::CeilToInt(FaceCorner.X+2),FMath::CeilToInt(FaceCorner.Y+2));
            Session.BoxSelect(Client,Viewport,Fragment,EBlenderSelectionOperation::Replace);
            Check(TEXT("selection.box_solid_faces_use_visible_fragments"),Session.GetSelectedElementIDs(0,EBlenderMeshSelectionMode::Face)==TArray<int32>({3}) && Session.GetSelectedElementIDs(1,EBlenderMeshSelectionMode::Face).IsEmpty());
            Session.SetXRay(true);Session.BoxSelect(Client,Viewport,Fragment,EBlenderSelectionOperation::Replace);
            Check(TEXT("selection.box_xray_faces_use_centers"),Session.GetSelectedVertices().IsEmpty());
            const TSharedPtr<SEditorViewport> Widget=Client->GetEditorViewportWidget();
            const TSharedPtr<FSceneViewport> SceneViewport=Widget.IsValid()?Widget->GetSceneViewport():nullptr;
            if(SceneViewport.IsValid() && SceneViewport.Get()==Viewport && !SceneViewport->HasMouseCapture() && !Client->IsTracking())
            {
                const FGeometry Geometry=SceneViewport->GetCachedGeometry();
                const FVector2D OldMouse(SceneViewport->GetMouseX(),SceneViewport->GetMouseY());
                auto MoveMouse=[&](const FVector2D& P)
                {
                    const FVector2D Screen=Geometry.LocalToAbsolute(P/Geometry.Scale);const TSet<FKey> Buttons;
                    SceneViewport->OnMouseMove(Geometry,FPointerEvent(0,Screen,Screen,Buttons,EKeys::Invalid,0,FModifierKeysState()));
                };
                FVector2D RearFaceCenter;View->WorldToPixel(Offset+FVector(80,50,50),RearFaceCenter);MoveMouse(RearFaceCenter);
                Session.SetXRay(false);Session.Pick(Client,Viewport,EBlenderPickAction::Normal);
                Check(TEXT("selection.face_pick_solid_uses_front_surface"),Session.GetSelectedElementIDs(0,EBlenderMeshSelectionMode::Face)==TArray<int32>({3}) && Session.GetSelectedElementIDs(1,EBlenderMeshSelectionMode::Face).IsEmpty());
                Session.SetXRay(true);Session.Pick(Client,Viewport,EBlenderPickAction::Normal);
                Check(TEXT("selection.face_pick_xray_uses_rear_projected_center"),Session.GetSelectedElementIDs(1,EBlenderMeshSelectionMode::Face)==TArray<int32>({3}) && Session.GetSelectedElementIDs(0,EBlenderMeshSelectionMode::Face).IsEmpty());
                Session.SelectElement(1,EBlenderMeshSelectionMode::Face,0,false);
                Session.Pick(Client,Viewport,EBlenderPickAction::ShortestPath);
                Check(TEXT("selection.face_path_xray_reaches_occluded_destination"),Session.GetSelectedElementIDs(1,EBlenderMeshSelectionMode::Face).Num()==3 && Session.GetSelectedElementIDs(1,EBlenderMeshSelectionMode::Face).Contains(3) && Session.GetSelectedElementIDs(0,EBlenderMeshSelectionMode::Face).IsEmpty());
                MoveMouse(OldMouse);
            }
            else Check(TEXT("selection.face_pick_fixture_mouse_idle"),false);
            if(Occluder)
            {
                Occluder->SetIsTemporarilyHiddenInEditor(false);
                Session.SetSelectionMode(EBlenderMeshSelectionMode::Vertex);Session.SetXRay(false);
                Session.BoxSelect(Client,Viewport,Box,EBlenderSelectionOperation::Replace);
                Check(TEXT("selection.box_no_collision_external_mesh_occludes"),Session.GetSelectedVertices().IsEmpty());
                Session.SetXRay(true);Session.BoxSelect(Client,Viewport,Box,EBlenderSelectionOperation::Replace);
                Check(TEXT("selection.box_xray_bypasses_external_no_collision_mesh"),Session.GetSelectedVertices().Num()==2);
                Occluder->SetIsTemporarilyHiddenInEditor(true);
            }
            else Check(TEXT("selection.external_occluder_fixture"),false);
            Check(TEXT("selection.selection_does_not_change_source"),!Session.HasChanges() && Front->GetStaticMeshComponent()->GetStaticMesh()==SheetMesh.Get());
        }
        else Check(TEXT("selection.box_fixture_projection"),false);
        Session.Cancel();
    }
    else Check(TEXT("selection.box_fixture_begin"),false,Error.ToString());

    FSelectionFixture Small,Large;Small.Grid(1,1,100);Large.Grid(1,1,200);
    TStrongObjectPtr<UStaticMesh> SmallMesh(Small.Build()),LargeMesh(Large.Build());
    AStaticMeshActor* Left=Spawn(SmallMesh.Get(),Offset+FVector(0,-100,0));
    AStaticMeshActor* Right=Spawn(LargeMesh.Get(),Offset+FVector(0,100,0));
    if(Left && Right && Session.Begin(World,{Left,Right},Error))
    {
        Session.SetSelectionMode(EBlenderMeshSelectionMode::Edge);Session.SetXRay(true);
        FSceneViewFamilyContext Family(FSceneViewFamily::ConstructionValues(Viewport,Client->GetScene(),Client->EngineShowFlags));
        FSceneView* View=Client->CalcSceneView(&Family);FVector2D A,B;
        if(View && View->WorldToPixel(Offset+FVector(0,-110,-4),A) && View->WorldToPixel(Offset+FVector(0,200,4),B))
        {
            const FIntRect Box(FMath::FloorToInt(FMath::Min(A.X,B.X)),FMath::FloorToInt(FMath::Min(A.Y,B.Y)),FMath::CeilToInt(FMath::Max(A.X,B.X)),FMath::CeilToInt(FMath::Max(A.Y,B.Y)));
            Session.BoxSelect(Client,Viewport,Box,EBlenderSelectionOperation::Replace);
            Check(TEXT("selection.box_edge_fallback_is_per_object"),Session.GetSelectedElementIDs(0,EBlenderMeshSelectionMode::Edge)==TArray<int32>({Small.Edge(0,1)}) && Session.GetSelectedElementIDs(1,EBlenderMeshSelectionMode::Edge).Contains(Large.Edge(0,1)));
        }
        else Check(TEXT("selection.box_multiobject_projection"),false);
        Session.Cancel();
    }
    else Check(TEXT("selection.box_multiobject_begin"),false,Error.ToString());
    Client->SetViewportType(OldType); Client->SetViewLocation(OldLocation); Client->SetViewRotation(OldRotation); Client->Invalidate();
    for(AStaticMeshActor* A:Actors) if(IsValid(A)) World->DestroyActor(A);
    World->GetOutermost()->SetDirtyFlag(WasDirty); GetTransientPackage()->SetDirtyFlag(TransientDirty);
}
