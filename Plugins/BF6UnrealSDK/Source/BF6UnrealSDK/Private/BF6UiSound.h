#pragma once

// ============================================================================
// The tool's UI sounds.
//
// One line at each place where the user does something, and nothing else in
// the tool has to know where a sound comes from. That separation is the point:
//
//   the BASE TOOL owns the EVENTS. What happened, when, and the rules about
//   when a sound is allowed (hover throttled, one move sound per drag, silent
//   while a preview is playing). It reads no game files and ships no audio.
//
//   an ADD-ON owns the SOUNDS. It reads the player's own install, decodes the
//   game's UI audio and registers itself as the provider. Without it the tool
//   is silent and every call site still compiles and runs.
//
// So a user with no add-on gets the tool they have today, and a user with one
// gets the game's own voice, from their own install, with nothing shipped.
//
// Settings live in GEditorPerProjectIni under [BF6UnrealSDK]:
//   UiSoundEnabled  (default true)
//   UiSoundVolume   (default 0.8)
// and are reachable from the console as BF6.UiSound 0|1 and BF6.UiSound.Volume.
// BF6.UiSound.Test <event> plays one event by name, which is how you check a
// call site fired without having to guess from the log.
// ============================================================================

#include "CoreMinimal.h"

class USoundWave;

enum class EBF6UiSound : uint8
{
	Place,      // an object was placed
	Delete,     // objects were deleted
	MoveEnd,    // a drag or gizmo move was committed
	Assign,     // a value was assigned through the wizard
	Link,       // a link field was set
	Unlink,     // a link was cleared or a pick was abandoned
	RingOpen,   // the radial opened
	RingClose,  // the radial closed
	Confirm,    // a dialog's primary action
	Cancel,     // a dialog was dismissed
	Error,      // something refused
	Save,       // the map was written
	Select,     // an object was picked in the viewport
	Hover,      // the pointer moved onto something
	Undo,
	Redo,
	ImportDone, // an SDK import finished

	Count
};

namespace BF6UiSound
{
	// What an add-on implements to give the events a voice.
	class IBF6UiSoundProvider
	{
	public:
		virtual ~IBF6UiSoundProvider() {}

		// The sound for one event, or null if this provider has none for it.
		// Called immediately before every play, not once: a provider whose
		// waves are procedural re-arms its buffer here. The object itself is
		// expected to be created once and kept alive by the provider.
		virtual USoundWave* SoundFor(EBF6UiSound Event) = 0;

		// A one-line summary for the log, e.g. "17 event(s) mapped, 15 decoded".
		virtual FString Describe() const { return FString(); }
	};

	// Preview of a placeable's own sound, for the SFX_* rows in the object
	// library. The provider plays it: only it knows whether the sound loops and
	// how to stop it. The tool guarantees one preview at a time and stops the
	// running one before starting another.
	class IBF6PlaceableSoundPreview
	{
	public:
		virtual ~IBF6PlaceableSoundPreview() {}

		// Cheap and synchronous: this is asked while a library row is built.
		// Answer from an index, never by decoding.
		virtual bool CanPreview(const FString& PlaceableType) const = 0;

		// Start. Decoding on first use is fine; it must not block the game
		// thread for long.
		virtual void Preview(const FString& PlaceableType) = 0;

		// Stop whatever is playing. Safe to call when nothing is.
		virtual void Stop() = 0;

		// The type currently playing, empty when nothing is.
		virtual FString Playing() const = 0;
	};

	// ---- what the tool calls ------------------------------------------------

	void Startup();
	void Shutdown();

	// Play one event. Silent with no provider, with sounds turned off, or when
	// the provider has nothing mapped to that event. Never blocks, never warns
	// on a missing sound: a UI sound that fails is not worth a message.
	void Play(EBF6UiSound Event);

	// Hover fires from paint and mouse-move code, so it would otherwise machine
	// gun. This is Play(Hover) with a floor of 120 ms between sounds.
	void Hover();

	// A drag makes one sound at the end, not one per tick. Arm on drag start,
	// and the first Play(MoveEnd) after that fires and disarms itself.
	void ArmMoveEnd();

	// Unreal raises one delegate for undo AND redo, so a hook on it cannot tell
	// them apart. The tool's own redo chord knows, so it plays Redo itself and
	// calls this; the delegate that follows then stays quiet instead of calling
	// the same action an undo.
	void SuppressNextUndo();

	FString Name(EBF6UiSound Event);

	// ---- the add-on seam's back end ----------------------------------------

	void SetProvider(TSharedPtr<IBF6UiSoundProvider> Provider);
	void SetPlaceablePreview(TSharedPtr<IBF6PlaceableSoundPreview> Preview);

	// The object library asks these; they answer false / do nothing with no
	// add-on, which is what keeps the play button off the row.
	bool           CanPreviewPlaceable(const FString& PlaceableType);
	void           TogglePreviewPlaceable(const FString& PlaceableType);
	void           StopPreview();
	FString        PreviewPlaying();

	// ---- settings -----------------------------------------------------------

	bool  IsEnabled();
	void  SetEnabled(bool bEnabled);
	float Volume();
	void  SetVolume(float Volume);
}
