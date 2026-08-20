// A small 3D preview: renders a single decoded BF6 model in its own lit scene
// with orbit navigation, so the user can inspect a placeable before dropping it
// into the level. Mirrors how the static-mesh editor previews an asset.
//
// Orbiting is handled by our own transparent input layer ON TOP of the
// viewport (drag = spin, wheel = zoom) instead of the viewport client's
// built-in orbit camera: the built-in path needs the scene viewport's input
// routing, which never engages when this widget lives inside a Slate popup
// menu (the library card pop-out) - the camera just sat locked at the iso
// framing.
#pragma once

#include "CoreMinimal.h"
#include "SEditorViewport.h"

class FAdvancedPreviewScene;
class FBF6PreviewClient;
class UProceduralMeshComponent;

class SBF6PreviewViewport : public SEditorViewport
{
public:
	SLATE_BEGIN_ARGS(SBF6PreviewViewport) {}
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);
	virtual ~SBF6PreviewViewport() override;

	// Show a placeable's SDK low-poly model by its 'mesh' name. Clears the previous
	// model and frames the camera at the corner iso.
	void ShowModel(const FString& MeshName);
	void ClearModel();

	// Driven by the orbit input layer.
	void OrbitBy(const FVector2D& CursorDelta);
	void ZoomBy(float WheelDelta);

protected:
	virtual TSharedRef<FEditorViewportClient> MakeEditorViewportClient() override;

private:
	void ApplyCamera();

	TSharedPtr<FAdvancedPreviewScene>            PreviewScene;
	TSharedPtr<FBF6PreviewClient>                Client;
	TObjectPtr<UProceduralMeshComponent>         Mesh;   // lives in the preview world

	// the turntable state our input layer drives
	FVector Center  = FVector::ZeroVector;
	float   Dist    = 300.f;
	float   MinDist = 60.f;
	float   MaxDist = 3000.f;
	float   Yaw     = -135.f;
	float   Pitch   = -35.264f;
};
