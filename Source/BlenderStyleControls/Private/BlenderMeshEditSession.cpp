#include "BlenderMeshEditSession.h"
#include "BlenderMeshTopology.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetToolsModule.h"
#include "Components/StaticMeshComponent.h"
#include "Components/InstancedStaticMeshComponent.h"
#include "DynamicMesh/DynamicMesh3.h"
#include "DynamicMesh/DynamicMeshAABBTree3.h"
#include "DynamicMesh/MeshNormals.h"
#include "Editor.h"
#include "EditorViewportClient.h"
#include "Engine/Level.h"
#include "Engine/Selection.h"
#include "Engine/StaticMesh.h"
#include "Engine/StaticMeshActor.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "HAL/FileManager.h"
#include "IAssetTools.h"
#include "LevelUtils.h"
#include "MeshDescription.h"
#include "MeshDescriptionToDynamicMesh.h"
#include "Misc/PackageName.h"
#include "Modules/ModuleManager.h"
#include "PhysicsEngine/BodySetup.h"
#include "PreviewMesh.h"
#include "SceneManagement.h"
#include "SceneView.h"
#include "ScopedTransaction.h"
#include "StaticMeshComponentLODInfo.h"
#include "StaticMeshCompiler.h"
#include "StaticMeshOperations.h"
#include "StaticMeshAttributes.h"
#include "Materials/Material.h"
#include "MaterialDomain.h"
#include "StaticMeshResources.h"
#include "UObject/Package.h"
#include "UObject/StrongObjectPtr.h"
#include "UnrealClient.h"
#include "Materials/MaterialInterface.h"
#include <queue>
#include <vector>

#define LOCTEXT_NAMESPACE "BlenderMeshEditSession"

namespace
{
	using FPositions = TMap<int32, FVector3f>;
	struct FObjectSnapshot
	{
		FMeshDescription Mesh;
		TSet<int32> Vertices, Edges, Faces;
		int32 TopologyRevision = 0;
		int32 ActiveObject = INDEX_NONE, ActiveElement = INDEX_NONE;
		EBlenderMeshSelectionMode Mode = EBlenderMeshSelectionMode::Vertex;
		EBlenderMeshSelectionMode ActiveMode = EBlenderMeshSelectionMode::Vertex;
	};
	using FSnapshot = TArray<FObjectSnapshot>;
	FPositions ReadPositions(const FMeshDescription& Mesh);

	bool PositionsEqual(const FPositions& A, const FPositions& B)
	{
		if (A.Num() != B.Num()) return false;
		for (const auto& Pair : A)
		{
			const FVector3f* Other = B.Find(Pair.Key);
			if (!Other || !Pair.Value.Equals(*Other, 1.e-5f)) return false;
		}
		return true;
	}

	bool SnapshotsEqual(const FSnapshot& A, const FSnapshot& B)
	{
		if (A.Num() != B.Num()) return false;
		for (int32 I = 0; I < A.Num(); ++I)
			if (A[I].TopologyRevision != B[I].TopologyRevision || !PositionsEqual(ReadPositions(A[I].Mesh), ReadPositions(B[I].Mesh))) return false;
		return true;
	}

	FPositions ReadPositions(const FMeshDescription& Mesh)
	{
		FPositions Result;
		const auto Positions = Mesh.GetVertexPositions();
		for (const FVertexID ID : Mesh.Vertices().GetElementIDs()) Result.Add(ID.GetValue(), Positions[ID]);
		return Result;
	}

	struct FPendingAsset
	{
		explicit FPendingAsset(UPackage* InPackage) : Package(InPackage) {}
		TStrongObjectPtr<UPackage> Package;
		TStrongObjectPtr<UStaticMesh> Mesh;
		bool bRegistered = false;
	};

	void AbandonNewAssets(TArray<TUniquePtr<FPendingAsset>>& Assets)
	{
		// Every package was verified absent before creation; none has ever been saved or
		// assigned to a user actor. This also covers a package whose duplication failed.
		for (auto& Asset : Assets)
		{
			if (UStaticMesh* Mesh = Asset->Mesh.Get())
			{
				if (Asset->bRegistered) FAssetRegistryModule::AssetDeleted(Mesh);
				Mesh->ClearFlags(RF_Public | RF_Standalone);
				Mesh->SetFlags(RF_Transient);
				Mesh->MarkAsGarbage();
			}
			if (UPackage* Package = Asset->Package.Get())
			{
				Package->SetDirtyFlag(false);
				Package->MarkAsGarbage();
			}
			Asset->Mesh.Reset(); Asset->Package.Reset();
		}
		Assets.Reset();
	}
}

struct FBlenderMeshEditSession::FImpl
{
	struct FObject
	{
		TObjectPtr<AActor> Actor = nullptr;
		TObjectPtr<UStaticMeshComponent> Component = nullptr;
		TObjectPtr<UStaticMesh> Source = nullptr;
		TObjectPtr<UPreviewMesh> Preview = nullptr;
		FTransform Transform;
		FTransform ComponentTransform;
		bool bWasHidden = false;
		FMeshDescription Mesh;
		FPositions Original;
		UE::Geometry::FDynamicMesh3 Dynamic;
		TArray<FVertexID> DynamicToSourceVertex;
		TArray<FTriangleID> DynamicToSourceTriangle;
		TSet<int32> PerimeterEdges;
		TSet<int32> SelectedVertices;
		TSet<int32> SelectedEdges;
		TSet<int32> SelectedFaces;
		int32 TopologyRevision = 0;

		TSet<int32>& Selection(EBlenderMeshSelectionMode Mode)
		{
			return Mode == EBlenderMeshSelectionMode::Vertex ? SelectedVertices :
				Mode == EBlenderMeshSelectionMode::Edge ? SelectedEdges : SelectedFaces;
		}
		const TSet<int32>& Selection(EBlenderMeshSelectionMode Mode) const
		{
			return Mode == EBlenderMeshSelectionMode::Vertex ? SelectedVertices :
				Mode == EBlenderMeshSelectionMode::Edge ? SelectedEdges : SelectedFaces;
		}
		TArray<int32> Elements(EBlenderMeshSelectionMode Mode) const
		{
			TArray<int32> Result;
			if (Mode == EBlenderMeshSelectionMode::Vertex)
			{
				for (const FVertexID ID : Mesh.Vertices().GetElementIDs()) Result.Add(ID.GetValue());
			}
			else if (Mode == EBlenderMeshSelectionMode::Edge) Result = PerimeterEdges.Array();
			else for (const FPolygonID ID : Mesh.Polygons().GetElementIDs()) Result.Add(ID.GetValue());
			Result.Sort();
			return Result;
		}
		TArray<int32> ElementVertices(EBlenderMeshSelectionMode Mode, int32 ID) const
		{
			TArray<int32> Result;
			if (Mode == EBlenderMeshSelectionMode::Vertex)
			{
				if (Mesh.Vertices().IsValid(FVertexID(ID))) Result.Add(ID);
			}
			else if (Mode == EBlenderMeshSelectionMode::Edge)
			{
				if (PerimeterEdges.Contains(ID))
					for (const FVertexID Vertex : Mesh.GetEdgeVertices(FEdgeID(ID))) Result.Add(Vertex.GetValue());
			}
			else if (Mesh.Polygons().IsValid(FPolygonID(ID)))
			{
				for (const FVertexID Vertex : Mesh.GetPolygonVertices(FPolygonID(ID))) Result.Add(Vertex.GetValue());
			}
			return Result;
		}
		TSet<int32> SelectedVertexUnion(EBlenderMeshSelectionMode Mode) const
		{
			TSet<int32> Result;
			for (int32 ID : Selection(Mode)) for (int32 Vertex : ElementVertices(Mode, ID)) Result.Add(Vertex);
			return Result;
		}
		FVector WorldVertex(int32 ID) const
		{
			return Transform.TransformPosition(FVector(Mesh.GetVertexPositions()[FVertexID(ID)]));
		}
		FVector LocalVertex(int32 ID) const { return FVector(Mesh.GetVertexPositions()[FVertexID(ID)]); }
		TArray<int32> VertexEdges(int32 ID) const
		{
			TArray<int32> Result;
			for (FEdgeID E : Mesh.GetVertexConnectedEdgeIDs(FVertexID(ID))) if (PerimeterEdges.Contains(E.GetValue())) Result.Add(E.GetValue());
			Result.Sort(); return Result;
		}
		TArray<int32> EdgeFaces(int32 ID) const
		{
			TArray<int32> Result;
			for (FPolygonID F : Mesh.GetEdgeConnectedPolygons(FEdgeID(ID))) Result.Add(F.GetValue());
			Result.Sort(); return Result;
		}
		TArray<int32> FaceEdges(int32 ID) const
		{
			TArray<int32> Result;
			for (FEdgeID E : Mesh.GetPolygonPerimeterEdges(FPolygonID(ID))) Result.Add(E.GetValue());
			return Result;
		}
		int32 OtherVertex(int32 Edge, int32 Vertex) const
		{
			const auto Ends = Mesh.GetEdgeVertices(FEdgeID(Edge));
			return Ends[0].GetValue() == Vertex ? Ends[1].GetValue() : Ends[0].GetValue();
		}
		FVector Center(EBlenderMeshSelectionMode SelectionMode, int32 ID, bool bWeighted = false) const
		{
			const auto Vertices = ElementVertices(SelectionMode, ID); FVector Sum=FVector::ZeroVector; double Total=0;
			for (int32 I=0; I<Vertices.Num(); ++I)
			{
				const FVector P=LocalVertex(Vertices[I]);
				const double Weight=bWeighted && Vertices.Num()>2 ? FVector::Distance(P,LocalVertex(Vertices[(I+1)%Vertices.Num()])) + FVector::Distance(P,LocalVertex(Vertices[(I+Vertices.Num()-1)%Vertices.Num()])) : 1.0;
				Sum+=P*Weight; Total+=Weight;
			}
			return Total>UE_SMALL_NUMBER ? Sum/Total : Sum;
		}
		TArray<int32> Neighbors(EBlenderMeshSelectionMode SelectionMode, int32 ID, bool bFaceStep) const
		{
			TSet<int32> Result;
			const auto Vertices = ElementVertices(SelectionMode,ID);
			if (SelectionMode == EBlenderMeshSelectionMode::Face && !bFaceStep)
			{
				for (int32 E:FaceEdges(ID)) for(int32 F:EdgeFaces(E)) Result.Add(F);
			}
			else if (SelectionMode == EBlenderMeshSelectionMode::Edge && !bFaceStep)
			{
				for(int32 V:Vertices) for(int32 E:VertexEdges(V)) Result.Add(E);
			}
			else if (SelectionMode == EBlenderMeshSelectionMode::Vertex && !bFaceStep)
			{
				for(int32 E:VertexEdges(ID)) Result.Add(OtherVertex(E,ID));
			}
			else
			{
				for(int32 V:Vertices)
				{
					for(FPolygonID F:Mesh.GetVertexConnectedPolygons(FVertexID(V)))
					{
						if(SelectionMode==EBlenderMeshSelectionMode::Face) Result.Add(F.GetValue());
						else if(SelectionMode==EBlenderMeshSelectionMode::Edge) for(int32 E:FaceEdges(F.GetValue())) Result.Add(E);
						else for(FVertexID FV:Mesh.GetPolygonVertices(F)) Result.Add(FV.GetValue());
					}
				}
			}
			Result.Remove(ID); TArray<int32> Sorted=Result.Array(); Sorted.Sort(); return Sorted;
		}
		static double TurnCost(const FVector& A,const FVector& B,const FVector& C,bool bSkipFirst=false,bool bSkipLast=false)
		{
			const FVector D1=B-A,D2=C-B;
			return ((bSkipFirst?0:D1.Size())+(bSkipLast?0:D2.Size())) * (2.0-0.5*FMath::Sqrt(FMath::Abs(FVector::DotProduct(D1.GetSafeNormal(),D2.GetSafeNormal()))));
		}
		double PathCost(EBlenderMeshSelectionMode SelectionMode,int32 A,int32 B,int32 SourceElement,int32 TargetElement) const
		{
			if(SelectionMode==EBlenderMeshSelectionMode::Vertex) return FVector::Distance(LocalVertex(A),LocalVertex(B));
			if(SelectionMode==EBlenderMeshSelectionMode::Edge)
			{
				const auto AV=ElementVertices(SelectionMode,A),BV=ElementVertices(SelectionMode,B);
				for(int32 V:AV) if(BV.Contains(V)) return TurnCost(LocalVertex(OtherVertex(A,V)),LocalVertex(V),LocalVertex(OtherVertex(B,V)));
			}
			const FVector CA=Center(SelectionMode,A,true),CB=Center(SelectionMode,B,true);
			const auto BE=FaceEdges(B);
			for(int32 E:FaceEdges(A)) if(BE.Contains(E))
			{
				const auto EV=ElementVertices(EBlenderMeshSelectionMode::Edge,E);
				const FVector P=LocalVertex(EV[0]), U=LocalVertex(EV[1])-P,V=CB-CA,W=P-CA;
				const double AA=U.SizeSquared(),BB=FVector::DotProduct(U,V),CC=V.SizeSquared();
				const double Den=AA*CC-BB*BB;
				const double T=FMath::Abs(Den)>1.e-12 ? FMath::Clamp((BB*FVector::DotProduct(V,W)-CC*FVector::DotProduct(U,W))/Den,0.0,1.0) : 0.5;
				return TurnCost(CA,P+U*T,CB,A==SourceElement,B==TargetElement);
			}
			return TNumericLimits<double>::Max();
		}
		void RefreshPreview()
		{
			if (!IsValid(Preview)) return;
			const auto Positions = Mesh.GetVertexPositions();
			for (int32 ID : Dynamic.VertexIndicesItr())
			{
				if (DynamicToSourceVertex.IsValidIndex(ID))
					Dynamic.SetVertex(ID, FVector3d(Positions[DynamicToSourceVertex[ID]]));
			}
			UE::Geometry::FMeshNormals::QuickRecomputeOverlayNormals(Dynamic);
			Preview->UpdatePreview(&Dynamic);
		}
		void RebuildPreview()
		{
			PerimeterEdges.Reset();
			for (FPolygonID P : Mesh.Polygons().GetElementIDs())
				for (FEdgeID E : Mesh.GetPolygonPerimeterEdges(P)) PerimeterEdges.Add(E.GetValue());
			FMeshDescriptionToDynamicMesh Converter;
			Converter.GroupMode = FMeshDescriptionToDynamicMesh::EPrimaryGroupMode::SetToPolygonID;
			Converter.bTransformVertexColorsLinearToSRGB = false;
			// Convert appends vertices and indexes a source-sized map; its output must be empty.
			// Replace only the geometry so the existing preview component retains its materials.
			UE::Geometry::FDynamicMesh3 Rebuilt;
			Converter.Convert(&Mesh, Rebuilt);
			Dynamic = MoveTemp(Rebuilt);
			DynamicToSourceVertex = MoveTemp(Converter.VertIDMap);
			DynamicToSourceTriangle = MoveTemp(Converter.TriIDMap);
			UE::Geometry::FMeshNormals::QuickRecomputeOverlayNormals(Dynamic);
			if (IsValid(Preview)) Preview->UpdatePreview(&Dynamic);
		}
		bool HasChanges() const { return TopologyRevision != 0 || !PositionsEqual(Original, ReadPositions(Mesh)); }
	};

	TObjectPtr<UWorld> World = nullptr;
	TArray<TUniquePtr<FObject>> Objects;
	TArray<TWeakObjectPtr<AActor>> OriginalSelection;
	EBlenderMeshSelectionMode Mode = EBlenderMeshSelectionMode::Vertex;
	bool bXRay = false;
	TObjectPtr<UMaterialInterface> PreviewMaterial = nullptr;
	int32 ActiveObject = INDEX_NONE;
	int32 ActiveElement = INDEX_NONE;
	EBlenderMeshSelectionMode ActiveMode = EBlenderMeshSelectionMode::Vertex;
	FSnapshot TransformStart;
	TArray<TSet<int32>> TransformVertices;
	bool bTransforming = false;
	struct FHistory { FSnapshot Before; FSnapshot After; };
	TArray<FHistory> History;
	int32 HistoryCursor = 0;
	enum class ETopologyOperation : uint8 { None, Extrude, LoopCut };
	ETopologyOperation TopologyOperation = ETopologyOperation::None;
	FSnapshot TopologyStart;
	TArray<BlenderMeshTopology::FExtrusion> Extrusions;
	TArray<FMeshDescription> ExtrudeBases;
	FVector ExtrudeDirection = FVector::UpVector;
	BlenderMeshTopology::FLoopPlan LoopPlan;
	int32 LoopObject = INDEX_NONE;
	struct FExternalGeometry
	{
		FGuid BuildGuid;
		bool bRenderData = false;
		UE::Geometry::FDynamicMesh3 Mesh;
		TUniquePtr<UE::Geometry::FDynamicMeshAABBTree3> Spatial;
	};
	struct FExternalOccluder
	{
		TWeakObjectPtr<UStaticMeshComponent> Component;
		FExternalGeometry* Geometry = nullptr;
		FTransform Transform;
		FBox Bounds;
	};
	TMap<TWeakObjectPtr<UStaticMesh>,TUniquePtr<FExternalGeometry>> ExternalGeometry;
	TArray<FExternalOccluder> ExternalOccluders;

	void UpdateExternalOccluders(const FSceneView* View)
	{
		ExternalOccluders.Reset();
		if(bXRay || !IsValid(World)) return;
		for(auto It=ExternalGeometry.CreateIterator();It;++It) if(!It.Key().IsValid()) It.RemoveCurrent();
		for(TActorIterator<AActor> It(World);It;++It)
		{
			AActor* Actor=*It;if(!IsValid(Actor) || Actor->IsHiddenEd())continue;
			bool bSessionActor=false;
			for(const auto& Object:Objects) if(Actor==Object->Actor || (Object->Preview && Actor==Object->Preview->GetActor())) {bSessionActor=true;break;}
			if(bSessionActor)continue;
			TInlineComponentArray<UStaticMeshComponent*> Components(Actor);
			for(UStaticMeshComponent* Component:Components)
			{
				if(!IsValid(Component) || !Component->IsRegistered() || !Component->IsVisibleInEditor() || Cast<UInstancedStaticMeshComponent>(Component))continue;
				if(View && !View->ViewFrustum.IntersectBox(Component->Bounds.Origin,Component->Bounds.BoxExtent))continue;
				UStaticMesh* SourceMesh=Component->GetStaticMesh();if(!IsValid(SourceMesh))continue;
				const TWeakObjectPtr<UStaticMesh> Key(SourceMesh);
				TUniquePtr<FExternalGeometry>& Geometry=ExternalGeometry.FindOrAdd(Key);
				if(!Geometry || Geometry->BuildGuid!=SourceMesh->GetLightingGuid())
				{
					Geometry=MakeUnique<FExternalGeometry>();Geometry->BuildGuid=SourceMesh->GetLightingGuid();
					if(const FMeshDescription* Description=SourceMesh->GetMeshDescription(0))
					{
						FMeshDescriptionToDynamicMesh Converter;Converter.Convert(Description,Geometry->Mesh);
					}
					else if(const FStaticMeshRenderData* Data=SourceMesh->GetRenderData();Data && Data->LODResources.Num()>0)
					{
						// Render-data positions already contain the source model's build scale.
						Geometry->bRenderData=true;const FStaticMeshLODResources& LOD=Data->LODResources[0];
						for(uint32 V=0;V<LOD.VertexBuffers.PositionVertexBuffer.GetNumVertices();++V)
							Geometry->Mesh.AppendVertex(FVector3d(LOD.VertexBuffers.PositionVertexBuffer.VertexPosition(V)));
						const FIndexArrayView Indices=LOD.IndexBuffer.GetArrayView();
						for(int32 T=0;T+2<Indices.Num();T+=3)Geometry->Mesh.AppendTriangle(Indices[T],Indices[T+1],Indices[T+2]);
					}
					if(Geometry->Mesh.TriangleCount()>0)Geometry->Spatial=MakeUnique<UE::Geometry::FDynamicMeshAABBTree3>(&Geometry->Mesh,true);
				}
				if(!Geometry->Spatial)continue;
				const FVector Scale=Component->GetComponentTransform().GetScale3D();
				const FVector BuildScale=Geometry->bRenderData || SourceMesh->GetNumSourceModels()==0 ? FVector::OneVector : SourceMesh->GetSourceModel(0).BuildSettings.BuildScale3D;
				if(FMath::Abs(Scale.X*BuildScale.X)<UE_SMALL_NUMBER || FMath::Abs(Scale.Y*BuildScale.Y)<UE_SMALL_NUMBER || FMath::Abs(Scale.Z*BuildScale.Z)<UE_SMALL_NUMBER)continue;
				FExternalOccluder Occluder;Occluder.Component=Component;Occluder.Geometry=Geometry.Get();
				Occluder.Transform=FTransform(FQuat::Identity,FVector::ZeroVector,BuildScale)*Component->GetComponentTransform();
				Occluder.Bounds=Component->Bounds.GetBox();ExternalOccluders.Add(MoveTemp(Occluder));
			}
		}
	}

	FSnapshot Snapshot() const
	{
		FSnapshot Result;
		for (const auto& Object : Objects)
		{
			FObjectSnapshot State; State.Mesh = Object->Mesh; State.TopologyRevision = Object->TopologyRevision;
			State.Vertices = Object->SelectedVertices; State.Edges = Object->SelectedEdges; State.Faces = Object->SelectedFaces;
			State.ActiveObject = ActiveObject; State.ActiveElement = ActiveElement; State.Mode = Mode; State.ActiveMode = ActiveMode;
			Result.Add(MoveTemp(State));
		}
		return Result;
	}
	void Restore(const FSnapshot& Snapshot)
	{
		for (int32 I = 0; I < Objects.Num() && I < Snapshot.Num(); ++I)
		{
			Objects[I]->Mesh = Snapshot[I].Mesh; Objects[I]->TopologyRevision = Snapshot[I].TopologyRevision;
			Objects[I]->SelectedVertices = Snapshot[I].Vertices; Objects[I]->SelectedEdges = Snapshot[I].Edges; Objects[I]->SelectedFaces = Snapshot[I].Faces;
			Objects[I]->RebuildPreview();
		}
		if (!Snapshot.IsEmpty()) { ActiveObject = Snapshot[0].ActiveObject; ActiveElement = Snapshot[0].ActiveElement; Mode = Snapshot[0].Mode; ActiveMode = Snapshot[0].ActiveMode; }
	}
	void ClearSelection()
	{
		ActiveObject=INDEX_NONE; ActiveElement=INDEX_NONE;
		for (auto& Object : Objects)
		{
			Object->SelectedVertices.Reset(); Object->SelectedEdges.Reset(); Object->SelectedFaces.Reset();
		}
	}
	void Cleanup()
	{
		for (auto& Object : Objects)
		{
			if (IsValid(Object->Actor)) Object->Actor->SetIsTemporarilyHiddenInEditor(Object->bWasHidden);
			if (IsValid(Object->Preview)) { Object->Preview->SetVisible(false); Object->Preview->Disconnect(); }
			Object->Preview = nullptr;
		}
		if (GEditor && World && GEditor->GetEditorWorldContext().World() == World)
		{
			GEditor->SelectNone(false, true, false);
			for (const TWeakObjectPtr<AActor>& Actor : OriginalSelection)
				if (Actor.IsValid()) GEditor->SelectActor(Actor.Get(), true, false, true);
			GEditor->NoteSelectionChange();
			GEditor->RedrawLevelEditingViewports();
		}
		Objects.Reset(); OriginalSelection.Reset(); World = nullptr;
		TransformStart.Reset(); TransformVertices.Reset(); bTransforming = false;
		History.Reset(); HistoryCursor = 0;
		TopologyOperation = ETopologyOperation::None; TopologyStart.Reset(); Extrusions.Reset(); ExtrudeBases.Reset(); LoopPlan = {}; LoopObject = INDEX_NONE;
		ActiveObject=INDEX_NONE; ActiveElement=INDEX_NONE; bXRay=false; PreviewMaterial=nullptr;
		ExternalOccluders.Reset();ExternalGeometry.Reset();
	}
	bool VisiblePoint(const FSceneView* View, const FVector& Point) const
	{
		if (bXRay) return true;
		FVector2D Pixel;
		if (!View->WorldToPixel(Point, Pixel)) return false;
		FVector Origin, Direction;
		View->DeprojectFVector2D(Pixel, Origin, Direction);
		const FVector Delta = Point - Origin;
		const double Distance = Delta.Size();
		if (Distance < UE_SMALL_NUMBER) return true;
		const FRay3d Ray(Origin, Delta / Distance);
		const double Tolerance = FMath::Clamp(Distance * 2.e-5, 0.05, 0.5);
		for (const auto& Object : Objects)
		{
			FHitResult Hit;
			// UPreviewMesh returns LOCAL ray distance, so compare transformed impact points.
			if (Object->Preview && Object->Preview->FindRayIntersection(Ray, Hit) &&
				FVector::Dist(Origin, Hit.ImpactPoint) < Distance - Tolerance) return false;
		}
		for(const FExternalOccluder& Occluder:ExternalOccluders)
		{
			if(!Occluder.Component.IsValid() || !FMath::LineBoxIntersection(Occluder.Bounds,Origin,Point,Delta))continue;
			const FVector LocalOrigin=Occluder.Transform.InverseTransformPosition(Origin);
			const FVector LocalDirection=Occluder.Transform.InverseTransformVector(Delta).GetSafeNormal();
			double LocalT;int32 Triangle;
			if(Occluder.Geometry->Spatial->FindNearestHitTriangle(FRay3d(LocalOrigin,LocalDirection),LocalT,Triangle))
			{
				const FVector WorldHit=Occluder.Transform.TransformPosition(LocalOrigin+LocalDirection*LocalT);
				if(FVector::Distance(Origin,WorldHit)<Distance-Tolerance)return false;
			}
		}
		if (IsValid(World))
		{
			FCollisionQueryParams Params(SCENE_QUERY_STAT(BlenderMeshPick), true);
			for (const auto& Object : Objects)
			{
				Params.AddIgnoredActor(Object->Actor);
				if (Object->Preview) Params.AddIgnoredActor(Object->Preview->GetActor());
			}
			// Accurate static geometry above supersedes simplified collision hulls.
			for(const FExternalOccluder& Occluder:ExternalOccluders) if(Occluder.Component.IsValid())Params.AddIgnoredComponent(Occluder.Component.Get());
			for (int32 Attempt = 0; Attempt < 64; ++Attempt)
			{
				FHitResult Hit;
				if (!World->LineTraceSingleByChannel(Hit, Origin, Point, ECC_Visibility, Params)) return true;
				if (AActor* HitActor = Hit.GetActor(); HitActor && HitActor->IsHiddenEd())
				{
					Params.AddIgnoredActor(HitActor);
					continue;
				}
				// HiddenInGame does not hide an editor component, so use editor visibility.
				if (UPrimitiveComponent* HitComponent = Hit.GetComponent(); HitComponent && !HitComponent->IsVisibleInEditor())
				{
					Params.AddIgnoredComponent(HitComponent);
					continue;
				}
				return Hit.Distance >= Distance - Tolerance;
			}
			return false;
		}
		return true;
	}
};

FBlenderMeshEditSession::FBlenderMeshEditSession() : Impl(MakeUnique<FImpl>()) {}
FBlenderMeshEditSession::~FBlenderMeshEditSession() { Cancel(); }

bool FBlenderMeshEditSession::Begin(UWorld* World, const TArray<AActor*>& Actors, FText& Error)
{
	Error = FText::GetEmpty();
	if (IsActive()) { Error = LOCTEXT("AlreadyEditing", "已有网格编辑会话，请先退出。"); return false; }
	if (!IsValid(World) || World->IsGameWorld() || Actors.IsEmpty())
	{
		Error = LOCTEXT("NoObjects", "请先在编辑关卡中选择静态网格物体。"); return false;
	}
	TArray<TUniquePtr<FImpl::FObject>> Pending;
	TSet<AActor*> Seen;
	for (AActor* Actor : Actors)
	{
		if (Seen.Contains(Actor)) continue;
		Seen.Add(Actor);
		AStaticMeshActor* StaticActor = Cast<AStaticMeshActor>(Actor);
		if (!IsValid(StaticActor) || Actor->GetWorld() != World || !Actor->IsEditable() || Actor->IsLockLocation() ||
			FLevelUtils::IsLevelLocked(Actor) || Actor->IsHiddenEd())
		{
			Error = LOCTEXT("UnsupportedActor", "所选物体包含非静态网格、已锁定、隐藏或不可编辑物体。请只选择可编辑的静态网格物体。"); return false;
		}
		TInlineComponentArray<UPrimitiveComponent*> Primitives(Actor);
		UStaticMeshComponent* Component = StaticActor->GetStaticMeshComponent();
		UStaticMesh* Mesh = Component ? Component->GetStaticMesh() : nullptr;
		if (!IsValid(Component) || !IsValid(Mesh) || Primitives.Num() != 1 || Primitives[0] != Component || Component->IsSimulatingPhysics())
		{
			Error = LOCTEXT("UnsupportedComponent", "暂不编辑包含多个几何组件、实例网格或正在模拟物理的物体。"); return false;
		}
		const FVector Scale = Component->GetComponentTransform().GetScale3D();
		if (FMath::Abs(Scale.X) < UE_KINDA_SMALL_NUMBER || FMath::Abs(Scale.Y) < UE_KINDA_SMALL_NUMBER || FMath::Abs(Scale.Z) < UE_KINDA_SMALL_NUMBER)
		{
			Error = LOCTEXT("ZeroScale", "物体缩放不能为零；请先修正缩放。"); return false;
		}
		FString MapFilename;
		if (FPackageName::DoesPackageExist(Actor->GetPackage()->GetName(), &MapFilename) && IFileManager::Get().IsReadOnly(*MapFilename))
		{
			Error = LOCTEXT("ReadOnlyMap", "物体所在关卡文件为只读，请先解除只读或签出文件。"); return false;
		}
		if (Mesh->GetNumSourceModels() != 1 || Mesh->IsHiResMeshDescriptionValid())
		{
			Error = LOCTEXT("UnsupportedLOD", "暂不编辑具有多个 LOD 或独立高精度源模型的网格，避免其他细节级别保留旧形状。"); return false;
		}
		for (const FStaticMeshComponentLODInfo& LOD : Component->LODData)
		{
			if (LOD.OverrideVertexColors || !LOD.PaintedVertices.IsEmpty())
			{
				Error = LOCTEXT("PaintedInstance", "所选物体有实例顶点涂色；本版暂不编辑这类物体，以保留涂色数据。"); return false;
			}
		}
		const FMeshDescription* Description = Mesh->GetMeshDescription(0);
		if (!Description || Description->Vertices().Num() == 0 || Description->Polygons().Num() == 0)
		{
			Error = LOCTEXT("NoSource", "网格没有可编辑的源顶点与面数据。"); return false;
		}
		auto Object = MakeUnique<FImpl::FObject>();
		Object->Actor = Actor; Object->Component = Component; Object->Source = Mesh;
		Object->ComponentTransform = Component->GetComponentTransform();
		const FVector BuildScale = Mesh->GetSourceModel(0).BuildSettings.BuildScale3D;
		if (FMath::Abs(BuildScale.X) < UE_KINDA_SMALL_NUMBER || FMath::Abs(BuildScale.Y) < UE_KINDA_SMALL_NUMBER || FMath::Abs(BuildScale.Z) < UE_KINDA_SMALL_NUMBER)
		{
			Error = LOCTEXT("ZeroBuildScale", "模型导入构建缩放不能为零。"); return false;
		}
		Object->Transform = FTransform(FQuat::Identity, FVector::ZeroVector, BuildScale) * Object->ComponentTransform;
		Object->bWasHidden = Actor->IsTemporarilyHiddenInEditor();
		Object->Mesh = *Description; Object->Original = ReadPositions(Object->Mesh);
		for (const FPolygonID ID : Object->Mesh.Polygons().GetElementIDs())
			for (const FEdgeID Edge : Object->Mesh.GetPolygonPerimeterEdges(ID)) Object->PerimeterEdges.Add(Edge.GetValue());
		FMeshDescriptionToDynamicMesh Converter;
		Converter.GroupMode = FMeshDescriptionToDynamicMesh::EPrimaryGroupMode::SetToPolygonID;
		Converter.bTransformVertexColorsLinearToSRGB = false;
		Converter.Convert(&Object->Mesh, Object->Dynamic);
		Object->DynamicToSourceVertex = MoveTemp(Converter.VertIDMap);
		Object->DynamicToSourceTriangle = MoveTemp(Converter.TriIDMap);
		if (Object->Dynamic.TriangleCount() == 0)
		{
			Error = LOCTEXT("NoTriangles", "网格转换失败，没有可显示的三角面。"); return false;
		}
		Pending.Add(MoveTemp(Object));
	}
	Impl->World = World;
	Impl->Objects = MoveTemp(Pending);
	Impl->Mode = EBlenderMeshSelectionMode::Vertex;
	if (GEditor)
	{
		for (FSelectionIterator It(*GEditor->GetSelectedActors()); It; ++It)
			if (AActor* Actor = Cast<AActor>(*It)) Impl->OriginalSelection.Add(Actor);
	}
	for (auto& Object : Impl->Objects)
	{
		Object->Preview = NewObject<UPreviewMesh>(GetTransientPackage());
		Object->Preview->bBuildSpatialDataStructure = true;
		Object->Preview->CreateInWorld(World, Object->Transform);
		Object->Preview->SetTangentsMode(EDynamicMeshComponentTangentsMode::AutoCalculated);
		Object->Preview->SetMaterials(Object->Component->GetMaterials());
		Object->Preview->SetShadowsEnabled(Object->Component->CastShadow);
		Object->Preview->UpdatePreview(&Object->Dynamic);
		Object->Preview->SetVisible(true);
		Object->Actor->SetIsTemporarilyHiddenInEditor(true);
	}
	// Entering edit mode must not force-select every element. Keep the gray mesh
	// and unselected topology until the user picks elements or explicitly presses A.
	SelectAll(false);
	if (GEditor) GEditor->RedrawLevelEditingViewports();
	return true;
}

bool FBlenderMeshEditSession::IsActive() const { return Impl->World && !Impl->Objects.IsEmpty(); }
UWorld* FBlenderMeshEditSession::GetWorld() const { return Impl->World.Get(); }
int32 FBlenderMeshEditSession::GetObjectCount() const { return Impl->Objects.Num(); }
void FBlenderMeshEditSession::Cancel() { if (IsActive()) Impl->Cleanup(); }
EBlenderMeshSelectionMode FBlenderMeshEditSession::GetSelectionMode() const { return Impl->Mode; }

void FBlenderMeshEditSession::SetSelectionMode(EBlenderMeshSelectionMode Mode)
{
	if (Mode == Impl->Mode || Impl->bTransforming) return;
	for (auto& Object : Impl->Objects)
	{
		const TSet<int32> Vertices = Object->SelectedVertexUnion(Impl->Mode);
		Object->SelectedVertices.Reset(); Object->SelectedEdges.Reset(); Object->SelectedFaces.Reset();
		for (int32 ID : Object->Elements(Mode))
		{
			const TArray<int32> Required = Object->ElementVertices(Mode, ID);
			bool bAllSelected = !Required.IsEmpty();
			for (int32 Vertex : Required) if (!Vertices.Contains(Vertex)) { bAllSelected = false; break; }
			if (bAllSelected) Object->Selection(Mode).Add(ID);
		}
	}
	Impl->Mode = Mode;
	if (Impl->Objects.IsValidIndex(Impl->ActiveObject))
	{
		const auto& Object=Impl->Objects[Impl->ActiveObject];
		const auto PreviousVertices=Object->ElementVertices(Impl->ActiveMode,Impl->ActiveElement);
		Impl->ActiveElement=INDEX_NONE;
		for(int32 ID:Object->Elements(Mode)) if(Object->Selection(Mode).Contains(ID))
		{
			const auto Candidates=Object->ElementVertices(Mode,ID); bool bRelated=false;
			for(int32 V:Candidates) if(PreviousVertices.Contains(V)) {bRelated=true;break;}
			if(bRelated) {Impl->ActiveElement=ID;break;}
		}
		Impl->ActiveMode=Mode;
	}
}

void FBlenderMeshEditSession::SelectAll(bool bSelect)
{
	if (Impl->bTransforming) return;
	Impl->ClearSelection();
	if (bSelect) for (auto& Object : Impl->Objects) for (int32 ID : Object->Elements(Impl->Mode)) Object->Selection(Impl->Mode).Add(ID);
}

bool FBlenderMeshEditSession::SelectElement(int32 ObjectIndex, EBlenderMeshSelectionMode Mode, int32 ElementID, bool bAdd)
{
	if (Impl->bTransforming || !Impl->Objects.IsValidIndex(ObjectIndex) || Impl->Objects[ObjectIndex]->ElementVertices(Mode, ElementID).IsEmpty()) return false;
	SetSelectionMode(Mode);
	if (!bAdd) Impl->ClearSelection();
	Impl->Objects[ObjectIndex]->Selection(Mode).Add(ElementID);
	Impl->ActiveObject=ObjectIndex; Impl->ActiveMode=Mode; Impl->ActiveElement=ElementID;
	return true;
}

void FBlenderMeshEditSession::SetXRay(bool bEnabled) { Impl->bXRay=bEnabled; }
bool FBlenderMeshEditSession::IsXRay() const { return Impl->bXRay; }
void FBlenderMeshEditSession::SetPreviewMaterial(UMaterialInterface* Material)
{
	Impl->PreviewMaterial=Material;
	for(auto& Object:Impl->Objects) if(Object->Preview) Object->Preview->SetOverrideRenderMaterial(Material);
}
bool FBlenderMeshEditSession::GetActiveElement(int32& Object,EBlenderMeshSelectionMode& Mode,int32& Element) const
{
	Object=Impl->ActiveObject; Mode=Impl->ActiveMode; Element=Impl->ActiveElement;
	return Impl->Objects.IsValidIndex(Object) && Impl->Objects[Object]->Selection(Mode).Contains(Element);
}
FBox FBlenderMeshEditSession::GetGeometryBounds(bool bSelectedOnly) const
{
	FBox Result(ForceInit);
	for(const auto& Object:Impl->Objects)
	{
		const auto Vertices=bSelectedOnly ? Object->SelectedVertexUnion(Impl->Mode).Array() : Object->Elements(EBlenderMeshSelectionMode::Vertex);
		for(int32 V:Vertices) Result+=Object->WorldVertex(V);
	}
	return Result;
}

bool FBlenderMeshEditSession::SelectLoop(int32 ObjectIndex,int32 EdgeID,bool bRing,bool bExtend,int32 ActiveSeedID)
{
	if(Impl->bTransforming || !Impl->Objects.IsValidIndex(ObjectIndex)) return false;
	auto& Object=Impl->Objects[ObjectIndex]; if(!Object->PerimeterEdges.Contains(EdgeID)) return false;
	TSet<int32> Edges{EdgeID}, Faces;
	if(bRing || Impl->Mode==EBlenderMeshSelectionMode::Face)
	{
		TArray<int32> Queue{EdgeID};
		for(int32 Head=0;Head<Queue.Num();++Head)
		{
			const int32 Current=Queue[Head];
			// Nonmanifold branches have no unique continuation.
			const auto Adjacent=Object->EdgeFaces(Current); if(Adjacent.Num()>2) continue;
			for(int32 Face:Adjacent)
			{
				const auto Boundary=Object->FaceEdges(Face); if(Boundary.Num()!=4) continue;
				Faces.Add(Face); const int32 Index=Boundary.Find(Current); if(Index==INDEX_NONE) continue;
				const int32 Opposite=Boundary[(Index+2)%4];
				if(!Edges.Contains(Opposite)) {Edges.Add(Opposite);Queue.Add(Opposite);}
			}
		}
	}
	else
	{
		const auto Ends=Object->ElementVertices(EBlenderMeshSelectionMode::Edge,EdgeID);
		for(int32 StartVertex:Ends)
		{
			int32 Current=EdgeID, Vertex=StartVertex;
			for(int32 Guard=0;Guard<Object->PerimeterEdges.Num();++Guard)
			{
				const auto Incident=Object->VertexEdges(Vertex), CurrentFaces=Object->EdgeFaces(Current);
				int32 Next=INDEX_NONE;
				if(CurrentFaces.Num()==1)
				{
					// Continue a boundary through a regular fan, but stop at a corner.
					// A single isolated ngon is its own boundary loop.
					bool bIsolatedFace=Incident.Num()==2;
					if(bIsolatedFace) for(int32 Boundary:Object->FaceEdges(CurrentFaces[0])) if(Object->EdgeFaces(Boundary).Num()!=1) {bIsolatedFace=false;break;}
					if(Incident.Num()!=3 && !bIsolatedFace) break;
					for(int32 Candidate:Incident) if(Candidate!=Current && Object->EdgeFaces(Candidate).Num()==1)
					{ if(Next!=INDEX_NONE) {Next=INDEX_NONE;break;} Next=Candidate; }
				}
				else if(CurrentFaces.Num()==2 && (Incident.Num()==4 || Incident.Num()==2))
				{
					for(int32 Candidate:Incident) if(Candidate!=Current && Object->EdgeFaces(Candidate).Num()==2)
					{
						bool bSharesFace=false; for(int32 F:Object->EdgeFaces(Candidate)) if(CurrentFaces.Contains(F)) bSharesFace=true;
						if(!bSharesFace || Incident.Num()==2) {if(Next!=INDEX_NONE) {Next=INDEX_NONE;break;} Next=Candidate;}
					}
				}
				if(Next==INDEX_NONE || Edges.Contains(Next)) break;
				Edges.Add(Next); Vertex=Object->OtherVertex(Next,Vertex); Current=Next;
			}
		}
	}
	TSet<int32> Elements;
	if(Impl->Mode==EBlenderMeshSelectionMode::Face) Elements=MoveTemp(Faces);
	else if(Impl->Mode==EBlenderMeshSelectionMode::Edge) Elements=MoveTemp(Edges);
	else for(int32 E:Edges) for(int32 V:Object->ElementVertices(EBlenderMeshSelectionMode::Edge,E)) Elements.Add(V);
	if(Elements.IsEmpty()) return false;
	auto& Selection=Object->Selection(Impl->Mode);
	bool bRemove=bExtend;
	// Blender toggles according to the clicked seed, preserving unrelated selection.
	TArray<int32> OrderedElements=Elements.Array();OrderedElements.Sort();
	const int32 Seed=Elements.Contains(ActiveSeedID) ? ActiveSeedID : Impl->Mode==EBlenderMeshSelectionMode::Edge ? EdgeID : OrderedElements[0];
	bRemove=bRemove && Selection.Contains(Seed);
	if(!bExtend) Impl->ClearSelection();
	for(int32 ID:Elements) {if(bRemove) Selection.Remove(ID);else Selection.Add(ID);}
	if(!bRemove) {Impl->ActiveObject=ObjectIndex;Impl->ActiveMode=Impl->Mode;Impl->ActiveElement=Seed;}
	else if(Impl->ActiveObject==ObjectIndex && Elements.Contains(Impl->ActiveElement)) Impl->ActiveElement=INDEX_NONE;
	return true;
}

bool FBlenderMeshEditSession::SelectShortestPath(int32 ObjectIndex,EBlenderMeshSelectionMode Mode,int32 TargetID,bool bFill)
{
	if(Impl->bTransforming || !Impl->Objects.IsValidIndex(ObjectIndex)) return false;
	auto& Object=Impl->Objects[ObjectIndex]; if(Object->ElementVertices(Mode,TargetID).IsEmpty()) return false;
	SetSelectionMode(Mode);
	const int32 Source=Impl->ActiveObject==ObjectIndex && Impl->ActiveMode==Mode && Object->Selection(Mode).Contains(Impl->ActiveElement) ? Impl->ActiveElement : INDEX_NONE;
	TSet<int32> Path;
	if(Source!=INDEX_NONE && !bFill)
	{
		using FEntry=std::pair<double,int32>;
		std::priority_queue<FEntry,std::vector<FEntry>,std::greater<FEntry>> Queue;
		TMap<int32,double> Distance; TMap<int32,int32> Previous; Distance.Add(Source,0); Queue.push({0,Source});
		while(!Queue.empty())
		{
			const auto Item=Queue.top(); Queue.pop(); if(Item.first>Distance.FindChecked(Item.second)) continue;
			if(Item.second==TargetID) break;
			for(int32 Next:Object->Neighbors(Mode,Item.second,false))
			{
				const double Cost=Item.first+Object->PathCost(Mode,Item.second,Next,Source,TargetID);
				const double* Old=Distance.Find(Next);
				if(!Old || Cost<*Old) {Distance.Add(Next,Cost);Previous.Add(Next,Item.second);Queue.push({Cost,Next});}
			}
		}
		if(Distance.Contains(TargetID))
		{
			int32 Cursor=TargetID;Path.Add(Cursor);
			while(Cursor!=Source) {const int32* Prev=Previous.Find(Cursor);if(!Prev) break;Cursor=*Prev;Path.Add(Cursor);}
		}
	}
	else if(Source!=INDEX_NONE)
	{
		// Fill Region uses topological distances from endpoint vertex sets, independently
		// of weighted shortest-path costs. Degree-two manifold chains count as one step.
		const auto Starts=Object->ElementVertices(Mode,Source),Targets=Object->ElementVertices(Mode,TargetID);
		auto IsChain=[&](int32 V) {const auto VE=Object->VertexEdges(V);return VE.Num()==2 && Object->EdgeFaces(VE[0]).Num()==2 && Object->EdgeFaces(VE[1]).Num()==2;};
		auto ChainEnds=[&](int32 Pivot,TArray<int32>& Ends)
		{
			if(!IsChain(Pivot)) return false;
			for(int32 E:Object->VertexEdges(Pivot))
			{
				int32 At=Object->OtherVertex(E,Pivot), Last=E;
				while(IsChain(At) && At!=Pivot)
				{const auto VE=Object->VertexEdges(At);Last=VE[0]==Last?VE[1]:VE[0];At=Object->OtherVertex(Last,At);}
				if(At==Pivot) return false; Ends.Add(At);
			}
			return Ends.Num()==2;
		};
		auto Depths=[&](const TArray<int32>& Seeds,const TArray<int32>& Dest,TMap<int32,int32>& Depth)->int32
		{
			TArray<int32> Frontier;
			for(int32 V:Seeds) {Depth.Add(V,0);Frontier.AddUnique(V);}
			if(Mode!=EBlenderMeshSelectionMode::Face) for(int32 V:Seeds)
			{TArray<int32> Ends;if(ChainEnds(V,Ends)) for(int32 End:Ends) if(!Depth.Contains(End)){Depth.Add(End,0);Frontier.Add(End);}}
			for(int32 Pass=1;!Frontier.IsEmpty();++Pass)
			{
				TArray<int32> Next;
				for(int32 V:Frontier) for(int32 E:Object->VertexEdges(V))
				{
					int32 At=Object->OtherVertex(E,V),Last=E;
					while(IsChain(At) && !Depth.Contains(At))
					{Depth.Add(At,Pass);const auto VE=Object->VertexEdges(At);Last=VE[0]==Last?VE[1]:VE[0];At=Object->OtherVertex(Last,At);}
					if(!Depth.Contains(At)) {Depth.Add(At,Pass);Next.Add(At);}
				}
				bool bReached=true;for(int32 V:Dest) if(!Depth.Contains(V)) bReached=false;
				if(bReached) return Pass+1;
				Frontier=MoveTemp(Next);
			}
			return INDEX_NONE;
		};
		TMap<int32,int32> DS,DT;
		const int32 FirstPass=Depths(Starts,Targets,DS),Pass=Depths(Targets,Starts,DT);
		if(FirstPass!=INDEX_NONE && Pass!=INDEX_NONE)
		{
			auto InRegion=[&](int32 V){const int32* A=DS.Find(V);const int32* B=DT.Find(V);return A&&B&&*A+*B<Pass;};
			TSet<int32> Region;
			for(int32 V:Object->Elements(EBlenderMeshSelectionMode::Vertex))
			{TArray<int32> Ends;if(InRegion(V)||(ChainEnds(V,Ends)&&InRegion(Ends[0])&&InRegion(Ends[1])))Region.Add(V);}
			for(int32 ID:Object->Elements(Mode))
			{
				const auto Vertices=Object->ElementVertices(Mode,ID);int32 Misses=0;for(int32 V:Vertices) if(!Region.Contains(V)) ++Misses;
				if(Misses<=(Mode==EBlenderMeshSelectionMode::Face && Vertices.Num()>3 ? 1:0)) Path.Add(ID);
			}
		}
	}
	if(Path.IsEmpty()) Path.Add(TargetID);
	auto& Selection=Object->Selection(Mode); bool bAllSelected=true;
	for(int32 ID:Path) if(!Selection.Contains(ID)) bAllSelected=false;
	for(int32 ID:Path) {if(bAllSelected) Selection.Remove(ID); else Selection.Add(ID);}
	if(!bAllSelected) {Impl->ActiveObject=ObjectIndex;Impl->ActiveMode=Mode;Impl->ActiveElement=TargetID;}
	else if(!Impl->Objects.IsValidIndex(Impl->ActiveObject) || !Impl->Objects[Impl->ActiveObject]->Selection(Impl->ActiveMode).Contains(Impl->ActiveElement))
		{Impl->ActiveObject=INDEX_NONE;Impl->ActiveElement=INDEX_NONE;}
	return true;
}

bool FBlenderMeshEditSession::GrowSelection(bool bGrow)
{
	if(!IsActive() || Impl->bTransforming) return false;
	for(auto& Object:Impl->Objects)
	{
		const TSet<int32> Before=Object->Selection(Impl->Mode); auto& Selection=Object->Selection(Impl->Mode);
		for(int32 ID:Before)
		{
			for(int32 Neighbor:Object->Neighbors(Impl->Mode,ID,true))
			{
				if(bGrow) Selection.Add(Neighbor);
				else if(!Before.Contains(Neighbor)) {Selection.Remove(ID);break;}
			}
		}
	}
	return true;
}

bool FBlenderMeshEditSession::SelectLinked(bool bSelect)
{
	if(!IsActive() || Impl->bTransforming) return false;
	for(auto& Object:Impl->Objects)
	{
		TSet<int32> Region=Object->Selection(Impl->Mode); TArray<int32> Queue=Region.Array();
		for(int32 Head=0;Head<Queue.Num();++Head) for(int32 N:Object->Neighbors(Impl->Mode,Queue[Head],true))
			if(!Region.Contains(N)) {Region.Add(N);Queue.Add(N);}
		for(int32 ID:Region) {if(bSelect) Object->Selection(Impl->Mode).Add(ID);else Object->Selection(Impl->Mode).Remove(ID);}
	}
	if(!bSelect) {Impl->ActiveObject=INDEX_NONE;Impl->ActiveElement=INDEX_NONE;}
	return true;
}

TArray<int32> FBlenderMeshEditSession::GetElementIDs(int32 ObjectIndex, EBlenderMeshSelectionMode Mode) const
{
	return Impl->Objects.IsValidIndex(ObjectIndex) ? Impl->Objects[ObjectIndex]->Elements(Mode) : TArray<int32>();
}

TArray<int32> FBlenderMeshEditSession::GetSelectedElementIDs(int32 ObjectIndex, EBlenderMeshSelectionMode Mode) const
{
	TArray<int32> Result;
	if (Impl->Objects.IsValidIndex(ObjectIndex)) Result = Impl->Objects[ObjectIndex]->Selection(Mode).Array();
	Result.Sort(); return Result;
}

TArray<FBlenderMeshVertexRef> FBlenderMeshEditSession::GetSelectedVertices() const
{
	TArray<FBlenderMeshVertexRef> Result;
	for (int32 I = 0; I < Impl->Objects.Num(); ++I)
	{
		const auto& Object = Impl->Objects[I];
		TArray<int32> Vertices = Object->SelectedVertexUnion(Impl->Mode).Array(); Vertices.Sort();
		for (int32 Vertex : Vertices) Result.Add({I, Vertex, Object->WorldVertex(Vertex)});
	}
	return Result;
}

FVector FBlenderMeshEditSession::GetSelectionCenter() const
{
	const TArray<FBlenderMeshVertexRef> Vertices = GetSelectedVertices();
	FVector Center = FVector::ZeroVector;
	for (const auto& Vertex : Vertices) Center += Vertex.WorldPosition;
	return Vertices.IsEmpty() ? Center : Center / Vertices.Num();
}

FQuat FBlenderMeshEditSession::GetReferenceRotation() const
{
	// Use the first selected object's orientation, consistent with object mode.
	return Impl->Objects.IsEmpty() ? FQuat::Identity : Impl->Objects[0]->ComponentTransform.GetRotation();
}

void FBlenderMeshEditSession::BeginTransform()
{
	if (!IsActive() || Impl->bTransforming || IsTopologyEditing()) return;
	Impl->TransformStart = Impl->Snapshot();
	Impl->TransformVertices.Reset();
	for (const auto& Object : Impl->Objects) Impl->TransformVertices.Add(Object->SelectedVertexUnion(Impl->Mode));
	Impl->bTransforming = true;
}

void FBlenderMeshEditSession::PreviewTransform(TFunctionRef<FVector(const FVector&)> WorldPositionMap)
{
	if (!Impl->bTransforming) return;
	for (int32 I = 0; I < Impl->Objects.Num(); ++I)
	{
		auto& Object = Impl->Objects[I];
		auto Positions = Object->Mesh.GetVertexPositions();
		for (int32 Vertex : Impl->TransformVertices[I])
		{
			const FVector WorldStart = Object->Transform.TransformPosition(FVector(Impl->TransformStart[I].Mesh.GetVertexPositions()[FVertexID(Vertex)]));
			const FVector WorldEnd = WorldPositionMap(WorldStart);
			if (!WorldEnd.ContainsNaN()) Positions[FVertexID(Vertex)] = FVector3f(Object->Transform.InverseTransformPosition(WorldEnd));
		}
		if (!Impl->TransformVertices[I].IsEmpty()) Object->RefreshPreview();
	}
}

void FBlenderMeshEditSession::ConfirmTransform()
{
	if (!Impl->bTransforming) return;
	FSnapshot After = Impl->Snapshot();
	if (!SnapshotsEqual(Impl->TransformStart, After))
	{
		Impl->History.SetNum(Impl->HistoryCursor);
		Impl->History.Add({MoveTemp(Impl->TransformStart), MoveTemp(After)});
		if (Impl->History.Num() > 64) Impl->History.RemoveAt(0);
		Impl->HistoryCursor = Impl->History.Num();
	}
	Impl->TransformStart.Reset(); Impl->TransformVertices.Reset(); Impl->bTransforming = false;
}

void FBlenderMeshEditSession::CancelTransform()
{
	if (!Impl->bTransforming) return;
	Impl->Restore(Impl->TransformStart);
	Impl->TransformStart.Reset(); Impl->TransformVertices.Reset(); Impl->bTransforming = false;
}

bool FBlenderMeshEditSession::Undo()
{
	if (Impl->bTransforming || IsTopologyEditing() || Impl->HistoryCursor <= 0) return false;
	Impl->Restore(Impl->History[--Impl->HistoryCursor].Before); return true;
}

bool FBlenderMeshEditSession::Redo()
{
	if (Impl->bTransforming || IsTopologyEditing() || Impl->HistoryCursor >= Impl->History.Num()) return false;
	Impl->Restore(Impl->History[Impl->HistoryCursor++].After); return true;
}

bool FBlenderMeshEditSession::Pick(FEditorViewportClient* ViewportClient, FViewport* Viewport, bool bToggle)
{
	return Pick(ViewportClient,Viewport,EBlenderPickAction::Normal,bToggle);
}

bool FBlenderMeshEditSession::Pick(FEditorViewportClient* ViewportClient, FViewport* Viewport,
	EBlenderPickAction Action,bool bExtend,bool bSubtract,bool bFill)
{
	if (!IsActive() || Impl->bTransforming || !ViewportClient || !Viewport) return false;
	FSceneViewFamilyContext Family(FSceneViewFamily::ConstructionValues(Viewport, ViewportClient->GetScene(), ViewportClient->EngineShowFlags).SetRealtimeUpdate(ViewportClient->IsRealtime()));
	FSceneView* View = ViewportClient->CalcSceneView(&Family);
	if (!View) return false;
	Impl->UpdateExternalOccluders(View);
	const FVector2D Mouse(Viewport->GetMouseX(), Viewport->GetMouseY());
	const EBlenderMeshSelectionMode PickMode=(Action==EBlenderPickAction::Loop || Action==EBlenderPickAction::Ring) ? EBlenderMeshSelectionMode::Edge : Impl->Mode;
	int32 BestObject = INDEX_NONE, BestElement = INDEX_NONE;
	double BestScore = PickMode == EBlenderMeshSelectionMode::Face ? (Impl->bXRay ? 12.0 : TNumericLimits<double>::Max()) : 12.0 * 12.0;
	if(PickMode==EBlenderMeshSelectionMode::Face && Impl->bXRay)
	{
		// Blender's X-ray face picker compares projected face centers, bypassing surface depth.
		for(int32 I=0;I<Impl->Objects.Num();++I)
		{
			const auto& Object=Impl->Objects[I];
			for(int32 ID:Object->Elements(PickMode))
			{
				const FVector P=Object->Transform.TransformPosition(Object->Center(PickMode,ID));FVector2D Pixel;
				if((View->IsPerspectiveProjection() && View->WorldToScreen(P).W<=0) || !View->WorldToPixel(P,Pixel)) continue;
				const double Score=FMath::Abs(Mouse.X-Pixel.X)+FMath::Abs(Mouse.Y-Pixel.Y);
				if(Score<BestScore) {BestScore=Score;BestObject=I;BestElement=ID;}
			}
		}
	}
	else if (PickMode == EBlenderMeshSelectionMode::Face)
	{
		FVector RayOrigin, RayDirection;
		View->DeprojectFVector2D(Mouse, RayOrigin, RayDirection);
		const FRay3d Ray(RayOrigin, RayDirection);
		for (int32 I = 0; I < Impl->Objects.Num(); ++I)
		{
			const auto& Object = Impl->Objects[I]; FHitResult Hit;
			if (!Object->Preview || !Object->Preview->FindRayIntersection(Ray, Hit) || !Object->DynamicToSourceTriangle.IsValidIndex(Hit.FaceIndex)) continue;
			const double Distance = FVector::DistSquared(RayOrigin, Hit.ImpactPoint);
			if (Distance < BestScore && Impl->VisiblePoint(View, Hit.ImpactPoint))
			{
				BestScore = Distance; BestObject = I;
				BestElement = Object->Mesh.GetTrianglePolygon(Object->DynamicToSourceTriangle[Hit.FaceIndex]).GetValue();
			}
		}
	}
	else for (int32 I = 0; I < Impl->Objects.Num(); ++I)
	{
		const auto& Object = Impl->Objects[I];
		for (int32 ID : Object->Elements(PickMode))
		{
			const auto Vertices = Object->ElementVertices(PickMode, ID);
			const FVector A = Object->WorldVertex(Vertices[0]); FVector Candidate = A;
			FVector2D ScreenA;
			if (!View->WorldToPixel(A, ScreenA) || (View->IsPerspectiveProjection() && View->WorldToScreen(A).W <= 0)) continue;
			double Score;
			if (PickMode == EBlenderMeshSelectionMode::Edge)
			{
				const FVector B = Object->WorldVertex(Vertices[1]); FVector2D ScreenB;
				if (!View->WorldToPixel(B, ScreenB) || (View->IsPerspectiveProjection() && View->WorldToScreen(B).W <= 0)) continue;
				const FVector2D Segment = ScreenB - ScreenA;
				const double T = FMath::Clamp(FVector2D::DotProduct(Mouse - ScreenA, Segment) / FMath::Max(Segment.SizeSquared(), 1.e-10), 0.0, 1.0);
				Score = FVector2D::DistSquared(Mouse, ScreenA + Segment * T);
				const double WA = View->WorldToScreen(A).W, WB = View->WorldToScreen(B).W;
				const double WorldT = (T / WB) / ((1.0 - T) / WA + T / WB);
				Candidate = FMath::Lerp(A, B, WorldT);
			}
			else Score = FVector2D::DistSquared(Mouse, ScreenA);
			if (Score < BestScore && Impl->VisiblePoint(View, Candidate))
			{
				BestScore = Score; BestObject = I; BestElement = ID;
			}
		}
	}
	if (BestObject != INDEX_NONE)
	{
		auto& Object=Impl->Objects[BestObject];
		auto& Selection = Object->Selection(Impl->Mode);
		if(Action==EBlenderPickAction::Loop || Action==EBlenderPickAction::Ring)
		{
			int32 ActiveSeed=BestElement;
			if(Impl->Mode!=EBlenderMeshSelectionMode::Edge)
			{
				ActiveSeed=INDEX_NONE;double Nearest=TNumericLimits<double>::Max();
				const auto Candidates=Impl->Mode==EBlenderMeshSelectionMode::Vertex ? Object->ElementVertices(PickMode,BestElement) : Object->EdgeFaces(BestElement);
				for(int32 ID:Candidates)
				{FVector2D P;if(View->WorldToPixel(Object->Transform.TransformPosition(Object->Center(Impl->Mode,ID)),P) && FVector2D::DistSquared(Mouse,P)<Nearest){Nearest=FVector2D::DistSquared(Mouse,P);ActiveSeed=ID;}}
			}
			SelectLoop(BestObject,BestElement,Action==EBlenderPickAction::Ring,bExtend,ActiveSeed);
			if(Impl->Mode!=EBlenderMeshSelectionMode::Edge)
			{
				const auto Candidates=Impl->Mode==EBlenderMeshSelectionMode::Vertex ? Object->ElementVertices(PickMode,BestElement) : Object->EdgeFaces(BestElement);
				double Nearest=TNumericLimits<double>::Max();
				for(int32 ID:Candidates) if(Selection.Contains(ID))
				{FVector2D P;if(View->WorldToPixel(Object->Transform.TransformPosition(Object->Center(Impl->Mode,ID)),P) && FVector2D::DistSquared(Mouse,P)<Nearest){Nearest=FVector2D::DistSquared(Mouse,P);Impl->ActiveObject=BestObject;Impl->ActiveMode=Impl->Mode;Impl->ActiveElement=ID;}}
			}
		}
		else if(Action==EBlenderPickAction::ShortestPath) SelectShortestPath(BestObject,Impl->Mode,BestElement,bFill);
		else if(Action==EBlenderPickAction::Linked)
		{
			TSet<int32> Region{BestElement};TArray<int32> Queue{BestElement};
			for(int32 Head=0;Head<Queue.Num();++Head) for(int32 N:Object->Neighbors(Impl->Mode,Queue[Head],true))
				if(!Region.Contains(N)) {Region.Add(N);Queue.Add(N);}
			for(int32 ID:Region) {if(bSubtract) Selection.Remove(ID);else Selection.Add(ID);}
			if(!bSubtract){Impl->ActiveObject=BestObject;Impl->ActiveMode=Impl->Mode;Impl->ActiveElement=BestElement;}
		}
		else
		{
			const bool bRemove=bSubtract || (bExtend && Selection.Contains(BestElement));
			if(!bExtend && !bSubtract) Impl->ClearSelection();
			if(bRemove) {Selection.Remove(BestElement);if(Impl->ActiveObject==BestObject && Impl->ActiveElement==BestElement) Impl->ActiveElement=INDEX_NONE;}
			else {Selection.Add(BestElement);Impl->ActiveObject=BestObject;Impl->ActiveMode=Impl->Mode;Impl->ActiveElement=BestElement;}
		}
	}
	else if(Action==EBlenderPickAction::Normal && !bExtend && !bSubtract) Impl->ClearSelection();
	ViewportClient->Invalidate();
	return true;
}

bool FBlenderMeshEditSession::BoxSelect(FEditorViewportClient* Client,FViewport* Viewport,const FIntRect& InputRect,EBlenderSelectionOperation Operation)
{
	if(!IsActive() || Impl->bTransforming || !Client || !Viewport) return false;
	FSceneViewFamilyContext Family(FSceneViewFamily::ConstructionValues(Viewport,Client->GetScene(),Client->EngineShowFlags).SetRealtimeUpdate(Client->IsRealtime()));
	FSceneView* View=Client->CalcSceneView(&Family);if(!View) return false;
	Impl->UpdateExternalOccluders(View);
	const FIntRect Rect(FMath::Min(InputRect.Min.X,InputRect.Max.X),FMath::Min(InputRect.Min.Y,InputRect.Max.Y),FMath::Max(InputRect.Min.X,InputRect.Max.X),FMath::Max(InputRect.Min.Y,InputRect.Max.Y));
	if(Rect.Width()<1 || Rect.Height()<1) return false;
	auto Contains=[&](const FVector2D& P){return P.X>=Rect.Min.X && P.X<=Rect.Max.X && P.Y>=Rect.Min.Y && P.Y<=Rect.Max.Y;};
	auto Project=[&](const FVector& P,FVector2D& Pixel){return (!View->IsPerspectiveProjection() || View->WorldToScreen(P).W>0) && View->WorldToPixel(P,Pixel);};
	auto ClipSegment=[&](const FVector2D& A,const FVector2D& B,double& Enter,double& Exit)
	{
		Enter=0;Exit=1;const FVector2D D=B-A;
		const double P[4]={-D.X,D.X,-D.Y,D.Y},Q[4]={A.X-Rect.Min.X,Rect.Max.X-A.X,A.Y-Rect.Min.Y,Rect.Max.Y-A.Y};
		for(int32 I=0;I<4;++I)
		{
			if(FMath::Abs(P[I])<1.e-12){if(Q[I]<0)return false;continue;}
			const double T=Q[I]/P[I];if(P[I]<0)Enter=FMath::Max(Enter,T);else Exit=FMath::Min(Exit,T);
			if(Enter>Exit)return false;
		}
		return true;
	};
	auto ClipPolygon=[&](TArray<FVector2D> Points)
	{
		for(int32 Side=0;Side<4 && !Points.IsEmpty();++Side)
		{
			auto Distance=[&](const FVector2D& P){return Side==0 ? P.X-Rect.Min.X : Side==1 ? Rect.Max.X-P.X : Side==2 ? P.Y-Rect.Min.Y : Rect.Max.Y-P.Y;};
			TArray<FVector2D> Next;FVector2D A=Points.Last();double DA=Distance(A);
			for(const FVector2D& B:Points)
			{const double DB=Distance(B);if((DA>=0)!=(DB>=0))Next.Add(FMath::Lerp(A,B,DA/(DA-DB)));if(DB>=0)Next.Add(B);A=B;DA=DB;}
			Points=MoveTemp(Next);
		}
		return Points;
	};
	TArray<TSet<int32>> Hits,Partial;Hits.SetNum(Impl->Objects.Num());Partial.SetNum(Impl->Objects.Num());
	for(int32 I=0;I<Impl->Objects.Num();++I)
	{
		bool bAnyEnclosedEdge=false;
		const auto& Object=Impl->Objects[I];
		for(int32 ID:Object->Elements(Impl->Mode))
		{
			const bool bSelected=Object->Selection(Impl->Mode).Contains(ID);
			const bool bApplicable=Operation==EBlenderSelectionOperation::Subtract || Operation==EBlenderSelectionOperation::Intersect ? bSelected : Operation==EBlenderSelectionOperation::Replace || !bSelected;
			if(Impl->Mode==EBlenderMeshSelectionMode::Vertex || (Impl->Mode==EBlenderMeshSelectionMode::Face && Impl->bXRay))
			{
				const FVector P=Object->Transform.TransformPosition(Object->Center(Impl->Mode,ID));FVector2D Pixel;
				if(Project(P,Pixel) && Contains(Pixel) && Impl->VisiblePoint(View,P)) Hits[I].Add(ID);
			}
			else if(Impl->Mode==EBlenderMeshSelectionMode::Edge)
			{
				const auto V=Object->ElementVertices(Impl->Mode,ID);const FVector A=Object->WorldVertex(V[0]),B=Object->WorldVertex(V[1]);FVector2D PA,PB;
				if(!Project(A,PA)||!Project(B,PB))continue;
				double Enter,Exit;if(!ClipSegment(PA,PB,Enter,Exit))continue;
				bool bVisible=Impl->bXRay;
				// Test multiple positions inside the clipped fragment, not only the whole
				// edge midpoint, so a partially occluded edge can still be selected.
				for(int32 Sample=0;Sample<5 && !bVisible;++Sample)
				{
					const double T=FMath::Lerp(Enter,Exit,(Sample+0.5)/5.0);
					const double WA=View->WorldToScreen(A).W,WB=View->WorldToScreen(B).W;
					const double WorldT=(T/WB)/((1.0-T)/WA+T/WB);
					bVisible=Impl->VisiblePoint(View,FMath::Lerp(A,B,WorldT));
				}
				if(!bVisible)continue;
				if(Contains(PA)&&Contains(PB)) {Hits[I].Add(ID);if(bApplicable)bAnyEnclosedEdge=true;}
				else Partial[I].Add(ID);
			}
			else
			{
				// Solid mode includes visible face fragments. X-ray above uses face centers.
				// Clip each source triangle to the box and ray-test interior sample points.
				bool bHit=false;
				for(FTriangleID Triangle:Object->Mesh.GetPolygonTriangles(FPolygonID(ID)))
				{
					TArray<FVector2D> Points;bool bProjected=true;
					for(FVertexID V:Object->Mesh.GetTriangleVertices(Triangle)) {FVector2D P;if(!Project(Object->WorldVertex(V.GetValue()),P)){bProjected=false;break;}Points.Add(P);}
					if(!bProjected)continue;Points=ClipPolygon(MoveTemp(Points));if(Points.Num()<3)continue;
					FVector2D Center=FVector2D::ZeroVector;for(const FVector2D& P:Points)Center+=P;Center/=Points.Num();
					TArray<FVector2D> Samples{Center};for(const FVector2D& P:Points)Samples.Add(FMath::Lerp(P,Center,0.1));
					for(const FVector2D& P:Samples)
					{
						FVector Origin,Direction;View->DeprojectFVector2D(P,Origin,Direction);FHitResult Hit;
						if(Object->Preview && Object->Preview->FindRayIntersection(FRay3d(Origin,Direction),Hit) && Object->DynamicToSourceTriangle.IsValidIndex(Hit.FaceIndex) &&
							Object->Mesh.GetTrianglePolygon(Object->DynamicToSourceTriangle[Hit.FaceIndex]).GetValue()==ID && Impl->VisiblePoint(View,Hit.ImpactPoint)) {bHit=true;break;}
					}
					if(bHit)break;
				}
				if(bHit) Hits[I].Add(ID);
			}
		}
		if(Impl->Mode==EBlenderMeshSelectionMode::Edge && !bAnyEnclosedEdge)
			for(int32 ID:Partial[I]) Hits[I].Add(ID);
	}
	if(Operation==EBlenderSelectionOperation::Replace)Impl->ClearSelection();
	for(int32 I=0;I<Hits.Num();++I)
	{
		auto& Selection=Impl->Objects[I]->Selection(Impl->Mode);
		if(Operation==EBlenderSelectionOperation::Intersect)
		{
			for(auto It=Selection.CreateIterator();It;++It) if(!Hits[I].Contains(*It))It.RemoveCurrent();
		}
		else for(int32 ID:Hits[I]){if(Operation==EBlenderSelectionOperation::Subtract)Selection.Remove(ID);else Selection.Add(ID);}
	}
	if(Impl->Objects.IsValidIndex(Impl->ActiveObject) && !Impl->Objects[Impl->ActiveObject]->Selection(Impl->Mode).Contains(Impl->ActiveElement))Impl->ActiveElement=INDEX_NONE;
	Client->Invalidate();return true;
}

void FBlenderMeshEditSession::Render(const FSceneView* View, FViewport* Viewport, FPrimitiveDrawInterface* PDI)
{
	if (!IsActive() || !PDI) return;
	const FLinearColor Orange(1.0f, 0.43f, 0.035f), Dark(0.025f, 0.025f, 0.025f), Active(1.0f,0.9f,0.7f);
	const uint8 Depth=Impl->bXRay ? SDPG_Foreground : SDPG_World;
	for (int32 ObjectIndex=0;ObjectIndex<Impl->Objects.Num();++ObjectIndex)
	{
		const auto& Object=Impl->Objects[ObjectIndex];
		TSet<int32> SelectedEdges;
		if (Impl->Mode == EBlenderMeshSelectionMode::Face)
			for (int32 Face : Object->SelectedFaces) for (FEdgeID Edge : Object->Mesh.GetPolygonPerimeterEdges(FPolygonID(Face))) SelectedEdges.Add(Edge.GetValue());
		else if (Impl->Mode == EBlenderMeshSelectionMode::Edge) SelectedEdges = Object->SelectedEdges;
		const TSet<int32> SelectedVertices = Object->SelectedVertexUnion(Impl->Mode);
		for (int32 Edge : Object->PerimeterEdges)
		{
			const auto Ends = Object->Mesh.GetEdgeVertices(FEdgeID(Edge));
			const bool bSelected = Impl->Mode == EBlenderMeshSelectionMode::Vertex ?
				SelectedVertices.Contains(Ends[0].GetValue()) && SelectedVertices.Contains(Ends[1].GetValue()) : SelectedEdges.Contains(Edge);
			const bool bActive=bSelected && Impl->Mode==EBlenderMeshSelectionMode::Edge && Impl->ActiveObject==ObjectIndex && Impl->ActiveElement==Edge;
			PDI->DrawLine(Object->WorldVertex(Ends[0].GetValue()), Object->WorldVertex(Ends[1].GetValue()), bActive ? Active : bSelected ? Orange : Dark, Depth, bSelected ? 1.7f : 0.8f, 0.1f, true);
		}
		if (Impl->Mode == EBlenderMeshSelectionMode::Vertex)
			for (const FVertexID Vertex : Object->Mesh.Vertices().GetElementIDs())
				PDI->DrawPoint(Object->WorldVertex(Vertex.GetValue()), Object->SelectedVertices.Contains(Vertex.GetValue()) ? (Impl->ActiveObject==ObjectIndex && Impl->ActiveElement==Vertex.GetValue() ? Active : Orange) : Dark,
					Object->SelectedVertices.Contains(Vertex.GetValue()) ? 7.0f : 5.0f, Depth);
		else if(Impl->Mode==EBlenderMeshSelectionMode::Face)
			for(int32 Face:Object->Elements(Impl->Mode))
				PDI->DrawPoint(Object->Transform.TransformPosition(Object->Center(Impl->Mode,Face)),Object->SelectedFaces.Contains(Face) ? (Impl->ActiveObject==ObjectIndex && Impl->ActiveElement==Face ? Active : Orange) : Dark,5.0f,Depth);
	}
}

FText FBlenderMeshEditSession::GetStatusText() const
{
	int32 Count = 0;
	for (const auto& Object : Impl->Objects) Count += Object->Selection(Impl->Mode).Num();
	const FText Kind = Impl->Mode == EBlenderMeshSelectionMode::Vertex ? LOCTEXT("Vertex", "点") :
		Impl->Mode == EBlenderMeshSelectionMode::Edge ? LOCTEXT("Edge", "边") : LOCTEXT("Face", "面");
	return FText::Format(LOCTEXT("Status", "网格编辑：{0} 个物体 | 已选 {1} 个{2} | 1/2/3 点边面 | Shift 加减选 | Tab 提交 | Esc 放弃"),
		FText::AsNumber(Impl->Objects.Num()), FText::AsNumber(Count), Kind);
}

AActor* FBlenderMeshEditSession::GetSourceActor(int32 ObjectIndex) const { return Impl->Objects.IsValidIndex(ObjectIndex) ? Impl->Objects[ObjectIndex]->Actor.Get() : nullptr; }
UStaticMesh* FBlenderMeshEditSession::GetSourceMesh(int32 ObjectIndex) const { return Impl->Objects.IsValidIndex(ObjectIndex) ? Impl->Objects[ObjectIndex]->Source.Get() : nullptr; }

bool FBlenderMeshEditSession::GetVertexWorldPosition(int32 ObjectIndex, int32 VertexId, FVector& OutPosition) const
{
	if (!Impl->Objects.IsValidIndex(ObjectIndex) || !Impl->Objects[ObjectIndex]->Mesh.Vertices().IsValid(FVertexID(VertexId))) return false;
	OutPosition = Impl->Objects[ObjectIndex]->WorldVertex(VertexId); return true;
}

bool FBlenderMeshEditSession::HasChanges() const
{
	for (const auto& Object : Impl->Objects) if (Object->HasChanges()) return true;
	return false;
}

bool FBlenderMeshEditSession::Commit(TArray<UStaticMesh*>& CreatedAssets, FText& Error)
{
	CreatedAssets.Reset(); Error = FText::GetEmpty();
	if (!IsActive()) { Error = LOCTEXT("NoSession", "没有正在编辑的网格。"); return false; }
	if (Impl->bTransforming) ConfirmTransform();
	if (IsTopologyEditing()) AcceptTopologyEdit();
	TArray<int32> Changed;
	for (int32 I = 0; I < Impl->Objects.Num(); ++I)
	{
		const auto& Object = Impl->Objects[I];
		if (!IsValid(Object->Actor) || !IsValid(Object->Component) || !IsValid(Object->Source) ||
			Object->Component->GetStaticMesh() != Object->Source || !Object->Component->GetComponentTransform().Equals(Object->ComponentTransform) ||
			FLevelUtils::IsLevelLocked(Object->Actor) || Object->Actor->IsLockLocation() || Object->Component->IsSimulatingPhysics())
		{
			Error = LOCTEXT("ObjectChanged", "原物体已被删除、锁定、移动或替换模型；请放弃本次编辑后重试。"); return false;
		}
		if (Object->HasChanges()) Changed.Add(I);
	}
	if (Changed.IsEmpty()) { Impl->Cleanup(); return true; }
	IAssetTools& AssetTools = FModuleManager::LoadModuleChecked<FAssetToolsModule>("AssetTools").Get();
	TArray<TUniquePtr<FPendingAsset>> PendingAssets;
	for (int32 I : Changed)
	{
		auto& Object = Impl->Objects[I];
		FString PackageName, AssetName;
		int32 SuffixIndex = 0;
		do
		{
			const FString Suffix = SuffixIndex == 0 ? TEXT("_Edit") : FString::Printf(TEXT("_Edit_%d"), SuffixIndex);
			AssetTools.CreateUniqueAssetName(TEXT("/Game/BlenderEdits/") + Object->Source->GetName(), Suffix, PackageName, AssetName);
			++SuffixIndex;
			// CreateUniqueAssetName checks the registry; also reserve pending in-memory
			// packages and existing files before the new assets are registered.
		} while (FindPackage(nullptr, *PackageName) || FPackageName::DoesPackageExist(PackageName));
		UPackage* Package = CreatePackage(*PackageName);
		auto& Pending = PendingAssets.Add_GetRef(MakeUnique<FPendingAsset>(Package));
		// AssetTools.DuplicateAsset can save through source control. Plain duplication
		// keeps the new package in memory until the user chooses the normal Save flow.
		UStaticMesh* NewMesh = Package ? Cast<UStaticMesh>(StaticDuplicateObject(Object->Source.Get(), Package, FName(*AssetName))) : nullptr;
		Pending->Mesh.Reset(NewMesh);
		if (!NewMesh)
		{
			AbandonNewAssets(PendingAssets); CreatedAssets.Reset();
			Error = LOCTEXT("DuplicateFailed", "无法创建独立网格资源，原物体未改变。"); return false;
		}
		NewMesh->ClearFlags(RF_Transient);
		NewMesh->SetFlags(RF_Public | RF_Standalone | RF_Transactional);
		CreatedAssets.Add(NewMesh);
		FMeshDescription Description(Object->Mesh);
		FStaticMeshOperations::ComputeTriangleTangentsAndNormals(Description);
		FStaticMeshOperations::ComputeTangentsAndNormals(Description, EComputeNTBsFlags::Normals | EComputeNTBsFlags::Tangents | EComputeNTBsFlags::UseMikkTSpace | EComputeNTBsFlags::IgnoreDegenerateTriangles);
		NewMesh->CreateMeshDescription(0, MoveTemp(Description));
		NewMesh->CommitMeshDescription(0);
		// Duplicate inherits build scale. Working source positions stay pre-build, exactly as source.
		// Preserve source UVs/colors and inherit lightmap generation settings: generated lightmap
		// channels may exist only in render data, so the duplicate must generate them again.
		auto& BuildSettings = NewMesh->GetSourceModel(0).BuildSettings;
		BuildSettings.bRecomputeNormals = true; BuildSettings.bRecomputeTangents = true;
		NewMesh->ComplexCollisionMesh = nullptr;
		if (!NewMesh->GetBodySetup()) NewMesh->CreateBodySetup();
		UBodySetup* Body = NewMesh->GetBodySetup();
		Body->AggGeom.EmptyElements(); Body->CollisionTraceFlag = CTF_UseComplexAsSimple;
		Body->InvalidatePhysicsData();
		TArray<FText> BuildErrors;
		NewMesh->Build(true, &BuildErrors);
		TArray<UStaticMesh*> MeshesToFinish{NewMesh};
		FStaticMeshCompilingManager::Get().FinishCompilation(MeshesToFinish);
		Body->CreatePhysicsMeshes();
		NewMesh->RecreateNavCollision();
		NewMesh->MarkPackageDirty();
		if (!NewMesh->GetRenderData() || !BuildErrors.IsEmpty())
		{
			AbandonNewAssets(PendingAssets); CreatedAssets.Reset();
			Error = BuildErrors.IsEmpty() ? LOCTEXT("BuildFailed", "独立模型构建失败，原物体未改变。") : BuildErrors[0]; return false;
		}
	}
	for (auto& Pending : PendingAssets)
	{
		FAssetRegistryModule::AssetCreated(Pending->Mesh.Get());
		Pending->bRegistered = true;
	}
	// The editor transaction must record normal visibility, not the session's transient hide.
	for (auto& Object : Impl->Objects) Object->Actor->SetIsTemporarilyHiddenInEditor(Object->bWasHidden);
	{
		const FScopedTransaction Transaction(LOCTEXT("CommitTransaction", "Blender 式多物体网格编辑"));
		for (int32 N = 0; N < Changed.Num(); ++N)
		{
			auto& Object = Impl->Objects[Changed[N]];
			Object->Actor->Modify(); Object->Component->Modify();
			Object->Component->SetStaticMesh(CreatedAssets[N]);
			Object->Component->RecreatePhysicsState();
			Object->Actor->MarkPackageDirty();
		}
	}
	Impl->Cleanup();
	return true;
}

void FBlenderMeshEditSession::AddReferencedObjects(FReferenceCollector& Collector)
{
	Collector.AddReferencedObject(Impl->World);
	Collector.AddReferencedObject(Impl->PreviewMaterial);
	for (auto& Object : Impl->Objects)
	{
		Collector.AddReferencedObject(Object->Actor); Collector.AddReferencedObject(Object->Component);
		Collector.AddReferencedObject(Object->Source); Collector.AddReferencedObject(Object->Preview);
	}
}

bool FBlenderMeshEditSession::CopyMeshDescription(int32 ObjectIndex, FMeshDescription& OutMesh) const
{
	if (!Impl->Objects.IsValidIndex(ObjectIndex)) return false;
	OutMesh = Impl->Objects[ObjectIndex]->Mesh; return true;
}

bool FBlenderMeshEditSession::IsTopologyEditing() const
{
	return Impl->TopologyOperation != FImpl::ETopologyOperation::None;
}

bool FBlenderMeshEditSession::BeginExtrude(bool bAlongNormals, FText& Error)
{
	Error = FText::GetEmpty();
	if (!IsActive() || Impl->bTransforming || IsTopologyEditing()) { Error = LOCTEXT("TopologyBusy", "请先完成当前操作。"); return false; }
	if (Impl->Mode != EBlenderMeshSelectionMode::Face) { Error = LOCTEXT("ExtrudeFacesOnly", "当前挤出支持面选区；请按 3 切到面选择。"); return false; }
	TArray<FMeshDescription> Candidates; TArray<BlenderMeshTopology::FExtrusion> Extrusions;
	Candidates.SetNum(Impl->Objects.Num()); Extrusions.SetNum(Impl->Objects.Num());
	bool bAny = false; FVector AreaNormal = FVector::ZeroVector;
	for (int32 I = 0; I < Impl->Objects.Num(); ++I)
	{
		const auto& Object = Impl->Objects[I]; if (Object->SelectedFaces.IsEmpty()) continue;
		Candidates[I] = Object->Mesh;
		if (!BlenderMeshTopology::Extrude(Candidates[I], Object->SelectedFaces, Object->Transform, bAlongNormals, Extrusions[I], Error)) return false;
		AreaNormal += Extrusions[I].AreaNormalSum; bAny = true;
	}
	if (!bAny) { Error = LOCTEXT("ExtrudeEmpty", "请先选择要挤出的面。"); return false; }
	if (!bAlongNormals && AreaNormal.IsNearlyZero()) { Error = LOCTEXT("ExtrudeCancellingNormals", "选区平均法线相互抵消，请缩小选区或使用沿法线挤出。"); return false; }
	Impl->TopologyStart = Impl->Snapshot(); Impl->Extrusions = MoveTemp(Extrusions);
	Impl->ExtrudeBases = MoveTemp(Candidates);
	Impl->ExtrudeDirection = AreaNormal.GetSafeNormal();
	if (Impl->ExtrudeDirection.IsNearlyZero())
		for (const auto& E : Impl->Extrusions) if (!E.WorldDirections.IsEmpty()) { Impl->ExtrudeDirection = E.WorldDirections.CreateConstIterator().Value(); break; }
	Impl->ClearSelection();
	for (int32 I = 0; I < Impl->Objects.Num(); ++I)
	{
		if (Impl->Extrusions[I].CapFaces.IsEmpty()) continue;
		auto& Object = Impl->Objects[I]; Object->Mesh = Impl->ExtrudeBases[I]; ++Object->TopologyRevision;
		Object->SelectedFaces = Impl->Extrusions[I].CapFaces;
		if (!bAlongNormals) for (auto& Pair : Impl->Extrusions[I].WorldDirections) Pair.Value = Impl->ExtrudeDirection;
		Object->RebuildPreview();
		if (Impl->ActiveObject == INDEX_NONE) { Impl->ActiveObject = I; Impl->ActiveElement = Object->SelectedFaces.Array()[0]; Impl->ActiveMode = EBlenderMeshSelectionMode::Face; }
	}
	Impl->TopologyOperation = FImpl::ETopologyOperation::Extrude; return true;
}

bool FBlenderMeshEditSession::UpdateExtrude(double DistanceCm, const FVector& UniformWorldDirection)
{
	if (Impl->TopologyOperation != FImpl::ETopologyOperation::Extrude || !FMath::IsFinite(DistanceCm) || UniformWorldDirection.ContainsNaN()) return false;
	const FVector Override = UniformWorldDirection.GetSafeNormal();
	// Validate all transformed positions before touching any object.
	TArray<FPositions> NewPositions; NewPositions.SetNum(Impl->Objects.Num());
	for (int32 I = 0; I < Impl->Objects.Num(); ++I) for (const auto& Pair : Impl->Extrusions[I].WorldStarts)
	{
		const FVector Direction = Override.IsNearlyZero() ? Impl->Extrusions[I].WorldDirections.FindChecked(Pair.Key) : Override;
		const FVector Local = Impl->Objects[I]->Transform.InverseTransformPosition(Pair.Value + Direction * DistanceCm);
		const FVector3f Position(Local); if (Position.ContainsNaN()) return false;
		NewPositions[I].Add(Pair.Key, Position);
	}
	for (int32 I = 0; I < Impl->Objects.Num(); ++I)
	{
		if (NewPositions[I].IsEmpty()) continue;
		auto& Object = Impl->Objects[I];
		// Ear clipping can rotate the perimeter start and select a different diagonal. Always
		// retriangulate from the same zero-offset topology, never the previous mouse update.
		FMeshDescription Candidate = Impl->ExtrudeBases[I]; auto Positions = Candidate.GetVertexPositions();
		for (const auto& Pair : NewPositions[I]) Positions[FVertexID(Pair.Key)] = Pair.Value;
		// The cap may be an ngon: retriangulate it after displacement, preserving its logical polygon.
		TArray<int32> CapFaces = Impl->Extrusions[I].CapFaces.Array(); CapFaces.Sort();
		for (int32 Face : CapFaces) Candidate.ComputePolygonTriangulation(FPolygonID(Face));
		Object->Mesh = MoveTemp(Candidate);
		Object->RebuildPreview();
	}
	return true;
}

FVector FBlenderMeshEditSession::GetExtrudeDirection() const { return Impl->ExtrudeDirection; }

bool FBlenderMeshEditSession::FindLoopCutEdge(FEditorViewportClient* Client, FViewport* Viewport, int32& OutObject, int32& OutEdge) const
{
	OutObject = OutEdge = INDEX_NONE;
	if (!IsActive() || !Client || !Viewport || Impl->bTransforming || IsTopologyEditing()) return false;
	FSceneViewFamilyContext Family(FSceneViewFamily::ConstructionValues(Viewport, Client->GetScene(), Client->EngineShowFlags).SetRealtimeUpdate(Client->IsRealtime()));
	FSceneView* View = Client->CalcSceneView(&Family); if (!View) return false;
	Impl->UpdateExternalOccluders(View);
	const FVector2D Mouse(Viewport->GetMouseX(), Viewport->GetMouseY()); double Best = 14.0 * 14.0;
	// Blender's hover accepts the whole face: a surface hit chooses the closest boundary edge.
	FVector RayOrigin, RayDirection; View->DeprojectFVector2D(Mouse, RayOrigin, RayDirection);
	double NearestFace = TNumericLimits<double>::Max(); int32 HitObject = INDEX_NONE, HitFace = INDEX_NONE;
	for (int32 I = 0; I < Impl->Objects.Num(); ++I)
	{
		const auto& Object = Impl->Objects[I]; FHitResult Hit;
		if (!Object->Preview || !Object->Preview->FindRayIntersection(FRay3d(RayOrigin, RayDirection), Hit) || !Object->DynamicToSourceTriangle.IsValidIndex(Hit.FaceIndex)) continue;
		const double Distance = FVector::DistSquared(RayOrigin, Hit.ImpactPoint);
		if (Distance < NearestFace && Impl->VisiblePoint(View, Hit.ImpactPoint))
		{ NearestFace = Distance; HitObject = I; HitFace = Object->Mesh.GetTrianglePolygon(Object->DynamicToSourceTriangle[Hit.FaceIndex]).GetValue(); }
	}
	if (HitObject != INDEX_NONE)
	{
		const auto& Object = Impl->Objects[HitObject]; double NearestEdge = TNumericLimits<double>::Max();
		for (FEdgeID E : Object->Mesh.GetPolygonPerimeterEdges(FPolygonID(HitFace)))
		{
			const auto Ends = Object->Mesh.GetEdgeVertices(E); FVector2D A, B;
			if (!View->WorldToPixel(Object->WorldVertex(Ends[0].GetValue()), A) || !View->WorldToPixel(Object->WorldVertex(Ends[1].GetValue()), B)) continue;
			const FVector2D Delta = B - A;
			const double T = FMath::Clamp(FVector2D::DotProduct(Mouse - A, Delta) / FMath::Max(Delta.SizeSquared(), 1.e-10), 0.0, 1.0);
			const double Score = FVector2D::DistSquared(Mouse, A + T * Delta);
			if (Score < NearestEdge) { NearestEdge = Score; OutObject = HitObject; OutEdge = E.GetValue(); }
		}
		if (OutEdge != INDEX_NONE) return true;
	}
	for (int32 I = 0; I < Impl->Objects.Num(); ++I)
	{
		const auto& Object = Impl->Objects[I];
		for (int32 Edge : Object->PerimeterEdges)
		{
			const auto Ends = Object->Mesh.GetEdgeVertices(FEdgeID(Edge));
			const FVector A = Object->WorldVertex(Ends[0].GetValue()), B = Object->WorldVertex(Ends[1].GetValue()); FVector2D SA, SB;
			const double WA = View->WorldToScreen(A).W, WB = View->WorldToScreen(B).W;
			if ((View->IsPerspectiveProjection() && (WA <= 0 || WB <= 0)) || !View->WorldToPixel(A, SA) || !View->WorldToPixel(B, SB)) continue;
			const FVector2D Delta = SB - SA;
			const double T = FMath::Clamp(FVector2D::DotProduct(Mouse - SA, Delta) / FMath::Max(Delta.SizeSquared(), 1.e-10), 0.0, 1.0);
			const double Score = FVector2D::DistSquared(Mouse, SA + T * Delta);
			const double WorldT = (T / WB) / ((1 - T) / WA + T / WB);
			if (Score < Best && Impl->VisiblePoint(View, FMath::Lerp(A, B, WorldT))) { Best = Score; OutObject = I; OutEdge = Edge; }
		}
	}
	return OutObject != INDEX_NONE;
}

bool FBlenderMeshEditSession::GetLoopCutPreview(int32 ObjectIndex, int32 Edge, TArray<TPair<FVector,FVector>>& OutSegments, FText& Error) const
{
	OutSegments.Reset(); Error = FText::GetEmpty();
	if (!Impl->Objects.IsValidIndex(ObjectIndex)) return false;
	const auto& Object = Impl->Objects[ObjectIndex]; BlenderMeshTopology::FLoopPlan Plan;
	if (!BlenderMeshTopology::PlanLoop(Object->Mesh, Edge, Plan, Error)) return false;
	for (const auto& Face : Plan.Faces)
	{
		const auto Edges = Object->Mesh.GetPolygonPerimeterEdges(FPolygonID(Face.Face));
		int32 Index = INDEX_NONE; for (int32 I = 0; I < Edges.Num(); ++I) if (Edges[I].GetValue() == Face.FirstEdge) Index = I;
		if (Index == INDEX_NONE || Edges.Num() != 4) return false;
		const auto A = Object->Mesh.GetEdgeVertices(Edges[Index]), B = Object->Mesh.GetEdgeVertices(Edges[(Index + 2) % 4]);
		OutSegments.Add({(Object->WorldVertex(A[0].GetValue()) + Object->WorldVertex(A[1].GetValue())) * .5,
			(Object->WorldVertex(B[0].GetValue()) + Object->WorldVertex(B[1].GetValue())) * .5});
	}
	return true;
}

bool FBlenderMeshEditSession::BeginLoopCut(int32 ObjectIndex, int32 Edge, FText& Error)
{
	Error = FText::GetEmpty();
	if (!IsActive() || Impl->bTransforming || IsTopologyEditing() || !Impl->Objects.IsValidIndex(ObjectIndex)) return false;
	BlenderMeshTopology::FLoopPlan Plan;
	if (!BlenderMeshTopology::PlanLoop(Impl->Objects[ObjectIndex]->Mesh, Edge, Plan, Error)) return false;
	FMeshDescription Candidate = Impl->Objects[ObjectIndex]->Mesh; TSet<int32> NewEdges;
	if (!BlenderMeshTopology::CutLoop(Candidate, Plan, .5, NewEdges, Error)) return false;
	Impl->TopologyStart = Impl->Snapshot(); Impl->LoopPlan = MoveTemp(Plan); Impl->LoopObject = ObjectIndex;
	Impl->TopologyOperation = FImpl::ETopologyOperation::LoopCut;
	Impl->ClearSelection(); Impl->Mode = EBlenderMeshSelectionMode::Edge;
	auto& Object = Impl->Objects[ObjectIndex]; Object->Mesh = MoveTemp(Candidate); ++Object->TopologyRevision; Object->SelectedEdges = MoveTemp(NewEdges);
	Impl->ActiveObject = ObjectIndex; Impl->ActiveMode = EBlenderMeshSelectionMode::Edge; Impl->ActiveElement = Object->SelectedEdges.IsEmpty() ? INDEX_NONE : Object->SelectedEdges.Array()[0];
	Object->RebuildPreview(); return true;
}

bool FBlenderMeshEditSession::UpdateLoopCut(double Fraction)
{
	if (Impl->TopologyOperation != FImpl::ETopologyOperation::LoopCut || !FMath::IsFinite(Fraction)) return false;
	FMeshDescription Candidate = Impl->TopologyStart[Impl->LoopObject].Mesh; TSet<int32> Edges; FText Error;
	if (!BlenderMeshTopology::CutLoop(Candidate, Impl->LoopPlan, Fraction, Edges, Error)) return false;
	auto& Object = Impl->Objects[Impl->LoopObject]; Object->Mesh = MoveTemp(Candidate); Object->SelectedEdges = MoveTemp(Edges);
	Impl->ActiveElement = Object->SelectedEdges.IsEmpty() ? INDEX_NONE : Object->SelectedEdges.Array()[0];
	Object->RebuildPreview(); return true;
}

bool FBlenderMeshEditSession::GetLoopCutSlideGuide(FVector& Start, FVector& End) const
{
	if (Impl->TopologyOperation != FImpl::ETopologyOperation::LoopCut || Impl->LoopPlan.Edges.IsEmpty()) return false;
	const auto& E = Impl->LoopPlan.Edges[0]; const auto Positions = Impl->TopologyStart[Impl->LoopObject].Mesh.GetVertexPositions();
	const FTransform& Transform = Impl->Objects[Impl->LoopObject]->Transform;
	Start = Transform.TransformPosition(FVector(Positions[FVertexID(E.A)])); End = Transform.TransformPosition(FVector(Positions[FVertexID(E.B)])); return true;
}

void FBlenderMeshEditSession::AcceptTopologyEdit()
{
	if (!IsTopologyEditing()) return;
	FSnapshot After = Impl->Snapshot();
	if (!SnapshotsEqual(Impl->TopologyStart, After))
	{
		Impl->History.SetNum(Impl->HistoryCursor); Impl->History.Add({MoveTemp(Impl->TopologyStart), MoveTemp(After)});
		if (Impl->History.Num() > 64) Impl->History.RemoveAt(0); Impl->HistoryCursor = Impl->History.Num();
	}
	Impl->TopologyStart.Reset(); Impl->Extrusions.Reset(); Impl->ExtrudeBases.Reset(); Impl->LoopPlan = {}; Impl->LoopObject = INDEX_NONE;
	Impl->TopologyOperation = FImpl::ETopologyOperation::None;
}

void FBlenderMeshEditSession::CancelTopologyEdit()
{
	if (!IsTopologyEditing()) return;
	Impl->Restore(Impl->TopologyStart); Impl->TopologyStart.Reset(); Impl->Extrusions.Reset(); Impl->ExtrudeBases.Reset(); Impl->LoopPlan = {}; Impl->LoopObject = INDEX_NONE;
	Impl->TopologyOperation = FImpl::ETopologyOperation::None;
}

UStaticMesh* CreateBlenderQuadCube()
{
	IAssetTools& AssetTools = FModuleManager::LoadModuleChecked<FAssetToolsModule>("AssetTools").Get();
	FString PackageName, AssetName; int32 Attempt = 0;
	do { AssetTools.CreateUniqueAssetName(TEXT("/Game/BlenderPrimitives/SM_QuadCube"), Attempt == 0 ? TEXT("") : FString::Printf(TEXT("_%d"), Attempt), PackageName, AssetName); ++Attempt; }
	while (FindPackage(nullptr, *PackageName) || FPackageName::DoesPackageExist(PackageName));
	UPackage* Package = CreatePackage(*PackageName); if (!Package) return nullptr;
	UStaticMesh* Mesh = NewObject<UStaticMesh>(Package, FName(*AssetName), RF_Public | RF_Standalone | RF_Transactional);
	Mesh->AddSourceModel(); Mesh->GetStaticMaterials().Add(FStaticMaterial(UMaterial::GetDefaultMaterial(MD_Surface), FName(TEXT("Default")), FName(TEXT("Default"))));
	FMeshDescription Description; BlenderMeshTopology::MakeCube(Description);
	FStaticMeshOperations::ComputeTriangleTangentsAndNormals(Description);
	FStaticMeshOperations::ComputeTangentsAndNormals(Description, EComputeNTBsFlags::Normals | EComputeNTBsFlags::Tangents | EComputeNTBsFlags::UseMikkTSpace);
	Mesh->CreateMeshDescription(0, MoveTemp(Description)); Mesh->CommitMeshDescription(0);
	Mesh->GetSourceModel(0).BuildSettings.bRecomputeNormals = true; Mesh->GetSourceModel(0).BuildSettings.bRecomputeTangents = true;
	Mesh->CreateBodySetup(); Mesh->GetBodySetup()->CollisionTraceFlag = CTF_UseComplexAsSimple;
	TArray<FText> Errors; Mesh->Build(true, &Errors); TArray<UStaticMesh*> Meshes{Mesh}; FStaticMeshCompilingManager::Get().FinishCompilation(Meshes);
	if (!Errors.IsEmpty() || !Mesh->GetRenderData()) { Mesh->ClearFlags(RF_Public | RF_Standalone); Mesh->MarkAsGarbage(); Package->SetDirtyFlag(false); return nullptr; }
	Mesh->GetBodySetup()->CreatePhysicsMeshes(); Mesh->MarkPackageDirty(); FAssetRegistryModule::AssetCreated(Mesh); return Mesh;
}

#undef LOCTEXT_NAMESPACE
