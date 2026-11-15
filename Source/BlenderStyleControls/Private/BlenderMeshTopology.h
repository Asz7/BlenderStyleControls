#pragma once

#include "CoreMinimal.h"
#include "MeshDescription.h"

namespace BlenderMeshTopology
{
struct FExtrusion
{
	TMap<int32, FVector> WorldStarts;
	TMap<int32, FVector> WorldDirections;
	TSet<int32> CapFaces;
	FVector AverageNormal = FVector::ZeroVector;
	FVector AreaNormalSum = FVector::ZeroVector;
};
struct FLoopEdge { int32 Edge; int32 A; int32 B; };
struct FLoopFace { int32 Face; int32 FirstEdge; };
struct FLoopPlan
{
	TArray<FLoopEdge> Edges;
	TArray<FLoopFace> Faces;
};
bool Extrude(FMeshDescription& Mesh, const TSet<int32>& Faces, const FTransform& Transform,
	bool bAlongNormals, FExtrusion& Out, FText& Error);
bool PlanLoop(const FMeshDescription& Mesh, int32 SeedEdge, FLoopPlan& Out, FText& Error);
bool CutLoop(FMeshDescription& Mesh, const FLoopPlan& Plan, double Fraction, TSet<int32>& NewEdges, FText& Error);
void MakeCube(FMeshDescription& Mesh);
}
