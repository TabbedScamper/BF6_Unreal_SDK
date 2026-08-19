// A small 3D preview: renders a single decoded BF6 model in its own lit scene
// with orbit navigation, so the user can inspect a placeable before dropping it
// into the level. Mirrors how the static-mesh editor previews an asset.
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
	// model and frames the camera.
	void ShowModel(const FString& MeshName);
	void ClearModel();

protected:
	virtual TSharedRef<FEditorViewportClient> MakeEditorViewportClient() override;

private:
	TSharedPtr<FAdvancedPreviewScene>            PreviewScene;
	TSharedPtr<FBF6PreviewClient>                Client;
	TObjectPtr<UProceduralMeshComponent>         Mesh;   // lives in the preview world
};
