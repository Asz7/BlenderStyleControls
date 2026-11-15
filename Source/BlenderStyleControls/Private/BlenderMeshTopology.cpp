#include "BlenderMeshTopology.h"
#include "StaticMeshAttributes.h"

namespace BlenderMeshTopology
{
namespace
{
struct FCorner
{
	int32 Vertex;
	FVector3f Normal;
	FVector3f Tangent;
	float Sign;
	FVector4f Color;
	TArray<FVector2f> UV;
};
struct FFace
{
	int32 ID;
	FPolygonGroupID Group;
	TArray<FCorner> Corners;
};
FFace ReadFace(const FMeshDescription& Mesh, int32 ID)
{
	FStaticMeshConstAttributes A(Mesh);
	FFace F; F.ID = ID; F.Group = Mesh.GetPolygonPolygonGroup(FPolygonID(ID));
	for (FVertexInstanceID VI : Mesh.GetPolygonVertexInstances(FPolygonID(ID)))
	{
		FCorner C; C.Vertex = Mesh.GetVertexInstanceVertex(VI).GetValue();
		C.Normal = A.GetVertexInstanceNormals()[VI]; C.Tangent = A.GetVertexInstanceTangents()[VI];
		C.Sign = A.GetVertexInstanceBinormalSigns()[VI]; C.Color = A.GetVertexInstanceColors()[VI];
		for (int32 Channel = 0; Channel < A.GetVertexInstanceUVs().GetNumChannels(); ++Channel)
			C.UV.Add(A.GetVertexInstanceUVs().Get(VI, Channel));
		F.Corners.Add(MoveTemp(C));
	}
	return F;
}
FVertexInstanceID MakeCorner(FMeshDescription& Mesh, const FCorner& C)
{
	FStaticMeshAttributes A(Mesh);
	const FVertexInstanceID VI = Mesh.CreateVertexInstance(FVertexID(C.Vertex));
	A.GetVertexInstanceNormals()[VI] = C.Normal; A.GetVertexInstanceTangents()[VI] = C.Tangent;
	A.GetVertexInstanceBinormalSigns()[VI] = C.Sign; A.GetVertexInstanceColors()[VI] = C.Color;
	for (int32 Channel = 0; Channel < C.UV.Num(); ++Channel) A.GetVertexInstanceUVs().Set(VI, Channel, C.UV[Channel]);
	return VI;
}
FPolygonID MakeFace(FMeshDescription& Mesh, FPolygonGroupID Group, const TArray<FCorner>& Corners)
{
	TArray<FVertexInstanceID> Instances;
	for (const FCorner& C : Corners) Instances.Add(MakeCorner(Mesh, C));
	return Mesh.CreatePolygon(Group, Instances);
}
FCorner Interpolate(const FCorner& A, const FCorner& B, int32 Vertex, double T)
{
	FCorner C = A; C.Vertex = Vertex;
	C.Normal = FMath::Lerp(A.Normal, B.Normal, float(T)).GetSafeNormal();
	C.Tangent = FMath::Lerp(A.Tangent, B.Tangent, float(T)).GetSafeNormal();
	C.Color = FMath::Lerp(A.Color, B.Color, float(T)); C.Sign = T < .5 ? A.Sign : B.Sign;
	for (int32 Channel = 0; Channel < C.UV.Num(); ++Channel) C.UV[Channel] = FMath::Lerp(A.UV[Channel], B.UV[Channel], float(T));
	return C;
}
struct FExistingOrphans
{
	TSet<int32> Edges, Instances, Vertices;
	explicit FExistingOrphans(const FMeshDescription& Mesh)
	{
		for (FEdgeID E : Mesh.Edges().GetElementIDs()) if (Mesh.GetEdgeConnectedTriangleIDs(E).IsEmpty()) Edges.Add(E.GetValue());
		for (FVertexInstanceID VI : Mesh.VertexInstances().GetElementIDs()) if (Mesh.GetVertexInstanceConnectedTriangleIDs(VI).IsEmpty()) Instances.Add(VI.GetValue());
		for (FVertexID V : Mesh.Vertices().GetElementIDs()) if (Mesh.IsVertexOrphaned(V)) Vertices.Add(V.GetValue());
	}
};
void RemoveOrphans(FMeshDescription& Mesh, const FExistingOrphans& Existing)
{
	TArray<FEdgeID> Edges;
	for (FEdgeID E : Mesh.Edges().GetElementIDs()) if (!Existing.Edges.Contains(E.GetValue()) && Mesh.GetEdgeConnectedTriangleIDs(E).IsEmpty()) Edges.Add(E);
	for (FEdgeID E : Edges) Mesh.DeleteEdge(E);
	TArray<FVertexInstanceID> Instances;
	for (FVertexInstanceID VI : Mesh.VertexInstances().GetElementIDs())
		if (!Existing.Instances.Contains(VI.GetValue()) && Mesh.GetVertexInstanceConnectedTriangleIDs(VI).IsEmpty()) Instances.Add(VI);
	for (FVertexInstanceID VI : Instances) Mesh.DeleteVertexInstance(VI);
	TArray<FVertexID> Vertices;
	for (FVertexID V : Mesh.Vertices().GetElementIDs())
		if (!Existing.Vertices.Contains(V.GetValue()) && Mesh.GetVertexConnectedEdgeIDs(V).IsEmpty() && Mesh.GetVertexVertexInstanceIDs(V).IsEmpty()) Vertices.Add(V);
	for (FVertexID V : Vertices) Mesh.DeleteVertex(V);
}
bool Fail(FText& Error, const TCHAR* Message) { Error = FText::FromString(Message); return false; }
}

bool Extrude(FMeshDescription& Mesh, const TSet<int32>& Selection, const FTransform& Transform,
	bool bAlongNormals, FExtrusion& Out, FText& Error)
{
	Out = {}; Error = FText::GetEmpty();
	const FExistingOrphans ExistingOrphans(Mesh);
	if (Selection.IsEmpty()) return Fail(Error, TEXT("请先选择要挤出的面。"));
	TArray<FFace> Faces; TMap<int32, FVector> Directions; FVector Sum = FVector::ZeroVector;
	TMap<int32, bool> SourceHardness;
	FStaticMeshAttributes Attributes(Mesh);
	for (FEdgeID E : Mesh.Edges().GetElementIDs()) SourceHardness.Add(E.GetValue(), Attributes.GetEdgeHardnesses()[E]);
	TArray<int32> FaceIDs = Selection.Array(); FaceIDs.Sort();
	for (int32 ID : FaceIDs)
	{
		if (!Mesh.Polygons().IsValid(FPolygonID(ID))) return Fail(Error, TEXT("选中的面已失效。"));
		FFace Face = ReadFace(Mesh, ID); if (Face.Corners.Num() < 3) return Fail(Error, TEXT("无法挤出退化面。"));
		FVector AreaNormal = FVector::ZeroVector;
		for (int32 I = 0; I < Face.Corners.Num(); ++I)
		{
			const int32 A = Face.Corners[I].Vertex, B = Face.Corners[(I + 1) % Face.Corners.Num()].Vertex;
			const FEdgeID E = Mesh.GetVertexPairEdge(FVertexID(A), FVertexID(B));
			if (!Mesh.Edges().IsValid(E) || Mesh.GetEdgeConnectedPolygons(E).Num() > 2)
				return Fail(Error, TEXT("当前挤出不支持非流形边。"));
			const FVector P = Transform.TransformPosition(FVector(Mesh.GetVertexPositions()[FVertexID(A)]));
			const FVector Q = Transform.TransformPosition(FVector(Mesh.GetVertexPositions()[FVertexID(B)]));
			// Match UE MeshDescription / StaticMeshOperations: its left-handed surface normal
			// uses the reverse of the usual right-handed cross product.
			AreaNormal += FVector::CrossProduct(Q, P);
		}
		// Mirrored components reverse culling: use the visible outward normal, not mirrored winding.
		const FVector Scale = Transform.GetScale3D();
		if (Scale.X * Scale.Y * Scale.Z < 0) AreaNormal *= -1;
		if (AreaNormal.IsNearlyZero() || AreaNormal.ContainsNaN()) return Fail(Error, TEXT("选区包含无法确定法线的退化面。"));
		Sum += AreaNormal;
		for (const FCorner& C : Face.Corners) Directions.FindOrAdd(C.Vertex) += AreaNormal;
		Faces.Add(MoveTemp(Face));
	}
	Out.AverageNormal = Sum.GetSafeNormal();
	Out.AreaNormalSum = Sum;
	if (!bAlongNormals && Out.AverageNormal.IsNearlyZero()) return Fail(Error, TEXT("选区平均法线相互抵消，请缩小选区或使用沿法线挤出。"));
	if (bAlongNormals) for (const auto& Pair : Directions) if (Pair.Value.IsNearlyZero()) return Fail(Error, TEXT("顶点处的所选面法线相互抵消，无法沿法线挤出。"));
	struct FSide { FPolygonGroupID Group; FCorner A; FCorner B; };
	TArray<FSide> Sides;
	for (const FFace& Face : Faces) for (int32 I = 0; I < Face.Corners.Num(); ++I)
	{
		const FCorner& A = Face.Corners[I]; const FCorner& B = Face.Corners[(I + 1) % Face.Corners.Num()];
		const FEdgeID E = Mesh.GetVertexPairEdge(FVertexID(A.Vertex), FVertexID(B.Vertex));
		int32 SelectedNeighbors = 0;
		for (FPolygonID P : Mesh.GetEdgeConnectedPolygons(E)) if (Selection.Contains(P.GetValue())) ++SelectedNeighbors;
		if (SelectedNeighbors == 1) Sides.Add({Face.Group, A, B});
	}
	TMap<int32, int32> NewVertices;
	TArray<int32> VertexIDs; Directions.GetKeys(VertexIDs); VertexIDs.Sort();
	for (int32 Old : VertexIDs)
	{
		const FVector3f Position = Mesh.GetVertexPositions()[FVertexID(Old)];
		const FVertexID New = Mesh.CreateVertex(); Mesh.GetVertexPositions()[New] = Position;
		NewVertices.Add(Old, New.GetValue());
		Out.WorldStarts.Add(New.GetValue(), Transform.TransformPosition(FVector(Position)));
		Out.WorldDirections.Add(New.GetValue(), bAlongNormals ? Directions[Old].GetSafeNormal() : Out.AverageNormal);
	}
	// DeletePolygon deliberately leaves corners/edges until replacement faces have been created.
	for (const FFace& Face : Faces) Mesh.DeletePolygon(FPolygonID(Face.ID));
	for (const FFace& Face : Faces)
	{
		TArray<FCorner> Cap = Face.Corners;
		for (FCorner& C : Cap) C.Vertex = NewVertices[C.Vertex];
		Out.CapFaces.Add(MakeFace(Mesh, Face.Group, Cap).GetValue());
	}
	for (const FSide& S : Sides)
	{
		FCorner A = S.A, B = S.B, C = S.B, D = S.A;
		C.Vertex = NewVertices[C.Vertex]; D.Vertex = NewVertices[D.Vertex];
		const double U = FVector::Distance(Transform.TransformPosition(FVector(Mesh.GetVertexPositions()[FVertexID(A.Vertex)])),
			Transform.TransformPosition(FVector(Mesh.GetVertexPositions()[FVertexID(B.Vertex)]))) / 100.0;
		for (int32 Channel = 0; Channel < A.UV.Num(); ++Channel)
		{ A.UV[Channel] = FVector2f(0, 0); B.UV[Channel] = FVector2f(U, 0); C.UV[Channel] = FVector2f(U, 1); D.UV[Channel] = FVector2f(0, 1); }
		MakeFace(Mesh, S.Group, {A, B, C, D});
	}
	// Copy cap edge smoothing boundaries and keep the newly created vertical wall corners sharp.
	for (const FFace& Face : Faces) for (int32 I = 0; I < Face.Corners.Num(); ++I)
	{
		const int32 A = Face.Corners[I].Vertex, B = Face.Corners[(I + 1) % Face.Corners.Num()].Vertex;
		const FEdgeID Old = Mesh.GetVertexPairEdge(FVertexID(A), FVertexID(B));
		const FEdgeID New = Mesh.GetVertexPairEdge(FVertexID(NewVertices[A]), FVertexID(NewVertices[B]));
		if (Mesh.Edges().IsValid(New)) Attributes.GetEdgeHardnesses()[New] = SourceHardness.FindRef(Old.GetValue());
	}
	for (const FSide& S : Sides) for (int32 V : {S.A.Vertex, S.B.Vertex})
	{
		const FEdgeID E = Mesh.GetVertexPairEdge(FVertexID(V), FVertexID(NewVertices[V]));
		if (Mesh.Edges().IsValid(E)) Attributes.GetEdgeHardnesses()[E] = true;
	}
	RemoveOrphans(Mesh, ExistingOrphans); return true;
}

bool PlanLoop(const FMeshDescription& Mesh, int32 SeedEdge, FLoopPlan& Out, FText& Error)
{
	Out = {}; Error = FText::GetEmpty();
	if (!Mesh.Edges().IsValid(FEdgeID(SeedEdge))) return Fail(Error, TEXT("请将鼠标放在四边面的一条边上。"));
	TMap<int32, int32> EdgeIndices; TSet<int32> SeenFaces;
	const auto SeedVertices = Mesh.GetEdgeVertices(FEdgeID(SeedEdge));
	Out.Edges.Add({SeedEdge, SeedVertices[0].GetValue(), SeedVertices[1].GetValue()}); EdgeIndices.Add(SeedEdge, 0);
	for (int32 Cursor = 0; Cursor < Out.Edges.Num(); ++Cursor)
	{
		const FLoopEdge Incoming = Out.Edges[Cursor];
		const auto Neighbors = Mesh.GetEdgeConnectedPolygons(FEdgeID(Incoming.Edge));
		if (Neighbors.IsEmpty() || Neighbors.Num() > 2) return Fail(Error, TEXT("环切不支持孤立边或非流形边。"));
		for (FPolygonID P : Neighbors)
		{
			if (SeenFaces.Contains(P.GetValue())) continue;
			const auto Vertices = Mesh.GetPolygonVertices(P);
			if (Vertices.Num() != 4) return Fail(Error, TEXT("环切需要连续四边面；当前路径遇到三角面或多边面。"));
			int32 Index = INDEX_NONE;
			for (int32 I = 0; I < 4; ++I) if (Mesh.GetVertexPairEdge(Vertices[I], Vertices[(I + 1) % 4]).GetValue() == Incoming.Edge) Index = I;
			if (Index == INDEX_NONE) return Fail(Error, TEXT("无法确定四边面的相对边。"));
			const bool Forward = Vertices[Index].GetValue() == Incoming.A;
			const int32 A = Vertices[(Index + (Forward ? 3 : 2)) % 4].GetValue();
			const int32 B = Vertices[(Index + (Forward ? 2 : 3)) % 4].GetValue();
			const int32 Opposite = Mesh.GetVertexPairEdge(FVertexID(A), FVertexID(B)).GetValue();
			if (const int32* Existing = EdgeIndices.Find(Opposite))
			{
				if (Out.Edges[*Existing].A != A) return Fail(Error, TEXT("环切路径方向冲突，无法安全切分此网格。"));
			}
			else { EdgeIndices.Add(Opposite, Out.Edges.Num()); Out.Edges.Add({Opposite, A, B}); }
			SeenFaces.Add(P.GetValue()); Out.Faces.Add({P.GetValue(), Incoming.Edge});
		}
	}
	return !Out.Faces.IsEmpty();
}

bool CutLoop(FMeshDescription& Mesh, const FLoopPlan& Plan, double Fraction, TSet<int32>& NewEdges, FText& Error)
{
	NewEdges.Reset(); Error = FText::GetEmpty(); Fraction = FMath::Clamp(Fraction, .001, .999);
	const FExistingOrphans ExistingOrphans(Mesh);
	TMap<int32, int32> Vertices; TMap<int32, FLoopEdge> Edges;
	TMap<int32, bool> SourceHardness; FStaticMeshAttributes Attributes(Mesh);
	TArray<FFace> Faces;
	for (const FLoopFace& F : Plan.Faces)
	{
		if (!Mesh.Polygons().IsValid(FPolygonID(F.Face))) return Fail(Error, TEXT("环切预览已失效。"));
		Faces.Add(ReadFace(Mesh, F.Face));
	}
	for (const FLoopEdge& E : Plan.Edges)
	{
		if (!Mesh.Edges().IsValid(FEdgeID(E.Edge))) return Fail(Error, TEXT("环切预览边已失效。"));
		SourceHardness.Add(E.Edge, Attributes.GetEdgeHardnesses()[FEdgeID(E.Edge)]);
		const FVector3f Position = FMath::Lerp(Mesh.GetVertexPositions()[FVertexID(E.A)], Mesh.GetVertexPositions()[FVertexID(E.B)], float(Fraction));
		const FVertexID New = Mesh.CreateVertex(); Mesh.GetVertexPositions()[New] = Position;
		Vertices.Add(E.Edge, New.GetValue()); Edges.Add(E.Edge, E);
	}
	// Cache polygon-corner data before deleting; shared geometric edge vertices retain separate UV/color corners.
	TArray<TPair<int32, int32>> CutPairs;
	for (int32 F = 0; F < Faces.Num(); ++F)
	{
		const FFace& Face = Faces[F]; int32 I = 0;
		for (; I < 4; ++I) if (Mesh.GetVertexPairEdge(FVertexID(Face.Corners[I].Vertex), FVertexID(Face.Corners[(I + 1) % 4].Vertex)).GetValue() == Plan.Faces[F].FirstEdge) break;
		if (I == 4) return Fail(Error, TEXT("无法切分当前四边面。"));
		const FCorner A = Face.Corners[I], B = Face.Corners[(I + 1) % 4], C = Face.Corners[(I + 2) % 4], D = Face.Corners[(I + 3) % 4];
		const int32 E1 = Plan.Faces[F].FirstEdge, E2 = Mesh.GetVertexPairEdge(FVertexID(C.Vertex), FVertexID(D.Vertex)).GetValue();
		const FCorner M = Interpolate(A, B, Vertices[E1], Edges[E1].A == A.Vertex ? Fraction : 1 - Fraction);
		const FCorner N = Interpolate(D, C, Vertices[E2], Edges[E2].A == D.Vertex ? Fraction : 1 - Fraction);
		Mesh.DeletePolygon(FPolygonID(Face.ID));
		MakeFace(Mesh, Face.Group, {A, M, N, D}); MakeFace(Mesh, Face.Group, {M, B, C, N});
		CutPairs.Add({M.Vertex, N.Vertex});
	}
	for (const FLoopEdge& E : Plan.Edges) for (int32 End : {E.A, E.B})
	{
		const FEdgeID Segment = Mesh.GetVertexPairEdge(FVertexID(End), FVertexID(Vertices[E.Edge]));
		if (Mesh.Edges().IsValid(Segment)) Attributes.GetEdgeHardnesses()[Segment] = SourceHardness[E.Edge];
	}
	RemoveOrphans(Mesh, ExistingOrphans);
	for (const auto& Pair : CutPairs) NewEdges.Add(Mesh.GetVertexPairEdge(FVertexID(Pair.Key), FVertexID(Pair.Value)).GetValue());
	return true;
}

void MakeCube(FMeshDescription& Mesh)
{
	Mesh.Empty(); FStaticMeshAttributes A(Mesh); A.Register(); A.GetVertexInstanceUVs().SetNumChannels(1);
	const FPolygonGroupID Group = Mesh.CreatePolygonGroup(); A.GetPolygonGroupMaterialSlotNames()[Group] = FName(TEXT("Default"));
	const FVector3f Positions[] = {{-50,-50,-50},{50,-50,-50},{50,50,-50},{-50,50,-50},{-50,-50,50},{50,-50,50},{50,50,50},{-50,50,50}};
	for (const FVector3f& P : Positions) { const FVertexID V = Mesh.CreateVertex(); Mesh.GetVertexPositions()[V] = P; }
	// UE derives normals as Cross(P2-P0, P1-P0); these windings face away from the box center.
	const int32 Faces[6][4] = {{0,1,2,3},{4,7,6,5},{0,4,5,1},{1,5,6,2},{2,6,7,3},{3,7,4,0}};
	const FVector2f UVs[] = {{0,0},{1,0},{1,1},{0,1}};
	for (const auto& Indices : Faces)
	{
		TArray<FCorner> Corners;
		const FVector3f Normal = FVector3f::CrossProduct(Positions[Indices[2]] - Positions[Indices[0]], Positions[Indices[1]] - Positions[Indices[0]]).GetSafeNormal();
		for (int32 I = 0; I < 4; ++I) { FCorner C; C.Vertex = Indices[I]; C.Normal = Normal; C.Tangent = (Positions[Indices[1]] - Positions[Indices[0]]).GetSafeNormal(); C.Sign = 1; C.Color = FVector4f(1,1,1,1); C.UV.Add(UVs[I]); Corners.Add(C); }
		MakeFace(Mesh, Group, Corners);
	}
	for (FEdgeID E : Mesh.Edges().GetElementIDs()) A.GetEdgeHardnesses()[E] = true;
}
}
