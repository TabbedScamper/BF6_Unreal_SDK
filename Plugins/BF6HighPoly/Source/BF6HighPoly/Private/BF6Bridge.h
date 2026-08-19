// Shared access to the libbf6 decode core, so both the browser and the preview
// viewport can build meshes without each duplicating the DLL plumbing.
#pragma once

#include "CoreMinimal.h"

class UProceduralMeshComponent;

// Decode a resource and fill `Mesh` with it, centered at the origin in Unreal
// space (real BF6 scale, cm). Returns false if the resource is not a single mesh
// (e.g. a prefab awaiting the EBX walk). OutRadius is the bounding radius, for
// framing a camera.
bool BF6_ReadMeshInto(UProceduralMeshComponent* Mesh, const FString& ResName, float& OutRadius);

// Load a placeable's SDK low-poly model (by its 'mesh' name) into `Mesh`, centered,
// at real scale. OutRadius = bounding radius for camera framing.
bool BF6_LoadSdkModelInto(UProceduralMeshComponent* Mesh, const FString& MeshName, float& OutRadius);

// Resolve an SDK placeable's short 'mesh' stem (or type) to a full resource id.
FString BF6_ResolvePlaceableRes(const FString& StemOrType);
