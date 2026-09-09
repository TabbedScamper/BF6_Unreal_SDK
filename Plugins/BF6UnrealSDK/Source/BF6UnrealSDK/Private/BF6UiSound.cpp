// ============================================================================
// The tool's UI sounds, implemented.
//
// HOW A SOUND ACTUALLY REACHES THE SPEAKERS. The editor is not in PIE, so
// there is no game world and UGameplayStatics::PlaySound2D has nothing to play
// into: it takes a WorldContextObject, resolves an FAudioDevice from that
// world, and an editor world that is not playing does not have one. The
// editor's own path is UEditorEngine::PlayPreviewSound (Editor/UnrealEd,
// UEditorEngine::PlayPreviewSound(USoundBase*, USoundNode*)), which owns a
// preview UAudioComponent on the editor's audio device and is what the content
// browser uses to audition a sound with no map open. That is what this uses,
// and UEditorEngine::ResetPreviewAudioComponent() is what stops it.
//
// Volume rides on the wave rather than the component: the preview component is
// created inside PlayPreviewSound, so setting a volume on it from out here is a
// race with its own construction. USoundWave::Volume is read when the source
// starts, which is exactly the moment we want it applied.
// ============================================================================

#include "BF6UiSound.h"
#include "BF6Internal.h"

#include "Editor.h"
#include "Editor/EditorEngine.h"
#include "HAL/IConsoleManager.h"
#include "Misc/ConfigCacheIni.h"
#include "Sound/SoundWave.h"

namespace
{
	const TCHAR* kIniSection = TEXT("BF6UnrealSDK");

	TSharedPtr<BF6UiSound::IBF6UiSoundProvider>       GProvider;
	TSharedPtr<BF6UiSound::IBF6PlaceableSoundPreview> GPreview;

	bool   GEnabled = true;
	float  GVolume  = 0.8f;
	bool   GLoaded  = false;

	// Hover fires from paint and move handlers. One every 120 ms is a texture;
	// one per event is a rattle.
	double GLastHoverAt = 0.0;
	constexpr double kHoverFloorSeconds = 0.120;

	// A drag is a stream of transform writes and exactly one commit. The commit
	// site is shared with paths that are not drags, so the sound is armed when a
	// drag starts and the first MoveEnd after that consumes the arming.
	bool GMoveArmed = false;

	// Set by the redo chord, consumed by the undo/redo delegate that follows it.
	bool GSuppressUndo = false;

	// A placeable preview and a UI click share one preview audio component, so
	// a click during a preview would cut the preview off mid-note. While a
	// preview is playing the UI events stand down.
	bool GPreviewActive = false;

	const TCHAR* const kNames[] = {
		TEXT("Place"), TEXT("Delete"), TEXT("MoveEnd"), TEXT("Assign"),
		TEXT("Link"), TEXT("Unlink"), TEXT("RingOpen"), TEXT("RingClose"),
		TEXT("Confirm"), TEXT("Cancel"), TEXT("Error"), TEXT("Save"),
		TEXT("Select"), TEXT("Hover"), TEXT("Undo"), TEXT("Redo"),
		TEXT("ImportDone")
	};
	static_assert(UE_ARRAY_COUNT(kNames) == int32(EBF6UiSound::Count),
		"BF6UiSound: the event names and the enum have drifted apart");

	void LoadSettings()
	{
		if (GLoaded) return;
		GLoaded = true;
		if (!GConfig) return;
		bool bEnabled = true;
		if (GConfig->GetBool(kIniSection, TEXT("UiSoundEnabled"), bEnabled, GEditorPerProjectIni))
			GEnabled = bEnabled;
		float Vol = 0.8f;
		if (GConfig->GetFloat(kIniSection, TEXT("UiSoundVolume"), Vol, GEditorPerProjectIni))
			GVolume = FMath::Clamp(Vol, 0.f, 1.f);
	}

	void SaveSettings()
	{
		if (!GConfig) return;
		GConfig->SetBool(kIniSection, TEXT("UiSoundEnabled"), GEnabled, GEditorPerProjectIni);
		GConfig->SetFloat(kIniSection, TEXT("UiSoundVolume"), GVolume, GEditorPerProjectIni);
		GConfig->Flush(false, GEditorPerProjectIni);
	}

	int32 EventFromName(const FString& Name)
	{
		for (int32 i = 0; i < int32(EBF6UiSound::Count); ++i)
			if (Name.Equals(kNames[i], ESearchCase::IgnoreCase)) return i;
		return INDEX_NONE;
	}

	FAutoConsoleCommand GCmdEnable(
		TEXT("BF6.UiSound"),
		TEXT("BF6.UiSound 0|1 - turn the tool's UI sounds off or on."),
		FConsoleCommandWithArgsDelegate::CreateLambda([](const TArray<FString>& Args)
		{
			if (Args.Num() == 0)
			{
				UE_LOG(LogBF6, Log, TEXT("ui sounds: %s"),
					BF6UiSound::IsEnabled() ? TEXT("on") : TEXT("off"));
				return;
			}
			BF6UiSound::SetEnabled(FCString::Atoi(*Args[0]) != 0);
			UE_LOG(LogBF6, Log, TEXT("ui sounds: %s"),
				BF6UiSound::IsEnabled() ? TEXT("on") : TEXT("off"));
		}));

	FAutoConsoleCommand GCmdVolume(
		TEXT("BF6.UiSound.Volume"),
		TEXT("BF6.UiSound.Volume <0..1> - how loud the tool's UI sounds are."),
		FConsoleCommandWithArgsDelegate::CreateLambda([](const TArray<FString>& Args)
		{
			if (Args.Num() > 0) BF6UiSound::SetVolume(FCString::Atof(*Args[0]));
			UE_LOG(LogBF6, Log, TEXT("ui sound volume: %.2f"), BF6UiSound::Volume());
		}));

	FAutoConsoleCommand GCmdTest(
		TEXT("BF6.UiSound.Test"),
		TEXT("BF6.UiSound.Test <event> - play one event by name, or list them."),
		FConsoleCommandWithArgsDelegate::CreateLambda([](const TArray<FString>& Args)
		{
			if (Args.Num() == 0)
			{
				FString All;
				for (int32 i = 0; i < int32(EBF6UiSound::Count); ++i)
				{
					if (i) All += TEXT(" ");
					All += kNames[i];
				}
				UE_LOG(LogBF6, Log, TEXT("ui sound events: %s"), *All);
				// UE_LOG expands to a braced block, so an if/else around two of
				// them needs its own braces or the else has nothing to attach to.
				if (GProvider.IsValid())
				{
					UE_LOG(LogBF6, Log, TEXT("ui sound provider: %s"), *GProvider->Describe());
				}
				else
				{
					UE_LOG(LogBF6, Log, TEXT("ui sound provider: none, the tool is silent"));
				}
				return;
			}
			const int32 Which = EventFromName(Args[0]);
			if (Which == INDEX_NONE)
			{
				UE_LOG(LogBF6, Warning, TEXT("ui sound: no event called '%s'"), *Args[0]);
				return;
			}
			USoundWave* W = GProvider.IsValid()
				? GProvider->SoundFor(EBF6UiSound(Which)) : nullptr;
			UE_LOG(LogBF6, Log, TEXT("ui sound test: %s -> %s"),
				kNames[Which], W ? *W->GetName() : TEXT("nothing mapped"));
			// Deliberately routed through Play, so the test exercises the same
			// gate the call sites do rather than a private path that always
			// works.
			BF6UiSound::Play(EBF6UiSound(Which));
		}));
}

namespace BF6UiSound
{
	void Startup()
	{
		LoadSettings();
	}

	void Shutdown()
	{
		StopPreview();
		GProvider.Reset();
		GPreview.Reset();
	}

	FString Name(EBF6UiSound Event)
	{
		const int32 i = int32(Event);
		return i >= 0 && i < int32(EBF6UiSound::Count) ? FString(kNames[i]) : FString();
	}

	void Play(EBF6UiSound Event)
	{
		LoadSettings();
		if (!GEnabled || !GProvider.IsValid() || !GEditor) return;
		if (GPreviewActive) return;
		if (Event == EBF6UiSound::Undo && GSuppressUndo)
		{
			GSuppressUndo = false;
			return;
		}
		if (Event == EBF6UiSound::MoveEnd)
		{
			// One per drag. A commit that no drag armed is not a move the user
			// made with the mouse, and it stays silent.
			if (!GMoveArmed) return;
			GMoveArmed = false;
		}
		USoundWave* Wave = GProvider->SoundFor(Event);
		if (!Wave) return;
		Wave->Volume = FMath::Clamp(GVolume, 0.f, 1.f);
		GEditor->PlayPreviewSound(Wave);
	}

	void Hover()
	{
		const double Now = FPlatformTime::Seconds();
		if (Now - GLastHoverAt < kHoverFloorSeconds) return;
		GLastHoverAt = Now;
		Play(EBF6UiSound::Hover);
	}

	void ArmMoveEnd()
	{
		GMoveArmed = true;
	}

	void SuppressNextUndo()
	{
		GSuppressUndo = true;
	}

	void SetProvider(TSharedPtr<IBF6UiSoundProvider> Provider)
	{
		GProvider = Provider;
		if (GProvider.IsValid())
		{
			UE_LOG(LogBF6, Log, TEXT("ui sounds: provider attached, %s"),
				*GProvider->Describe());
		}
		else
		{
			UE_LOG(LogBF6, Log, TEXT("ui sounds: provider detached, the tool is silent"));
		}
	}

	void SetPlaceablePreview(TSharedPtr<IBF6PlaceableSoundPreview> Preview)
	{
		if (GPreview.IsValid() && !Preview.IsValid()) StopPreview();
		GPreview = Preview;
	}

	bool CanPreviewPlaceable(const FString& PlaceableType)
	{
		return GPreview.IsValid() && GPreview->CanPreview(PlaceableType);
	}

	void TogglePreviewPlaceable(const FString& PlaceableType)
	{
		if (!GPreview.IsValid()) return;
		// A second click on the row that is playing stops it. Anything else
		// stops what was playing first, so two previews can never overlap.
		if (GPreview->Playing() == PlaceableType) { StopPreview(); return; }
		StopPreview();
		if (!GPreview->CanPreview(PlaceableType)) return;
		GPreviewActive = true;
		GPreview->Preview(PlaceableType);
	}

	void StopPreview()
	{
		GPreviewActive = false;
		if (GPreview.IsValid()) GPreview->Stop();
	}

	FString PreviewPlaying()
	{
		return GPreview.IsValid() ? GPreview->Playing() : FString();
	}

	bool IsEnabled() { LoadSettings(); return GEnabled; }

	void SetEnabled(bool bEnabled)
	{
		LoadSettings();
		GEnabled = bEnabled;
		if (!GEnabled) StopPreview();
		SaveSettings();
	}

	float Volume() { LoadSettings(); return GVolume; }

	void SetVolume(float InVolume)
	{
		LoadSettings();
		GVolume = FMath::Clamp(InVolume, 0.f, 1.f);
		SaveSettings();
	}
}
