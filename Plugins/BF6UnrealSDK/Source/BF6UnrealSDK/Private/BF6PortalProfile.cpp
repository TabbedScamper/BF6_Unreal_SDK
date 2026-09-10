#include "BF6PortalProfile.h"
#include "BF6PortalWeb.h"
#include "BF6PortalSettings.h"   // the settings half of a whole-experience file
#include "BF6Script.h"    // session hooks: the script editor journals while signed out
#include "BF6Blocks.h"    // session hooks: the block editor journals while signed out
#include "BF6BuildMode.h"
#include "BF6EditorOverlay.h"   // taking the editor sheet down when a map opens
#include "BF6Project.h"         // the per-save record, and the Godot scene offer
#include "BF6Internal.h"
#include "BF6Theme.h"

#include "Containers/Ticker.h"
#include "DesktopPlatformModule.h"
#include "Dom/JsonObject.h"
#include "Editor.h"
#include "Engine/Texture2D.h"
#include "HAL/PlatformApplicationMisc.h"
#include "IDesktopPlatform.h"
#include "IImageWrapper.h"
#include "IImageWrapperModule.h"
#include "Modules/ModuleManager.h"
#include "Rendering/DrawElements.h"
#include "TextureResource.h"   // FTexture2DMipMap, to fill a transient texture
#include "UnrealClient.h"
#include "Framework/Application/SlateApplication.h"
#include "HAL/FileManager.h"
#include "HAL/IConsoleManager.h"
#include "HttpModule.h"
#include "ImageUtils.h"
#include "Interfaces/IHttpRequest.h"
#include "Interfaces/IHttpResponse.h"
#include "Internationalization/Regex.h"
#include "Misc/Base64.h"
#include "Misc/ConfigCacheIni.h"
#include "Misc/DateTime.h"
#include "Misc/FileHelper.h"
#include "Misc/MessageDialog.h"   // the push asks before it replaces a live experience
#include "Misc/Paths.h"
#include "Misc/SecureHash.h"     // FMD5, for a stable id for a file with none
#include "Widgets/Input/SEditableTextBox.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "Styling/CoreStyle.h"
#include "Styling/SlateTypes.h"
#include "Brushes/SlateColorBrush.h"
#include "Brushes/SlateRoundedBoxBrush.h"
#include "Styling/SlateBrush.h"
#include "UObject/Package.h"
#include "UObject/StrongObjectPtr.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Images/SImage.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SScrollBox.h"
#include "Widgets/Input/SComboButton.h"
#include "Widgets/Layout/SSpacer.h"
#include "Widgets/Layout/SWrapBox.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/SCompoundWidget.h"
#include "Widgets/SNullWidget.h"
#include "Widgets/Text/STextBlock.h"

// ============================================================================
// See BF6PortalProfile.h for what this is. Layout of this file:
//   1. protobuf and gRPC-web readers (the site speaks grpc-web+proto)
//   2. state, config and the on-disk cache
//   3. thumbnails
//   4. page checks (ExpectPage)
//   5. the capture handlers: getOwnedPlayElementsV2 and getPlayElement
//   6. import: one save per map in the rotation
//   7. the tool's own widgets
//   8. the bridge object, console commands, module hooks
// ============================================================================

namespace
{
	// ---- 1. protobuf ---------------------------------------------------------
	//
	// A generic reader, because the site's .proto is not ours and the field
	// NUMBERS are the only contract we verified. Everything below reads by
	// number and validates what it finds; nothing assumes a shape.

	struct FPbView
	{
		const uint8* P = nullptr;
		int32        N = 0;
		bool IsValid() const { return P != nullptr && N > 0; }
	};

	struct FPbField
	{
		uint32  Num  = 0;
		uint8   Wire = 0;
		uint64  Var  = 0;
		FPbView Bytes;
	};

	bool PbVarint(const uint8*& P, const uint8* E, uint64& Out)
	{
		Out = 0;
		int32 Shift = 0;
		while (P < E && Shift < 64)
		{
			const uint8 B = *P++;
			Out |= (uint64)(B & 0x7F) << Shift;
			if (!(B & 0x80)) return true;
			Shift += 7;
		}
		return false;
	}

	bool PbNext(const uint8*& P, const uint8* E, FPbField& F)
	{
		uint64 Key;
		if (!PbVarint(P, E, Key)) return false;
		F.Num = (uint32)(Key >> 3);
		F.Wire = (uint8)(Key & 7);
		F.Var = 0;
		F.Bytes = FPbView();
		if (F.Num == 0) return false;
		switch (F.Wire)
		{
		case 0:
			return PbVarint(P, E, F.Var);
		case 1:
			if (E - P < 8) return false;
			FMemory::Memcpy(&F.Var, P, 8); P += 8; return true;
		case 5:
		{
			if (E - P < 4) return false;
			uint32 V32 = 0; FMemory::Memcpy(&V32, P, 4); F.Var = V32; P += 4; return true;
		}
		case 2:
		{
			uint64 L;
			if (!PbVarint(P, E, L)) return false;
			if ((uint64)(E - P) < L) return false;
			F.Bytes.P = P; F.Bytes.N = (int32)L; P += L; return true;
		}
		default:
			return false;   // groups (3, 4): not used by this wire format
		}
	}

	// The first length-delimited field with this number, as a submessage.
	bool PbSub(const FPbView& M, uint32 Num, FPbView& Out)
	{
		if (!M.IsValid()) return false;
		const uint8* P = M.P; const uint8* E = M.P + M.N;
		FPbField F;
		while (PbNext(P, E, F))
			if (F.Num == Num && F.Wire == 2) { Out = F.Bytes; return true; }
		return false;
	}

	void PbAll(const FPbView& M, uint32 Num, TArray<FPbView>& Out)
	{
		if (!M.IsValid()) return;
		const uint8* P = M.P; const uint8* E = M.P + M.N;
		FPbField F;
		while (PbNext(P, E, F))
			if (F.Num == Num && F.Wire == 2) Out.Add(F.Bytes);
	}

	bool PbVar(const FPbView& M, uint32 Num, uint64& Out)
	{
		if (!M.IsValid()) return false;
		const uint8* P = M.P; const uint8* E = M.P + M.N;
		FPbField F;
		while (PbNext(P, E, F))
			if (F.Num == Num && F.Wire == 0) { Out = F.Var; return true; }
		return false;
	}

	// Bytes as text, and ONLY when they really are text. A submessage read as a
	// string would be noise in the log and never a name, so control bytes and
	// invalid UTF-8 both come back empty.
	FString PbText(const FPbView& V)
	{
		if (!V.IsValid()) return FString();
		for (int32 i = 0; i < V.N; i++)
		{
			const uint8 C = V.P[i];
			if (C < 0x09 || (C > 0x0D && C < 0x20) || C == 0x7F) return FString();
		}
		FUTF8ToTCHAR Conv((const ANSICHAR*)V.P, V.N);
		FString S(Conv.Length(), Conv.Get());
		// A lone replacement character means the bytes were not UTF-8 after all.
		return S.Contains(FString(TEXT("\xFFFD"))) ? FString() : S;
	}

	FString PbStr(const FPbView& M, uint32 Num)
	{
		FPbView V;
		return PbSub(M, Num, V) ? PbText(V) : FString();
	}

	// M.A.B, the shape the site uses for every optional scalar.
	FString PbStr2(const FPbView& M, uint32 A, uint32 B)
	{
		FPbView V;
		return PbSub(M, A, V) ? PbStr(V, B) : FString();
	}

	int64 PbVar2(const FPbView& M, uint32 A, uint32 B)
	{
		FPbView V; uint64 Out = 0;
		if (PbSub(M, A, V) && PbVar(V, B, Out)) return (int64)Out;
		return 0;
	}

	// gRPC-web framing: 1 byte flags, 4 byte big-endian length, payload.
	// A frame with flags & 0x80 is the trailer and carries no message.
	void ForEachGrpcFrame(const TArray<uint8>& Body, TFunctionRef<void(const FPbView&)> Fn)
	{
		int32 Off = 0;
		while (Off + 5 <= Body.Num())
		{
			const uint8 Flags = Body[Off];
			const uint32 Len =
				((uint32)Body[Off + 1] << 24) | ((uint32)Body[Off + 2] << 16) |
				((uint32)Body[Off + 3] << 8)  | ((uint32)Body[Off + 4]);
			Off += 5;
			if ((int64)Off + (int64)Len > (int64)Body.Num()) break;
			if (!(Flags & 0x80) && Len > 0)
			{
				FPbView V; V.P = Body.GetData() + Off; V.N = (int32)Len;
				Fn(V);
			}
			Off += (int32)Len;
		}
	}

	// ---- 2. state ------------------------------------------------------------

	const TCHAR* kIniSection = TEXT("BF6UnrealSDK");
	const TCHAR* kBridgeName = TEXT("bf6portal");
	const TCHAR* kScriptId   = TEXT("bf6portalcapture");
	const int32  kUuidLen    = 36;

	using BF6PortalProfile::EState;
	using BF6PortalProfile::FExperienceRow;
	using BF6PortalProfile::FRotationRow;

	struct FAttachment
	{
		FString FileName;
		FString Path;         // where we wrote it
		// The site's own id for this attachment. Kept because the site's export
		// carries it and a document written without it is not the same document.
		FString AttId;
		FString Version;      // "123" on a spatial, empty on the rest
		int32   Kind = 0;     // 1 spatial json, 2 typescript, 3 blacklist, 4 strings
		int32   MapIdx = -1;
		// An attachment the site keeps with nothing in it, which both real
		// exports have (an empty blank.ts and an empty blacklist.json). It has
		// no file on disk and it still belongs in the document.
		bool    bEmpty = false;
	};

	struct FExp
	{
		FString Id, Name, Description, ThumbUrl, ScrapedThumbUrl;
		// THE SECOND ID. A getPlayElement response carries two: the outer header
		// names the experience that was asked for, and the play element nested
		// inside it is that experience's editable REVISION, with an id of its
		// own and a name like "Remix for Coupe(<experience name>)". Keying the
		// parsed result by the inner id is what made a perfectly good 1.9 MB
		// response arrive, parse, and then be reported as a failed import: the
		// importer waited for the id it had asked for and never saw it. The
		// experience is keyed by the OUTER id; the revision id is kept here
		// because the api wants it where it wants it.
		FString RevisionId, RevisionName;
		// The map rotation as the site writes it, "MP_Isolated-ModBuilderCustom0".
		// Maps below is the codename half of each, which is what opens a level.
		TArray<FString> RotationIds;
		// The experience's own mutator values, in the shape the site's whole
		// experience export uses, serialised so the cache can hold them.
		FString MutatorsJson;
		// The game mode the site's own export names, so a file written back out
		// says what came in rather than a default.
		FString GameMode = TEXT("ModBuilderCustom");
		// What the SITE last had, so a push can find the old value in the site's
		// own message and swap it for the new one. Never a guess: a field with
		// no remembered old value is not pushed.
		FString SiteName, SiteDescription;
		// The address the site ACTUALLY landed on after its own card was
		// clicked, and only when that address carries the uuid. An address that
		// does not name the experience is not reusable: the site keeps the open
		// experience in app state, so going back to it later would open
		// whatever was open last, not this one.
		FString OpenUrl;
		int64   UpdatedUnix = 0;
		// WHERE THE SITE PUTS IT IN THE LIST. Remembered on disk, because the
		// cache is read by walking a folder and a folder comes back in
		// alphabetical uuid order, which is no order at all. Without this the
		// tiles rearrange themselves the moment the list is rebuilt from cache
		// rather than fetched, and an experience is hard to find twice.
		// -1 means the site has never said where this one goes.
		int32   SiteOrder = -1;
		bool    bFetched = false;
		// bFetched means "the site has answered for this one AT SOME POINT",
		// and it is saved to the profile, so it is already true the moment the
		// editor starts. The import loop needs a different fact - "the answer
		// for the attempt running RIGHT NOW has landed" - and reading bFetched
		// for it made an import finish 123 ms after opening the browser, on the
		// previous run's cached rotation, before the site had said anything.
		// That is what reported "0 imported, 1 without map data, 0 failed" for
		// an experience that does have a map. Transient on purpose: never
		// serialized, cleared at the start of every attempt.
		bool    bFetchedThisAttempt = false;
		TArray<FString>      Maps;        // the rotation, by codename
		TArray<FAttachment>  Files;
		TArray<FString>      SaveNames;   // parallel to Maps; empty where not imported

		// WHAT THE SITE IS COMPLAINING ABOUT, for this experience.
		//
		// The tool works from a downloaded copy rather than driving the site's
		// own editors, which costs the one thing that page gave for free: the
		// site telling you the rules are wrong. So its warnings are kept here
		// and shown beside the experience they belong to.
		//
		// Cleared when the experience is imported again, because a warning
		// about the version that was on the site is not a warning about the one
		// that just came down.
		TArray<FString>      Notices;
		FDateTime            NoticesAt = FDateTime(0);
	};

	TArray<FExp>            GExps;
	TMap<FString, int32>    GExpIdx;      // id -> index into GExps
	EState                  GState = EState::Unlinked;
	FString                 GAccount;
	FString                 GPageStatus = TEXT("No page checked yet.");
	FString                 GWorkStatus;
	// While the tool is driving the page, the probe's silence means the page is
	// busy, not that the session went. An import job sets its own flag; this
	// covers the shorter jobs that have no flag of their own, and is pushed
	// forward rather than predicted, so nothing has to guess a duration.
	double                  GBusyUntil = 0.0;
	// True only while the startup check is asking the site whether an existing
	// session is still good. See ResumeLinkQuietly.
	bool                    GResumingLink = false;
	bool                    GLastOwnedListShown = false;
	int32                   GResumeReports = 0;
	void NoteWorking(double Seconds = 30.0)
	{
		const double Until = FPlatformTime::Seconds() + Seconds;
		if (Until > GBusyUntil) GBusyUntil = Until;
	}
	FString                 GLastKind, GLastUrl;
	// Where the tool last ASKED the page to go, and where it actually ended up.
	// The two disagreeing is the whole diagnosis of a failed import.
	FString                 GNavTarget, GNavLanded;
	// The address shape the site really uses for an open experience, learned
	// from a landed address rather than guessed. Empty until one is seen.
	FString                 GUrlPattern;
	bool                    GLoggedPattern = false;
	bool                    GHasOwnedList = false;
	bool                    GSiteShown = true;
	// The WebPlay methods the page has watched the site issue this session. A
	// method in here can be REPLAYED, which is the tool's normal path; a method
	// that is not is the one case where the site's own pages still have to be
	// driven. Reported by the page probe, never inferred.
	TSet<FString>           GApiSeen;
	FString                 GApiNote;
	// Signing in. The panel covers the editor while it happens and gets out of
	// the way the moment it is done. This flag is what makes the page reports
	// read as "signing in" rather than as ordinary navigation; it is NOT what
	// decides whether the surface comes down, because a take-down that only
	// happens when a flag survived is a take-down that silently does not.
	bool                    GLinkInProgress = false;
	// Watching the site for live changes. Off by default: it observes and never
	// writes, but it is still the user's choice whether the tool follows along.
	bool                    GWatch = false;
	FString                 GWatchStatus = TEXT("not watching");
	FString                 GWatchLastChange;
	double                  GWatchLastAt = 0.0;
	// The panel's search box, and what a push last did.
	FString                 GSearch;
	FString                 GPushStatus;
	// Keeping the tool and the site in step. On by default; it stands down while
	// the session is signed out and picks the deferred experience back up when
	// the session returns.
	bool                    GAutoSync = true;
	FString                 GPendingSyncId;
	// AUTO-SYNC MUST NOT CHASE ITS OWN TAIL.
	//
	// Importing eleven maps writes eleven saves, and importing a map opens a
	// level, which pumps the editor, which runs this module's ticker, which saw
	// those writes as the user saving and fired a sync per map. Five fetches
	// then raced the page ("replay failed: TypeError: Failed to fetch") and the
	// session was lost and recovered for no reason at all.
	//
	// So a run suspends auto-sync for its whole duration, and every save made
	// while it is suspended is the tool's own. Counted rather than a flag,
	// because a push can happen inside a sync.
	int32                   GSyncSuspend = 0;
	struct FSyncGuard
	{
		FSyncGuard() { GSyncSuspend++; }
		~FSyncGuard() { GSyncSuspend = FMath::Max(0, GSyncSuspend - 1); }
	};
	// Which experience the tool asked the site's own Export for, so the document
	// that comes back lands on it rather than on whatever its name resolves to.
	FString                 GExportWantId;
	double                  GLastSyncAt = 0.0;
	bool                    GLoggedOwnedLayout = false;
	uint32                  GFingerprint = 1;
	// What one card draws, worked out when the card fingerprint is recomputed
	// and read by the card's bound attributes. Never recomputed per frame.
	struct FCardView
	{
		FString Sub;
		bool    bImported = false;
	};
	TMap<FString, FCardView> GCardView;
	// The card fingerprint and its cache. See ListFingerprint below.
	uint32                  GListHash = 1;
	uint32                  GListSourceFp = 0;
	double                  GListHashAt = 0.0;
	int32                   GGridRebuilds = 0;
	TArray<IConsoleObject*> GCmds;
	FTSTicker::FDelegateHandle GTick;
	TStrongObjectPtr<UBF6PortalBridge> GBridge;
	FDelegateHandle         GUrlHandle, GLoadHandle;

	// ---- session loss and recovery ------------------------------------------
	//
	// Four ways the site tells us it signed the user out, and one way it tells
	// us nothing at all: the page going quiet. All five land in NoteSessionLost,
	// which is the only place the tool decides a session is gone.
	enum class ERecovery : uint8 { Idle, Reloading, WaitingForUser };

	BF6PortalProfile::FBF6PortalSessionEvent GOnLost, GOnRestored;
	bool      GLost = false;
	FString   GLossReason;
	FString   GRecoveryResult = TEXT("no recovery has been needed yet");
	FString   GBanner;
	ERecovery GRecovery = ERecovery::Idle;
	int32     GRecoveryTries = 0;
	double    GRecoveryDeadline = 0.0;
	double    GNextRetryAt = 0.0;
	// Where to put the page back once the session returns: the last page the
	// tool deliberately went to, so recovery lands where the work was.
	FString   GReturnUrl;
	FString   GReturnKey;
	// The page's heartbeat. Twenty seconds of silence from a page that WAS
	// answering is a sign-out (or a dead renderer), and either way the editors
	// must stop pushing.
	double    GLastProbeAt = 0.0;
	// When the tool last asked a quiet page to speak, so it asks once and then
	// waits for an answer rather than asking every quarter second.
	double    GPokeSentAt = 0.0;
	bool      GKeepAlive = true;
	double    GLastKeepAliveAt = 0.0;
	bool      GKeepAliveGaveUpSaid = false;
	// How often to ping, and how long an idle editor keeps being worth pinging
	// for. Six hours because that is roughly how long the site itself holds a
	// session on an untouched page: giving up sooner than the thing we are
	// keeping alive is what made this a bug.
	constexpr double kKeepAliveEverySecs  = 240.0;
	constexpr double kKeepAliveGiveUpSecs = 6.0 * 3600.0;

	// ---- the experience thumbnail's working set -----------------------------
	// Declared up here with the rest of the state because the capture handler
	// writes the upload result into it, and that runs long before the panel
	// that fills the rest of it is ever built.
	struct FThumbWork
	{
		TArray<FColor> Src;                 // BGRA, the capture or the loaded file
		int32   SrcW = 0, SrcH = 0;
		FString SrcWhat;                    // "the map viewport" or the file name
		// The crop frame over the source. Zoom 1 is the largest 352 x 248 frame
		// that fits; higher zooms in.
		float   CenterU = 0.5f, CenterV = 0.5f, Zoom = 1.f;
		TStrongObjectPtr<UTexture2D> SrcTex, OutTex;
		TSharedPtr<FSlateBrush>      SrcBrush, OutBrush;
		TArray<uint8> Jpeg;
		int32   Quality = 0;
		FString Path;
		FString Status = TEXT("Nothing captured yet. SCREENSHOT MAP takes the view, or UPLOAD IMAGE takes a file.");
		FString UploadedUrl, VerifyUrl, VerifyState, SetState;
		TArray<TPair<FString, FString>> SiteOptions;   // pre-approved images: src, label
	};
	FThumbWork GThumb;

	// Defined below; SetState needs to be able to finish a recovery the moment
	// the evidence rule flips the profile back to Linked.
	void NoteSessionLost(const FString& Why);
	void FinishRecovery(const FString& How);
	// Defined with the thumbnail panel further down; the watch journal needs to
	// know which experience it is journalling for, and runs before that point.
	FString CurrentExperienceId();

	void Bump() { GFingerprint++; }

	FString PortalRoot()
	{
		return FPaths::ConvertRelativePathToFull(FPaths::Combine(
			FPaths::ProjectSavedDir(), TEXT("BF6UnrealSDK"), TEXT("portal"), TEXT("experiences")));
	}
	FString ExpDir(const FString& Id) { return FPaths::Combine(PortalRoot(), Id); }

	// The tool's own session file for a save, in either layout. Mirrors
	// BF6PortalWeb's copy of the same path, which mirrors BF6UnrealSDK's, which
	// is file-local there. It is read here for one reason: its timestamp is how
	// this file notices that the user saved, without a hook in the save path.
	FString ToolSessionPath(const FString& Level, const FString& Save)
	{
		const FString Root = FPaths::ConvertRelativePathToFull(
			FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("BF6UnrealSDK")));
		const FString New = FPaths::Combine(Root, TEXT("saves"), Save, Level + TEXT(".json"));
		if (FPaths::FileExists(New)) return New;
		const FString Old = FPaths::Combine(Root, Level, Save + TEXT(".json"));
		return FPaths::FileExists(Old) ? Old : FString();
	}
	TMap<FString, FDateTime> GSaveStamps;
	double GLastSaveScanAt = 0.0;

	// Everything that came off the site (or out of an imported file) is written
	// twice: once as the working file the tool edits, and once as ".orig", which
	// is never touched again. The pair is what makes a push possible without
	// composing a message: the old text is what to FIND in the site's own last
	// request, and the new text is what to put there. No old text, no push for
	// that field, and nothing is guessed.
	//
	// UTF-8 without a BOM, because that is what the level importer and the block
	// editor already read, and because the default encoding would write UTF-16
	// the moment a name carried an accent.
	bool WriteWithOriginal(const FString& Path, const FString& Text)
	{
		if (!FFileHelper::SaveStringToFile(Text, *Path, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM)) return false;
		FFileHelper::SaveStringToFile(Text, *(Path + TEXT(".orig")), FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
		return true;
	}

	FString JsonToString(const TSharedPtr<FJsonObject>& O)
	{
		FString Out;
		if (!O.IsValid()) return Out;
		TSharedRef<TJsonWriter<>> W = TJsonWriterFactory<>::Create(&Out);
		FJsonSerializer::Serialize(O.ToSharedRef(), W);
		return Out;
	}

	TSharedPtr<FJsonObject> JsonFromString(const FString& Text)
	{
		TSharedPtr<FJsonObject> Out;
		if (Text.IsEmpty()) return Out;
		TSharedRef<TJsonReader<>> R = TJsonReaderFactory<>::Create(Text);
		if (!FJsonSerializer::Deserialize(R, Out)) Out.Reset();
		return Out;
	}

	FString ReadTextFile(const FString& Path)
	{
		FString Out;
		return FFileHelper::LoadFileToString(Out, *Path) ? Out : FString();
	}

	// The working file and its untouched twin, for one push pair.
	bool ChangedSinceSite(const FString& Path, FString& OutOld, FString& OutNew)
	{
		OutOld = ReadTextFile(Path + TEXT(".orig"));
		OutNew = ReadTextFile(Path);
		return !OutOld.IsEmpty() && !OutNew.IsEmpty() && OutOld != OutNew;
	}

	bool IsUuid(const FString& S)
	{
		if (S.Len() != kUuidLen) return false;
		const FRegexPattern P(TEXT("^[0-9a-fA-F]{8}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{12}$"));
		FRegexMatcher M(P, S);
		return M.FindNext();
	}

	FString FindUuid(const FString& In)
	{
		const FRegexPattern P(TEXT("[0-9a-fA-F]{8}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{12}"));
		FRegexMatcher M(P, In);
		return M.FindNext() ? M.GetCaptureGroup(0).ToLower() : FString();
	}

	FString ExperienceUrl(const FString& Id, const TCHAR* Section)
	{
		// teams=1%2C2, the team list, exactly as the site writes it when the
		// experience is opened from its own card. teams=0 lands on the login page.
		return FString::Printf(TEXT("%s/experience/%s?id=%s&teams=1%%2C2"), *BF6PortalWeb::BaseUrl(), Section, *Id);
	}

	FExp* Find(const FString& Id)
	{
		const int32* At = GExpIdx.Find(Id);
		return At && GExps.IsValidIndex(*At) ? &GExps[*At] : nullptr;
	}

	FExp& FindOrAdd(const FString& Id)
	{
		if (FExp* E = Find(Id)) return *E;
		GExpIdx.Add(Id, GExps.Num());
		FExp& E = GExps.AddDefaulted_GetRef();
		E.Id = Id;
		return E;
	}

	// A name becomes a FOLDER on disk, so the filesystem has a veto. Same rules
	// the tool's own save validator uses, applied silently instead of refused:
	// the name came off the site and the user cannot retype it here.
	FString Sanitise(const FString& In)
	{
		FString Out;
		Out.Reserve(In.Len());
		const FString Bad = TEXT("\\/:*?\"<>|");
		for (const TCHAR C : In)
		{
			int32 Ignore;
			if (C < 32 || Bad.FindChar(C, Ignore)) { Out.AppendChar(TEXT(' ')); continue; }
			Out.AppendChar(C);
		}
		while (Out.Contains(TEXT("  "))) Out.ReplaceInline(TEXT("  "), TEXT(" "));
		Out.TrimStartAndEndInline();
		while (Out.EndsWith(TEXT(".")) || Out.EndsWith(TEXT(" "))) Out.LeftChopInline(1);
		if (Out.Len() > 100) Out.LeftInline(100);
		Out.TrimStartAndEndInline();
		while (Out.EndsWith(TEXT(".")) || Out.EndsWith(TEXT(" "))) Out.LeftChopInline(1);
		return Out.IsEmpty() ? FString(TEXT("Portal experience")) : Out;
	}

	// ---- BF6Project ----
	// ONE FOLDER PER EXPERIENCE, ITS MAPS INSIDE IT.
	//
	// An experience is one game mode: one script project, one settings set, one
	// thumbnail, one workspace, and many maps. This used to name every map of
	// it as a separate top-level save - "Night Ops Breakthrough copy
	// (MP_Isolated)" eleven times over - and the shared half was duplicated
	// eleven times with it.
	//
	// Now every map of an experience answers to the SAME save name, which is
	// the experience's folder, and the level tells them apart inside it. The
	// tool's paths were all (Level, SaveName) already, so this needed no new
	// shape anywhere: RESUME under a map card now reads as the game mode.
	//
	// The old per-map name is still what an unmigrated save on disk is called,
	// which is why LegacySaveNameFor is kept and still searched.
	FString LegacySaveNameFor(const FExp& E, int32 MapIdx)
	{
		const FString Map = E.Maps.IsValidIndex(MapIdx) ? E.Maps[MapIdx] : FString(TEXT("map"));
		return Sanitise(FString::Printf(TEXT("%s (%s)"), *E.Name, *Map));
	}

	FString SaveNameFor(const FExp& E, int32 MapIdx)
	{
		const FString Folder = BF6Project::EnsureExperience(E.Id, E.Name);
		return Folder.IsEmpty() ? LegacySaveNameFor(E, MapIdx) : Folder;
	}
	// ---- end BF6Project ----

	// ---- the on-disk cache ---------------------------------------------------
	//
	// The experience list, its rotation and which save belongs to which slot,
	// written next to the files we downloaded. It is the user's own data on the
	// user's own machine, and it is what makes the map screen show MY
	// EXPERIENCES before the panel has even been opened this session.

	void WriteCache(const FExp& E)
	{
		TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
		Root->SetStringField(TEXT("id"), E.Id);
		Root->SetStringField(TEXT("name"), E.Name);
		Root->SetStringField(TEXT("description"), E.Description);
		Root->SetStringField(TEXT("thumbUrl"), E.ThumbUrl);
		Root->SetStringField(TEXT("thumbScraped"), E.ScrapedThumbUrl);
		Root->SetStringField(TEXT("openUrl"), E.OpenUrl);
		Root->SetStringField(TEXT("revisionId"), E.RevisionId);
		Root->SetStringField(TEXT("revisionName"), E.RevisionName);
		Root->SetStringField(TEXT("gameMode"), E.GameMode);
		Root->SetStringField(TEXT("siteName"), E.SiteName);
		Root->SetStringField(TEXT("siteDescription"), E.SiteDescription);
		Root->SetNumberField(TEXT("updated"), (double)E.UpdatedUnix);
		Root->SetNumberField(TEXT("siteOrder"), (double)E.SiteOrder);
		Root->SetBoolField(TEXT("fetched"), E.bFetched);
		TArray<TSharedPtr<FJsonValue>> Rot;
		for (int32 i = 0; i < E.Maps.Num(); i++)
		{
			TSharedPtr<FJsonObject> R = MakeShared<FJsonObject>();
			R->SetNumberField(TEXT("mapIdx"), i);
			R->SetStringField(TEXT("map"), E.Maps[i]);
			R->SetStringField(TEXT("rotationId"), E.RotationIds.IsValidIndex(i) ? E.RotationIds[i] : FString());
			R->SetStringField(TEXT("save"), E.SaveNames.IsValidIndex(i) ? E.SaveNames[i] : FString());
			bool bSpatial = false;
			for (const FAttachment& A : E.Files) if (A.Kind == 1 && A.MapIdx == i) bSpatial = true;
			R->SetBoolField(TEXT("spatial"), bSpatial);
			Rot.Add(MakeShared<FJsonValueObject>(R));
		}
		Root->SetArrayField(TEXT("rotation"), Rot);
		// The attachments, so a cold start can export or push an experience
		// without having to fetch it again: the files are already on disk beside
		// this one, and this is what says which is which.
		TArray<TSharedPtr<FJsonValue>> Files;
		for (const FAttachment& A : E.Files)
		{
			TSharedPtr<FJsonObject> F = MakeShared<FJsonObject>();
			F->SetStringField(TEXT("file"), A.FileName);
			F->SetStringField(TEXT("attId"), A.AttId);
			F->SetStringField(TEXT("version"), A.Version);
			F->SetNumberField(TEXT("kind"), A.Kind);
			F->SetNumberField(TEXT("mapIdx"), A.MapIdx);
			F->SetBoolField(TEXT("empty"), A.bEmpty);
			Files.Add(MakeShared<FJsonValueObject>(F));
		}
		Root->SetArrayField(TEXT("attachments"), Files);
		if (!E.MutatorsJson.IsEmpty())
		{
			if (const TSharedPtr<FJsonObject> M = JsonFromString(E.MutatorsJson)) Root->SetObjectField(TEXT("mutators"), M);
		}
		FString Out;
		TSharedRef<TJsonWriter<>> W = TJsonWriterFactory<>::Create(&Out);
		FJsonSerializer::Serialize(Root.ToSharedRef(), W);
		IFileManager::Get().MakeDirectory(*ExpDir(E.Id), true);
		FFileHelper::SaveStringToFile(Out, *FPaths::Combine(ExpDir(E.Id), TEXT("experience.json")));
	}

	void ReadCacheAll()
	{
		TArray<FString> Dirs;
		IFileManager::Get().FindFiles(Dirs, *(PortalRoot() / TEXT("*")), false, true);
		for (const FString& D : Dirs)
		{
			if (!IsUuid(D)) continue;
			FString In;
			if (!FFileHelper::LoadFileToString(In, *FPaths::Combine(ExpDir(D), TEXT("experience.json")))) continue;
			TSharedPtr<FJsonObject> Root;
			TSharedRef<TJsonReader<>> R = TJsonReaderFactory<>::Create(In);
			if (!FJsonSerializer::Deserialize(R, Root) || !Root.IsValid()) continue;
			FExp& E = FindOrAdd(D.ToLower());
			Root->TryGetStringField(TEXT("name"), E.Name);
			Root->TryGetStringField(TEXT("description"), E.Description);
			Root->TryGetStringField(TEXT("thumbUrl"), E.ThumbUrl);
			Root->TryGetStringField(TEXT("thumbScraped"), E.ScrapedThumbUrl);
			Root->TryGetStringField(TEXT("openUrl"), E.OpenUrl);
			Root->TryGetStringField(TEXT("revisionId"), E.RevisionId);
			Root->TryGetStringField(TEXT("revisionName"), E.RevisionName);
			Root->TryGetStringField(TEXT("gameMode"), E.GameMode);
			Root->TryGetStringField(TEXT("siteName"), E.SiteName);
			Root->TryGetStringField(TEXT("siteDescription"), E.SiteDescription);
			if (E.GameMode.IsEmpty()) E.GameMode = TEXT("ModBuilderCustom");
			double U = 0; if (Root->TryGetNumberField(TEXT("updated"), U)) E.UpdatedUnix = (int64)U;
			double SO = -1; if (Root->TryGetNumberField(TEXT("siteOrder"), SO)) E.SiteOrder = (int32)SO;
			Root->TryGetBoolField(TEXT("fetched"), E.bFetched);
			const TArray<TSharedPtr<FJsonValue>>* Rot = nullptr;
			if (Root->TryGetArrayField(TEXT("rotation"), Rot))
			{
				for (const auto& RV : *Rot)
				{
					const TSharedPtr<FJsonObject> O = RV->AsObject(); if (!O.IsValid()) continue;
					FString Map, Save, RotId;
					O->TryGetStringField(TEXT("map"), Map);
					O->TryGetStringField(TEXT("save"), Save);
					O->TryGetStringField(TEXT("rotationId"), RotId);
					E.Maps.Add(Map);
					E.SaveNames.Add(Save);
					E.RotationIds.Add(RotId.IsEmpty()
						? FString::Printf(TEXT("%s-%s0"), *Map, *E.GameMode) : RotId);
				}
			}
			const TArray<TSharedPtr<FJsonValue>>* Files = nullptr;
			if (Root->TryGetArrayField(TEXT("attachments"), Files))
			{
				for (const auto& FV : *Files)
				{
					const TSharedPtr<FJsonObject> O = FV->AsObject(); if (!O.IsValid()) continue;
					FAttachment A;
					O->TryGetStringField(TEXT("file"), A.FileName);
					O->TryGetStringField(TEXT("attId"), A.AttId);
					O->TryGetStringField(TEXT("version"), A.Version);
					double K = 0, MI = -1;
					O->TryGetNumberField(TEXT("kind"), K);
					O->TryGetNumberField(TEXT("mapIdx"), MI);
					O->TryGetBoolField(TEXT("empty"), A.bEmpty);
					A.Kind = (int32)K;
					A.MapIdx = (int32)MI;
					if (A.FileName.IsEmpty()) continue;
					if (!A.bEmpty)
					{
						A.Path = FPaths::Combine(ExpDir(D.ToLower()), A.FileName);
						if (!FPaths::FileExists(A.Path)) continue;
					}
					E.Files.Add(A);
				}
			}
			const TSharedPtr<FJsonObject>* Muts = nullptr;
			if (Root->TryGetObjectField(TEXT("mutators"), Muts) && Muts && Muts->IsValid())
				E.MutatorsJson = JsonToString(*Muts);
		}
		// PUT THEM BACK IN THE SITE'S ORDER. The loop above walked a folder, and
		// a folder comes back sorted by uuid, which is the same as random. An
		// experience the site has placed keeps its place; one the site has
		// never mentioned goes after them, newest first, which is where a new
		// experience belongs anyway.
		GExps.Sort([](const FExp& A, const FExp& B)
		{
			const bool bA = A.SiteOrder >= 0, bB = B.SiteOrder >= 0;
			if (bA != bB) return bA;
			if (bA) return A.SiteOrder < B.SiteOrder;
			if (A.UpdatedUnix != B.UpdatedUnix) return A.UpdatedUnix > B.UpdatedUnix;
			return A.Name < B.Name;
		});
		GExpIdx.Reset();
		for (int32 i = 0; i < GExps.Num(); i++) GExpIdx.Add(GExps[i].Id, i);

		UE_LOG(LogBF6Portal, Display, TEXT("Portal profile: %d experience(s) restored from %s"), GExps.Num(), *PortalRoot());
	}

	void LoadConfig()
	{
		FString S;
		if (GConfig->GetString(kIniSection, TEXT("PortalProfileState"), S, GEditorPerProjectIni))
		{
			if (S == TEXT("linked"))  GState = EState::Linked;
			else if (S == TEXT("expired")) GState = EState::Expired;
			else GState = EState::Unlinked;
		}
		GConfig->GetString(kIniSection, TEXT("PortalProfileAccount"), GAccount, GEditorPerProjectIni);
		GConfig->GetString(kIniSection, TEXT("PortalExperienceUrlPattern"), GUrlPattern, GEditorPerProjectIni);
		// The keep-alive is on unless the project says otherwise: a session that
		// never times out is the whole point, and some people will still want it
		// off, so it is a setting rather than a decision.
		if (!GConfig->GetBool(kIniSection, TEXT("PortalKeepAlive"), GKeepAlive, GEditorPerProjectIni)) GKeepAlive = true;
		// Same reasoning for keeping the two in step: on unless the project says
		// otherwise, because a tool that quietly drifts out of step with the
		// site is worse than one that says what it is doing.
		if (!GConfig->GetBool(kIniSection, TEXT("PortalAutoSync"), GAutoSync, GEditorPerProjectIni)) GAutoSync = true;
		// Watching is OFF unless the project says otherwise: it is harmless, but
		// following someone around their own site is their choice to make.
		if (!GConfig->GetBool(kIniSection, TEXT("PortalWatchSite"), GWatch, GEditorPerProjectIni)) GWatch = false;
	}

	void SaveConfig()
	{
		const TCHAR* S = GState == EState::Linked ? TEXT("linked")
			: (GState == EState::Expired ? TEXT("expired") : TEXT("unlinked"));
		GConfig->SetString(kIniSection, TEXT("PortalProfileState"), S, GEditorPerProjectIni);
		GConfig->SetString(kIniSection, TEXT("PortalProfileAccount"), *GAccount, GEditorPerProjectIni);
		GConfig->Flush(false, GEditorPerProjectIni);
	}

	void SetState(EState New, const TCHAR* Why)
	{
		if (GState == New) return;
		GState = New;
		SaveConfig();
		Bump();
		const TCHAR* Name = New == EState::Linked ? TEXT("Linked")
			: New == EState::SigningIn ? TEXT("SigningIn")
			: New == EState::Expired ? TEXT("Expired") : TEXT("Unlinked");
		UE_LOG(LogBF6Portal, Display, TEXT("Portal profile state: %s (%s)"), Name, Why);
		// Linked again, by the same evidence rule as the first time: that is
		// what completes a recovery the user finished by hand.
		if (New == EState::Linked && GLost) FinishRecovery(TEXT("signed in again on the page"));

		// SIGN-IN IS OVER, and only this transition ends it. A page that merely
		// finished loading, or a redirect, is not enough: Linked means the
		// experiences page AND a parsed owned-experiences response.
		//
		// THE TAKE-DOWN IS UNCONDITIONAL. It used to be guarded on the flag the
		// start had set, and on a live run the state machine reached Linked
		// exactly as designed while the panel never moved: the guard was false
		// by the time the transition arrived and the whole block, log line
		// included, never ran. Nothing now stands between reaching Linked and
		// closing whatever presentation is up: EndSignIn is a no-op when the
		// sign-in surface is not the placement, so this costs nothing when the
		// user signed in from the dock tab or the column instead.
		if (New == EState::Linked)
		{
			const bool bWasSigningIn = GLinkInProgress;
			GLinkInProgress = false;
			const bool bSurfaceWasUp = BF6PortalWeb::IsSignIn();
			// What it did, in words, because a panel that vanishes needs the
			// same one line of explanation as a panel that appears.
			const FString What = BF6PortalWeb::EndSignIn();
			if (bWasSigningIn || bSurfaceWasUp)
			{
				const FString Said = What.IsEmpty()
					? FString(TEXT("Signed in. Your experiences are on the map screen."))
					: FString::Printf(TEXT("Signed in: %s. Your experiences are on the map screen."), *What);
				GPageStatus = Said;
				BF6Api::Toast(Said);
			}
			UE_LOG(LogBF6Portal, Display,
				TEXT("Portal profile: Linked reached. Sign-in was in progress: %s. Sign-in surface was up: %s. Panel is now %s (%s)."),
				bWasSigningIn ? TEXT("yes") : TEXT("no"),
				bSurfaceWasUp ? TEXT("yes") : TEXT("no"),
				BF6PortalWeb::IsShown() ? TEXT("open") : TEXT("closed"),
				What.IsEmpty() ? TEXT("the surface was not up, nothing was moved") : *What);
		}
	}

	// ---- the one place a session is declared lost ---------------------------

	void NoteSessionLost(const FString& Why)
	{
		if (GLost)
		{
			// Already known. Keep the FIRST reason, which is the one that
			// actually happened; the rest are its echoes.
			UE_LOG(LogBF6Portal, Verbose, TEXT("Portal session already lost, also saw: %s"), *Why);
			return;
		}
		// NOTHING WAS LOST IF NOTHING WAS HELD. Seeing the login page when the
		// profile was never Linked in the first place is not the site signing
		// anyone out, and saying so reads as a fault the user did not cause.
		// It is simply the state they were already in, so say that instead and
		// start no recovery: there is nothing to recover to.
		if (GState != EState::Linked)
		{
			GPageStatus = TEXT("You are not signed in to Portal. LINK PORTAL PROFILE opens the sign-in page.");
			UE_LOG(LogBF6Portal, Display,
				TEXT("Portal: the login page came up and the profile was not linked, so this is not a lost session (%s)."), *Why);
			Bump();
			return;
		}
		GLost = true;
		GLossReason = Why;
		GRecoveryTries = 0;
		GRecoveryResult = TEXT("recovery starting");
		GBanner = TEXT("Portal signed you out. Nothing is lost: your blocks, script and saves are kept in the tool. Signing you back in...");
		SetState(EState::Expired, TEXT("the session was lost"));
		UE_LOG(LogBF6Portal, Warning, TEXT("Portal session lost: %s"), *Why);
		BF6Api::Toast(GBanner);
		// Subscribers first, and before any navigation: an editor that is still
		// pushing when the page reloads under it is exactly the loss this
		// feature exists to prevent.
		GOnLost.Broadcast();

		// Where to come back to. The tool's own last deliberate destination
		// beats whatever the site bounced us to.
		if (GReturnUrl.IsEmpty() || GReturnUrl.Contains(TEXT("/bf6/login")))
		{
			const FString Now = BF6PortalWeb::CurrentUrl();
			GReturnUrl = (Now.Contains(TEXT("/bf6/")) && !Now.Contains(TEXT("/bf6/login")))
				? Now : (BF6PortalWeb::BaseUrl() + TEXT("/experiences"));
		}
		GRecovery = ERecovery::Reloading;
		GNextRetryAt = FPlatformTime::Seconds();   // the first attempt is immediate
		Bump();
	}

	void FinishRecovery(const FString& How)
	{
		if (!GLost) return;
		GLost = false;
		GRecovery = ERecovery::Idle;
		GBanner.Reset();
		GRecoveryResult = How;
		UE_LOG(LogBF6Portal, Display, TEXT("Portal session restored: %s"), *How);
		BF6Api::Toast(TEXT("Portal signed you back in. The tool is pushing your work again."));
		// Put the page back where the work was BEFORE telling the editors, so
		// what they re-apply lands on the right experience.
		if (!GReturnUrl.IsEmpty() && BF6PortalWeb::CurrentUrl() != GReturnUrl) BF6PortalWeb::OpenQuiet(GReturnUrl);   // recovery is background work, not a reason to show the site
		GOnRestored.Broadcast();
		Bump();
	}

	// ---- 3. thumbnails -------------------------------------------------------

	struct FThumb
	{
		FString                      Url;      // what is on disk
		TStrongObjectPtr<UTexture2D> Tex;
		TSharedPtr<FSlateBrush>      Brush;
		bool                         bBusy = false;
		bool                         bFailed = false;
	};
	TMap<FString, FThumb> GThumbs;

	// ---- the site's own game mode art, as the default card ------------------
	//
	// An experience with no thumbnail of its own used to draw a plain panel with
	// its name in it. The site draws its game mode's picture instead, and the
	// site mirror on this machine already holds those pictures, so the tool
	// draws the same thing.
	//
	// EA'S ART STAYS WHERE IT IS. Nothing is copied into the plugin: the mirror
	// is local, only derived facts ship, and these files are read from the
	// mirror at runtime or not at all.
	//
	// Matched by the GameMode_<name> PREFIX and never by the hash in the file
	// name, so a site update that rehashes the assets still matches. Each mode
	// ships in two sizes; the larger is the card art.
	FString StyleAssetDir()
	{
		return FPaths::ConvertRelativePathToFull(FPaths::Combine(
			FPaths::ProjectSavedDir(), TEXT("BF6UnrealSDK"), TEXT("portalstyle"),
			TEXT("portal.battlefield.com"), TEXT("bf6"), TEXT("15249075"), TEXT("assets")));
	}

	// The site's own game mode names, as its asset files spell them.
	FString ModeAssetName(const FString& GameMode)
	{
		static const TCHAR* kModes[] = {
			TEXT("BattleRoyale"), TEXT("Breakthrough"), TEXT("Conquest"), TEXT("Escalation"),
			TEXT("Gauntlet"), TEXT("Obliteration"), TEXT("Portal_Custom"), TEXT("Rush"),
			TEXT("SquadDeathMatch"), TEXT("TacticalObliteration"), TEXT("TeamDeathMatch")
		};
		// The tool's own mode is the site's Portal Custom card.
		if (GameMode.Equals(TEXT("ModBuilderCustom"), ESearchCase::IgnoreCase)) return TEXT("Portal_Custom");
		for (const TCHAR* M : kModes) if (GameMode.Equals(M, ESearchCase::IgnoreCase)) return M;
		// A mode this build has never heard of gets the site's own "no mode
		// chosen" card rather than a blank panel.
		return TEXT("Unselected");
	}

	struct FModeArt
	{
		TStrongObjectPtr<UTexture2D> Tex;
		TSharedPtr<FSlateBrush>      Brush;
		bool                         bTried = false;
	};
	TMap<FString, FModeArt> GModeArt;      // cached PER MODE, not per experience
	bool GSaidNoMirror = false;

	const FSlateBrush* ModeArtFor(const FString& GameMode)
	{
		const FString Mode = ModeAssetName(GameMode);
		FModeArt& A = GModeArt.FindOrAdd(Mode);
		if (A.bTried) return A.Brush.IsValid() ? A.Brush.Get() : nullptr;
		A.bTried = true;

		const FString Dir = StyleAssetDir();
		if (!FPaths::DirectoryExists(Dir))
		{
			if (!GSaidNoMirror)
			{
				GSaidNoMirror = true;
				UE_LOG(LogBF6Portal, Display,
					TEXT("Portal cards: the site style mirror is not downloaded, so experiences with no thumbnail keep the plain panel. Running the style downloader fills the cards in. Looked in %s"),
					*Dir);
			}
			return nullptr;
		}
		TArray<FString> Files;
		IFileManager::Get().FindFiles(Files, *FPaths::Combine(Dir, FString::Printf(TEXT("GameMode_%s-*.jpg"), *Mode)), true, false);
		if (Files.Num() == 0)
		{
			UE_LOG(LogBF6Portal, Verbose, TEXT("Portal cards: no art for game mode %s in the mirror"), *Mode);
			return nullptr;
		}
		// Two variants of different sizes ship for each mode. The larger is the
		// card art; the smaller is the thumbnail size and is not used here.
		FString Best;
		int64 BestSize = -1;
		for (const FString& F : Files)
		{
			const FString Path = FPaths::Combine(Dir, F);
			const int64 Size = IFileManager::Get().FileSize(*Path);
			if (Size > BestSize) { BestSize = Size; Best = Path; }
		}
		UTexture2D* T = FImageUtils::ImportFileAsTexture2D(Best);
		if (!T)
		{
			UE_LOG(LogBF6Portal, Warning, TEXT("Portal cards: %s would not decode"), *Best);
			return nullptr;
		}
		A.Tex.Reset(T);
		A.Brush = MakeShared<FSlateBrush>();
		A.Brush->SetResourceObject(T);
		A.Brush->ImageSize = FVector2D(300.f, 169.f);
		UE_LOG(LogBF6Portal, Display, TEXT("Portal cards: game mode %s drawn from %s (%lld bytes, the larger of %d variant(s))"),
			*Mode, *FPaths::GetCleanFilename(Best), BestSize, Files.Num());
		return A.Brush.Get();
	}

	FString ThumbPathFor(const FString& Id, const FString& Url)
	{
		FString Ext = FPaths::GetExtension(Url.Left(Url.Contains(TEXT("?")) ? Url.Find(TEXT("?")) : Url.Len()));
		if (Ext.IsEmpty() || Ext.Len() > 4) Ext = TEXT("jpg");
		return FPaths::Combine(ExpDir(Id), FString(TEXT("thumb.")) + Ext.ToLower());
	}

	void AdoptThumbFile(const FString& Id, const FString& Path, const FString& Url)
	{
		FThumb& T = GThumbs.FindOrAdd(Id);
		UTexture2D* Tex = FImageUtils::ImportFileAsTexture2D(Path);
		if (!Tex)
		{
			T.bFailed = true;
			UE_LOG(LogBF6Portal, Warning, TEXT("Portal thumbnail for %s could not be decoded: %s"), *Id.Left(8), *Path);
			return;
		}
		T.Tex.Reset(Tex);
		T.Brush = MakeShared<FSlateBrush>();
		T.Brush->SetResourceObject(Tex);
		T.Brush->ImageSize = FVector2D(300.f, 169.f);
		T.Url = Url;
		T.bFailed = false;
		Bump();
	}

	void EnsureThumb(const FString& Id)
	{
		FExp* E = Find(Id);
		if (!E) return;
		// The page's resolved src first: the raw field can be a
		// "[BB_PREFIX]/..." placeholder only the site knows how to expand.
		FString Url = E->ScrapedThumbUrl;
		if (!Url.StartsWith(TEXT("http"))) Url = E->ThumbUrl;
		if (!Url.StartsWith(TEXT("http"))) return;

		FThumb& T = GThumbs.FindOrAdd(Id);
		if (T.bBusy) return;
		if (T.Brush.IsValid() && T.Url == Url) return;
		if (T.bFailed && T.Url == Url) return;

		const FString Path = ThumbPathFor(Id, Url);
		const FString UrlFile = FPaths::Combine(ExpDir(Id), TEXT("thumb.url"));
		FString Cached;
		if (FPaths::FileExists(Path) && FFileHelper::LoadFileToString(Cached, *UrlFile) && Cached.TrimStartAndEnd() == Url)
		{
			AdoptThumbFile(Id, Path, Url);
			return;
		}

		T.bBusy = true;
		T.Url = Url;
		// The engine's HTTP, plainly: a GET with no headers of ours and no
		// cookies. A thumbnail is a public image; nothing about the account
		// belongs on this request.
		TSharedRef<IHttpRequest, ESPMode::ThreadSafe> Req = FHttpModule::Get().CreateRequest();
		Req->SetURL(Url);
		Req->SetVerb(TEXT("GET"));
		Req->OnProcessRequestComplete().BindLambda(
			[Id, Url, Path, UrlFile](FHttpRequestPtr, FHttpResponsePtr Res, bool bOk)
			{
				FThumb& Th = GThumbs.FindOrAdd(Id);
				Th.bBusy = false;
				if (!bOk || !Res.IsValid() || Res->GetResponseCode() != 200 || Res->GetContent().Num() == 0)
				{
					Th.bFailed = true;
					UE_LOG(LogBF6Portal, Verbose, TEXT("Portal thumbnail for %s did not download (%d): %s"),
						*Id.Left(8), Res.IsValid() ? Res->GetResponseCode() : 0, *Url);
					return;
				}
				IFileManager::Get().MakeDirectory(*ExpDir(Id), true);
				if (!FFileHelper::SaveArrayToFile(Res->GetContent(), *Path)) { Th.bFailed = true; return; }
				FFileHelper::SaveStringToFile(Url, *UrlFile);
				AdoptThumbFile(Id, Path, Url);
			});
		Req->ProcessRequest();
	}

	// ---- 4. page checks ------------------------------------------------------
	//
	// Every navigation the TOOL makes is checked before the tool acts on the
	// page. A route that moved shows up here as one line rather than as a
	// feature that silently does nothing.

	struct FPageCheck
	{
		const TCHAR* Key;
		const TCHAR* PathPrefix;
		bool         bNeedsId;
		bool         bNeedsBlockly;
	};
	const FPageCheck kPages[] =
	{
		{ TEXT("login"),       TEXT("/bf6/login"),                   false, false },
		{ TEXT("experiences"), TEXT("/bf6/experiences"),             false, false },
		{ TEXT("editor"),      TEXT("/bf6/experience/"),             true,  false },
		{ TEXT("choose-maps"), TEXT("/bf6/experience/choose-maps"),  true,  false },
		{ TEXT("blocks"),      TEXT("/bf6/experience/rules/blocks"), true,  true  },
	};

	const FPageCheck* PageCheckFor(const FString& Key)
	{
		for (const FPageCheck& P : kPages) if (Key == P.Key) return &P;
		return nullptr;
	}

	struct FExpect
	{
		FString Key;
		double  Deadline = 0.0;
		bool    bActive = false;
		TFunction<void(bool, const FString&)> Done;
	};
	FExpect GExpect;

	void ResolveExpect(bool bOk, const FString& What)
	{
		if (!GExpect.bActive) return;
		GExpect.bActive = false;
		TFunction<void(bool, const FString&)> Fn = MoveTemp(GExpect.Done);
		GExpect.Done = nullptr;
		if (Fn) Fn(bOk, What);
	}

	// Navigate (or just wait) and call Done when OnUrlChanged and the injected
	// probe agree that the page is what was asked for.
	void ExpectPage(const FString& Key, const FString& NavUrl, float Timeout, TFunction<void(bool, const FString&)> Done)
	{
		ResolveExpect(false, TEXT("superseded by another page check"));
		GExpect.Key = Key;
		GExpect.Deadline = FPlatformTime::Seconds() + Timeout;
		GExpect.bActive = true;
		GExpect.Done = MoveTemp(Done);
		// Remembered so a session lost halfway through knows where to come back
		// to. The login page is never a destination worth returning to.
		if (!NavUrl.IsEmpty() && !NavUrl.Contains(TEXT("/bf6/login"))) { GReturnUrl = NavUrl; GReturnKey = Key; }
		if (!NavUrl.IsEmpty()) BF6PortalWeb::OpenQuiet(NavUrl);   // THE funnel: every automated page the tool drives goes through here
		// The site is a single-page app, so a page that is ALREADY right would
		// otherwise never fire a load. Ask the probe to report now.
		BF6PortalWeb::Exec(TEXT("try { if (window.BF6PortalCapture) window.BF6PortalCapture.report(); } catch (e) {}"));
	}

	bool PageAgrees(const FString& Key, const FString& Kind, const FString& Url, bool bBlockly)
	{
		const FPageCheck* C = PageCheckFor(Key);
		if (!C) return false;
		if (Kind != Key)
		{
			// choose-maps and blocks are both "editor" to the coarse probe.
			const bool bEditorFamily = (Kind == TEXT("editor") || Kind == TEXT("blocks"));
			if (!(bEditorFamily && Url.Contains(C->PathPrefix))) return false;
		}
		if (!Url.Contains(C->PathPrefix)) return false;
		if (C->bNeedsId && FindUuid(Url).IsEmpty()) return false;
		if (C->bNeedsBlockly && !bBlockly) return false;
		return true;
	}

	// ---- 5. the captures -----------------------------------------------------

	struct FChunkBuf
	{
		int32           Of = 0;
		TArray<FString> Parts;
		double          Started = 0.0;
	};
	TMap<FString, FChunkBuf> GChunks;

	// getOwnedPlayElementsV2 was not captured verbatim before this was written,
	// so it is parsed DEFENSIVELY: any submessage that carries a 36 character
	// uuid and a printable name is an experience, whatever field it sits in.
	bool HarvestExperience(const FPbView& M, int32 Depth, TArray<FString>& OutOrder)
	{
		if (!M.IsValid() || Depth > 3) return false;
		FString Id, Name, Desc, Thumb;
		int64 Updated = 0;
		{
			const uint8* P = M.P; const uint8* E = M.P + M.N;
			FPbField F;
			TArray<FString> Texts;
			while (PbNext(P, E, F))
			{
				if (F.Wire != 2) continue;
				const FString S = PbText(F.Bytes);
				if (S.IsEmpty()) continue;
				if (Id.IsEmpty() && IsUuid(S)) { Id = S.ToLower(); continue; }
				if (F.Num == 4 && Name.IsEmpty() && !IsUuid(S)) Name = S;
				Texts.Add(S);
			}
			if (Name.IsEmpty())
				for (const FString& S : Texts)
					if (!IsUuid(S) && S.Len() >= 1 && S.Len() <= 120 && !S.StartsWith(TEXT("http"))) { Name = S; break; }
		}
		Desc    = PbStr2(M, 5, 1);
		Updated = PbVar2(M, 7, 1);
		Thumb   = PbStr2(M, 12, 1);

		if (!Id.IsEmpty() && !Name.IsEmpty())
		{
			FExp& X = FindOrAdd(Id);
			X.Name = Name;
			if (!Desc.IsEmpty())  X.Description = Desc;
			if (Updated > 0)      X.UpdatedUnix = Updated;
			if (!Thumb.IsEmpty()) X.ThumbUrl = Thumb;
			OutOrder.AddUnique(Id);
			WriteCache(X);
			return true;
		}

		// Not itself an experience: it may be a wrapper around them.
		bool bAny = false;
		const uint8* P = M.P; const uint8* E = M.P + M.N;
		FPbField F;
		while (PbNext(P, E, F))
			if (F.Wire == 2 && HarvestExperience(F.Bytes, Depth + 1, OutOrder)) bAny = true;
		return bAny;
	}

	void ParseOwnedList(const TArray<uint8>& Body)
	{
		TArray<FString> Order;
		FString Layout;
		ForEachGrpcFrame(Body, [&Order, &Layout](const FPbView& Msg)
		{
			const uint8* P = Msg.P; const uint8* E = Msg.P + Msg.N;
			FPbField F;
			while (PbNext(P, E, F))
			{
				Layout += FString::Printf(TEXT("f%u:w%u"), F.Num, F.Wire);
				if (F.Wire == 2) Layout += FString::Printf(TEXT("(%d)"), F.Bytes.N);
				Layout += TEXT(" ");
				if (F.Wire == 2) HarvestExperience(F.Bytes, 0, Order);
			}
		});

		// Said ONCE, at Verbose: if the response shape ever moves, this line in
		// the Output Log is what tells an integrator where it moved to, with no
		// debugger and no second capture.
		if (!GLoggedOwnedLayout)
		{
			GLoggedOwnedLayout = true;
			UE_LOG(LogBF6Portal, Verbose, TEXT("getOwnedPlayElementsV2 top-level layout: %s"), *Layout);
		}

		GHasOwnedList = true;
		// Keep the site's order, and drop nothing: an experience already known
		// from the cache stays even if this response did not mention it.
		if (Order.Num() > 0)
		{
			TArray<FExp> Sorted;
			TSet<FString> Seen;
			for (int32 i = 0; i < Order.Num(); i++)
				if (FExp* E = Find(Order[i])) { E->SiteOrder = i; Sorted.Add(*E); Seen.Add(Order[i]); }
			for (const FExp& E : GExps) if (!Seen.Contains(E.Id)) Sorted.Add(E);
			GExps = MoveTemp(Sorted);
			GExpIdx.Reset();
			for (int32 i = 0; i < GExps.Num(); i++) GExpIdx.Add(GExps[i].Id, i);
		}
		UE_LOG(LogBF6Portal, Display, TEXT("Portal profile: getOwnedPlayElementsV2 parsed, %d experience(s)"), Order.Num());
		Bump();
	}

	// ---- the experience's own values, out of the same bytes ------------------
	//
	// The site's whole-experience export is, field for field, a decoded
	// getPlayElement. The tool already has those bytes, so it can write that
	// document itself and nobody has to click an export control on the site.
	//
	// Shape, verified against the site's own responses and against a real
	// export of the same experience:
	//   msg.f2                the play element
	//   msg.f2.f7 (repeated)  one mutator each
	//     f1 key, f2 applicability tags, f3 the value container
	//   the container:
	//     f1            the single value
	//     f3 (repeated) one entry per team, {f1 team id, f2 value}
	//
	// A varint is read SIGNED, because the site stores a negative enum that way
	// (FactionID_PerTeam's PAX is -1865993703) and reading it unsigned turns it
	// into a nonsense positive.

	bool PbNumber(const FPbView& M, uint32 Num, double& Out)
	{
		if (!M.IsValid()) return false;
		const uint8* P = M.P; const uint8* E = M.P + M.N;
		FPbField F;
		while (PbNext(P, E, F))
		{
			if (F.Num != Num) continue;
			if (F.Wire == 0) { Out = (double)(int64)F.Var; return true; }
			if (F.Wire == 1) { double D = 0; FMemory::Memcpy(&D, &F.Var, sizeof(double)); Out = D; return true; }
			if (F.Wire == 5) { const uint32 U = (uint32)F.Var; float Fl = 0; FMemory::Memcpy(&Fl, &U, sizeof(float)); Out = (double)Fl; return true; }
		}
		return false;
	}

	// The mutators, in the shape the site's own export writes them: a bare
	// number when the setting has one value, and an array of [team, value] PAIRS
	// when it has one per team.
	TSharedPtr<FJsonObject> DecodeMutators(const FPbView& El, int32& OutCount)
	{
		TSharedPtr<FJsonObject> Out = MakeShared<FJsonObject>();
		OutCount = 0;
		TArray<FPbView> Muts;
		PbAll(El, 7, Muts);
		for (const FPbView& M : Muts)
		{
			const FString Key = PbStr(M, 1);
			if (Key.IsEmpty()) continue;
			FPbView Container;
			if (!PbSub(M, 3, Container)) continue;

			TArray<FPbView> PerTeam;
			PbAll(Container, 3, PerTeam);
			if (PerTeam.Num() > 0)
			{
				TArray<TSharedPtr<FJsonValue>> Pairs;
				for (const FPbView& T : PerTeam)
				{
					uint64 TeamId = 0;
					PbVar(T, 1, TeamId);
					double Value = 0;
					// A per-team entry with no value field at all is that team
					// set to off; the site leaves the zero out of the wire.
					if (!PbNumber(T, 2, Value)) Value = 0;
					TArray<TSharedPtr<FJsonValue>> Pair;
					Pair.Add(MakeShared<FJsonValueNumber>((double)(int64)TeamId));
					Pair.Add(MakeShared<FJsonValueNumber>(Value));
					Pairs.Add(MakeShared<FJsonValueArray>(Pair));
				}
				Out->SetArrayField(Key, Pairs);
				OutCount++;
				continue;
			}
			double Single = 0;
			if (PbNumber(Container, 1, Single))
			{
				Out->SetNumberField(Key, Single);
				OutCount++;
			}
		}
		return Out;
	}

	// THE MAP ROTATION.
	//
	// Field 6 of the play element was read as the rotation and gave one entry
	// with no map name at all, against a rotation of eleven: it is something
	// else. The rotation ids the site writes have a shape nothing else in the
	// message has - "<map codename>-<game mode><slot>", as in
	// "MP_Isolated-ModBuilderCustom0" - so they are found by that shape,
	// anywhere in the message, in the order the message holds them. This is the
	// same defensive read the owned-experiences list already uses, and it stops
	// being a guess the moment one is matched.
	bool LooksLikeRotationId(const FString& S)
	{
		if (S.Len() < 6 || S.Len() > 120) return false;
		if (!S.StartsWith(TEXT("MP_"))) return false;
		int32 Dash = INDEX_NONE;
		if (!S.FindLastChar(TEXT('-'), Dash) || Dash <= 3 || Dash >= S.Len() - 1) return false;
		// the tail is a game mode name and a slot number
		const FString Tail = S.RightChop(Dash + 1);
		int32 Digits = 0;
		while (Digits < Tail.Len() && FChar::IsDigit(Tail[Tail.Len() - 1 - Digits])) Digits++;
		if (Digits == 0 || Digits == Tail.Len()) return false;
		for (int32 i = 0; i < Tail.Len() - Digits; i++)
			if (!FChar::IsAlpha(Tail[i]) && Tail[i] != TEXT('_')) return false;
		for (int32 i = 0; i < Dash; i++)
			if (!FChar::IsAlnum(S[i]) && S[i] != TEXT('_')) return false;
		return true;
	}

	// "MP_Isolated-ModBuilderCustom0" -> "MP_Isolated". Map codenames carry
	// underscores and never a hyphen, so the last hyphen is the seam.
	FString MapFromRotationId(const FString& RotId)
	{
		int32 At = INDEX_NONE;
		if (RotId.FindLastChar(TEXT('-'), At) && At > 0) return RotId.Left(At);
		return RotId;
	}

	// "MP_Isolated-ModBuilderCustom0" -> "ModBuilderCustom".
	FString GameModeFromRotationId(const FString& RotId)
	{
		int32 At = INDEX_NONE;
		if (!RotId.FindLastChar(TEXT('-'), At) || At < 0) return FString();
		FString Tail = RotId.RightChop(At + 1);
		while (Tail.Len() > 0 && FChar::IsDigit(Tail[Tail.Len() - 1])) Tail.LeftChopInline(1);
		return Tail;
	}

	void ScanRotationIds(const FPbView& M, TArray<FString>& Out, int32 Depth = 0)
	{
		if (!M.IsValid() || Depth > 8) return;
		const uint8* P = M.P; const uint8* E = M.P + M.N;
		FPbField F;
		while (PbNext(P, E, F))
		{
			if (F.Wire != 2) continue;
			const FString S = PbText(F.Bytes);
			if (LooksLikeRotationId(S)) { Out.AddUnique(S); continue; }
			// A huge payload is an attachment, not a wrapper worth walking.
			if (F.Bytes.N < 4096) ScanRotationIds(F.Bytes, Out, Depth + 1);
		}
	}

	// THE ROTATION, READ BY FIELD NUMBER.
	//
	// The scan above looks for a joined "MP_Isolated-ModBuilderCustom0". No
	// getPlayElement response contains that string anywhere: the joined id is
	// the SITE's own concatenation, and the wire keeps the two halves apart:
	//
	//   play element
	//     6            the rotation, exactly one
	//       1          one per slot, repeated
	//         1        "MP_Aftermath"          the map codename
	//         2        "ModBuilderCustom0"     the game mode and the slot
	//         3, 4     varints, 1 and 4 on every entry seen
	//         5        the entry's spatial attachment
	//       2          an empty string
	//
	// Field 6 was read once before, one level too shallow: it is the wrapper,
	// so it looks like a single entry with no map name on it, which is exactly
	// the "one entry, no map name, against a rotation of eleven" that sent the
	// reader off to guess by shape instead. Confirmed against two payloads, a
	// one-map rotation and an eleven-map one, both of which carry zero joined
	// ids. An experience whose rotation could not be read imports zero maps,
	// which is what "the import keeps failing" was.
	void ReadRotation(const FPbView& El, TArray<FString>& Out)
	{
		FPbView Rot;
		if (!PbSub(El, 6, Rot)) return;
		TArray<FPbView> Entries;
		PbAll(Rot, 1, Entries);
		for (const FPbView& Entry : Entries)
		{
			const FString Map  = PbStr(Entry, 1);
			const FString Mode = PbStr(Entry, 2);
			if (Map.IsEmpty() || !Map.StartsWith(TEXT("MP_"))) continue;
			// A slot with no mode on it still names a map, and dropping it
			// would silently shorten the rotation.
			Out.Add(Mode.IsEmpty() ? Map : Map + TEXT("-") + Mode);
		}
	}

	// THE TWO IDS.
	//
	// A getPlayElement response is a header and a play element. The HEADER names
	// the experience that was asked for. The play element nested inside it is
	// that experience's editable REVISION: a different uuid, and a name of the
	// form "Remix for Coupe(<experience name>)". Two live responses show it the
	// same way - outer 267737f0 with its own name, and outer bad326a0 "SFX"
	// wrapping inner bace44a0 "Remix for Coupe(BF_Undead)".
	//
	// Keying by the inner id is why a 1.9 MB response that arrived and parsed
	// cleanly ("1 map(s) in rotation, 11 attachment(s)") was still reported as
	// "1 failed": the importer waits for the id it asked for and that id was
	// never written. So the experience is keyed by the OUTER id and shown under
	// the OUTER name, and the inner pair is kept beside it as the revision.
	//
	// The id the tool last asked for, so a response whose header is missing or
	// unreadable still lands on the right experience rather than inventing one.
	FString GRequestedId;

	// ---- the shared half of an experience, into the project ------------------
	//
	// THERE ARE TWO IMPORT ROUTES and both carry the whole experience: the
	// site's own export file, and getPlayElement off the live page. Each used
	// to write the workspace, the settings and the rotation only into the
	// download folder under portal/experiences/<uuid>, which is where the raw
	// file the site sent is kept for diffing. Nothing put them where the tool
	// actually reads them, so a 5,192 block workspace downloaded perfectly and
	// the project reported "no block workspace has been captured".
	//
	// Fixing one route left the other broken, which is why this is one function
	// now instead of two copies.
	bool WriteProjectShared(const FString& Id, const FString& Name,
		const TCHAR* Rel, const FString& Text)
	{
		if (Text.IsEmpty() || Id.IsEmpty()) return false;
		const FString Save = BF6Project::EnsureExperience(Id, Name);
		if (Save.IsEmpty()) return false;
		const FString Dir = BF6Project::DirFor(Save);
		if (Dir.IsEmpty()) return false;
		const FString Path = FPaths::Combine(Dir, Rel);
		IFileManager::Get().MakeDirectory(*FPaths::GetPath(Path), true);
		if (!FFileHelper::SaveStringToFile(Text, *Path,
			FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
		{
			UE_LOG(LogBF6Portal, Warning, TEXT("could not write %s"), *Path);
			return false;
		}
		// The stamp says the site caused this write, so the next sync does not
		// offer to re-import what we just took from it.
		BF6Project::NoteArtefactWritten(Save, Rel, TEXT("site"));
		return true;
	}

	// The rotation as the project records it: which map sits in which slot.
	FString RotationDoc(const FExp& E)
	{
		if (E.Maps.Num() == 0) return FString();
		TArray<TSharedPtr<FJsonValue>> Slots;
		for (int32 i = 0; i < E.Maps.Num(); i++)
		{
			TSharedRef<FJsonObject> R = MakeShared<FJsonObject>();
			R->SetNumberField(TEXT("mapIdx"), i);
			R->SetStringField(TEXT("map"), E.Maps[i]);
			R->SetStringField(TEXT("rotationId"),
				E.RotationIds.IsValidIndex(i) ? E.RotationIds[i] : FString());
			Slots.Add(MakeShared<FJsonValueObject>(R));
		}
		TSharedRef<FJsonObject> Doc = MakeShared<FJsonObject>();
		Doc->SetStringField(TEXT("experienceId"), E.Id);
		Doc->SetStringField(TEXT("name"), E.Name);
		Doc->SetArrayField(TEXT("rotation"), Slots);
		return JsonToString(Doc);
	}

	void ParsePlayElement(const TArray<uint8>& Body)
	{
		FString Id;
		ForEachGrpcFrame(Body, [&Id, &Body](const FPbView& Msg)
		{
			FPbView Hdr, El;
			const bool bHdr = PbSub(Msg, 1, Hdr);
			if (!PbSub(Msg, 2, El)) return;

			const FString InnerId   = PbStr(El, 1).ToLower();
			const FString InnerName = PbStr(El, 2);
			const FString OuterId   = bHdr ? PbStr(Hdr, 1).ToLower() : FString();
			const FString OuterName = bHdr ? PbStr(Hdr, 4) : FString();

			FString ElemId;
			if (IsUuid(OuterId))              ElemId = OuterId;
			else if (IsUuid(GRequestedId))    ElemId = GRequestedId;
			else if (IsUuid(InnerId))         ElemId = InnerId;
			if (!IsUuid(ElemId)) { UE_LOG(LogBF6Portal, Warning, TEXT("getPlayElement carried no experience id")); return; }
			Id = ElemId;

			if (IsUuid(InnerId) && InnerId != ElemId)
			{
				UE_LOG(LogBF6Portal, Verbose,
					TEXT("getPlayElement: experience %s ('%s'), editable revision %s ('%s')"),
					*ElemId.Left(8), OuterName.IsEmpty() ? TEXT("unnamed") : *OuterName,
					*InnerId.Left(8), InnerName.IsEmpty() ? TEXT("unnamed") : *InnerName);
			}

			FExp& E = FindOrAdd(ElemId);
			// A NEW COPY MAKES THE OLD COMPLAINTS STALE. Whatever the site was
			// warning about was about the version that was up there; this one
			// has just come down, and leaving the old list attached to it would
			// have people chasing problems in code they have already replaced.
			if (E.Notices.Num() > 0)
			{
				UE_LOG(LogBF6Portal, Display,
					TEXT("Cleared %d Portal warning(s) on '%s': it has just been imported again."),
					E.Notices.Num(), *E.Name);
			}
			E.Notices.Reset();
			E.NoticesAt = FDateTime(0);
			if (IsUuid(InnerId) && InnerId != ElemId)
			{
				E.RevisionId = InnerId;
				E.RevisionName = InnerName;
			}
			// The OUTER name is the experience's name; the inner one names the
			// revision and would show in the panel as "Remix for Coupe(...)".
			FString Name = OuterName;
			if (Name.IsEmpty() && !InnerName.IsEmpty() && InnerId == ElemId) Name = InnerName;
			if (!Name.IsEmpty()) { E.Name = Name; E.SiteName = Name; }
			if (bHdr)
			{
				const FString D = PbStr2(Hdr, 5, 1); if (!D.IsEmpty()) { E.Description = D; E.SiteDescription = D; }
				const int64  U = PbVar2(Hdr, 7, 1);  if (U > 0) E.UpdatedUnix = U;
				const FString T = PbStr2(Hdr, 12, 1); if (!T.IsEmpty()) E.ThumbUrl = T;
			}
			if (E.UpdatedUnix == 0) E.UpdatedUnix = PbVar2(El, 4, 1);

			// THE MAP ROTATION, by field number, with the shape scan kept only
			// as a fallback. Reading it by shape found nothing at all on a real
			// experience, because the joined id it looks for is the site's own
			// concatenation and is not on the wire. See ReadRotation.
			TArray<FString> RotIds;
			ReadRotation(El, RotIds);
			if (RotIds.Num() == 0)
			{
				ScanRotationIds(El, RotIds);
				if (RotIds.Num() > 0)
				{
					UE_LOG(LogBF6Portal, Verbose,
						TEXT("Rotation read by id shape, not by field 6: %d map(s)."), RotIds.Num());
				}
			}
			TArray<FString> NewMaps;
			for (const FString& R : RotIds) NewMaps.Add(MapFromRotationId(R));
			if (RotIds.Num() > 0)
			{
				E.RotationIds = RotIds;
				const FString Mode = GameModeFromRotationId(RotIds[0]);
				if (!Mode.IsEmpty()) E.GameMode = Mode;
			}
			// The mutators, so the tool can write the site's own export format
			// without the site having to export anything.
			{
				int32 MutCount = 0;
				TSharedPtr<FJsonObject> Muts = DecodeMutators(El, MutCount);
				if (MutCount > 0) E.MutatorsJson = JsonToString(Muts);
			}
			if (NewMaps.Num() > 0)
			{
				// A rotation that GAINED a map keeps every save already made.
				TArray<FString> Keep = E.SaveNames;
				E.Maps = NewMaps;
				E.SaveNames.SetNum(NewMaps.Num());
				for (int32 i = 0; i < NewMaps.Num(); i++)
					E.SaveNames[i] = Keep.IsValidIndex(i) ? Keep[i] : FString();
			}

			IFileManager::Get().MakeDirectory(*ExpDir(ElemId), true);
			// The response itself, kept beside what was read out of it. It is
			// the whole experience, it is the only place a field this reader
			// does not know about still exists, and it is what lets a decode be
			// checked against the site's own export without another round trip.
			FFileHelper::SaveArrayToFile(Body, *FPaths::Combine(ExpDir(ElemId), TEXT("playelement.bin")));

			// the rules blocks: f10.f1.f1 is the Blockly JSON string
			{
				FPbView A, B;
				if (PbSub(El, 10, A) && PbSub(A, 1, B))
				{
					const FString Blocks = PbStr(B, 1);
					if (Blocks.StartsWith(TEXT("{")))
					{
						WriteWithOriginal(FPaths::Combine(ExpDir(ElemId), TEXT("rules.blocks.json")), Blocks);
						// AND into the project, which is what the block editor
						// opens. Writing only the line above is the bug that
						// made an imported experience arrive with no blocks.
						WriteProjectShared(ElemId, E.Name,
							TEXT("unreal/blockly/workspace.json"), Blocks);
					}
				}
			}

			// attachments: repeated f15
			E.Files.Reset();
			TArray<FPbView> Atts;
			PbAll(El, 15, Atts);
			for (const FPbView& A : Atts)
			{
				FAttachment At;
				At.FileName = PbStr2(A, 3, 1);
				uint64 K = 0; PbVar(A, 7, K); At.Kind = (int32)K;
				const FString Content = PbStr2(A, 6, 1);
				const FString Tie = PbStr2(A, 8, 1);       // "mapIdx=<n>"
				if (Tie.StartsWith(TEXT("mapIdx=")))       At.MapIdx = FCString::Atoi(*Tie.RightChop(7));
				if (At.FileName.IsEmpty()) At.FileName = FString::Printf(TEXT("attachment_%d.txt"), E.Files.Num());
				At.FileName = FPaths::GetCleanFilename(At.FileName);
				// The site's own id and version for this attachment, read by
				// shape rather than by a field number nobody has verified: the
				// id is the uuid in the message, the version the short numeric
				// string beside it.
				{
					const uint8* AP = A.P; const uint8* AE = A.P + A.N;
					FPbField AF;
					while (PbNext(AP, AE, AF))
					{
						if (AF.Wire != 2 || AF.Bytes.N > 64) continue;
						FString S = PbText(AF.Bytes);
						if (S.IsEmpty()) { FPbView In; if (PbSub(AF.Bytes, 1, In)) S = PbText(In); }
						if (At.AttId.IsEmpty() && IsUuid(S)) { At.AttId = S.ToLower(); continue; }
						if (At.Version.IsEmpty() && S.Len() > 0 && S.Len() <= 8 && S.IsNumeric()) At.Version = S;
					}
				}
				// The kind field is the contract; the content is the check.
				if (At.Kind == 0 && Content.StartsWith(TEXT("{\"Portal_Dynamic\""))) At.Kind = 1;
				// An attachment the site keeps empty is still an attachment.
				At.bEmpty = Content.IsEmpty();
				if (!At.bEmpty)
				{
					At.Path = FPaths::Combine(ExpDir(ElemId), At.FileName);
					if (!WriteWithOriginal(At.Path, Content)) continue;
				}
				if (At.Kind == 1 && At.MapIdx < 0) At.MapIdx = 0;
				// THE SCRIPT COMES IN TOO.
				//
				// Attachment kinds, read off two live responses: 1 spatial,
				// 2 the bundled TypeScript, 3 the blacklist, 4 its strings.
				// Only the spatial was ever consumed, so a script-mode
				// experience - one whose rules are a bundle rather than blocks -
				// imported its map and left its whole script sitting in the
				// cache where nothing opens it. The project already knows these
				// by name (BF6Project lists dist/bundle.ts as "bundle"), so they
				// go where it already looks rather than somewhere new.
				//
				// dist/, not src/: this is BUILT output, headed by "BUNDLED
				// TYPESCRIPT OUTPUT" and "@ts-nocheck". Writing it to src would
				// hand the bundler its own output to bundle again. Push refuses
				// a bundle it did not itself check, so an imported one asks for
				// BUILD before it can go back up, which is the safe way round.
				if (!At.bEmpty)
				{
					if (At.Kind == 2)
					{
						WriteProjectShared(ElemId, E.Name, TEXT("dist/bundle.ts"), Content);
					}
					else if (At.Kind == 4)
					{
						WriteProjectShared(ElemId, E.Name, TEXT("dist/bundle.strings.json"), Content);
						// AND INTO THE SOURCE, which is the copy that survives.
						//
						// dist/ is BUILD OUTPUT: the bundler rebuilds
						// dist/bundle.strings.json by merging src/**/strings.json,
						// so an experience imported with 409 strings kept them
						// only until the next BUILD, which replaced the lot with
						// the template starter's single key. Writing the source
						// as well means a rebuild reproduces what came down
						// instead of throwing it away.
						//
						// The template's own starter is a stub and is meant to be
						// replaced; anything the user had written is kept beside
						// it as .orig rather than simply lost.
						const FString Save = BF6Project::EnsureExperience(ElemId, E.Name);
						const FString Dir = Save.IsEmpty() ? FString() : BF6Project::DirFor(Save);
						if (!Dir.IsEmpty())
						{
							const FString SrcStrings = FPaths::Combine(Dir, TEXT("src"), TEXT("strings.json"));
							FString Existing;
							if (FFileHelper::LoadFileToString(Existing, *SrcStrings) && Existing != Content)
							{
								FFileHelper::SaveStringToFile(Existing, *(SrcStrings + TEXT(".orig")),
									FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
							}
						}
						WriteProjectShared(ElemId, E.Name, TEXT("src/strings.json"), Content);
					}
				}
				E.Files.Add(At);
			}

			// EVERY IMPORTED EXPERIENCE BECOMES A TEMPLATE PROJECT.
			//
			// Rules reach Portal in two forms and plenty of creators only ever
			// use one of them: a block workspace, or bundled TypeScript. Either
			// way what came down is not something anyone can work on here - a
			// workspace is JSON, and a bundle is generated output with
			// "@ts-nocheck" at the top - and neither arrives with the bundler,
			// the strict tsc, the utility modules or the debug tool that the
			// scripting template carries. So the template is scaffolded for
			// both, and a block workspace is additionally converted into real
			// TypeScript under src/.
			//
			// The conversion is a node run, so it is started and not waited for.
			{
				bool bHasBundle = false;
				for (const FAttachment& A : E.Files)
				{
					if (A.Kind == 2 && !A.bEmpty) { bHasBundle = true; break; }
				}
				const FString BlocksPath = FPaths::Combine(ExpDir(ElemId), TEXT("rules.blocks.json"));
				FString Why, ProjDir;
				if (!bHasBundle && FPaths::FileExists(BlocksPath))
				{
					if (!BF6Script::ConvertBlocksToTemplate(ElemId, E.Name, BlocksPath, Why))
					{
						UE_LOG(LogBF6Portal, Warning,
							TEXT("Imported '%s' but could not convert its blocks into a script project: %s"),
							*E.Name, *Why);
					}
				}
				else if (!BF6Script::EnsureTemplateProject(ElemId, E.Name, ProjDir, Why))
				{
					UE_LOG(LogBF6Portal, Warning,
						TEXT("Imported '%s' but could not give it a script project: %s"), *E.Name, *Why);
					ProjDir.Reset();
				}
				// AND POINT THE SCRIPT PANEL AT IT.
				//
				// Writing the bundle into the project is not the same as showing
				// it: the panel restores whichever project it had last, so an
				// experience imported with its whole script still opened behind a
				// leftover project from a previous session and looked as though
				// nothing had come in.
				if (ProjDir.IsEmpty())
				{
					FString D;
					if (BF6Script::EnsureTemplateProject(ElemId, E.Name, D, Why)) { ProjDir = D; }
				}
				if (!ProjDir.IsEmpty()) { BF6Script::OpenProjectAt(ProjDir); }

				// A BUNDLED SCRIPT IS NOT SPLIT BEHIND YOUR BACK.
				//
				// The bundle records which files it was built from, so the tool
				// can say how many there were - but the bundler strips every
				// import, and reconstructing those by guessing which file
				// exports each name was measured against a real 19-file mod and
				// produced a bundle 2,000 lines larger than the original. It
				// builds; it is not the same mod.
				//
				// Somebody who has this experience open nearly always has the
				// project it was built from on disk, and that is exact. So the
				// tool says what it found and offers to take the real thing.
				if (!ProjDir.IsEmpty())
				{
					const FString Bundle = FPaths::Combine(ProjDir, TEXT("dist"), TEXT("bundle.ts"));
					int32 Modules = 0;
					// WHERE THIS BUNDLE CAME FROM, WRITTEN DOWN NOW.
					//
					// The build gate used to infer "this was imported" from the
					// bundle looking like bundler output - and every bundle does,
					// including the one the user's own build just produced. So a
					// perfectly ordinary template project was refused its SECOND
					// build, forever, with a message telling its author to import
					// source they had never lost. Verified on a real project.
					//
					// Only the import knows it imported. It says so here, and the
					// gate reads this instead of guessing from the file.
					BF6Script::MarkBundleImported(ProjDir, E.Name);
					if (BF6Script::LooksBundled(Bundle, Modules))
					{
						UE_LOG(LogBF6Portal, Display,
							TEXT("'%s' was built from %d source file(s) with the TypeScript template. ")
							TEXT("The bundle cannot be turned back into them exactly, because bundling ")
							TEXT("strips the imports. Run BF6.Script.UseMySource to point the tool at ")
							TEXT("your own project folder, or leave it as the one bundled file."),
							*E.Name, Modules);
						BF6Api::Toast(FString::Printf(
							TEXT("'%s' came in as one bundled file built from %d of your own. ")
							TEXT("BF6.Script.UseMySource brings your real source in."),
							*E.Name, Modules));
						// SHOW THEM THEIR SCRIPT, not the template's example.
						// src/index.ts in a freshly scaffolded project is the
						// template's 69-line sample, and the panel opened that
						// by default - so an import that worked perfectly still
						// looked like no script had arrived.
						BF6Script::ShowFileFirst(TEXT("dist/bundle.ts"));
					}
					else
					{
						// Not built with the template, so there is nothing to
						// split and something to offer instead.
						UE_LOG(LogBF6Portal, Display,
							TEXT("'%s' was not built with the TypeScript template, so its script stays ")
							TEXT("as one file. BF6.Script.AddTemplateFeatures adds the template's ")
							TEXT("debug tool, logging and helpers alongside it."), *E.Name);
					}
				}
			}

			// The settings and the rotation, into the project beside the
			// workspace. Only what the response actually carried is written: a
			// field the site did not send is left out rather than invented.
			if (!E.MutatorsJson.IsEmpty())
			{
				TSharedRef<FJsonObject> S = MakeShared<FJsonObject>();
				if (const TSharedPtr<FJsonObject> M = JsonFromString(E.MutatorsJson))
					S->SetObjectField(TEXT("mutators"), M);
				if (!E.GameMode.IsEmpty()) S->SetStringField(TEXT("gameMode"), E.GameMode);
				WriteProjectShared(ElemId, E.Name,
					TEXT("unreal/settings/settings.json"), JsonToString(S));
			}
			WriteProjectShared(ElemId, E.Name, TEXT("unreal/rotation.json"), RotationDoc(E));

			E.bFetched = true;
			E.bFetchedThisAttempt = true;
			WriteCache(E);
			int32 MutN = 0;
			if (const TSharedPtr<FJsonObject> MJ = JsonFromString(E.MutatorsJson)) MutN = MJ->Values.Num();
			// WHAT THE RULES ARE, said out loud. "0 map(s)" was the only thing
			// this line ever reported about an import that brought nothing
			// usable back, and a script-mode experience whose script was
			// dropped looked identical to one that imported perfectly.
			int32 ScriptBytes = 0, StringBytes = 0;
			for (const FAttachment& A : E.Files)
			{
				if (A.Kind == 2 && !A.bEmpty) ScriptBytes = 1;
				if (A.Kind == 4 && !A.bEmpty) StringBytes = 1;
			}
			UE_LOG(LogBF6Portal, Display,
				TEXT("Portal profile: getPlayElement '%s' (%s) parsed - %d map(s) in rotation, %d attachment(s), %d setting value(s), game mode %s, rules: %s"),
				*E.Name, *ElemId.Left(8), E.Maps.Num(), E.Files.Num(), MutN, *E.GameMode,
				ScriptBytes
					? (StringBytes ? TEXT("bundled script and strings") : TEXT("bundled script"))
					: TEXT("no bundled script (blocks or empty)"));
		});
		if (Id.IsEmpty()) UE_LOG(LogBF6Portal, Warning, TEXT("getPlayElement response carried no PlayElement"));
		Bump();
	}

	void HandleBody(const FString& Method, int32 Status, const FString& B64)
	{
		if (Status == 401 || Status == 403)
		{
			GPageStatus = FString::Printf(TEXT("The site refused %s (%d)."), *Method, Status);
			NoteSessionLost(FString::Printf(TEXT("the site answered %s with %d"), *Method, Status));
			return;
		}
		// A 3xx on an API call is a sign-out in disguise: the body that follows
		// is a login page, not a gRPC frame.
		if (Status >= 300 && Status < 400)
		{
			GPageStatus = FString::Printf(TEXT("The site redirected %s (%d)."), *Method, Status);
			NoteSessionLost(FString::Printf(TEXT("the site redirected %s with %d"), *Method, Status));
			return;
		}
		if (Status != 200)
		{
			UE_LOG(LogBF6Portal, Verbose, TEXT("Portal capture %s ignored (status %d)"), *Method, Status);
			return;
		}
		TArray<uint8> Bytes;
		if (!FBase64::Decode(B64, Bytes) || Bytes.Num() == 0)
		{
			UE_LOG(LogBF6Portal, Warning, TEXT("Portal capture %s: the body did not decode"), *Method);
			return;
		}
		if (Method == TEXT("getOwnedPlayElementsV2"))
		{
			ParseOwnedList(Bytes);
			// The ONLY evidence that makes a profile Linked: the experiences
			// page, plus a 200 from this call that actually parsed.
			if (GLastKind == TEXT("experiences"))
				SetState(EState::Linked, TEXT("the experiences page and a parsed owned-experiences response"));
		}
		else if (Method == TEXT("getPlayElement"))
		{
			ParsePlayElement(Bytes);
		}
		else if (Method == TEXT("UploadExperienceThumbnail"))
		{
			// The proto in the community client returns a url and a
			// verificationUrl. Read them by shape rather than by number, since
			// only the names are documented: the first two http strings in the
			// message, in order, and the layout is logged either way.
			TArray<FString> Urls;
			FString Layout;
			ForEachGrpcFrame(Bytes, [&Urls, &Layout](const FPbView& Msg)
			{
				const uint8* P = Msg.P; const uint8* E = Msg.P + Msg.N;
				FPbField F;
				while (PbNext(P, E, F))
				{
					Layout += FString::Printf(TEXT("f%u:w%u "), F.Num, F.Wire);
					if (F.Wire != 2) continue;
					FString S = PbText(F.Bytes);
					if (S.StartsWith(TEXT("http"))) { Urls.AddUnique(S); continue; }
					// one level down: the site wraps optional scalars
					FPbView In;
					if (PbSub(F.Bytes, 1, In))
					{
						S = PbText(In);
						if (S.StartsWith(TEXT("http"))) Urls.AddUnique(S);
					}
				}
			});
			UE_LOG(LogBF6Portal, Display, TEXT("UploadExperienceThumbnail layout: %s"), *Layout);
			if (Urls.Num() > 0) GThumb.UploadedUrl = Urls[0];
			if (Urls.Num() > 1) GThumb.VerifyUrl = Urls[1];
			if (GThumb.UploadedUrl.IsEmpty())
			{
				GThumb.Status = TEXT("The upload came back with no image url. Use COPY PATH and pick the file in the site's own dialog.");
				UE_LOG(LogBF6Portal, Warning, TEXT("Portal thumbnail: upload response carried no url"));
			}
			else
			{
				GThumb.Status = FString::Printf(TEXT("Uploaded. Waiting for the site to scan it: %s"), *GThumb.UploadedUrl);
				UE_LOG(LogBF6Portal, Display, TEXT("Portal thumbnail uploaded: %s (verify: %s)"),
					*GThumb.UploadedUrl, GThumb.VerifyUrl.IsEmpty() ? TEXT("none given") : *GThumb.VerifyUrl);
				if (!GThumb.VerifyUrl.IsEmpty())
				{
					FString Esc = GThumb.VerifyUrl; Esc.ReplaceInline(TEXT("'"), TEXT("\\'"));
					BF6PortalWeb::Exec(FString::Printf(
						TEXT("try { window.BF6PortalCapture.pollVerification('%s', 10); } catch (e) {}"), *Esc));
				}
				else
				{
					// Nothing to poll: go straight to setting it.
					FString Esc = GThumb.UploadedUrl; Esc.ReplaceInline(TEXT("'"), TEXT("\\'"));
					BF6PortalWeb::Exec(FString::Printf(
						TEXT("try { window.BF6PortalCapture.selectImage('%s'); } catch (e) {}"), *Esc));
				}
			}
			Bump();
		}
		else
		{
			UE_LOG(LogBF6Portal, Verbose, TEXT("Portal capture: %s (%d bytes), not a method this tool reads"), *Method, Bytes.Num());
		}
	}

	// ---- 6. import -----------------------------------------------------------

	struct FImportJob
	{
		TArray<FString> Queue;
		FString         Current;
		double          Deadline = 0.0;
		// ONE attempt per experience plus ONE retry. The old code had no cap at
		// all, which is how a signed-out session turned into the same failed
		// navigation ten times over.
		int32           Attempt = 0;
		bool            bActive = false;
		bool            bCancel = false;
		bool            bSignedOut = false;   // the page bounced to login: stop, do not count failures
		FString         OpenAfter;            // open this experience for editing when done
		int32           Imported = 0, NoData = 0, Failed = 0;
		int32           Total = 0;            // how many were queued, for "2 of 11"
	};
	FImportJob GJob;
	const int32 kMaxAttempts = 2;

	// Signed in, as far as anything the tool can see. Every message that could
	// be either "not signed in" or "signed in but the experience would not
	// open" is decided here, so the two never get the same wording.
	bool LooksSignedIn()
	{
		return GState == EState::Linked && !GLost && GLastKind != TEXT("login");
	}

	// Everything one experience needs, on disk, as saves. One save per map in
	// the rotation, all carrying the same experience link.
	// Only, when given, is the set of rotation slots to touch: a re-import that
	// only changed one map must not rewrite the other ten saves under the user.
	void ImportFetched(FExp& E, int32& OutImported, int32& OutNoData, int32& OutFailed, const TSet<int32>* Only = nullptr)
	{
		// The link written into every save. The address the site itself landed
		// on wins; the constructed one is only a fallback, and it is used as an
		// IDENTITY (the tool reads the uuid back out of it), never navigated to
		// blind - opening an experience always goes through the site's own card.
		const FString Url = E.OpenUrl.IsEmpty() ? ExperienceUrl(E.Id, TEXT("settings/mode")) : E.OpenUrl;
		if (E.Maps.Num() == 0)
		{
			OutNoData++;
			UE_LOG(LogBF6Portal, Warning, TEXT("Portal import '%s': no map rotation, nothing to import"), *E.Name);
			return;
		}
		E.SaveNames.SetNum(E.Maps.Num());
		for (int32 i = 0; i < E.Maps.Num(); i++)
		{
			if (Only && !Only->Contains(i))
			{
				UE_LOG(LogBF6Portal, Verbose, TEXT("Portal import: '%s' map %d unchanged, left alone"), *E.Name, i + 1);
				continue;
			}
			const FString Level = E.Maps[i];
			const FString Save  = SaveNameFor(E, i);
			const FAttachment* Spatial = nullptr;
			for (const FAttachment& A : E.Files) if (A.Kind == 1 && A.MapIdx == i) { Spatial = &A; break; }

			bool bOk = false;
			if (Spatial && FPaths::FileExists(Spatial->Path))
			{
				bOk = BF6_ImportSpatialFile(Spatial->Path, Save, Url, i);
				if (bOk) OutImported++;
			}
			else
			{
				// No spatial for this slot: the map's own base setup, named and
				// linked like its siblings, so the switcher can reach it.
				bOk = BF6_CreatePortalBaseSave(Level, Save, Url, i);
				if (bOk) OutNoData++;
			}
			if (bOk) E.SaveNames[i] = Save;
			else     OutFailed++;
			// SAID WHERE THE USER IS LOOKING, not only in the log. With the
			// site off screen this line is the only thing telling them the
			// tool is working rather than stuck.
			GWorkStatus = FString::Printf(TEXT("Reading map %d of %d for '%s' (%s)"),
				i + 1, E.Maps.Num(), *E.Name, *BF6Api::DisplayName(Level));
			Bump();
			UE_LOG(LogBF6Portal, Display, TEXT("Portal import: '%s' map %d of %d (%s) -> %s"),
				*E.Name, i + 1, E.Maps.Num(), *Level, bOk ? *Save : TEXT("FAILED"));
		}
		WriteCache(E);
		Bump();
	}

	// Ask the page to replay the site's own getPlayElement. Inside the page this
	// falls back to clicking the card when the site has never made that call, so
	// no URL is ever constructed here or there.
	void RequestReplay(const FString& Id)
	{
		FExp* E = Find(Id);
		FString Name = E ? E->Name : FString();
		Name.ReplaceInline(TEXT("\\"), TEXT("\\\\"));
		Name.ReplaceInline(TEXT("'"), TEXT("\\'"));
		// Remembered so a response whose header is missing still lands on the
		// experience that was asked for rather than on its inner revision.
		GRequestedId = Id;
		BF6PortalWeb::Exec(FString::Printf(
			TEXT("try { window.BF6PortalCapture.getPlayElement('%s', '%s'); } catch (e) { console.log('BF6CAPTURE replay error ' + e); }"),
			*Id, *Name));
	}

	// True once the page has watched the site make the call the tool wants to
	// imitate. Until then, and only until then, the site's pages get driven.
	bool CanReplay(const TCHAR* Method) { return GApiSeen.Contains(Method); }

	// The way a user opens an experience, and now the tool's only way: stand on
	// the experiences list, find the card in the site's own DOM, click it.
	void OpenByCard(const FString& Id)
	{
		FExp* E = Find(Id);
		FString Name = E ? E->Name : FString();
		Name.ReplaceInline(TEXT("\\"), TEXT("\\\\"));
		Name.ReplaceInline(TEXT("'"), TEXT("\\'"));
		GNavTarget = BF6PortalWeb::BaseUrl() + TEXT("/experiences");
		UE_LOG(LogBF6Portal, Display, TEXT("Portal import: going to the experiences list to click the card for %s"), *Id.Left(8));
		ExpectPage(TEXT("experiences"), GNavTarget, 15.f,
			[Id, Name](bool bOk, const FString& What)
			{
				if (!bOk)
				{
					UE_LOG(LogBF6Portal, Warning, TEXT("Portal import: could not reach the experiences list (%s)"), *What);
					return;   // the job's own deadline decides what happens next
				}
				BF6PortalWeb::Exec(FString::Printf(
					TEXT("try { window.BF6PortalCapture.openExperience('%s', '%s'); } catch (e) { console.log('BF6CAPTURE open error ' + e); }"),
					*Id, *Name));
			});
	}

	// Put the panel on an experience WITHOUT inventing an address. A learned
	// address is used only when the site itself landed on one that names the
	// experience; otherwise the site's own card is clicked, which is the only
	// thing that reliably works.
	void GoToExperience(const FString& Id)
	{
		FExp* E = Find(Id);
		if (E && !E->OpenUrl.IsEmpty())
		{
			GNavTarget = E->OpenUrl;
			ExpectPage(TEXT("editor"), E->OpenUrl, 12.f,
				[Id](bool bOk, const FString& What)
				{
					if (bOk) return;
					UE_LOG(LogBF6Portal, Warning,
						TEXT("Portal: the learned address did not land (%s), clicking the site's card instead"), *What);
					OpenByCard(Id);
				});
			return;
		}
		OpenByCard(Id);
	}

	void StartAttempt(const FString& Id)
	{
		GJob.Deadline = FPlatformTime::Seconds() + 20.0;
		// This attempt has not been answered yet, whatever any earlier run
		// left behind. Without this the very first tick imports the cache.
		if (FExp* Prev = Find(Id)) { Prev->bFetchedThisAttempt = false; }
		// THE API IS THE NORMAL PATH. Once the page has seen the site ask for an
		// experience, the tool asks for one the same way: one replayed request,
		// from wherever the panel happens to be standing, with no navigation and
		// nothing clicked. Navigating and clicking a card is what the next block
		// still does, and it now only runs when there is no request to imitate.
		if (CanReplay(TEXT("getPlayElement")))
		{
			UE_LOG(LogBF6Portal, Display, TEXT("Portal import: asking the site's api for %s"), *Id.Left(8));
			RequestReplay(Id);
			return;
		}
		FExp* E = Find(Id);
		const FString Landed = E ? E->OpenUrl : FString();
		// Attempt 1 may use an address the site itself landed on for THIS
		// experience, and only that: it was observed, not built.
		if (GJob.Attempt == 1 && !Landed.IsEmpty())
		{
			GNavTarget = Landed;
			UE_LOG(LogBF6Portal, Display, TEXT("Portal import: trying the address the site landed on last time: %s"), *Landed);
			ExpectPage(TEXT("editor"), Landed, 12.f,
				[Id](bool bOk, const FString& What)
				{
					if (bOk) { RequestReplay(Id); return; }
					UE_LOG(LogBF6Portal, Warning,
						TEXT("Portal import: the learned address did not land (%s), going back to clicking the card"), *What);
					OpenByCard(Id);
				});
			return;
		}
		OpenByCard(Id);
	}

	void FinishJob()
	{
		GJob.bActive = false;
		GJob.Current.Reset();
		BF6PortalWeb::HoldOffscreen(false);   // the page may go now
		if (GJob.bSignedOut)
		{
			// NOT a failure count. Being signed out is one fact with one fix,
			// and reporting it as "1 failed" sent people looking at their map.
			GWorkStatus = TEXT("Portal signed you out. Sign in on the panel and press Import again.");
			UE_LOG(LogBF6Portal, Warning, TEXT("Portal import stopped: %s"), *GWorkStatus);
		}
		else
		{
			GWorkStatus = FString::Printf(TEXT("%d imported, %d without map data, %d failed."),
				GJob.Imported, GJob.NoData, GJob.Failed);
			UE_LOG(LogBF6Portal, Display, TEXT("Portal import finished: %s"), *GWorkStatus);
		}
		BF6Api::Toast(GWorkStatus);
		// OPENING AFTER AN IMPORT THAT IMPORTED NOTHING IS A LOOP.
		//
		// OpenForEditing asks for the rotation, finds none, decides the
		// experience has not been imported yet, and starts an import that opens
		// it afterwards. If that import brings back no maps, the rotation is
		// still empty when it finishes, so opening again starts the same import
		// again: once a second, against the live site, until the editor is
		// closed. Seen doing exactly that on an experience whose rotation is
		// empty.
		//
		// So the open only happens when the import actually produced something.
		// Nothing imported means the reason is already on screen and in the log,
		// and the useful thing is to stop and let it be read.
		const bool bWorthOpening = !GJob.bSignedOut && GJob.Imported > 0;
		const FString Open = bWorthOpening ? GJob.OpenAfter : FString();
		if (!GJob.OpenAfter.IsEmpty() && !bWorthOpening)
		{
			UE_LOG(LogBF6Portal, Warning,
				TEXT("Not opening %s: the import brought back no maps, so opening it would start the ")
				TEXT("same import again. %s"), *GJob.OpenAfter, *GWorkStatus);
		}
		GJob.OpenAfter.Reset();
		Bump();
		if (!Open.IsEmpty()) BF6PortalProfile::OpenForEditing(Open);
	}

	bool StartQueue(const TArray<FString>& Ids, const FString& OpenAfter)
	{
		// Refusing up front is the difference between one sentence and ten
		// rounds of the same failed navigation.
		if (!LooksSignedIn())
		{
			GWorkStatus = TEXT("Portal signed you out. Sign in on the panel and press Import again.");
			UE_LOG(LogBF6Portal, Warning, TEXT("Portal import refused: %s (state %s, page %s)"),
				*GWorkStatus, *BF6PortalProfile::StateLabel(), GLastKind.IsEmpty() ? TEXT("none") : *GLastKind);
			BF6Api::Toast(GWorkStatus);
			Bump();
			return false;
		}
		GJob = FImportJob();
		GJob.Queue = Ids;
		GJob.Total = Ids.Num();
		GJob.OpenAfter = OpenAfter;
		GJob.bActive = true;
		// The page has to stay alive for the whole run, whatever the user does
		// with the panel in the meantime. HIDE now takes it offscreen instead
		// of tearing it down.
		BF6PortalWeb::HoldOffscreen(true);
		GWorkStatus = Ids.Num() == 1
			? FString(TEXT("Starting..."))
			: FString::Printf(TEXT("Importing %d experiences..."), Ids.Num());
		Bump();
		return true;
	}

	void TickJob()
	{
		if (!GJob.bActive) return;
		if (GJob.bCancel)
		{
			UE_LOG(LogBF6Portal, Display, TEXT("Portal import cancelled."));
			GJob.OpenAfter.Reset();
			GJob.Queue.Reset();
			FinishJob();
			return;
		}
		// A replay answered 401 or 403 is a sign-out, not a failed experience:
		// NoteSessionLost has already said so, and the run stops here rather
		// than counting the same refusal once per experience.
		if (GLost && !GJob.bSignedOut)
		{
			GJob.bSignedOut = true;
			UE_LOG(LogBF6Portal, Warning, TEXT("Portal import: the session went away mid-run, stopping."));
		}
		// A sign-out ends the whole run at once. Carrying on would be nine more
		// timeouts against a login page.
		if (GJob.bSignedOut)
		{
			GJob.Queue.Reset();
			FinishJob();
			return;
		}
		const double Now = FPlatformTime::Seconds();
		if (!GJob.Current.IsEmpty())
		{
			FExp* E = Find(GJob.Current);
			if (E && E->bFetchedThisAttempt)
			{
				GWorkStatus = FString::Printf(TEXT("Importing '%s'..."), *E->Name);
				ImportFetched(*E, GJob.Imported, GJob.NoData, GJob.Failed);
				GJob.Current.Reset();
				GJob.Attempt = 0;
			}
			else if (Now > GJob.Deadline)
			{
				if (GJob.Attempt < kMaxAttempts)
				{
					GJob.Attempt++;
					UE_LOG(LogBF6Portal, Warning,
						TEXT("Portal import: '%s' did not answer, retrying (attempt %d of %d) by clicking its card"),
						E ? *E->Name : *GJob.Current.Left(8), GJob.Attempt, kMaxAttempts);
					StartAttempt(GJob.Current);
				}
				else
				{
					// Signed in, so this is the OTHER failure: the experience
					// itself could not be opened.
					UE_LOG(LogBF6Portal, Warning,
						TEXT("Portal import: signed in, but '%s' could not be opened on the site after %d attempts. Last asked for %s, landed on %s"),
						E ? *E->Name : *GJob.Current.Left(8), kMaxAttempts,
						GNavTarget.IsEmpty() ? TEXT("(nothing)") : *GNavTarget,
						GNavLanded.IsEmpty() ? *GLastUrl : *GNavLanded);
					GJob.Failed++;
					GJob.Current.Reset();
					GJob.Attempt = 0;
				}
			}
			return;
		}
		if (GJob.Queue.Num() == 0) { FinishJob(); return; }
		GJob.Current = GJob.Queue[0];
		GJob.Queue.RemoveAt(0);
		GJob.Attempt = 1;
		FExp* E = Find(GJob.Current);
		GWorkStatus = FString::Printf(TEXT("Opening '%s' on the site..."), E ? *E->Name : *GJob.Current.Left(8));
		StartAttempt(GJob.Current);
		Bump();
	}

	// ---- 6a. search ----------------------------------------------------------
	//
	// Nobody should have to read a uuid out of a cache folder to import their
	// own experience. Every match here is on something a person can actually
	// see: the name, the first characters of the id, or a map codename.

	bool MatchesSearch(const FExp& E, const FString& Needle)
	{
		if (Needle.IsEmpty()) return true;
		if (E.Name.Contains(Needle, ESearchCase::IgnoreCase)) return true;
		if (E.Id.StartsWith(Needle, ESearchCase::IgnoreCase)) return true;
		if (E.Description.Contains(Needle, ESearchCase::IgnoreCase)) return true;
		for (const FString& M : E.Maps) if (M.Contains(Needle, ESearchCase::IgnoreCase)) return true;
		return false;
	}

	// ---- 6c. the whole experience, as one file -------------------------------
	//
	// The site's own import and export format, verified against a real 2.9 MB
	// export rather than described from memory:
	//
	//   mutators           an object. A single-valued setting is a bare number;
	//                      a per-team one is an array of [team, value] PAIRS,
	//                      numbered from zero.
	//   assetRestrictions  an object, empty when nothing is restricted
	//   name, description  strings
	//   mapRotation        one entry per slot, {id, spatialAttachment}. The id is
	//                      "<map codename>-<game mode><n>".
	//   workspace          the block editor's workspace, {"mod":{...}}
	//   teamComposition    [[1, {"humanCapacity": n}], ...], numbered from one
	//   gameMode           a string
	//   attachments        every attachment, attachmentType 1 spatial, 2 script,
	//                      3 blacklist, 4 strings, each with attachmentData.
	//                      original holding the file's text as base64.
	//
	// Reading one needs no network at all, which is what makes this path
	// testable with the panel closed and the site unreachable.

	const TCHAR* kAttachmentKindNames[] = { TEXT("unknown"), TEXT("spatial"), TEXT("script"), TEXT("blacklist"), TEXT("strings") };
	const TCHAR* AttachmentKindName(int32 Kind)
	{
		return (Kind >= 0 && Kind < UE_ARRAY_COUNT(kAttachmentKindNames)) ? kAttachmentKindNames[Kind] : TEXT("unknown");
	}

	// An experience read out of a file has no id: the site does not put one in
	// its export. When the name matches something already in the list that id is
	// used, so an import lands on the real experience. Otherwise the file gets a
	// stable id of its own, derived from its name, so it has a folder, a cache
	// entry and a place in the panel even with no account linked at all.
	FString LocalIdFor(const FString& Name)
	{
		const FString Seed = FString(TEXT("bf6-unreal-sdk-local:")) + Name;
		const FString Hash = FMD5::HashAnsiString(*Seed);   // 32 hex characters
		if (Hash.Len() < 32) return FString();
		return FString::Printf(TEXT("%s-%s-%s-%s-%s"),
			*Hash.Mid(0, 8), *Hash.Mid(8, 4), *Hash.Mid(12, 4), *Hash.Mid(16, 4), *Hash.Mid(20, 12)).ToLower();
	}

	FString IdForImportedName(const FString& Name)
	{
		for (const FExp& E : GExps)
			if (!E.Name.IsEmpty() && E.Name.Equals(Name, ESearchCase::IgnoreCase)) return E.Id;
		return LocalIdFor(Name);
	}

	bool ReadJsonFile(const FString& Path, TSharedPtr<FJsonObject>& Out)
	{
		FString Text;
		if (!FFileHelper::LoadFileToString(Text, *Path)) return false;
		TSharedRef<TJsonReader<>> R = TJsonReaderFactory<>::Create(Text);
		return FJsonSerializer::Deserialize(R, Out) && Out.IsValid();
	}

	// The base64 an attachment carries is the file's text. Decoded here and
	// written as UTF-8, which is what the level importer and the block editor
	// both read.
	bool AttachmentTextOf(const TSharedPtr<FJsonObject>& Att, FString& Out)
	{
		const TSharedPtr<FJsonObject>* Data = nullptr;
		if (!Att->TryGetObjectField(TEXT("attachmentData"), Data) || !Data || !Data->IsValid()) return false;
		FString B64;
		if (!(*Data)->TryGetStringField(TEXT("original"), B64) || B64.IsEmpty()) return false;
		TArray<uint8> Bytes;
		if (!FBase64::Decode(B64, Bytes) || Bytes.Num() == 0) return false;
		Bytes.Add(0);
		Out = FString(UTF8_TO_TCHAR(reinterpret_cast<const ANSICHAR*>(Bytes.GetData())));
		return !Out.IsEmpty();
	}

	FString Base64Of(const FString& Text)
	{
		FTCHARToUTF8 Utf8(*Text);
		TArray<uint8> Bytes;
		Bytes.Append(reinterpret_cast<const uint8*>(Utf8.Get()), Utf8.Length());
		return FBase64::Encode(Bytes);
	}

	int32 MapIdxFromMetadata(const FString& Meta)
	{
		return Meta.StartsWith(TEXT("mapIdx=")) ? FCString::Atoi(*Meta.RightChop(7)) : -1;
	}

	// What an import actually did, so the report is a count and not a feeling.
	struct FFileImport
	{
		FString Id, Name, GameMode;
		int32 Mutators = 0, Teams = 0, Restrictions = 0;
		int32 Rotation = 0, Spatial = 0, Script = 0, Strings = 0, Blacklist = 0, OtherAtt = 0;
		int32 WorkspaceChars = 0, WorkspaceBlocks = 0;
		int32 Saves = 0, NoData = 0, Failed = 0;
		FString Note;
	};
	FFileImport GLastFileImport;

	// A rough block count for the report: every object in the workspace that has
	// both a "type" and an "id" is a block. It is a count, not a parser.
	int32 CountBlocks(const TSharedPtr<FJsonValue>& V, int32 Depth = 0)
	{
		if (!V.IsValid() || Depth > 64) return 0;
		int32 N = 0;
		if (V->Type == EJson::Object)
		{
			const TSharedPtr<FJsonObject> O = V->AsObject();
			if (O.IsValid())
			{
				if (O->HasTypedField<EJson::String>(TEXT("type")) && O->HasTypedField<EJson::String>(TEXT("id"))) N++;
				for (const auto& P : O->Values) N += CountBlocks(P.Value, Depth + 1);
			}
		}
		else if (V->Type == EJson::Array)
		{
			for (const TSharedPtr<FJsonValue>& E : V->AsArray()) N += CountBlocks(E, Depth + 1);
		}
		return N;
	}

	// Take a whole-experience document into the tool's model. NO NETWORK.
	bool ApplyExperienceJson(const TSharedPtr<FJsonObject>& Root, const FString& PreferId, FFileImport& Out)
	{
		if (!Root.IsValid()) return false;
		FString Name;
		Root->TryGetStringField(TEXT("name"), Name);
		if (Name.IsEmpty()) Name = TEXT("Portal experience");
		FString Id = PreferId.ToLower();
		if (!IsUuid(Id)) Id = IdForImportedName(Name);
		if (!IsUuid(Id)) return false;

		FExp& E = FindOrAdd(Id);
		E.Name = Name;
		Root->TryGetStringField(TEXT("description"), E.Description);
		FString Mode;
		if (Root->TryGetStringField(TEXT("gameMode"), Mode) && !Mode.IsEmpty()) E.GameMode = Mode;
		// The document IS what the site has, so it is also the "before" half of
		// a later push: the text to find in the site's own message.
		E.SiteName = E.Name;
		E.SiteDescription = E.Description;
		Out.Id = Id;
		Out.Name = E.Name;
		Out.GameMode = E.GameMode;

		IFileManager::Get().MakeDirectory(*ExpDir(Id), true);

		// ---- the rotation ----
		const TArray<FString> KeepSaves = E.SaveNames;
		E.Maps.Reset();
		E.RotationIds.Reset();
		TMap<FString, int32> SpatialIdToSlot;
		const TArray<TSharedPtr<FJsonValue>>* Rot = nullptr;
		if (Root->TryGetArrayField(TEXT("mapRotation"), Rot))
		{
			for (int32 i = 0; i < Rot->Num(); i++)
			{
				const TSharedPtr<FJsonObject> O = (*Rot)[i]->AsObject();
				if (!O.IsValid()) continue;
				FString RotId;
				O->TryGetStringField(TEXT("id"), RotId);
				E.RotationIds.Add(RotId);
				E.Maps.Add(MapFromRotationId(RotId));
				const TSharedPtr<FJsonObject>* Sp = nullptr;
				if (O->TryGetObjectField(TEXT("spatialAttachment"), Sp) && Sp && Sp->IsValid())
				{
					FString AttId;
					(*Sp)->TryGetStringField(TEXT("id"), AttId);
					if (!AttId.IsEmpty()) SpatialIdToSlot.Add(AttId, i);
				}
			}
		}
		E.SaveNames.SetNum(E.Maps.Num());
		for (int32 i = 0; i < E.SaveNames.Num(); i++)
			if (E.SaveNames[i].IsEmpty() && KeepSaves.IsValidIndex(i)) E.SaveNames[i] = KeepSaves[i];
		Out.Rotation = E.Maps.Num();

		// ---- the attachments ----
		E.Files.Reset();
		const TArray<TSharedPtr<FJsonValue>>* Atts = nullptr;
		TArray<TSharedPtr<FJsonValue>> NonSpatial;
		if (Root->TryGetArrayField(TEXT("attachments"), Atts))
		{
			for (const TSharedPtr<FJsonValue>& AV : *Atts)
			{
				const TSharedPtr<FJsonObject> A = AV->AsObject();
				if (!A.IsValid()) continue;
				FAttachment At;
				A->TryGetStringField(TEXT("filename"), At.FileName);
				double Kind = 0;
				A->TryGetNumberField(TEXT("attachmentType"), Kind);
				At.Kind = (int32)Kind;
				FString Meta;
				A->TryGetStringField(TEXT("metadata"), Meta);
				At.MapIdx = MapIdxFromMetadata(Meta);
				if (At.MapIdx < 0)
				{
					FString AttId;
					A->TryGetStringField(TEXT("id"), AttId);
					if (const int32* Slot = SpatialIdToSlot.Find(AttId)) At.MapIdx = *Slot;
				}
				if (At.FileName.IsEmpty()) At.FileName = FString::Printf(TEXT("attachment_%d.txt"), E.Files.Num());
				At.FileName = FPaths::GetCleanFilename(At.FileName);

				switch (At.Kind)
				{
				case 1: Out.Spatial++;   break;
				case 2: Out.Script++;    NonSpatial.Add(AV); break;
				case 3: Out.Blacklist++; NonSpatial.Add(AV); break;
				case 4: Out.Strings++;   NonSpatial.Add(AV); break;
				default: Out.OtherAtt++; NonSpatial.Add(AV); break;
				}

				A->TryGetStringField(TEXT("id"), At.AttId);
				A->TryGetStringField(TEXT("version"), At.Version);

				FString Text;
				// An empty attachment is a real thing on the site: both real
				// exports carry an empty blank.ts and an empty blacklist.json.
				// It gets no file on disk and it still belongs in the document,
				// so it is kept as an entry rather than dropped, which is what
				// made an export written from the tool two attachments short.
				At.bEmpty = !AttachmentTextOf(A, Text);
				if (!At.bEmpty)
				{
					At.Path = FPaths::Combine(ExpDir(Id), At.FileName);
					if (!WriteWithOriginal(At.Path, Text)) continue;
				}
				if (At.Kind == 1 && At.MapIdx < 0) At.MapIdx = 0;
				// The same for an experience imported from a file on disk: the
				// site export carries the very same attachments.
				if (!At.bEmpty)
				{
					if (At.Kind == 2)
						WriteProjectShared(Id, E.Name, TEXT("dist/bundle.ts"), Text);
					else if (At.Kind == 4)
						WriteProjectShared(Id, E.Name, TEXT("dist/bundle.strings.json"), Text);
				}
				E.Files.Add(At);
			}
		}

		// ---- where the shared half of an experience lives ----
		//
		// An experience has one workspace, one settings set and one rotation,
		// shared by every map in it, and they belong in the experience's own
		// project folder beside the script and the template. ExpDir(Id) is the
		// download folder: it keeps the raw file the site sent, with its .orig
		// beside it, which is what a later sync diffs against. Both are
		// written, because they answer different questions.
		//
		// Writing only the download folder is the bug this fixes: the import
		// pulled a 5,192 block workspace down correctly and the project then
		// reported "no block workspace has been captured", because nothing ever
		// put it where the project looks.
		// Both import routes go through the one writer, so fixing one can never
		// again leave the other silently broken.
		auto WriteShared = [&](const TCHAR* Rel, const FString& Text) -> bool
		{
			return WriteProjectShared(Id, E.Name, Rel, Text);
		};

		// ---- the block workspace ----
		const TSharedPtr<FJsonObject>* Ws = nullptr;
		TSharedPtr<FJsonObject> Workspace;
		if (Root->TryGetObjectField(TEXT("workspace"), Ws) && Ws && Ws->IsValid())
		{
			Workspace = *Ws;
			const FString Text = JsonToString(Workspace);
			Out.WorkspaceChars = Text.Len();
			Out.WorkspaceBlocks = CountBlocks(MakeShared<FJsonValueObject>(Workspace));
			WriteWithOriginal(FPaths::Combine(ExpDir(Id), TEXT("rules.blocks.json")), Text);
			WriteShared(TEXT("unreal/blockly/workspace.json"), Text);
		}

		// ---- the settings, teams and restrictions ----
		Out.Mutators = BF6PortalSettings::LoadExperienceJson(Id, Root);
		const TSharedPtr<FJsonObject>* MutObj = nullptr;
		if (Root->TryGetObjectField(TEXT("mutators"), MutObj) && MutObj && MutObj->IsValid())
			E.MutatorsJson = JsonToString(*MutObj);
		const TArray<TSharedPtr<FJsonValue>>* Teams = nullptr;
		if (Root->TryGetArrayField(TEXT("teamComposition"), Teams)) Out.Teams = Teams->Num();
		const TSharedPtr<FJsonObject>* Restrict = nullptr;
		if (Root->TryGetObjectField(TEXT("assetRestrictions"), Restrict) && Restrict && Restrict->IsValid())
			Out.Restrictions = (*Restrict)->Values.Num();

		// The three the site actually sent, gathered into the one file the
		// project reads. Only what came down is written: a key the export did
		// not carry is left out rather than invented, so the settings screen
		// can tell "the site says none" from "we never asked".
		{
			TSharedRef<FJsonObject> S = MakeShared<FJsonObject>();
			bool bAny = false;
			if (MutObj && MutObj->IsValid()) { S->SetObjectField(TEXT("mutators"), *MutObj); bAny = true; }
			if (Teams) { S->SetArrayField(TEXT("teamComposition"), *Teams); bAny = true; }
			if (Restrict && Restrict->IsValid())
			{
				S->SetObjectField(TEXT("assetRestrictions"), *Restrict); bAny = true;
			}
			if (!E.GameMode.IsEmpty()) S->SetStringField(TEXT("gameMode"), E.GameMode);
			if (bAny) WriteShared(TEXT("unreal/settings/settings.json"), JsonToString(S));
		}

		// The rotation: which map sits in which slot, and what each one is
		// called on disk. Without it a reopened project knows its maps only by
		// the folders they happen to have left behind.
		WriteShared(TEXT("unreal/rotation.json"), RotationDoc(E));

		// ---- the block editor and the script editor ----
		//
		// Handed the same document with the SPATIAL attachments taken out: the
		// profile makes the map saves itself, named and linked to their rotation
		// slot, and the block editor's own router would otherwise make a second
		// set under different names.
		if (Workspace.IsValid() || NonSpatial.Num() > 0)
		{
			TSharedRef<FJsonObject> ForBlocks = MakeShared<FJsonObject>();
			ForBlocks->SetStringField(TEXT("name"), E.Name);
			ForBlocks->SetStringField(TEXT("description"), E.Description);
			ForBlocks->SetStringField(TEXT("gameMode"), E.GameMode);
			if (Workspace.IsValid()) ForBlocks->SetObjectField(TEXT("workspace"), Workspace);
			ForBlocks->SetArrayField(TEXT("attachments"), NonSpatial);
			const FString Path = FPaths::Combine(ExpDir(Id), TEXT("experience.forblocks.json"));
			if (FFileHelper::SaveStringToFile(JsonToString(ForBlocks), *Path,
				FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
			{
				Out.Note = Path;
			}
		}

		E.bFetched = true;
		WriteCache(E);
		Bump();
		return true;
	}

	// The site's own export format, written by the tool from what it holds.
	// The key order is the site's, taken from a real export.
	TSharedPtr<FJsonObject> BuildExperienceJson(const FExp& E)
	{
		TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();

		// mutators: the site's live values when the panel has read them,
		// otherwise what the last response or file carried.
		TSharedPtr<FJsonObject> Muts;
		if (!BF6PortalSettings::ExperienceMutators(E.Id, Muts) || !Muts.IsValid())
			Muts = JsonFromString(E.MutatorsJson);
		Root->SetObjectField(TEXT("mutators"), Muts.IsValid() ? Muts : MakeShared<FJsonObject>());

		TSharedPtr<FJsonObject> Restrict;
		if (!BF6PortalSettings::ExperienceRestrictions(E.Id, Restrict) || !Restrict.IsValid())
			Restrict = MakeShared<FJsonObject>();
		Root->SetObjectField(TEXT("assetRestrictions"), Restrict);

		Root->SetStringField(TEXT("name"), E.Name);
		Root->SetStringField(TEXT("description"), E.Description);

		// One rotation entry per slot, each carrying its spatial attachment.
		TArray<TSharedPtr<FJsonValue>> Rot;
		TArray<TSharedPtr<FJsonValue>> AllAtts;
		auto MakeAttachment = [&E](const FAttachment& A, bool bForRotation) -> TSharedPtr<FJsonObject>
		{
			TSharedRef<FJsonObject> O = MakeShared<FJsonObject>();
			const FString Text = A.bEmpty ? FString() : ReadTextFile(A.Path);
			// The site's own id and version, carried through: a document that
			// drops them is not the document the site would import back.
			const FString Version = A.Version.IsEmpty() ? (A.Kind == 1 ? FString(TEXT("123")) : FString()) : A.Version;
			// The site's own key order, which differs between the rotation copy
			// and the attachments list copy; both are taken from a real export.
			if (bForRotation)
			{
				O->SetStringField(TEXT("id"), A.AttId);
				O->SetStringField(TEXT("filename"), A.FileName);
				O->SetStringField(TEXT("metadata"), A.MapIdx >= 0 ? FString::Printf(TEXT("mapIdx=%d"), A.MapIdx) : FString());
				O->SetStringField(TEXT("version"), Version);
			}
			else
			{
				O->SetStringField(TEXT("id"), A.AttId);
				O->SetStringField(TEXT("version"), Version);
				O->SetStringField(TEXT("filename"), A.FileName);
			}
			O->SetBoolField(TEXT("isProcessable"), true);
			O->SetNumberField(TEXT("processingStatus"), 2);
			TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
			Data->SetStringField(TEXT("original"), Base64Of(Text));
			Data->SetStringField(TEXT("compiled"), TEXT(""));
			O->SetObjectField(TEXT("attachmentData"), Data);
			O->SetNumberField(TEXT("attachmentType"), A.Kind);
			if (!bForRotation && A.Kind == 1)
				O->SetStringField(TEXT("metadata"), FString::Printf(TEXT("mapIdx=%d"), A.MapIdx));
			O->SetArrayField(TEXT("errors"), TArray<TSharedPtr<FJsonValue>>());
			return O;
		};

		for (int32 i = 0; i < E.Maps.Num(); i++)
		{
			TSharedRef<FJsonObject> Slot = MakeShared<FJsonObject>();
			const FString RotId = E.RotationIds.IsValidIndex(i) && !E.RotationIds[i].IsEmpty()
				? E.RotationIds[i]
				: FString::Printf(TEXT("%s-%s0"), *E.Maps[i], *E.GameMode);
			Slot->SetStringField(TEXT("id"), RotId);
			for (const FAttachment& A : E.Files)
				if (A.Kind == 1 && A.MapIdx == i)
				{
					Slot->SetObjectField(TEXT("spatialAttachment"), MakeAttachment(A, true));
					break;
				}
			Rot.Add(MakeShared<FJsonValueObject>(Slot));
		}
		Root->SetArrayField(TEXT("mapRotation"), Rot);

		const TSharedPtr<FJsonObject> Ws = JsonFromString(ReadTextFile(FPaths::Combine(ExpDir(E.Id), TEXT("rules.blocks.json"))));
		Root->SetObjectField(TEXT("workspace"), Ws.IsValid() ? Ws : MakeShared<FJsonObject>());

		// teamComposition, numbered from ONE, against mutator teams numbered
		// from zero. Where the site's own composition has been read it is used
		// as it stands; otherwise it is derived from MaxPlayerCount_PerTeam,
		// which is what humanCapacity holds in every export checked.
		TArray<TSharedPtr<FJsonValue>> Teams;
		if (!BF6PortalSettings::ExperienceTeams(E.Id, Teams) || Teams.Num() == 0)
		{
			Teams.Reset();
			TArray<double> Caps;
			if (Muts.IsValid())
			{
				const TSharedPtr<FJsonValue> Max = Muts->TryGetField(TEXT("MaxPlayerCount_PerTeam"));
				if (Max.IsValid() && Max->Type == EJson::Number) { Caps.Add(Max->AsNumber()); Caps.Add(Max->AsNumber()); }
				else if (Max.IsValid() && Max->Type == EJson::Array)
					for (const TSharedPtr<FJsonValue>& P : Max->AsArray())
						if (P.IsValid() && P->Type == EJson::Array && P->AsArray().Num() == 2) Caps.Add(P->AsArray()[1]->AsNumber());
			}
			for (int32 i = 0; i < Caps.Num(); i++)
			{
				TSharedRef<FJsonObject> Comp = MakeShared<FJsonObject>();
				Comp->SetNumberField(TEXT("humanCapacity"), Caps[i]);
				TArray<TSharedPtr<FJsonValue>> Pair;
				Pair.Add(MakeShared<FJsonValueNumber>((double)(i + 1)));
				Pair.Add(MakeShared<FJsonValueObject>(Comp));
				Teams.Add(MakeShared<FJsonValueArray>(Pair));
			}
		}
		Root->SetArrayField(TEXT("teamComposition"), Teams);
		Root->SetStringField(TEXT("gameMode"), E.GameMode);

		for (const FAttachment& A : E.Files) AllAtts.Add(MakeShared<FJsonValueObject>(MakeAttachment(A, false)));
		Root->SetArrayField(TEXT("attachments"), AllAtts);
		return Root;
	}

	// ---- 6d. push ------------------------------------------------------------
	//
	// One action that sends the whole experience back, and never a message built
	// from scratch: a partial updatePlayElement wipes the experience's
	// attachments, so the site's OWN last message is resent with only the
	// changed fields different. Each change is a pair - the text the site has
	// now, and the text the tool has - and the page refuses the whole push if
	// any "now" text is not in the message it is rewriting.
	//
	// This is exactly what the thumbnail path already does; it is the same code
	// in the page, given a list instead of one pair.
	int32 CollectPushPairs(const FExp& E, TArray<TPair<FString, FString>>& Out)
	{
		if (!E.SiteName.IsEmpty() && E.SiteName != E.Name) Out.Emplace(E.SiteName, E.Name);
		if (!E.SiteDescription.IsEmpty() && E.SiteDescription != E.Description) Out.Emplace(E.SiteDescription, E.Description);
		FString Old, New;
		if (ChangedSinceSite(FPaths::Combine(ExpDir(E.Id), TEXT("rules.blocks.json")), Old, New)) Out.Emplace(Old, New);
		for (const FAttachment& A : E.Files)
			if (!A.Path.IsEmpty() && ChangedSinceSite(A.Path, Old, New)) Out.Emplace(Old, New);
		return Out.Num();
	}

	// ---- 6e. the import path everything lands in -----------------------------
	//
	// A file the user picked, and the document the site's own Export produced,
	// are the same thing and go through the same code. That is what makes the
	// whole path testable with no network at all.

	FString FirstFetchedId()
	{
		for (const FExp& E : GExps) if (E.bFetched) return E.Id;
		return GExps.Num() ? GExps[0].Id : FString();
	}

	// Which rotation slots actually changed, so a re-sync leaves the rest alone.
	TSet<int32> ChangedSlotsOf(const FExp& E, const TMap<int32, FString>& Before)
	{
		TSet<int32> Out;
		for (const FAttachment& A : E.Files)
		{
			if (A.Kind != 1 || A.MapIdx < 0) continue;
			const FString* Was = Before.Find(A.MapIdx);
			const FString Now = ReadTextFile(A.Path);
			if (!Was || *Was != Now) Out.Add(A.MapIdx);
		}
		// A slot with no spatial at all still needs its base save the first time.
		for (int32 i = 0; i < E.Maps.Num(); i++)
			if (!Before.Contains(i) && !E.SaveNames.IsValidIndex(i)) Out.Add(i);
		for (int32 i = 0; i < E.Maps.Num(); i++)
			if (E.SaveNames.IsValidIndex(i) && E.SaveNames[i].IsEmpty()) Out.Add(i);
		return Out;
	}

	// Read a whole-experience document into the tool. Reason names where it came
	// from, for the one line this always logs.
	bool ImportExperienceText(const FString& Text, const FString& PreferId, const FString& Reason)
	{
		// Every save this makes is the tool's own, so nothing here may be read
		// back as the user having saved.
		FSyncGuard NoSyncWhileImporting;
		TSharedPtr<FJsonObject> Root;
		TSharedRef<TJsonReader<>> R = TJsonReaderFactory<>::Create(Text);
		if (!FJsonSerializer::Deserialize(R, Root) || !Root.IsValid())
		{
			GWorkStatus = FString::Printf(TEXT("%s is not a Portal experience export."), *Reason);
			UE_LOG(LogBF6Portal, Warning, TEXT("Portal import: %s"), *GWorkStatus);
			Bump();
			return false;
		}

		// What each map's spatial looked like before, so an in-place re-import
		// can leave the saves that did not change alone.
		TMap<int32, FString> Before;
		FString Probe = PreferId.ToLower();
		if (!IsUuid(Probe))
		{
			FString N;
			Root->TryGetStringField(TEXT("name"), N);
			Probe = IdForImportedName(N.IsEmpty() ? FString(TEXT("Portal experience")) : N);
		}
		if (const FExp* Old = Find(Probe))
			for (const FAttachment& A : Old->Files)
				if (A.Kind == 1 && A.MapIdx >= 0) Before.Add(A.MapIdx, ReadTextFile(A.Path));

		FFileImport Info;
		if (!ApplyExperienceJson(Root, PreferId, Info))
		{
			GWorkStatus = FString::Printf(TEXT("%s could not be read as an experience."), *Reason);
			UE_LOG(LogBF6Portal, Warning, TEXT("Portal import: %s"), *GWorkStatus);
			Bump();
			return false;
		}

		FExp* E = Find(Info.Id);
		if (!E) return false;
		const TSet<int32> Changed = ChangedSlotsOf(*E, Before);
		const bool bFirstTime = Before.Num() == 0;
		ImportFetched(*E, Info.Saves, Info.NoData, Info.Failed, bFirstTime ? nullptr : &Changed);

		// The block editor and the script editor get the rest.
		if (!Info.Note.IsEmpty()) BF6Blocks::LoadFile(Info.Note);

		GLastFileImport = Info;
		GWorkStatus = FString::Printf(
			TEXT("'%s': %d setting(s), %d team(s), %d map(s), %d spatial, %d script, %d strings, %d blacklist, %d block(s). %d save(s) written, %d without map data, %d failed."),
			*Info.Name, Info.Mutators, Info.Teams, Info.Rotation, Info.Spatial, Info.Script, Info.Strings,
			Info.Blacklist, Info.WorkspaceBlocks, Info.Saves, Info.NoData, Info.Failed);
		UE_LOG(LogBF6Portal, Display, TEXT("Portal import (%s): %s"), *Reason, *GWorkStatus);

		// The saves this run just made are the new baseline, so not one of them
		// is read back as the user saving once auto-sync starts watching again.
		GSaveStamps.Reset();

		// ONE LINE AT THE END, saying whether anything actually changed.
		if (bFirstTime)
		{
			UE_LOG(LogBF6Portal, Display,
				TEXT("Portal sync: first import of '%s', all %d map(s) written."), *Info.Name, Info.Rotation);
		}
		else if (Changed.Num() == 0)
		{
			UE_LOG(LogBF6Portal, Display,
				TEXT("Portal sync: nothing changed in '%s'; all %d map(s) were left alone."), *Info.Name, Info.Rotation);
		}
		else
		{
			UE_LOG(LogBF6Portal, Display,
				TEXT("Portal sync: %d of %d map(s) in '%s' changed and were rewritten; the rest were left alone."),
				Changed.Num(), Info.Rotation, *Info.Name);
		}
		BF6Api::Toast(GWorkStatus);
		Bump();
		return true;
	}

	// Bring one experience back in step. The site's own Export is the authority
	// on the payload, so it is what is asked for; the api replay is the fast
	// path for the values and runs alongside it.
	// The document the site's own Export produced, caught in the page and never
	// written to the user's downloads folder. A copy is kept beside the rest of
	// the experience's files, because it is the complete payload and the only
	// thing that can be diffed later against what the tool writes itself.
	// The script the site last showed, journalled where the tool can find it.
	// Journalled and never adopted on its own: the tool's own script project is
	// the user's work, and a keystroke on the site must not overwrite it.
	FString WatchedScriptPath()
	{
		const FString Id = CurrentExperienceId();
		const FString Dir = IsUuid(Id) ? ExpDir(Id) : PortalRoot();
		return FPaths::Combine(Dir, TEXT("site_script.ts"));
	}

	void HandleSiteScript(const FString& Text)
	{
		const FString Path = WatchedScriptPath();
		IFileManager::Get().MakeDirectory(*FPaths::GetPath(Path), true);
		FFileHelper::SaveStringToFile(Text, *Path, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
		GWatchStatus = FString::Printf(TEXT("the site's script, %d characters, journalled to %s"),
			Text.Len(), *FPaths::GetCleanFilename(Path));
		GWatchLastChange = FString::Printf(TEXT("the script, at %s"), *FDateTime::Now().ToString(TEXT("%H:%M:%S")));
		GWatchLastAt = FPlatformTime::Seconds();
		UE_LOG(LogBF6Portal, Display,
			TEXT("Portal watch: the site's script changed (%d characters). Written to %s. BF6.Portal.Watch.Adopt takes it into the tool."),
			Text.Len(), *Path);
		Bump();
	}

	void HandleExportText(const FString& Text, const FString& WantId, const FString& FileName)
	{
		FString Id = WantId.ToLower();
		if (!IsUuid(Id)) Id = GExportWantId;
		GExportWantId.Reset();
		UE_LOG(LogBF6Portal, Display, TEXT("Portal export from site: caught '%s', %d characters, for %s"),
			*FileName, Text.Len(), Id.IsEmpty() ? TEXT("whichever experience it names") : *Id.Left(8));
		if (IsUuid(Id))
		{
			IFileManager::Get().MakeDirectory(*ExpDir(Id), true);
			FFileHelper::SaveStringToFile(Text, *FPaths::Combine(ExpDir(Id), TEXT("site_export.json")),
				FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
		}
		ImportExperienceText(Text, Id, FileName.IsEmpty() ? FString(TEXT("the site's own export")) : FileName);
	}

	// ---- 6f. push by import, the site's own way in ---------------------------
	//
	// The site imports a whole experience, and that is the tool's push: the same
	// document the site itself exports, handed back to the site's own importer.
	// It carries everything at once - settings, teams, rotation, workspace,
	// script and every spatial - where a rewritten updatePlayElement can only
	// carry the strings it can find.
	//
	// The control matters. The import on the experiences front page makes a NEW
	// experience and asks the user to name it, so it is never used. The one that
	// replaces an open experience is inside the experience editor, reached the
	// way the tool already reaches it: the tile's Modify button.
	//
	// The payload is base64 so nothing in a block of script or a map name can
	// end a JS statement early, and it goes over in chunks because a whole
	// experience runs to megabytes.
	const int32 kPushChunk = 192 * 1024;
	FString GPushChunkId;
	FString GPushExpId;
	double  GPushStartedAt = 0.0;

	// A push waiting for the page. THE TOOL NEVER SENDS FIRST.
	//
	// A navigation replaces the document, and the capture script is injected
	// into the new one a beat AFTER the address changes. A push that streams as
	// soon as the address looks right lands every call on nothing:
	//   BF6CAPTURE chunk error TypeError: ... reading 'toolChunk'   x7
	//   BF6CAPTURE import error TypeError: ... reading 'importExperience'
	//   BF6CAPTURE ready v2                <- the injection arrives afterwards
	// So the push probes with a TOKEN and waits for that token to come back.
	// Only a live document can answer, because only a live document has the
	// function that answers in it, and the token is new for every push, so a
	// stale ready cannot pass for a fresh one.
	struct FPushWait
	{
		FString Id, Text, Token;
		double  Deadline = 0.0;
		double  NextProbeAt = 0.0;
		int32   Restarts = 0;
		bool    bActive = false;
	};
	FPushWait GPushWait;
	const int32 kMaxPushRestarts = 2;
	// A push runs across many frames, so it cannot hold a scoped guard. It sets
	// this instead, and auto-sync stands down for as long as it is up.
	bool GPushInFlight = false;

	// Auto-sync must not react to anything the tool is doing to the experience
	// itself, whether that is an import running inside one call or a push
	// spread over half a minute.
	bool SyncSuspended() { return GSyncSuspend > 0 || GPushInFlight || GPushWait.bActive; }

	void EndPush() { GPushInFlight = false; GPushWait = FPushWait(); GPushChunkId.Reset(); GPushStartedAt = 0.0; }

	void SendImportToPage(const FString& Id, const FString& Text)
	{
		const FExp* E = Find(Id);
		FString Name = Sanitise(E ? E->Name : Id).ToLower().Replace(TEXT(" "), TEXT("_"));
		Name.ReplaceInline(TEXT("'"), TEXT(""));
		Name.ReplaceInline(TEXT("\\"), TEXT(""));

		const FString B64 = Base64Of(Text);
		const FString ChunkId = FString::Printf(TEXT("push-%s-%lld-%d"),
			*Id.Left(8), (int64)FDateTime::Now().ToUnixTimestamp(), GPushWait.Restarts);
		const int32 Of = FMath::Max(1, FMath::DivideAndRoundUp(B64.Len(), kPushChunk));
		UE_LOG(LogBF6Portal, Display, TEXT("Portal push: streaming %d chunk(s) to the page as transfer %s"), Of, *ChunkId);
		// The receiver is opened first and drops every other transfer, so a
		// half-finished one from a previous document can never be spliced in.
		BF6PortalWeb::Exec(FString::Printf(
			TEXT("try { window.BF6PortalCapture.toolBegin('%s', %d); } catch (e) { console.log('BF6CAPTURE begin error ' + e); }"),
			*ChunkId, Of));
		for (int32 i = 0; i < Of; i++)
		{
			const FString Part = B64.Mid(i * kPushChunk, kPushChunk);
			BF6PortalWeb::Exec(FString::Printf(
				TEXT("try { window.BF6PortalCapture.toolChunk('%s', %d, %d, '%s'); } catch (e) { console.log('BF6CAPTURE chunk error ' + e); }"),
				*ChunkId, i, Of, *Part));
		}
		GPushChunkId = ChunkId;
		GPushExpId = Id;
		GPushStartedAt = FPlatformTime::Seconds();
		BF6PortalWeb::Exec(FString::Printf(
			TEXT("try { window.BF6PortalCapture.importExperience('%s', '%s', '%s'); } catch (e) { console.log('BF6CAPTURE import error ' + e); }"),
			*ChunkId, *Id, *Name));
		UE_LOG(LogBF6Portal, Display,
			TEXT("Portal push: handed %d characters of '%s' to the page in %d chunk(s) for the site's own importer"),
			Text.Len(), *Name, Of);
	}

	// When no import control can be found. The file is written, its path goes on
	// the clipboard, and the user is told exactly what to do with it.
	void PushFallbackToFile(const FString& Id, const FString& Text, const FString& Why)
	{
		EndPush();
		const FString Path = FPaths::Combine(ExpDir(Id), TEXT("push_experience.json"));
		if (FFileHelper::SaveStringToFile(Text, *Path, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
		{
			FPlatformApplicationMisc::ClipboardCopy(*Path);
			GPushStatus = FString::Printf(
				TEXT("%s The whole experience is written to %s and the path is on your clipboard: use the site's own import inside the experience."),
				*Why, *Path);
		}
		else
		{
			GPushStatus = FString::Printf(TEXT("%s The file could not be written either (%s)."), *Why, *Path);
		}
		UE_LOG(LogBF6Portal, Warning, TEXT("Portal push: %s"), *GPushStatus);
		BF6Api::Toast(GPushStatus);
		Bump();
	}

	void RequestSync(const FString& Id, const TCHAR* Why)
	{
		if (SyncSuspended())
		{
			UE_LOG(LogBF6Portal, Verbose,
				TEXT("Portal auto-sync: not refreshing %s while the tool is writing to it (%s)"), *Id.Left(8), Why);
			return;
		}
		const double Now = FPlatformTime::Seconds();
		if (Now - GLastSyncAt < 3.0) return;   // one save, not one per keystroke
		GLastSyncAt = Now;
		GPendingSyncId.Reset();
		UE_LOG(LogBF6Portal, Display, TEXT("Portal auto-sync: refreshing %s because %s"), *Id.Left(8), Why);
		if (CanReplay(TEXT("getPlayElement"))) RequestReplay(Id);
		BF6PortalProfile::ExportFromSite(Id);
	}

	// ---- 6b. the experience thumbnail ---------------------------------------
	//
	// The site's publish step two wants exactly 352 x 248, JPEG or PNG, at most
	// 78 KB (verified from De Luca's scripts\export-thumbnail.js). The tool
	// produces exactly that from either a live viewport capture or an image off
	// disk: scale to fit, crop, then walk the JPEG quality down from 90 until it
	// is under the cap - the same ladder the template uses.

	FString ThumbDir()
	{
		return FPaths::ConvertRelativePathToFull(FPaths::Combine(
			FPaths::ProjectSavedDir(), TEXT("BF6UnrealSDK"), TEXT("portal"), TEXT("thumbnails")));
	}

	// The experience the thumbnail panel is working on: the one the open save
	// belongs to, or the only one that has been fetched.
	FString CurrentExperienceId()
	{
		const FString FromSave = FindUuid(BF6PortalWeb::ExperienceForSave(BF6Api::CurrentLevel(), BF6Api::CurrentSave()));
		if (!FromSave.IsEmpty()) return FromSave;
		return FindUuid(BF6PortalWeb::CurrentUrl());
	}

	void MakeBrushFor(const TArray<FColor>& Pixels, int32 W, int32 H,
		TStrongObjectPtr<UTexture2D>& OutTex, TSharedPtr<FSlateBrush>& OutBrush, const TCHAR* Name)
	{
		if (W <= 0 || H <= 0 || Pixels.Num() < W * H) return;
		UTexture2D* T = UTexture2D::CreateTransient(W, H, PF_B8G8R8A8);
		if (!T) return;
		T->SRGB = true;
		void* Data = T->GetPlatformData()->Mips[0].BulkData.Lock(LOCK_READ_WRITE);
		FMemory::Memcpy(Data, Pixels.GetData(), (SIZE_T)W * H * sizeof(FColor));
		T->GetPlatformData()->Mips[0].BulkData.Unlock();
		T->UpdateResource();
		OutTex.Reset(T);
		OutBrush = MakeShared<FSlateBrush>();
		OutBrush->SetResourceObject(T);
		OutBrush->ImageSize = FVector2D((float)W, (float)H);
		UE_LOG(LogBF6Portal, Verbose, TEXT("Portal thumbnail: built %s texture %d x %d"), Name, W, H);
	}

	void AdoptSource(TArray<FColor>&& Pixels, int32 W, int32 H, const FString& What)
	{
		for (FColor& C : Pixels) C.A = 255;   // a screenshot's alpha is not a mask
		GThumb.Src = MoveTemp(Pixels);
		GThumb.SrcW = W; GThumb.SrcH = H;
		GThumb.SrcWhat = What;
		GThumb.CenterU = 0.5f; GThumb.CenterV = 0.5f; GThumb.Zoom = 1.f;
		MakeBrushFor(GThumb.Src, W, H, GThumb.SrcTex, GThumb.SrcBrush, TEXT("source"));
		GThumb.Status = FString::Printf(TEXT("Captured %s, %d x %d. Drag to move the frame, wheel to zoom."), *What, W, H);
		Bump();
	}

	// The crop rectangle in source pixels, at the site's 352:248 aspect.
	void CropRect(int32& X, int32& Y, int32& W, int32& H)
	{
		const float Aspect = (float)BF6PortalProfile::kThumbW / (float)BF6PortalProfile::kThumbH;
		float CW = (float)GThumb.SrcW, CH = CW / Aspect;
		if (CH > (float)GThumb.SrcH) { CH = (float)GThumb.SrcH; CW = CH * Aspect; }
		const float Z = FMath::Clamp(GThumb.Zoom, 1.f, 8.f);
		CW /= Z; CH /= Z;
		float CX = GThumb.CenterU * GThumb.SrcW - CW * 0.5f;
		float CY = GThumb.CenterV * GThumb.SrcH - CH * 0.5f;
		CX = FMath::Clamp(CX, 0.f, FMath::Max(0.f, GThumb.SrcW - CW));
		CY = FMath::Clamp(CY, 0.f, FMath::Max(0.f, GThumb.SrcH - CH));
		X = FMath::RoundToInt(CX); Y = FMath::RoundToInt(CY);
		W = FMath::Max(1, FMath::RoundToInt(CW)); H = FMath::Max(1, FMath::RoundToInt(CH));
		W = FMath::Min(W, GThumb.SrcW - X);
		H = FMath::Min(H, GThumb.SrcH - Y);
	}

	// ---- the tool's kit, as this file needs it -------------------------------
	// Mirrors the styles in BF6PortalWeb.cpp and BF6BuildMode.cpp.

	FSlateFontInfo FontBold(int32 Size) { return FCoreStyle::GetDefaultFontStyle("Bold", Size); }
	FSlateFontInfo FontReg(int32 Size)  { return FCoreStyle::GetDefaultFontStyle("Regular", Size); }
	const FSlateBrush* InkBrush()       { static FSlateColorBrush B(BF6Theme::Ink);        return &B; }
	const FSlateBrush* PanelBrush()     { static FSlateColorBrush B(BF6Theme::Panel);      return &B; }
	const FSlateBrush* PanelLightBrush(){ static FSlateColorBrush B(BF6Theme::PanelLight); return &B; }
	const FSlateBrush* LineBrush()      { static FSlateColorBrush B(BF6Theme::Line);       return &B; }

	const FButtonStyle& GhostStyle()
	{
		static FButtonStyle S = FButtonStyle()
			.SetNormal (FSlateRoundedBoxBrush(BF6Theme::PanelLight, 0.f, BF6Theme::Line, 1.f))
			.SetHovered(FSlateRoundedBoxBrush(FLinearColor(FColor(0x24,0x28,0x2B)), 0.f, FLinearColor::White, 1.f))
			.SetPressed(FSlateRoundedBoxBrush(FLinearColor(FColor(0x2E,0x33,0x37)), 0.f, FLinearColor::White, 1.f))
			.SetNormalPadding(FMargin(0)).SetPressedPadding(FMargin(0, 1, 0, -1));
		return S;
	}

	TSharedRef<SWidget> Btn(const FString& Label, TFunction<void()> Fn, const FString& Tip = FString())
	{
		return SNew(SButton).ButtonStyle(&GhostStyle()).ContentPadding(FMargin(10, 6))
			.ToolTipText(FText::FromString(Tip))
			.OnClicked_Lambda([Fn]{ if (Fn) Fn(); return FReply::Handled(); })
			[ SNew(STextBlock).Font(FontBold(9)).ColorAndOpacity(FSlateColor(BF6Theme::Text))
				.Text(FText::FromString(Label.ToUpper())) ];
	}

	TSharedRef<SWidget> Line(const FString& Text, int32 Size, const FLinearColor& C, bool bBold = false)
	{
		return SNew(STextBlock).AutoWrapText(false).OverflowPolicy(ETextOverflowPolicy::Ellipsis)
			.Font(bBold ? FontBold(Size) : FontReg(Size)).ColorAndOpacity(FSlateColor(C))
			.Text(FText::FromString(Text));
	}

	FString WhenText(int64 Unix)
	{
		if (Unix <= 0) return FString(TEXT("date unknown"));
		return FDateTime::FromUnixTimestamp(Unix).ToString(TEXT("%Y-%m-%d"));
	}
}

// ============================================================================
// 7. the tool's own widgets
// ============================================================================

// The profile column inside the Portal panel: what the tool knows, what it is
// doing, and the experience list it can act on.
class SBF6ProfileColumn : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SBF6ProfileColumn) {}
	SLATE_END_ARGS()

	void Construct(const FArguments&)
	{
		ChildSlot
		[
			SNew(SBorder).BorderImage(InkBrush()).Padding(FMargin(10, 8))
			[
				SNew(SVerticalBox)
				+ SVerticalBox::Slot().AutoHeight()[ SAssignNew(Head, SBox) ]
				// SEARCH. Built ONCE and never rebuilt: a text box that is
				// replaced every time the list changes loses the caret out from
				// under whoever is typing in it.
				+ SVerticalBox::Slot().AutoHeight().Padding(0, 8, 0, 0)
				[
					SNew(SEditableTextBox)
					.HintText(FText::FromString(TEXT("Search by name, id or map")))
					.Font(FontReg(9))
					.ToolTipText(FText::FromString(TEXT("Filters the list below. A name, the first characters of an id, or a map codename.")))
					.OnTextChanged_Lambda([](const FText& T){ BF6PortalProfile::SetSearchText(T.ToString()); })
					.OnTextCommitted_Lambda([](const FText& T, ETextCommit::Type){ BF6PortalProfile::SetSearchText(T.ToString()); })
				]
				+ SVerticalBox::Slot().FillHeight(1.f).Padding(0, 8, 0, 0)
				[
					SNew(SScrollBox)
					+ SScrollBox::Slot()[ SAssignNew(Body, SBox) ]
					// Through the namespace, not SNew: the panel's class is
					// declared further down this file.
					+ SScrollBox::Slot()[ BF6PortalProfile::MakeThumbnailPanel() ]
				]
			]
		];
		Rebuild();
	}

	virtual void Tick(const FGeometry& G, const double T, const float D) override
	{
		SCompoundWidget::Tick(G, T, D);
		const uint32 Now = BF6PortalProfile::UiFingerprint();
		if (Now != Sig) { Sig = Now; Rebuild(); }
	}

private:
	TSharedPtr<SBox> Head, Body;
	uint32 Sig = 0;

	void Rebuild()
	{
		if (!Head.IsValid() || !Body.IsValid()) return;
		Sig = BF6PortalProfile::UiFingerprint();

		const BF6PortalProfile::EState St = BF6PortalProfile::State();
		const FLinearColor PillColor = St == BF6PortalProfile::EState::Linked ? BF6Theme::Accent
			: (St == BF6PortalProfile::EState::Expired ? FLinearColor(0.85f, 0.35f, 0.30f) : BF6Theme::TextDim);

		Head->SetContent(
			SNew(SVerticalBox)
			// The session warning, above everything: it is the one thing on
			// this column that means "stop and read me".
			+ SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 8)
			[
				SNew(SBox)
				.Visibility(BF6PortalProfile::Banner().IsEmpty() ? EVisibility::Collapsed : EVisibility::Visible)
				[
					SNew(SBorder).BorderImage(PanelLightBrush()).Padding(FMargin(8, 6))
					[
						SNew(STextBlock).AutoWrapText(true).Font(FontBold(9))
						.ColorAndOpacity(FSlateColor(BF6Theme::Accent))
						.Text(FText::FromString(BF6PortalProfile::Banner()))
					]
				]
			]
			+ SVerticalBox::Slot().AutoHeight()
			[ Line(TEXT("PORTAL PROFILE"), 11, BF6Theme::TextBlue, true) ]
			+ SVerticalBox::Slot().AutoHeight().Padding(0, 6, 0, 0)
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0, 0, 8, 0)
				[
					SNew(SBorder).BorderImage(PanelBrush()).Padding(FMargin(8, 4))
					[ Line(BF6PortalProfile::StateLabel().ToUpper(), 9, PillColor, true) ]
				]
				+ SHorizontalBox::Slot().FillWidth(1.f).VAlign(VAlign_Center)
				[ Line(BF6PortalProfile::AccountName(), 9, BF6Theme::Text) ]
			]
			+ SVerticalBox::Slot().AutoHeight().Padding(0, 8, 0, 0)
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot().AutoWidth().Padding(0, 0, 4, 0)
				[
					St == BF6PortalProfile::EState::Linked
					? Btn(TEXT("Unlink"), []{ BF6PortalProfile::Unlink(); },
						TEXT("Clear the saved session and forget the profile. Your experiences on the site are untouched."))
					: Btn(TEXT("Link Portal profile"), []{ BF6PortalProfile::StartLink(); },
						TEXT("Sign in on the page. The tool never sees your password."))
				]
				+ SHorizontalBox::Slot().AutoWidth().Padding(0, 0, 4, 0)
				[ Btn(TEXT("Import all"), []{ BF6PortalProfile::ImportAll(); },
					TEXT("Bring every experience in your list into the tool as custom maps, one save per map in its rotation.")) ]
				+ SHorizontalBox::Slot().AutoWidth()
				[
					BF6PortalProfile::IsBusy()
					? Btn(TEXT("Cancel"), []{ BF6PortalProfile::CancelImport(); })
					: Btn(BF6PortalProfile::IsSiteShown() ? TEXT("Hide site") : TEXT("Show site"),
						[]{ BF6PortalProfile::SetSiteShown(!BF6PortalProfile::IsSiteShown()); },
						TEXT("The site keeps running and keeps feeding the tool either way. Hiding it just gives this column the room."))
				]
			]
			// The api row: everything here talks to the site's own calls rather
			// than clicking its pages.
			+ SVerticalBox::Slot().AutoHeight().Padding(0, 4, 0, 0)
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot().AutoWidth().Padding(0, 0, 4, 0)
				[ Btn(TEXT("Refresh"), []{ BF6PortalProfile::RefreshList(); },
					TEXT("Ask the site for your experiences list again, without leaving the page you are on.")) ]
				+ SHorizontalBox::Slot().AutoWidth().Padding(0, 0, 4, 0)
				[ Btn(TEXT("Export from site"), []{ BF6PortalProfile::ExportFromSite(FString()); },
					TEXT("Have the site export this experience through its own tile menu and read the file straight into the tool. Nothing is written to your downloads folder.")) ]
				+ SHorizontalBox::Slot().AutoWidth()
				[ Btn(TEXT("Push"), []{ BF6PortalProfile::PushExperience(); },
					TEXT("Send the whole experience back through the site's own import, inside the experience. It replaces that experience's contents and never makes a new one, and it never composes a save message, so nothing can be wiped.")) ]
			]
			+ SVerticalBox::Slot().AutoHeight().Padding(0, 4, 0, 0)
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot().AutoWidth().Padding(0, 0, 4, 0)
				[ Btn(TEXT("Import file"), []{ BF6PortalProfile::ImportExperienceFile(FString()); },
					TEXT("Read a whole experience export off disk: settings, teams, rotation, blocks, script and one save per map. Works with no network at all.")) ]
				+ SHorizontalBox::Slot().AutoWidth()
				[ Btn(TEXT("Export file"), []{ BF6PortalProfile::ExportExperienceFile(FString()); },
					TEXT("Write this experience out in the site's own export format, which the site can import.")) ]
				+ SHorizontalBox::Slot().AutoWidth().Padding(4, 0, 0, 0)
				[ Btn(BF6PortalProfile::WatchEnabled() ? TEXT("Stop watching") : TEXT("Watch the site"),
					[]{ BF6PortalProfile::SetWatchEnabled(!BF6PortalProfile::WatchEnabled()); },
					TEXT("Follow what you change on the site while you change it: the settings controls on the page the panel is on, and the site's script editor. It only ever watches; it never clicks anything and never sends anything.")) ]
			]
			+ SVerticalBox::Slot().AutoHeight().Padding(0, 4, 0, 0)
			[ Line(BF6PortalProfile::WatchStatus(), 8, BF6Theme::TextDim) ]
			+ SVerticalBox::Slot().AutoHeight().Padding(0, 8, 0, 0)
			[ Line(BF6PortalProfile::PageStatus(), 8, BF6Theme::TextDim) ]
			+ SVerticalBox::Slot().AutoHeight().Padding(0, 2, 0, 0)
			[ Line(BF6PortalProfile::PushStatus(), 8, BF6Theme::TextDim) ]
			+ SVerticalBox::Slot().AutoHeight().Padding(0, 2, 0, 0)
			[ Line(BF6PortalProfile::WorkStatus(), 8, BF6Theme::TextDim) ]
			+ SVerticalBox::Slot().AutoHeight().Padding(0, 8, 0, 0)
			[ SNew(SBox).HeightOverride(1.f)[ SNew(SBorder).BorderImage(LineBrush()).Padding(0) ] ]
		);

		const TArray<BF6PortalProfile::FExperienceRow> Rows = BF6PortalProfile::List();
		TSharedRef<SVerticalBox> List = SNew(SVerticalBox);
		if (Rows.Num() == 0)
		{
			List->AddSlot().AutoHeight().Padding(0, 8, 0, 0)
			[
				SNew(STextBlock).AutoWrapText(true).Font(FontReg(9)).ColorAndOpacity(FSlateColor(BF6Theme::TextDim))
				.Text(FText::FromString(TEXT("No experiences yet. Link the profile and open your experiences list on the page; the tool reads it as the page loads it.")))
			];
		}
		for (const BF6PortalProfile::FExperienceRow& R : Rows)
		{
			const FString Id = R.Id;
			List->AddSlot().AutoHeight().Padding(0, 0, 0, 6)
			[
				SNew(SBorder).BorderImage(PanelBrush()).Padding(FMargin(8, 6))
				[
					SNew(SVerticalBox)
					+ SVerticalBox::Slot().AutoHeight()
					[ Line(R.Name, 10, BF6Theme::Text, true) ]
					+ SVerticalBox::Slot().AutoHeight().Padding(0, 2, 0, 0)
					[ Line(FString::Printf(TEXT("%s   %s%s"),
						R.Maps.IsEmpty() ? TEXT("not imported yet") : *R.Maps,
						*WhenText(R.UpdatedUnix),
						R.bNoMapData ? TEXT("   no map data") : (R.bImported ? TEXT("   imported") : TEXT(""))),
						8, BF6Theme::TextDim) ]
					+ SVerticalBox::Slot().AutoHeight().Padding(0, 6, 0, 0)
					[
						SNew(SHorizontalBox)
						+ SHorizontalBox::Slot().AutoWidth().Padding(0, 0, 4, 0)
						[ Btn(TEXT("Import"), [Id]{ BF6PortalProfile::ImportOne(Id); }) ]
						+ SHorizontalBox::Slot().AutoWidth()
						[ Btn(TEXT("Open in site"), [Id]{ BF6PortalProfile::OpenOnSite(Id); }) ]
					]
				]
			];
		}
		Body->SetContent(List);
	}
};

// MY EXPERIENCES on the map screen: the same card geometry as the map cards,
// with the experience's own thumbnail.
class SBF6ExperienceGrid : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SBF6ExperienceGrid) {}
	SLATE_END_ARGS()

	void Construct(const FArguments&)
	{
		ChildSlot
		[
			SNew(SVerticalBox)
			// Built once, for the same reason as the column's: a text box
			// replaced on every list change loses the caret while typing.
			+ SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 8)
			[
				SNew(SBox).WidthOverride(302.f)
				[
					SNew(SEditableTextBox)
					.HintText(FText::FromString(TEXT("Search your experiences")))
					.Font(FontReg(9))
					.ToolTipText(FText::FromString(TEXT("A name, the first characters of an id, or a map codename.")))
					.OnTextChanged_Lambda([](const FText& T){ BF6PortalProfile::SetSearchText(T.ToString()); })
					.OnTextCommitted_Lambda([](const FText& T, ETextCommit::Type){ BF6PortalProfile::SetSearchText(T.ToString()); })
				]
			]
			+ SVerticalBox::Slot().AutoHeight()[ SAssignNew(Host, SBox) ]
		];
		Rebuild();
	}

	virtual void Tick(const FGeometry& G, const double T, const float D) override
	{
		SCompoundWidget::Tick(G, T, D);
		const uint32 Now = BF6PortalProfile::ListFingerprint();
		if (Now != Sig) { Sig = Now; Rebuild(); }
	}

private:
	TSharedPtr<SBox> Host;
	uint32 Sig = 0;

	// A card is built ONCE and updated in place. Everything that changes while
	// it is on screen - the thumbnail arriving, the map summary filling in, the
	// imported mark - is bound as an attribute, so none of it needs the grid to
	// be torn down and rebuilt.
	TSharedRef<SWidget> MakeCard(const BF6PortalProfile::FExperienceRow& R)
	{
		const FString Id = R.Id;

		return SNew(SBox).WidthOverride(302.f)
		[
			SNew(SButton).ButtonStyle(&GhostStyle()).ContentPadding(FMargin(1.f))
			.ToolTipText(FText::FromString(R.bImported
				? TEXT("Open this experience. Its saves are already in the tool.")
				: TEXT("Import this experience and open it. One save per map in its rotation.")))
			.OnClicked_Lambda([Id]{ BF6PortalProfile::OpenForEditing(Id); return FReply::Handled(); })
			[
				SNew(SVerticalBox)
				+ SVerticalBox::Slot().AutoHeight()
				[
					SNew(SBox).WidthOverride(300.f).HeightOverride(169.f)
					[
						// The name underneath, the picture over it once it has
						// downloaded. Bound, so a thumbnail arriving swaps the
						// picture rather than rebuilding the grid.
						SNew(SOverlay)
						// the plain panel, for when the mirror has no art either
						+ SOverlay::Slot()[ SNew(SBorder).BorderImage(PanelLightBrush()).Padding(0) ]
						// the picture: the experience's own thumbnail when it
						// has one, otherwise the site's own game mode art
						+ SOverlay::Slot()
						[
							SNew(SImage)
							.Visibility_Lambda([Id]{ return BF6PortalProfile::ThumbnailFor(Id) ? EVisibility::Visible : EVisibility::Collapsed; })
							.Image_Lambda([Id]{ const FSlateBrush* B = BF6PortalProfile::ThumbnailFor(Id); return B ? B : PanelLightBrush(); })
						]
						// the name OVER the game mode art, the way the site
						// draws it, and never over a real thumbnail
						+ SOverlay::Slot().HAlign(HAlign_Center).VAlign(VAlign_Center).Padding(FMargin(12))
						[
							SNew(STextBlock).AutoWrapText(true).Justification(ETextJustify::Center)
							.Font(FontBold(12)).ColorAndOpacity(FSlateColor(BF6Theme::Text))
							.Visibility_Lambda([Id]{ return BF6PortalProfile::HasOwnThumbnail(Id) ? EVisibility::Collapsed : EVisibility::Visible; })
							.Text(FText::FromString(R.Name.ToUpper()))
						]
					]
				]
				+ SVerticalBox::Slot().AutoHeight()
				[
					SNew(SBorder).BorderImage(PanelBrush()).Padding(FMargin(10, 8))
					[
						SNew(SHorizontalBox)
						+ SHorizontalBox::Slot().FillWidth(1).VAlign(VAlign_Center)
						[
							SNew(SVerticalBox)
							+ SVerticalBox::Slot().AutoHeight()[ Line(R.Name.ToUpper(), 13, BF6Theme::Text, true) ]
							+ SVerticalBox::Slot().AutoHeight().Padding(0, 2, 0, 0)
							[
								SNew(STextBlock).AutoWrapText(false).OverflowPolicy(ETextOverflowPolicy::Ellipsis)
								.Font(FontReg(9)).ColorAndOpacity(FSlateColor(BF6Theme::TextDim))
								.Text_Lambda([Id]{ return FText::FromString(BF6PortalProfile::CardSubtitle(Id)); })
							]
						]
						+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
						[
							SNew(STextBlock)
							.Font_Lambda([Id]{ return FontBold(BF6PortalProfile::CardImported(Id) ? 9 : 18); })
							.ColorAndOpacity_Lambda([Id]{ return FSlateColor(BF6PortalProfile::CardImported(Id) ? BF6Theme::Accent : BF6Theme::TextDim); })
							.Text_Lambda([Id]{ return FText::FromString(BF6PortalProfile::CardImported(Id) ? TEXT("IMPORTED") : TEXT("+")); })
						]
					]
				]
			]
		];
	}

	void Rebuild()
	{
		if (!Host.IsValid()) return;
		Sig = BF6PortalProfile::ListFingerprint();
		BF6PortalProfile::NoteGridRebuilt();
		const TArray<BF6PortalProfile::FExperienceRow> Rows = BF6PortalProfile::List();
		if (Rows.Num() == 0)
		{
			// A search that matched nothing has to say so; an empty grid reads
			// as "the tool lost my experiences".
			if (!BF6PortalProfile::SearchText().IsEmpty() && BF6PortalProfile::HasExperiences())
			{
				Host->SetContent(Line(FString::Printf(TEXT("Nothing matches \"%s\"."),
					*BF6PortalProfile::SearchText()), 10, BF6Theme::TextDim));
				return;
			}
			Host->SetContent(SNullWidget::NullWidget);
			return;
		}
		TSharedRef<SWrapBox> Grid = SNew(SWrapBox).UseAllottedSize(true).InnerSlotPadding(FVector2D(12, 12));
		for (const BF6PortalProfile::FExperienceRow& R : Rows) Grid->AddSlot()[ MakeCard(R) ];
		Host->SetContent(Grid);
	}
};

// The build HUD's map switcher. Visible only while the open save belongs to an
// experience, because that is the only time a sibling map exists to switch to.
class SBF6MapSwitcher : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SBF6MapSwitcher) {}
	SLATE_END_ARGS()

	void Construct(const FArguments&)
	{
		ChildSlot[ SAssignNew(Host, SBox) ];
		Rebuild();
	}

	virtual void Tick(const FGeometry& G, const double T, const float D) override
	{
		SCompoundWidget::Tick(G, T, D);
		const uint32 Now = BF6PortalProfile::ListFingerprint()
			+ GetTypeHash(BF6Api::CurrentLevel()) + GetTypeHash(BF6Api::CurrentSave());
		if (Now != Sig) { Sig = Now; Rebuild(); }
	}

private:
	TSharedPtr<SBox> Host;
	TSharedPtr<SComboButton> MapMenu;
	uint32 Sig = 0;

	void Rebuild()
	{
		if (!Host.IsValid()) return;
		Sig = BF6PortalProfile::ListFingerprint()
			+ GetTypeHash(BF6Api::CurrentLevel()) + GetTypeHash(BF6Api::CurrentSave());

		const FString Save = BF6Api::CurrentSave();
		const FString Level = BF6Api::CurrentLevel();
		const FString Id = FindUuid(BF6PortalWeb::ExperienceForSave(Level, Save));
		if (Save.IsEmpty() || Id.IsEmpty()) { Host->SetContent(SNullWidget::NullWidget); return; }

		const TArray<FRotationRow> Rot = BF6PortalProfile::RotationFor(Id);
		if (Rot.Num() == 0) { Host->SetContent(SNullWidget::NullWidget); return; }

		int32 Here = BF6PortalWeb::MapIdxForSave(Level, Save);
		if (!Rot.IsValidIndex(Here))
		{
			Here = 0;
			for (const FRotationRow& R : Rot) if (R.SaveName == Save) { Here = R.MapIdx; break; }
		}

		TSharedRef<SVerticalBox> Menu = SNew(SVerticalBox);
		for (const FRotationRow& R : Rot)
		{
			const int32 Idx = R.MapIdx;
			const FString ExpId = Id;
			const FString Label = FString::Printf(TEXT("%d.  %s%s"), Idx + 1, *R.Map,
				R.SaveName.IsEmpty() ? TEXT("   not imported yet") : (Idx == Here ? TEXT("   open") : TEXT("")));
			Menu->AddSlot().AutoHeight().Padding(0, 0, 0, 2)
			[ Btn(Label, [this, ExpId, Idx]{ MapMenu->SetIsOpen(false); BF6PortalProfile::SwitchToMap(ExpId, Idx); }) ];
		}
		Menu->AddSlot().AutoHeight().Padding(0, 4, 0, 0)
		[ Btn(TEXT("Add or reorder maps on site"), [this, Id]{ MapMenu->SetIsOpen(false); BF6PortalProfile::OpenOnSite(Id); },
			TEXT("Opens the experience's map rotation on the Portal site in the tool's panel.")) ];

		Host->SetContent(
			SAssignNew(MapMenu, SComboButton)
			.MenuPlacement(MenuPlacement_AboveAnchor)
			.ContentPadding(FMargin(8, 5))
			.ButtonContent()
			[
				Line(FString::Printf(TEXT("MAP  %s  %d OF %d"),
					Rot.IsValidIndex(Here) ? *Rot[Here].Map : *Level, Here + 1, Rot.Num()), 10, BF6Theme::Accent, true)
			]
			.MenuContent()
			[
				SNew(SBox).WidthOverride(360).MaxDesiredHeight(400)
				[ SNew(SScrollBox) + SScrollBox::Slot().Padding(6)[ Menu ] ]
			]);
	}
};

// The crop frame. The captured picture, with the site's 352 x 248 window drawn
// over it: drag to move it, wheel to zoom. Everything it changes is two numbers
// and a scale on GThumb, so the preview beside it is always what will be sent.
class SBF6ThumbCrop : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SBF6ThumbCrop) {}
		SLATE_EVENT(FSimpleDelegate, OnChanged)
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs)
	{
		OnChanged = InArgs._OnChanged;
		ChildSlot
		[
			SNew(SBox).HeightOverride(160.f)
			[
				SNew(SImage)
				.Image_Lambda([]{ return GThumb.SrcBrush.IsValid() ? GThumb.SrcBrush.Get() : PanelLightBrush(); })
			]
		];
	}

	virtual int32 OnPaint(const FPaintArgs& Args, const FGeometry& Geo, const FSlateRect& Cull,
		FSlateWindowElementList& Out, int32 LayerId, const FWidgetStyle& Style, bool bEnabled) const override
	{
		const int32 Base = SCompoundWidget::OnPaint(Args, Geo, Cull, Out, LayerId, Style, bEnabled);
		if (GThumb.SrcW <= 0 || GThumb.SrcH <= 0) return Base;
		int32 CX, CY, CW, CH;
		CropRect(CX, CY, CW, CH);
		const FVector2D Size = Geo.GetLocalSize();
		const float SX = (float)Size.X / (float)GThumb.SrcW;
		const float SY = (float)Size.Y / (float)GThumb.SrcH;
		const float X = CX * SX, Y = CY * SY, W = CW * SX, H = CH * SY;
		const float T = 2.f;
		auto Bar = [&](float bx, float by, float bw, float bh)
		{
			FSlateDrawElement::MakeBox(Out, Base + 1,
				Geo.ToPaintGeometry(FVector2f(bw, bh), FSlateLayoutTransform(FVector2f(bx, by))),
				LineBrush(), ESlateDrawEffect::None, FLinearColor(1.f, 0.35f, 0.10f, 0.95f));
		};
		Bar(X, Y, W, T); Bar(X, Y + H - T, W, T);
		Bar(X, Y, T, H); Bar(X + W - T, Y, T, H);
		return Base + 1;
	}

	virtual FReply OnMouseButtonDown(const FGeometry&, const FPointerEvent& E) override
	{
		if (E.GetEffectingButton() != EKeys::LeftMouseButton) return FReply::Unhandled();
		Last = E.GetScreenSpacePosition();
		return FReply::Handled().CaptureMouse(SharedThis(this));
	}

	virtual FReply OnMouseMove(const FGeometry& Geo, const FPointerEvent& E) override
	{
		if (!HasMouseCapture() || GThumb.SrcW <= 0) return FReply::Unhandled();
		const FVector2D Now = E.GetScreenSpacePosition();
		const FVector2D D = Now - Last;
		Last = Now;
		const FVector2D Size = Geo.GetLocalSize();
		if (Size.X > 1 && Size.Y > 1)
		{
			GThumb.CenterU = FMath::Clamp(GThumb.CenterU - (float)(D.X / Size.X), 0.f, 1.f);
			GThumb.CenterV = FMath::Clamp(GThumb.CenterV - (float)(D.Y / Size.Y), 0.f, 1.f);
		}
		return FReply::Handled();
	}

	virtual FReply OnMouseButtonUp(const FGeometry&, const FPointerEvent&) override
	{
		if (!HasMouseCapture()) return FReply::Unhandled();
		OnChanged.ExecuteIfBound();
		return FReply::Handled().ReleaseMouseCapture();
	}

	virtual FReply OnMouseWheel(const FGeometry&, const FPointerEvent& E) override
	{
		GThumb.Zoom = FMath::Clamp(GThumb.Zoom * (E.GetWheelDelta() > 0 ? 1.1f : 1.f / 1.1f), 1.f, 8.f);
		OnChanged.ExecuteIfBound();
		return FReply::Handled();
	}

private:
	FSimpleDelegate OnChanged;
	FVector2D Last = FVector2D::ZeroVector;
};

// THUMBNAIL: capture or load, frame it, and send it. Lives in the profile
// column so it is beside the experience it belongs to.
class SBF6ThumbnailPanel : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SBF6ThumbnailPanel) {}
	SLATE_END_ARGS()

	void Construct(const FArguments&)
	{
		ChildSlot
		[
			SNew(SVerticalBox)
			+ SVerticalBox::Slot().AutoHeight().Padding(0, 10, 0, 4)
			[ Line(TEXT("THUMBNAIL"), 10, BF6Theme::TextBlue, true) ]
			+ SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 6)
			[
				SNew(STextBlock).AutoWrapText(true).Font(FontReg(8)).ColorAndOpacity(FSlateColor(BF6Theme::TextDim))
				.Text(FText::FromString(FString::Printf(
					TEXT("The site wants exactly %d by %d, JPEG or PNG, under %d KB. The tool always produces that."),
					BF6PortalProfile::kThumbW, BF6PortalProfile::kThumbH, BF6PortalProfile::kThumbMaxBytes / 1024)))
			]
			+ SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 6)
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot().AutoWidth().Padding(0, 0, 4, 0)
				[ Btn(TEXT("Screenshot map"), []
					{
						// The panel covers the part of the map most people are
						// aiming at, so it steps out of the way for the frame.
						const bool bWasUp = BF6PortalWeb::IsShown();
						if (bWasUp) BF6PortalWeb::Hide();
						BF6PortalProfile::CaptureViewportForThumbnail();
						BF6PortalProfile::BuildThumbnail();
						if (bWasUp) BF6PortalWeb::Open();
					}, TEXT("Take the 3D view as it is now. The tool's own overlays are not in the picture.")) ]
				+ SHorizontalBox::Slot().AutoWidth()
				[ Btn(TEXT("Upload image"), []
					{
						if (BF6PortalProfile::PickThumbnailFile()) BF6PortalProfile::BuildThumbnail();
					}, TEXT("Pick a jpg, png or bmp. It is fitted and cropped to the site's size for you.")) ]
			]
			+ SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 6)
			[
				SNew(SBox)
				.Visibility_Lambda([]{ return GThumb.SrcBrush.IsValid() ? EVisibility::Visible : EVisibility::Collapsed; })
				[ SNew(SBF6ThumbCrop).OnChanged_Lambda([]{ BF6PortalProfile::BuildThumbnail(); }) ]
			]
			// the result, beside what the site is showing today
			+ SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 4)
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot().AutoWidth().Padding(0, 0, 6, 0)
				[
					SNew(SVerticalBox)
					+ SVerticalBox::Slot().AutoHeight()[ Line(TEXT("NEW"), 8, BF6Theme::TextDim, true) ]
					+ SVerticalBox::Slot().AutoHeight()
					[
						SNew(SBox).WidthOverride(140.f).HeightOverride(99.f)
						[ SNew(SImage).Image_Lambda([]{ return GThumb.OutBrush.IsValid() ? GThumb.OutBrush.Get() : PanelLightBrush(); }) ]
					]
				]
				+ SHorizontalBox::Slot().AutoWidth()
				[
					SNew(SVerticalBox)
					+ SVerticalBox::Slot().AutoHeight()[ Line(TEXT("ON THE SITE"), 8, BF6Theme::TextDim, true) ]
					+ SVerticalBox::Slot().AutoHeight()
					[
						SNew(SBox).WidthOverride(140.f).HeightOverride(99.f)
						[
							SNew(SImage).Image_Lambda([]
							{
								const FSlateBrush* B = BF6PortalProfile::ThumbnailFor(CurrentExperienceId());
								return B ? B : PanelLightBrush();
							})
						]
					]
				]
			]
			+ SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 6)
			[
				SNew(STextBlock).AutoWrapText(true).Font(FontReg(8)).ColorAndOpacity(FSlateColor(BF6Theme::Text))
				.Text_Lambda([]{ return FText::FromString(BF6PortalProfile::ThumbnailStatus()); })
			]
			+ SVerticalBox::Slot().AutoHeight()
			[
				SNew(SWrapBox).UseAllottedSize(true)
				+ SWrapBox::Slot().Padding(0, 0, 4, 4)
				[ Btn(TEXT("Send to site"), []{ BF6PortalProfile::SendThumbnailToSite(); },
					TEXT("Uploads it through the page, with the page's own sign-in. The tool never handles a credential.")) ]
				+ SWrapBox::Slot().Padding(0, 0, 4, 4)
				[ Btn(TEXT("Copy path"), []{ BF6PortalProfile::CopyThumbnailPath(); },
					TEXT("Put the prepared image's path on the clipboard, ready to paste into the site's own file picker.")) ]
				+ SWrapBox::Slot().Padding(0, 0, 4, 4)
				[ Btn(TEXT("Site images"), []
					{
						BF6PortalWeb::Exec(TEXT("try { window.BF6PortalCapture.report(); } catch (e) {}"));
					}, TEXT("Re-read the pre-approved generic images from the site's Image Select dialog.")) ]
			]
			+ SVerticalBox::Slot().AutoHeight().Padding(0, 6, 0, 0)
			[ SAssignNew(Options, SBox) ]
		];
		Rebuild();
	}

	virtual void Tick(const FGeometry& G, const double T, const float D) override
	{
		SCompoundWidget::Tick(G, T, D);
		const uint32 Now = BF6PortalProfile::UiFingerprint();
		if (Now != Sig) { Sig = Now; Rebuild(); }
	}

private:
	TSharedPtr<SBox> Options;
	uint32 Sig = 0;

	void Rebuild()
	{
		if (!Options.IsValid()) return;
		Sig = BF6PortalProfile::UiFingerprint();
		if (GThumb.SiteOptions.Num() == 0) { Options->SetContent(SNullWidget::NullWidget); return; }
		TSharedRef<SVerticalBox> List = SNew(SVerticalBox);
		List->AddSlot().AutoHeight().Padding(0, 0, 0, 4)
		[ Line(FString::Printf(TEXT("THE SITE'S OWN IMAGES (%d)"), GThumb.SiteOptions.Num()), 8, BF6Theme::TextDim, true) ];
		for (const TPair<FString, FString>& O : GThumb.SiteOptions)
		{
			const FString Src = O.Key;
			List->AddSlot().AutoHeight().Padding(0, 0, 0, 2)
			[ Btn(O.Value.IsEmpty() ? FPaths::GetCleanFilename(Src) : O.Value,
				[Src]
				{
					FString Esc = Src; Esc.ReplaceInline(TEXT("'"), TEXT("\\'"));
					BF6PortalWeb::Exec(FString::Printf(
						TEXT("try { window.BF6PortalCapture.selectImage('%s'); } catch (e) {}"), *Esc));
				},
				TEXT("Pick this pre-approved image on the site instead of a custom one.")) ];
		}
		Options->SetContent(List);
	}
};

// ============================================================================
// 8. the public surface
// ============================================================================

namespace BF6PortalProfile
{
	EState  State()       { return GState; }
	FString AccountName() { return GAccount; }
	FString PageStatus()  { return GPageStatus; }
	FString WorkStatus()  { return GWorkStatus; }
	bool    IsBusy()      { return GJob.bActive; }
	bool    IsSiteShown() { return GSiteShown; }

	// ---- what the site work is doing, right now -----------------------------
	//
	// The site does not appear on screen any more, so this line is the only
	// feedback there is. It is not a second mechanism: every word of it comes
	// from the same GWorkStatus the page reports already set, with the step
	// counter the job was already keeping put in front of it.
	bool IsWorking()
	{
		return GJob.bActive || GExpect.bActive || BF6PortalWeb::IsWorkingOffscreen();
	}

	FString ProgressLine()
	{
		if (GJob.bActive)
		{
			const int32 Done = FMath::Max(0, GJob.Total - GJob.Queue.Num() - (GJob.Current.IsEmpty() ? 0 : 1));
			if (GJob.Total > 1)
				return FString::Printf(TEXT("Experience %d of %d.  %s"), Done + 1, GJob.Total, *GWorkStatus);
			return GWorkStatus;
		}
		if (GExpect.bActive && !GWorkStatus.IsEmpty()) return GWorkStatus;
		if (BF6PortalWeb::IsWorkingOffscreen() && !GWorkStatus.IsEmpty()) return GWorkStatus;
		return FString();
	}

	bool CanCancelWork() { return GJob.bActive; }
	uint32  UiFingerprint()   { return GFingerprint; }
	void    NoteGridRebuilt() { GGridRebuilds++; }

	// What a card draws, out of the cache the fingerprint recompute fills.
	FString CardSubtitle(const FString& Id)
	{
		const FCardView* V = GCardView.Find(Id);
		return V ? V->Sub : FString();
	}
	bool CardImported(const FString& Id)
	{
		const FCardView* V = GCardView.Find(Id);
		return V && V->bImported;
	}

	// True only when the experience has a thumbnail of its OWN, downloaded from
	// the site. Game mode art is a fallback, not a thumbnail, and the card draws
	// the name over it.
	bool HasOwnThumbnail(const FString& Id)
	{
		const FThumb* T = GThumbs.Find(Id);
		return T && T->Brush.IsValid();
	}

	// THE CARD FINGERPRINT.
	//
	// A hash of exactly what a card draws, and nothing else. The general bump is
	// called from dozens of places - every page probe, every status string, the
	// recovery ticker - and the grid used to rebuild all 38 cards, thumbnails
	// and all, every time any of them fired. Now the general bump only decides
	// whether it is worth RECOMPUTING this hash; the hash itself moves when a
	// card would look different, and at no other time.
	//
	// The recompute is throttled as well, because the imported mark asks the
	// tool for the saves on disk and that is not a per-frame question.
	uint32 ListFingerprint()
	{
		const double Now = FPlatformTime::Seconds();
		if (GListSourceFp == GFingerprint) return GListHash;
		if (Now - GListHashAt < 0.5) return GListHash;
		GListSourceFp = GFingerprint;
		GListHashAt = Now;

		uint32 H = 2166136261u;
		auto Mix = [&H](const FString& S)
		{
			H = HashCombine(H, GetTypeHash(S));
		};
		// THE STATE IS PART OF THE LIST, because the list IS the state: signed
		// out it is empty, signed in it is the account's experiences. Leaving
		// it out meant the fingerprint was identical either side of linking -
		// same experiences, same search - so the grid, built empty while the
		// tool was unlinked, was never told to build itself again. Linking
		// worked and the section stayed blank.
		Mix(FString::Printf(TEXT("state=%d"), (int32)GState));
		Mix(GSearch);
		for (const FExp& E : GExps)
		{
			if (!MatchesSearch(E, GSearch.TrimStartAndEnd())) continue;
			Mix(E.Id);
			Mix(E.Name);
			Mix(E.GameMode);
			// The card's own text and mark, worked out HERE and cached, so the
			// bound attributes that draw them are a map lookup rather than a
			// walk of the list and a question to the disk on every frame.
			FCardView& CV = GCardView.FindOrAdd(E.Id);
			CV.Sub = FString::Printf(TEXT("%s   %s"),
				E.Maps.Num() ? *FString::Join(E.Maps, TEXT(", ")).ToUpper() : TEXT("NOT IMPORTED YET"),
				*WhenText(E.UpdatedUnix));
			CV.bImported = IsImported(E.Id);
			Mix(CV.Sub);
			H = HashCombine(H, CV.bImported ? 1u : 0u);
			// The brush POINTER, not the url: a thumbnail that finished
			// downloading is a new brush, and that is the only thumbnail change
			// a card can see. Read without EnsureThumb, which would start a
			// download from inside a hash.
			const FThumb* T = GThumbs.Find(E.Id);
			H = HashCombine(H, PointerHash(T && T->Brush.IsValid() ? T->Brush.Get() : nullptr));
		}
		GListHash = H ? H : 1u;
		return GListHash;
	}
	bool    HasExperiences()  { return GExps.Num() > 0; }

	// SIGNED OUT, THERE IS NO LIST. Not a shortened one either.
	//
	// MY EXPERIENCES is the account's own list, and an account's list on a
	// screen belonging to nobody is a claim the tool cannot make. Showing only
	// the imported ones was a half measure: it still put a section named after
	// an account in front of someone who is not signed into one.
	//
	// The maps are not lost by hiding it. Every map of an experience is a save
	// on its own map, under the experience's name, so the ordinary RESUME list
	// under each map card lists it as the game mode it belongs to and somebody
	// signed out simply carries on from the map they were working on.
	bool    HasImportedExperiences()
	{
		for (const FExp& E : GExps) if (IsImported(E.Id)) return true;
		return false;
	}
	bool    ShowExperienceList()
	{
		return GState == EState::Linked && HasExperiences();
	}
	FString ExperienceListHeading()
	{
		return TEXT("My experiences - from your Portal account");
	}

	void SetSiteShown(bool bShown) { GSiteShown = bShown; Bump(); }

	FString PushStatus() { return GPushStatus; }
	FString SearchText() { return GSearch; }

	void SetSearchText(const FString& Text)
	{
		if (GSearch == Text) return;
		GSearch = Text;
		Bump();
	}

	// ---- the api path -------------------------------------------------------

	bool ApiReady() { return CanReplay(TEXT("getPlayElement")); }

	FString ApiStatus()
	{
		if (GApiSeen.Num() == 0)
			return TEXT("No site request seen yet this session, so the tool still has to drive the page. Open your experiences list once.");
		TArray<FString> Names = GApiSeen.Array();
		Names.Sort();
		return FString::Printf(TEXT("%d site call(s) the tool can ask for directly: %s"), Names.Num(), *FString::Join(Names, TEXT(", ")));
	}

	void RefreshList()
	{
		if (CanReplay(TEXT("getOwnedPlayElementsV2")))
		{
			GWorkStatus = TEXT("Asking the site for your experiences...");
			NoteWorking();
			BF6PortalWeb::Exec(TEXT("try { window.BF6PortalCapture.getOwnedList(); } catch (e) { console.log('BF6CAPTURE list error ' + e); }"));
			Bump();
			return;
		}
		// Nothing to imitate yet. The experiences page makes the site issue the
		// call itself, and from then on the tool can ask for it directly.
		GWorkStatus = TEXT("Opening your experiences list once, so the tool can ask for it directly after that.");
			NoteWorking();
		UE_LOG(LogBF6Portal, Display, TEXT("Portal profile: no owned-list request to replay yet, opening the experiences page once."));
		ExpectPage(TEXT("experiences"), BF6PortalWeb::BaseUrl() + TEXT("/experiences"), 20.f,
			[](bool bOk, const FString& What)
			{
				if (!bOk) UE_LOG(LogBF6Portal, Warning, TEXT("Portal profile: the experiences list did not open (%s)"), *What);
			});
		Bump();
	}

	// ---- search -------------------------------------------------------------

	FString Resolve(const FString& Text, FString& OutWhat)
	{
		const FString Needle = Text.TrimStartAndEnd();
		if (Needle.IsEmpty()) { OutWhat = TEXT("nothing to look for"); return FString(); }

		// A whole uuid anywhere in what was typed, including a pasted address.
		const FString Uuid = FindUuid(Needle);
		if (!Uuid.IsEmpty())
		{
			const FExp* E = Find(Uuid);
			OutWhat = E ? FString::Printf(TEXT("the experience id %s ('%s')"), *Uuid.Left(8), *E->Name)
			            : FString::Printf(TEXT("the experience id %s, which is not in the list yet"), *Uuid.Left(8));
			return Uuid;
		}

		// An exact name beats everything else, however many things contain it.
		for (const FExp& E : GExps)
			if (E.Name.Equals(Needle, ESearchCase::IgnoreCase))
			{
				OutWhat = FString::Printf(TEXT("'%s' (%s)"), *E.Name, *E.Id.Left(8));
				return E.Id;
			}

		TArray<const FExp*> Hits;
		for (const FExp& E : GExps)
			if (E.Id.StartsWith(Needle, ESearchCase::IgnoreCase) && Needle.Len() >= 4) Hits.Add(&E);
		if (Hits.Num() == 0)
			for (const FExp& E : GExps) if (MatchesSearch(E, Needle)) Hits.Add(&E);

		if (Hits.Num() == 1)
		{
			OutWhat = FString::Printf(TEXT("'%s' (%s)"), *Hits[0]->Name, *Hits[0]->Id.Left(8));
			return Hits[0]->Id;
		}
		if (Hits.Num() == 0)
		{
			OutWhat = FString::Printf(TEXT("nothing matching '%s' in %d experience(s)"), *Needle, GExps.Num());
			return FString();
		}
		TArray<FString> Names;
		for (const FExp* E : Hits) Names.Add(FString::Printf(TEXT("%s (%s)"), *E->Name, *E->Id.Left(8)));
		OutWhat = FString::Printf(TEXT("%d experiences match '%s': %s"), Hits.Num(), *Needle, *FString::Join(Names, TEXT("; ")));
		return FString();
	}

	void LogFind(const FString& Text)
	{
		const TArray<FExperienceRow> Rows = Search(Text);
		UE_LOG(LogBF6Portal, Display, TEXT("---- Portal find '%s' ----"), *Text);
		if (Rows.Num() == 0)
		{
			UE_LOG(LogBF6Portal, Display, TEXT("  nothing matched. %d experience(s) known."), GExps.Num());
		}
		for (const FExperienceRow& R : Rows)
		{
			UE_LOG(LogBF6Portal, Display, TEXT("  %s  %-40s %s%s"),
				*R.Id, *R.Name,
				R.Maps.IsEmpty() ? TEXT("no rotation read yet") : *R.Maps,
				R.bImported ? TEXT("  [imported]") : TEXT(""));
		}
		UE_LOG(LogBF6Portal, Display, TEXT("  %d of %d experience(s)"), Rows.Num(), GExps.Num());
	}

	// ---- the whole experience, as one file ----------------------------------

	bool ImportExperienceFile(const FString& Path)
	{
		FString File = Path.TrimStartAndEnd().TrimQuotes();
		if (File.IsEmpty())
		{
			IDesktopPlatform* DP = FDesktopPlatformModule::Get();
			if (!DP) { UE_LOG(LogBF6Portal, Warning, TEXT("No file dialog available.")); return false; }
			TArray<FString> Picked;
			const void* Parent = FSlateApplication::IsInitialized()
				? FSlateApplication::Get().FindBestParentWindowHandleForDialogs(nullptr) : nullptr;
			if (!DP->OpenFileDialog(Parent, TEXT("Import a Portal experience export"),
				FPlatformProcess::UserDir(), TEXT(""),
				TEXT("Portal experience (*.json)|*.json|All files (*.*)|*.*"), EFileDialogFlags::None, Picked)
				|| Picked.Num() == 0) return false;
			File = Picked[0];
		}
		FString Text;
		if (!FFileHelper::LoadFileToString(Text, *File))
		{
			GWorkStatus = FString::Printf(TEXT("Could not read %s"), *File);
			UE_LOG(LogBF6Portal, Warning, TEXT("Portal import: %s"), *GWorkStatus);
			Bump();
			return false;
		}
		UE_LOG(LogBF6Portal, Display, TEXT("Portal import: reading %s (%d characters)"), *File, Text.Len());
		return ImportExperienceText(Text, FString(), FPaths::GetCleanFilename(File));
	}

	bool ExportExperienceFile(const FString& Path)
	{
		const FString Id = CurrentExperienceId().IsEmpty() ? FirstFetchedId() : CurrentExperienceId();
		if (!IsUuid(Id))
		{
			GWorkStatus = TEXT("Nothing to export yet. Import an experience, or open a save that belongs to one.");
			UE_LOG(LogBF6Portal, Warning, TEXT("Portal export: %s"), *GWorkStatus);
			Bump();
			return false;
		}
		TSharedPtr<FJsonObject> Doc;
		if (!ExperienceJson(Id, Doc)) return false;

		FString File = Path.TrimStartAndEnd().TrimQuotes();
		if (File.IsEmpty())
		{
			IDesktopPlatform* DP = FDesktopPlatformModule::Get();
			if (!DP) { UE_LOG(LogBF6Portal, Warning, TEXT("No file dialog available.")); return false; }
			const FExp* E = Find(Id);
			FString Suggested = Sanitise(E ? E->Name : Id).ToLower().Replace(TEXT(" "), TEXT("_")) + TEXT("_experience.json");
			TArray<FString> Picked;
			const void* Parent = FSlateApplication::IsInitialized()
				? FSlateApplication::Get().FindBestParentWindowHandleForDialogs(nullptr) : nullptr;
			if (!DP->SaveFileDialog(Parent, TEXT("Write the experience as the site's own export"),
				FPlatformProcess::UserDir(), Suggested,
				TEXT("Portal experience (*.json)|*.json"), EFileDialogFlags::None, Picked)
				|| Picked.Num() == 0) return false;
			File = Picked[0];
			if (!File.EndsWith(TEXT(".json"))) File += TEXT(".json");
		}
		const FString Text = JsonToString(Doc);
		if (!FFileHelper::SaveStringToFile(Text, *File, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
		{
			GWorkStatus = FString::Printf(TEXT("Could not write %s"), *File);
			UE_LOG(LogBF6Portal, Warning, TEXT("Portal export: %s"), *GWorkStatus);
			Bump();
			return false;
		}
		GWorkStatus = FString::Printf(TEXT("Wrote %s (%d characters)."), *FPaths::GetCleanFilename(File), Text.Len());
		UE_LOG(LogBF6Portal, Display, TEXT("Portal export: %s -> %s"), *Id.Left(8), *File);
		BF6Api::Toast(GWorkStatus);
		Bump();
		return true;
	}

	bool ExperienceJson(const FString& Id, TSharedPtr<FJsonObject>& Out)
	{
		const FExp* E = Find(Id.IsEmpty() ? CurrentExperienceId() : Id);
		if (!E)
		{
			UE_LOG(LogBF6Portal, Warning, TEXT("Portal: no experience %s to write out."), *Id.Left(8));
			return false;
		}
		Out = BuildExperienceJson(*E);
		return Out.IsValid();
	}

	void ExportFromSite(const FString& Id)
	{
		FString Want = Id.IsEmpty() ? CurrentExperienceId() : Id;
		if (Want.IsEmpty()) Want = FirstFetchedId();
		const FExp* E = Find(Want);
		FString Name = E ? E->Name : FString();
		if (Name.IsEmpty())
		{
			GWorkStatus = TEXT("The site's export finds a tile by its name, and the tool does not know this experience's name yet. Refresh the list first.");
			UE_LOG(LogBF6Portal, Warning, TEXT("Portal export from site: %s"), *GWorkStatus);
			Bump();
			return;
		}
		GExportWantId = Want;
		FString Esc = Name;
		Esc.ReplaceInline(TEXT("\\"), TEXT("\\\\"));
		Esc.ReplaceInline(TEXT("'"), TEXT("\\'"));
		const FString Js = FString::Printf(
			TEXT("try { window.BF6PortalCapture.exportExperience('%s', '%s'); } catch (e) { console.log('BF6CAPTURE export error ' + e); }"),
			*Want, *Esc);
		GWorkStatus = FString::Printf(TEXT("Asking the site to export '%s'..."), *Name);
		NoteWorking(60.0);   // the site builds the file on its own clock
		// The Export item lives on the experiences list, so the panel has to be
		// standing there. This is the one place the site's own pages are still
		// driven, and it is driven for a control, not for data.
		if (GLastKind == TEXT("experiences"))
		{
			BF6PortalWeb::Exec(Js);
		}
		else
		{
			ExpectPage(TEXT("experiences"), BF6PortalWeb::BaseUrl() + TEXT("/experiences"), 20.f,
				[Js](bool bOk, const FString& What)
				{
					if (!bOk)
					{
						GWorkStatus = FString::Printf(TEXT("The experiences list did not open (%s)."), *What);
						UE_LOG(LogBF6Portal, Warning, TEXT("Portal export from site: %s"), *GWorkStatus);
						Bump();
						return;
					}
					BF6PortalWeb::Exec(Js);
				});
		}
		Bump();
	}

	void VerifyAgainstFile(const FString& Path)
	{
		TSharedPtr<FJsonObject> File;
		if (!ReadJsonFile(Path, File))
		{
			UE_LOG(LogBF6Portal, Warning, TEXT("Portal verify: could not read %s as JSON"), *Path);
			return;
		}
		FString Name;
		File->TryGetStringField(TEXT("name"), Name);
		FString What;
		FString Id = Resolve(Name, What);
		if (!IsUuid(Id)) Id = CurrentExperienceId();
		TSharedPtr<FJsonObject> Mine;
		if (!IsUuid(Id) || !ExperienceJson(Id, Mine))
		{
			UE_LOG(LogBF6Portal, Warning, TEXT("Portal verify: nothing of ours to compare against '%s' (%s)"), *Name, *What);
			return;
		}
		UE_LOG(LogBF6Portal, Display, TEXT("---- Portal verify: our rebuild against %s ----"), *FPaths::GetCleanFilename(Path));
		UE_LOG(LogBF6Portal, Display, TEXT("  experience: %s (%s)"), *Name, *Id.Left(8));
		int32 Same = 0, Differ = 0;
		TSet<FString> Keys;
		for (const auto& P : File->Values) Keys.Add(FString(*P.Key));
		for (const auto& P : Mine->Values) Keys.Add(FString(*P.Key));
		TArray<FString> Sorted = Keys.Array();
		Sorted.Sort();
		for (const FString& K : Sorted)
		{
			const TSharedPtr<FJsonValue> A = File->TryGetField(K);
			const TSharedPtr<FJsonValue> B = Mine->TryGetField(K);
			if (!A.IsValid()) { Differ++; UE_LOG(LogBF6Portal, Warning, TEXT("  %-18s only ours has it"), *K); continue; }
			if (!B.IsValid()) { Differ++; UE_LOG(LogBF6Portal, Warning, TEXT("  %-18s only the file has it"), *K); continue; }
			FString TA, TB;
			TSharedRef<TJsonWriter<>> WA = TJsonWriterFactory<>::Create(&TA);
			TSharedRef<TJsonWriter<>> WB = TJsonWriterFactory<>::Create(&TB);
			FJsonSerializer::Serialize(A, FString(), WA);
			FJsonSerializer::Serialize(B, FString(), WB);
			if (TA == TB) { Same++; UE_LOG(LogBF6Portal, Display, TEXT("  %-18s same (%d chars)"), *K, TA.Len()); }
			else
			{
				Differ++;
				UE_LOG(LogBF6Portal, Warning, TEXT("  %-18s DIFFERS: file %d chars, ours %d chars"), *K, TA.Len(), TB.Len());
				UE_LOG(LogBF6Portal, Warning, TEXT("      file: %s"), *TA.Left(200));
				UE_LOG(LogBF6Portal, Warning, TEXT("      ours: %s"), *TB.Left(200));
			}
		}
		UE_LOG(LogBF6Portal, Display, TEXT("  %d field(s) match, %d differ"), Same, Differ);
	}

	// THE PUSH.
	//
	// One action that sends the whole experience back, through the site's own
	// import inside the experience editor. That control replaces the open
	// experience's contents; the one on the experiences front page makes a NEW
	// experience and asks for a name, and is never used.
	//
	// Order, best first:
	//   1. the site's own import, handed the whole document with no dialog
	//   2. the site's own last save message, with only the changed strings
	//      different, when no import control can be found but that message can
	//   3. the file, with its path on the clipboard
	//
	// Nothing composes a message from scratch at any point, because a partial
	// updatePlayElement wipes the experience's attachments.
	void PushExperience()
	{
		const FString Id = CurrentExperienceId().IsEmpty() ? FirstFetchedId() : CurrentExperienceId();
		const FExp* E = Find(Id);
		if (!E)
		{
			GPushStatus = TEXT("Nothing open to send back. Open a save that belongs to an experience first.");
			UE_LOG(LogBF6Portal, Warning, TEXT("Portal push: %s"), *GPushStatus);
			Bump();
			return;
		}

		TSharedPtr<FJsonObject> Doc;
		if (!ExperienceJson(Id, Doc) || !Doc.IsValid())
		{
			GPushStatus = TEXT("The experience could not be written out, so there is nothing to send.");
			UE_LOG(LogBF6Portal, Warning, TEXT("Portal push: %s"), *GPushStatus);
			Bump();
			return;
		}
		const FString Text = JsonToString(Doc);

		// THE USER'S CALL. This replaces a live experience's contents on the
		// site, so it is named and asked for before anything is dispatched. An
		// answer changes what happens next, which is what a dialog is for.
		{
			const FString Ask = FString::Printf(
				TEXT("Replace '%s' on Portal with what the tool has?\n\n")
				TEXT("Experience: %s\n%s\n\n")
				TEXT("This goes through the site's own import inside that experience and replaces its settings, map rotation, blocks, script and every map's objects. It does not make a new experience, and it does not publish.\n\n")
				TEXT("%d map(s), %d attachment(s), %d characters."),
				*E->Name, *Id, *E->Description,
				E->Maps.Num(), E->Files.Num(), Text.Len());
			if (FMessageDialog::Open(EAppMsgType::YesNo, FText::FromString(Ask)) != EAppReturnType::Yes)
			{
				GPushStatus = TEXT("Push cancelled. Nothing was sent.");
				UE_LOG(LogBF6Portal, Display, TEXT("Portal push: %s"), *GPushStatus);
				Bump();
				return;
			}
		}
		UE_LOG(LogBF6Portal, Display, TEXT("Portal push: the user approved replacing '%s' (%s) on the site"), *E->Name, *Id);

		// The page has to be standing IN the experience, not on the list, and it
		// has to be standing in the right one. The page checks that itself as
		// well, from the address, and refuses a mismatch.
		const FString Url = BF6PortalWeb::CurrentUrl();
		const bool bInEditor = Url.Contains(TEXT("/bf6/experience/")) && FindUuid(Url) == Id;

		// ALWAYS through the handshake, even when the address already looks
		// right: the document behind that address may have been replaced a
		// moment ago and not yet have the receiver in it.
		GPushWait = FPushWait();
		GPushWait.Id = Id;
		GPushWait.Text = Text;
		GPushWait.Token = FString::Printf(TEXT("t%lld"), (int64)(FPlatformTime::Seconds() * 1000.0));
		GPushWait.Deadline = FPlatformTime::Seconds() + (bInEditor ? 20.0 : 40.0);
		GPushWait.NextProbeAt = 0.0;
		GPushWait.bActive = true;
		GPushInFlight = true;

		if (!bInEditor)
		{
			GPushStatus = FString::Printf(TEXT("Opening '%s' on the site so its own import can take the file..."), *E->Name);
			UE_LOG(LogBF6Portal, Display, TEXT("Portal push: %s"), *GPushStatus);
			// GoToExperience clicks the tile's Modify button, which is the only
			// route into an experience that reliably works. It owns the page
			// check while it runs, so the push waits on the ticker rather than
			// starting a second check that would cancel its card fallback.
			GoToExperience(Id);
		}
		else
		{
			GPushStatus = FString::Printf(TEXT("Sending '%s' to the site's own import..."), *E->Name);
		}
		UE_LOG(LogBF6Portal, Display, TEXT("Portal push: waiting for the page (token %s)"), *GPushWait.Token);
		Bump();
	}

	// The second route, kept for when no import control can be found: the site's
	// own last save message with only the changed strings different.
	void PushByMessage()
	{
		const FString Id = GPushExpId.IsEmpty() ? CurrentExperienceId() : GPushExpId;
		const FExp* E = Find(Id);
		if (!E) return;
		TArray<TPair<FString, FString>> Pairs;
		if (CollectPushPairs(*E, Pairs) == 0)
		{
			GPushStatus = TEXT("Nothing has changed since the site last gave you this experience.");
			UE_LOG(LogBF6Portal, Display, TEXT("Portal push: %s"), *GPushStatus);
			Bump();
			return;
		}
		if (!CanReplay(TEXT("updatePlayElement")))
		{
			TSharedPtr<FJsonObject> Doc;
			if (ExperienceJson(Id, Doc) && Doc.IsValid())
				PushFallbackToFile(Id, JsonToString(Doc),
					TEXT("The site has no import control the tool could find and has not saved this experience itself yet this session."));
			return;
		}
		TArray<TSharedPtr<FJsonValue>> List;
		for (const TPair<FString, FString>& P : Pairs)
		{
			TArray<TSharedPtr<FJsonValue>> Two;
			Two.Add(MakeShared<FJsonValueString>(P.Key));
			Two.Add(MakeShared<FJsonValueString>(P.Value));
			List.Add(MakeShared<FJsonValueArray>(Two));
		}
		FString Payload;
		TSharedRef<TJsonWriter<>> W = TJsonWriterFactory<>::Create(&Payload);
		FJsonSerializer::Serialize(List, W);
		// Handed to the page as a JSON string literal, so nothing in a block of
		// script or a map name can end the statement early.
		Payload.ReplaceInline(TEXT("\\"), TEXT("\\\\"));
		Payload.ReplaceInline(TEXT("'"), TEXT("\\'"));
		Payload.ReplaceInline(TEXT("\r"), TEXT("\\r"));
		Payload.ReplaceInline(TEXT("\n"), TEXT("\\n"));
		GPushStatus = FString::Printf(TEXT("Sending %d changed field(s) back through the site's own save..."), Pairs.Num());
		UE_LOG(LogBF6Portal, Display, TEXT("Portal push: %s"), *GPushStatus);
		BF6PortalWeb::Exec(FString::Printf(
			TEXT("try { window.BF6PortalCapture.pushStrings('%s', 'push'); } catch (e) { console.log('BF6CAPTURE push error ' + e); }"),
			*Payload));
		Bump();
	}

	// ---- keeping the two in step --------------------------------------------

	// ---- WATCH THE SITE -----------------------------------------------------

	bool WatchEnabled() { return GWatch; }

	FString WatchStatus()
	{
		if (!GWatch) return TEXT("Not watching the site. WATCH THE SITE follows what you change there, live.");
		FString Line = FString::Printf(TEXT("Watching the site. Settings: %s. Script: %s."),
			*BF6PortalSettings::WatchStatus(),
			GWatchStatus.IsEmpty() ? TEXT("waiting for the script page") : *GWatchStatus);
		if (!GWatchLastChange.IsEmpty())
			Line += FString::Printf(TEXT(" Last change: %s."), *GWatchLastChange);
		return Line;
	}

	void SetWatchEnabled(bool bOn)
	{
		GWatch = bOn;
		GConfig->SetBool(kIniSection, TEXT("PortalWatchSite"), GWatch, GEditorPerProjectIni);
		GConfig->Flush(false, GEditorPerProjectIni);
		BF6PortalSettings::SetWatching(bOn);
		BF6PortalWeb::Exec(FString::Printf(
			TEXT("try { window.BF6PortalCapture.setWatch(%s); } catch (e) {}"), bOn ? TEXT("true") : TEXT("false")));
		if (!bOn) GWatchStatus = TEXT("not watching");
		UE_LOG(LogBF6Portal, Display, TEXT("Portal watch %s. It observes the site and never writes to it."),
			bOn ? TEXT("on") : TEXT("off"));
		Bump();
	}

	void AdoptWatchedScript()
	{
		const FString Path = WatchedScriptPath();
		if (!FPaths::FileExists(Path))
		{
			UE_LOG(LogBF6Portal, Warning,
				TEXT("Portal watch: no script has come off the site yet. Turn WATCH THE SITE on and open the site's Script page."));
			return;
		}
		if (BF6Script::ImportFile(Path))
		{
			GWatchStatus = FString::Printf(TEXT("adopted %s into the tool's script project"), *FPaths::GetCleanFilename(Path));
			UE_LOG(LogBF6Portal, Display, TEXT("Portal watch: %s"), *GWatchStatus);
			BF6Api::Toast(TEXT("The site's script is now the tool's script."));
		}
		else
		{
			UE_LOG(LogBF6Portal, Warning, TEXT("Portal watch: the script editor would not take %s"), *Path);
		}
		Bump();
	}

	bool AutoSyncEnabled() { return GAutoSync; }

	void SetAutoSyncEnabled(bool bOn)
	{
		GAutoSync = bOn;
		GConfig->SetBool(kIniSection, TEXT("PortalAutoSync"), GAutoSync, GEditorPerProjectIni);
		GConfig->Flush(false, GEditorPerProjectIni);
		UE_LOG(LogBF6Portal, Display, TEXT("Portal auto-sync %s."), bOn ? TEXT("on") : TEXT("off"));
		Bump();
	}

	void NoteToolSaved(const FString& Level, const FString& Save)
	{
		if (!GAutoSync) return;
		// A save the tool made itself, importing or pushing, is not the user
		// saving, and chasing it is how five fetches ended up racing the page.
		if (SyncSuspended()) return;
		const FString Id = FindUuid(BF6PortalWeb::ExperienceForSave(Level, Save));
		if (!IsUuid(Id)) return;
		if (GLost || !LooksSignedIn())
		{
			// Announced, never silent: a sync that did not happen is a fact the
			// user has to be able to see.
			GWorkStatus = TEXT("Saved. Portal is signed out, so the site was not brought back in step; it will be when you sign in.");
			GPendingSyncId = Id;
			UE_LOG(LogBF6Portal, Warning, TEXT("Portal auto-sync deferred for %s: the session is signed out."), *Id.Left(8));
			Bump();
			return;
		}
		RequestSync(Id, TEXT("the tool saved a map that belongs to it"));
	}

	// ---- session loss and recovery ------------------------------------------

	FBF6PortalSessionEvent& OnSessionLost()     { return GOnLost; }
	FBF6PortalSessionEvent& OnSessionRestored() { return GOnRestored; }

	bool    IsSessionLost()      { return GLost; }
	FString LastLossReason()     { return GLossReason.IsEmpty() ? FString(TEXT("no session has been lost this run")) : GLossReason; }
	FString LastRecoveryResult() { return GRecoveryResult; }
	FString Banner()             { return GBanner; }
	bool    KeepAliveEnabled()   { return GKeepAlive; }

	void SetKeepAliveEnabled(bool bOn)
	{
		GKeepAlive = bOn;
		GConfig->SetBool(kIniSection, TEXT("PortalKeepAlive"), GKeepAlive, GEditorPerProjectIni);
		GConfig->Flush(false, GEditorPerProjectIni);
		UE_LOG(LogBF6Portal, Display, TEXT("Portal keep-alive %s."), bOn ? TEXT("on") : TEXT("off"));
		Bump();
	}

	void SimulateSession(bool bLost)
	{
		if (bLost)
		{
			if (GLost) { UE_LOG(LogBF6Portal, Display, TEXT("Portal session is already lost.")); return; }
			// The subscribers see exactly what a real logout sends them. The
			// recovery does NOT run: this exists to watch the editors journal,
			// not to reload a page that never went anywhere.
			GLost = true;
			GLossReason = TEXT("simulated by BF6.Portal.Session.Simulate lost");
			GRecoveryResult = TEXT("simulated loss, no recovery attempted");
			GBanner = TEXT("Portal signed you out (simulated). Nothing is lost: your blocks, script and saves are kept in the tool.");
			GRecovery = ERecovery::WaitingForUser;
			UE_LOG(LogBF6Portal, Warning, TEXT("Portal session lost: %s"), *GLossReason);
			BF6Api::Toast(GBanner);
			GOnLost.Broadcast();
			Bump();
			return;
		}
		if (!GLost) { UE_LOG(LogBF6Portal, Display, TEXT("Portal session is not lost, nothing to restore.")); return; }
		FinishRecovery(TEXT("simulated by BF6.Portal.Session.Simulate restored"));
	}

	void LogSession()
	{
		UE_LOG(LogBF6Portal, Display, TEXT("---- Portal session ----"));
		UE_LOG(LogBF6Portal, Display, TEXT("  profile state   : %s"), *StateLabel());
		UE_LOG(LogBF6Portal, Display, TEXT("  session         : %s"), GLost ? TEXT("LOST") : TEXT("held"));
		UE_LOG(LogBF6Portal, Display, TEXT("  last loss reason: %s"), *LastLossReason());
		UE_LOG(LogBF6Portal, Display, TEXT("  last recovery   : %s"), *GRecoveryResult);
		const TCHAR* Phase = GRecovery == ERecovery::Reloading ? TEXT("reloading")
			: (GRecovery == ERecovery::WaitingForUser ? TEXT("waiting for the user to sign in") : TEXT("idle"));
		UE_LOG(LogBF6Portal, Display, TEXT("  recovery phase  : %s (%d of 2 reloads used)"), Phase, GRecoveryTries);
		UE_LOG(LogBF6Portal, Display, TEXT("  return page     : %s"), GReturnUrl.IsEmpty() ? TEXT("(none remembered)") : *GReturnUrl);
		const double Since = GLastProbeAt > 0.0 ? (FPlatformTime::Seconds() - GLastProbeAt) : -1.0;
		UE_LOG(LogBF6Portal, Display, TEXT("  page answered   : %s"),
			Since < 0.0 ? TEXT("never") : *FString::Printf(TEXT("%.1f seconds ago"), Since));
		UE_LOG(LogBF6Portal, Display, TEXT("  keep-alive      : %s (every 4 minutes while you are working)"), GKeepAlive ? TEXT("on") : TEXT("off"));
		UE_LOG(LogBF6Portal, Display, TEXT("  subscribers     : %d on lost, %d on restored"),
			GOnLost.IsBound() ? 1 : 0, GOnRestored.IsBound() ? 1 : 0);
		UE_LOG(LogBF6Portal, Display, TEXT("------------------------"));
	}

	FString StateLabel()
	{
		switch (GState)
		{
		case EState::Linked:    return GAccount.IsEmpty() ? FString(TEXT("Linked")) : FString(TEXT("Linked"));
		case EState::SigningIn: return TEXT("Signing in");
		case EState::Expired:   return TEXT("Sign in again");
		default:                return TEXT("Not linked");
		}
	}

	// THE SAVE AN IMPORT WOULD HAVE MADE for one rotation slot. The importer
	// names them "<experience> (<map>)", so the name can be recomputed rather
	// than only remembered.
	//
	// It has to be recomputed, because SaveNames is filled by an import run and
	// is lost whenever the experience is fetched again. Relying on it alone
	// meant that after a refetch the tool believed nothing had ever been
	// imported: clicking an experience whose eleven maps were sitting on disk
	// started the whole import over, which is what put the website on screen.
	FString ExpectedSaveName(const FExp& E, int32 MapIdx)
	{
		if (!E.Maps.IsValidIndex(MapIdx)) return FString();
		const FString Name = E.Name.IsEmpty() ? E.Id.Left(8) : E.Name;
		return FString::Printf(TEXT("%s (%s)"), *Name, *E.Maps[MapIdx]);
	}

	// The save for a slot: the one an import recorded if it is still on disk,
	// otherwise the one the naming convention points at if that is on disk.
	FString SaveForSlot(const FExp& E, int32 MapIdx)
	{
		if (!E.Maps.IsValidIndex(MapIdx)) return FString();
		const TArray<FString> Saves = BF6Api::SavesFor(E.Maps[MapIdx]);
		if (E.SaveNames.IsValidIndex(MapIdx) && !E.SaveNames[MapIdx].IsEmpty()
			&& Saves.Contains(E.SaveNames[MapIdx]))
			return E.SaveNames[MapIdx];
		// ---- BF6Project ----
		// The experience folder first: every map of an experience answers to
		// the same save name now, so a slot is imported when that name has a
		// save on that level. The old per-map name is still searched, because
		// a save that has not been migrated is still the user's map.
		const FString Folder = BF6Project::FolderForExperience(E.Id);
		if (!Folder.IsEmpty() && Saves.Contains(Folder)) return Folder;
		// ---- end BF6Project ----
		const FString Guess = ExpectedSaveName(E, MapIdx);
		return Saves.Contains(Guess) ? Guess : FString();
	}

	// SAVES ON DISK ARE THE TRUTH, even before the rotation is known. The owned
	// list carries no maps, so a cached experience has an empty Maps until a
	// getPlayElement fills it. Asking the rotation alone therefore answered "not
	// imported" for an experience whose eleven maps were sitting in the saves
	// folder, and clicking it started the whole import again, which is what put
	// the website on screen. So look for the importer's own naming across every
	// level as well: "<experience> (<map>)".
	//
	// Cached on the tool's save fingerprint, because the map screen asks per
	// card and the answer only changes when the saves folder does.
	TMap<FString, bool> GImportedCache;
	uint32              GImportedAt = 0;

	bool AnySaveNamedFor(const FString& Name)
	{
		if (Name.IsEmpty()) return false;
		const FString Prefix = Name + TEXT(" (");
		for (const FString& Level : BF6Api::AllLevels())
			for (const FString& Save : BF6Api::SavesFor(Level))
				if (Save.StartsWith(Prefix, ESearchCase::IgnoreCase) || Save == Name) return true;
		return false;
	}

	bool IsImported(const FString& Id)
	{
		const FExp* E = Find(Id);
		if (!E) return false;
		for (int32 i = 0; i < E->Maps.Num(); i++)
			if (!SaveForSlot(*E, i).IsEmpty()) return true;

		const uint32 Now = BF6Api::SavesFingerprint();
		if (Now != GImportedAt) { GImportedCache.Reset(); GImportedAt = Now; }
		if (const bool* Hit = GImportedCache.Find(Id)) return *Hit;
		const bool bFound = AnySaveNamedFor(E->Name.IsEmpty() ? E->Id.Left(8) : E->Name);
		GImportedCache.Add(Id, bFound);
		return bFound;
	}

	// What the map screen draws. Signed out, that is only the experiences whose
	// maps are on this machine: the rest cannot be opened without the site, and
	// showing an account's list to nobody claims something that is not true.
	TArray<FExperienceRow> List()
	{
		// Signed out the grid draws nothing at all, so it never has to decide
		// what a card that cannot be opened should do when pressed.
		if (GState != EState::Linked) return TArray<FExperienceRow>();
		return Search(GSearch);
	}
	TArray<FExperienceRow> ListAll() { return Search(FString()); }

	TArray<FExperienceRow> Search(const FString& Text)
	{
		const FString Needle = Text.TrimStartAndEnd();
		TArray<FExperienceRow> Out;
		Out.Reserve(GExps.Num());
		for (const FExp& E : GExps)
		{
			if (!MatchesSearch(E, Needle)) continue;
			FExperienceRow R;
			R.Id = E.Id;
			R.Name = E.Name.IsEmpty() ? E.Id.Left(8) : E.Name;
			R.Description = E.Description;
			R.ThumbUrl = E.ScrapedThumbUrl.IsEmpty() ? E.ThumbUrl : E.ScrapedThumbUrl;
			R.Maps = FString::Join(E.Maps, TEXT(", "));
			R.UpdatedUnix = E.UpdatedUnix;
			R.bFetched = E.bFetched;
			bool bAnySpatial = false;
			for (const FAttachment& A : E.Files) if (A.Kind == 1) bAnySpatial = true;
			R.bNoMapData = E.bFetched && !bAnySpatial;
			R.bImported = IsImported(E.Id);
			Out.Add(R);
		}
		return Out;
	}

	TArray<FRotationRow> RotationFor(const FString& Id)
	{
		TArray<FRotationRow> Out;
		const FExp* E = Find(Id);
		if (!E) return Out;

		// THE ROTATION IS NOT KNOWN UNTIL THE EXPERIENCE IS FETCHED, but its
		// maps may already be on disk from an earlier import. Rebuild the
		// rotation from those saves so the picker works, and so opening an
		// experience does not have to touch the site at all. The order is the
		// level order rather than the site's, which is stated in the picker.
		if (E->Maps.Num() == 0)
		{
			// ---- BF6Project ----
			// An experience project on disk already knows its maps, and knows
			// them in the order the manifest recorded, so it answers first.
			const FString Folder = BF6Project::FolderForExperience(E->Id);
			if (!Folder.IsEmpty())
			{
				for (const FString& Level : BF6Project::MapsIn(Folder))
				{
					FRotationRow R;
					R.MapIdx = Out.Num();
					R.Map = Level;
					R.SaveName = Folder;
					R.bHasSpatial = true;
					Out.Add(R);
				}
				if (Out.Num() > 0) return Out;
			}
			// ---- end BF6Project ----
			const FString Name = E->Name.IsEmpty() ? E->Id.Left(8) : E->Name;
			const FString Prefix = Name + TEXT(" (");
			for (const FString& Level : BF6Api::AllLevels())
				for (const FString& Save : BF6Api::SavesFor(Level))
					if (Save.StartsWith(Prefix, ESearchCase::IgnoreCase))
					{
						FRotationRow R;
						R.MapIdx = Out.Num();
						R.Map = Level;
						R.SaveName = Save;
						R.bHasSpatial = true;   // it came from one
						Out.Add(R);
					}
			return Out;
		}
		for (int32 i = 0; i < E->Maps.Num(); i++)
		{
			FRotationRow R;
			R.MapIdx = i;
			R.Map = E->Maps[i];
			// The recorded save if it is still there, else the one the naming
			// convention points at: a refetch clears SaveNames, and a save on
			// disk is a save whether or not this session is the one that made it.
			R.SaveName = SaveForSlot(*E, i);
			for (const FAttachment& A : E->Files) if (A.Kind == 1 && A.MapIdx == i) R.bHasSpatial = true;
			Out.Add(R);
		}
		return Out;
	}

	const FSlateBrush* ThumbnailFor(const FString& Id)
	{
		EnsureThumb(Id);
		const FThumb* T = GThumbs.Find(Id);
		if (T && T->Brush.IsValid()) return T->Brush.Get();
		// No thumbnail of its own: the site's own game mode picture, the way the
		// site draws it, with the name over the top. Cached per mode.
		const FExp* E = Find(Id);
		return ModeArtFor(E ? E->GameMode : FString());
	}

	// SIGNING IN IS THE ONE THING A PERSON MUST TOUCH THE SITE FOR, so while it
	// happens it is the only thing on screen: the panel fills the whole editor
	// window, over the map screen or the build screen alike, and it comes down
	// the moment the profile really becomes Linked - not on a page that merely
	// finished loading, and not on a redirect.
	//
	// WHERE IT GOES BACK TO IS NOT DECIDED HERE. The placement the panel had is
	// remembered by BF6PortalWeb::ShowSignIn, which is the only code that knows
	// what that placement actually was; this file used to read the ini key and
	// guess, and a guess is what left the panel in the wrong place.
	void StartLink()
	{
		GLinkInProgress = true;
		SetState(EState::SigningIn, TEXT("the user asked to link a profile"));
		GPageStatus = TEXT("Sign in on the page. The tool never sees your password. When your experiences list appears the link completes on its own.");
		BF6Api::Toast(TEXT("Sign in on the page. The tool never sees your password."));
		BF6PortalWeb::ShowSignIn();
		ExpectPage(TEXT("login"), BF6PortalWeb::BaseUrl() + TEXT("/login"), 30.f,
			[](bool bOk, const FString& What)
			{
				// Braces on both arms: UE_LOG expands to a block, so an if/else
				// without them is a syntax error rather than a style choice.
				if (bOk)
				{
					UE_LOG(LogBF6Portal, Display, TEXT("Portal profile: login page reached, waiting for the user to sign in."));
				}
				else
				{
					UE_LOG(LogBF6Portal, Display, TEXT("Portal profile: %s"), *What);
				}
			});

		// ASK THE PAGE, DO NOT WAIT TO BE TOLD.
		//
		// The page volunteers its state once, when the capture becomes ready,
		// and at that instant the site has loaded but has not drawn anything
		// yet. Every later change - the login form giving way to the list, or
		// an already signed-in browser going straight there - happens without
		// another word. The startup check learned this the hard way and polls;
		// the button did not, so the one moment that matters went unseen and
		// the overlay sat on "Signing in" with the experiences list visible
		// behind it.
		//
		// Same question, same cadence, for as long as the sign-in is up.
		//
		// It stops the moment the state moves, and in any case after ten
		// minutes: cancelling the overlay deliberately leaves the profile
		// SigningIn so a sign-in finished on the dock tab still completes, and
		// that must not become a page being poked for the rest of the session.
		TSharedRef<int32> Asked = MakeShared<int32>(0);
		FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda([Asked](float) -> bool
		{
			if (GState != EState::SigningIn) return false;  // linked, or given up
			if (++(*Asked) > 200) return false;             // ten minutes
			BF6PortalWeb::Exec(TEXT(
				"try { if (window.BF6PortalCapture) window.BF6PortalCapture.report(); } catch (e) {}"));
			return true;
		}), 3.0f);

		// AND MAKE THE LIST REQUEST HAPPEN AGAIN, for the browser that is
		// already signed in. Unlink leaves the site's session alone on purpose,
		// so pressing the button again lands on an experiences list that was
		// fetched long ago: nothing is requested, so nothing is captured, and
		// the parsed-response rule has nothing to work with. Sending the app
		// away through its own router and straight back remounts the list with
		// the capture in place. It is the app's own navigation, not a reload.
		FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda([](float) -> bool
		{
			if (GState != EState::SigningIn) return false;
			BF6PortalWeb::Exec(TEXT(
				"try {"
				"  if (window.BF6PortalCapture && /\\/experiences/.test(location.pathname)) {"
				"    var back = location.pathname + location.search;"
				"    history.pushState({}, '', '/bf6');"
				"    window.dispatchEvent(new PopStateEvent('popstate'));"
				"    setTimeout(function () {"
				"      history.pushState({}, '', back);"
				"      window.dispatchEvent(new PopStateEvent('popstate'));"
				"    }, 400);"
				"  }"
				"} catch (e) { console.log('BF6CAPTURE remount error ' + e); }"));
			return false;
		}), 6.0f);
	}

	// THERE IS ALWAYS A WAY OUT THAT IS NOT SIGNING IN. Cancel puts the panel
	// back where it was and leaves the profile exactly as it is: the page keeps
	// whatever it had reached, and a sign-in finished later on the dock tab or
	// the column still completes the link the normal way.
	void CancelLink()
	{
		if (!BF6PortalWeb::IsSignIn()) return;
		const FString What = BF6PortalWeb::EndSignIn();
		GPageStatus = FString::Printf(TEXT("Sign-in cancelled: %s. LINK PORTAL PROFILE opens it again."),
			What.IsEmpty() ? TEXT("the panel was put back") : *What);
		Bump();
		UE_LOG(LogBF6Portal, Display,
			TEXT("Portal profile: the sign-in surface was cancelled, %s. The profile state is unchanged (%s)."),
			What.IsEmpty() ? TEXT("the panel was put back") : *What, *BF6PortalProfile::StateLabel());
	}

	void Unlink()
	{
		// UNLINKING IS NOT SIGNING OUT. This used to call BF6PortalWeb::SignOut,
		// which writes a marker that deletes the whole browser store at the next
		// editor start. One press of a button labelled "unlink" therefore cost
		// the user their Portal sign-in silently, a restart later, with the log
		// line arriving so far from the click that it read as the site expiring
		// the session. Observed doing exactly that on 2026-09-06.
		//
		// So unlinking now forgets the tool's own link and leaves the browser
		// session alone. Clearing the session is its own deliberate action: SIGN
		// OUT in the panel, or BF6.Portal.SignOut.
		GAccount.Reset();
		GHasOwnedList = false;
		// Signing out on purpose is not a session LOSS: there is nothing to
		// recover and nothing to warn about, so the recovery is stood down
		// without telling the editors the session came back.
		GLost = false;
		GRecovery = ERecovery::Idle;
		GBanner.Reset();
		GRecoveryResult = TEXT("unlinked by the user, no recovery needed");
		SetState(EState::Unlinked, TEXT("unlinked by the user"));
		GPageStatus = TEXT("Not linked. LINK PORTAL PROFILE opens the sign-in page.");
		UE_LOG(LogBF6Portal, Display,
			TEXT("Portal profile unlinked. You are still signed in on the site: use SIGN OUT in the panel to clear that."));
		Bump();
	}

	void OpenOnSite(const FString& Id)
	{
		if (!IsUuid(Id)) return;
		if (!LooksSignedIn())
		{
			GPageStatus = TEXT("Portal signed you out. Sign in on the panel first.");
			BF6Api::Toast(GPageStatus);
			Bump();
			return;
		}
		GoToExperience(Id);
		// Sections are not addressable until the site has shown the tool an
		// address that names the experience, so the honest instruction is the
		// one the site's own navigation gives.
		GPageStatus = GUrlPattern.IsEmpty()
			? FString(TEXT("Opening the experience on the site. Its own menu has Maps, Rules and Publish."))
			: FString(TEXT("Opening the experience on the site."));
	}

	void ImportOne(const FString& Id)
	{
		if (!IsUuid(Id)) { UE_LOG(LogBF6Portal, Warning, TEXT("BF6.Portal.Import wants an experience uuid.")); return; }
		if (GJob.bActive) { BF6Api::Toast(TEXT("An import is already running.")); return; }
		TArray<FString> One; One.Add(Id);
		StartQueue(One, FString());   // refuses, with the right sentence, when signed out
	}

	void ImportAll()
	{
		if (GJob.bActive) { BF6Api::Toast(TEXT("An import is already running.")); return; }
		if (GExps.Num() == 0) { BF6Api::Toast(TEXT("No experiences to import yet.")); return; }
		TArray<FString> Ids;
		for (const FExp& E : GExps) Ids.Add(E.Id);
		// With the api open there is nothing to navigate to: each experience is
		// one replayed request from wherever the panel is.
		if (CanReplay(TEXT("getPlayElement"))) { StartQueue(Ids, FString()); return; }
		// Otherwise everything goes through a page check first: a tool that
		// starts clicking at a page that bounced to login gets nothing back and
		// cannot say why.
		ExpectPage(TEXT("experiences"), BF6PortalWeb::BaseUrl() + TEXT("/experiences"), 20.f,
			[Ids](bool bOk, const FString& What)
			{
				if (!bOk)
				{
					// Two different things, said two different ways.
					GWorkStatus = LooksSignedIn()
						? FString::Printf(TEXT("Signed in, but your experiences list did not open (%s)."), *What)
						: FString(TEXT("Portal signed you out. Sign in on the panel and press Import again."));
					UE_LOG(LogBF6Portal, Warning, TEXT("Portal import all stopped: %s"), *GWorkStatus);
					Bump();
					return;
				}
				StartQueue(Ids, FString());
			});
	}

	void CancelImport() { if (GJob.bActive) GJob.bCancel = true; }

	// ---- which map of the rotation ------------------------------------------
	//
	// An experience is a rotation, not a map. Clicking one used to take the
	// first slot that happened to be imported and open it without a word, so an
	// experience with eleven maps opened whichever one came first. Clicking now
	// asks, unless there is nothing to ask about.

	// The slot the user last opened for this experience, remembered per project
	// so the common case is one click. -1 when there is none.
	static FString LastMapKey(const FString& Id) { return FString::Printf(TEXT("PortalLastMap_%s"), *Id); }
	static int32 LastMapFor(const FString& Id)
	{
		int32 V = -1;
		GConfig->GetInt(kIniSection, *LastMapKey(Id), V, GEditorPerProjectIni);
		return V;
	}
	static void NoteLastMap(const FString& Id, int32 Idx)
	{
		GConfig->SetInt(kIniSection, *LastMapKey(Id), Idx, GEditorPerProjectIni);
		GConfig->Flush(false, GEditorPerProjectIni);
	}

	// Open one slot of the rotation, or offer to import it when it is not on
	// disk yet. The one place a rotation slot turns into an open map.
	static void OpenMapSlot(const FString& Id, int32 Idx)
	{
		const TArray<FRotationRow> Rot = RotationFor(Id);
		if (!Rot.IsValidIndex(Idx)) return;
		const FString Level = Rot[Idx].Map;

		if (Rot[Idx].SaveName.IsEmpty())
		{
			// NOT IMPORTED YET. The import path works an experience at a time,
			// not a map at a time, so this is said plainly rather than pretended
			// otherwise: importing brings the whole rotation in, and the picker
			// comes back when it finishes.
			if (GJob.bActive) { BF6Api::Toast(TEXT("An import is already running.")); return; }
			BF6Api::Toast(FString::Printf(
				TEXT("%s has not been imported yet. Importing this experience brings all of its maps in, then you can pick again."),
				*BF6Api::DisplayName(Level)));
			TArray<FString> One; One.Add(Id);
			StartQueue(One, Id);
			return;
		}

		NoteLastMap(Id, Idx);
		// ENTERBUILD, NOT OPENMAPWORLD. OpenMapWorld builds the level and
		// nothing else: the tool stays on the map selection screen, so a map
		// that loaded perfectly is invisible behind the selector and the click
		// reads as having done nothing. BF6Ext::OpenMap learned this the same
		// way and says so in its own comment. EnterBuild is the complete path:
		// the world, then the screen.
		BF6Api::EnterBuild(Level, Rot[Idx].SaveName);
		// AND NOTHING OVER IT. The build screen puts back whatever editor the
		// user last had covering it, which is right when they walk back into
		// build mode and wrong when they have just asked to look at a map.
		// Suspend rather than Hide: it is taken down, not forgotten, so the
		// editor still comes back the next time they enter the build screen.
		BF6EditorOverlay::Suspend();
		// OPENING AN EXPERIENCE IS NOT A REASON TO SHOW THE WEBSITE. This used
		// to navigate the panel onto the experience's page here, so asking for
		// a map put a browser on screen. The site is for signing in, and for
		// the few actions that genuinely need a page (reading settings off it,
		// pushing, sending a thumbnail), and each of those opens it itself.
		// The experience the tool is working on is remembered either way.
		GNavTarget.Reset();
		// The panel was just told to navigate, and a full window sheet left up
		// over a map that has finished opening is the bug this all started as.
		BF6PortalWeb::EndSignIn();
		UE_LOG(LogBF6Portal, Display,
			TEXT("Portal profile: opened '%s' on %s (rotation slot %d of %d), switched to the build screen and put the panel on its experience."),
			*Rot[Idx].SaveName, *Level, Idx + 1, Rot.Num());

		// THE SCENE FILE THIS MAP'S SPATIAL WAS AUTHORED FROM.
		//
		// A spatial keeps the tree only as each object's node path, so pure
		// group pivots have no transform in it and a minified one has renamed
		// every segment. The .tscn still has all of it, so this is the moment
		// to ask - once, and never again once the user has said no.
		//
		// A TICK LATER, not now: the build screen is still assembling itself
		// on this call stack, and a modal put up over a half-built screen is
		// the bug the map picker was already taught to avoid.
		const FString AskLevel = Level, AskSave = Rot[Idx].SaveName;
		FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda(
			[AskLevel, AskSave](float) -> bool
			{
				BF6Project::OfferTscn(AskLevel, AskSave);
				return false;   // once
			}), 0.6f);
	}

	// The picker itself: one row per rotation slot, in the tool's own style,
	// at the cursor rather than as a modal, so it reads as belonging to the
	// card that was clicked. Escape and clicking away dismiss it with nothing
	// opened, because it is an ordinary menu and that is what those do.
	static TSharedRef<SWidget> MakeMapPicker(const FString& Id, const TArray<FRotationRow>& Rot, int32 Last)
	{
		// Last opened first, then the rotation in its own order. The slot number
		// is on every row, so reordering never costs anyone the real order.
		TArray<int32> Order;
		if (Rot.IsValidIndex(Last)) Order.Add(Last);
		for (int32 i = 0; i < Rot.Num(); i++) if (i != Last) Order.Add(i);

		TSharedRef<SVerticalBox> Rows = SNew(SVerticalBox);
		for (int32 Idx : Order)
		{
			const bool bImported = !Rot[Idx].SaveName.IsEmpty();
			const bool bLast     = (Idx == Last);
			const FString Name   = BF6Api::DisplayName(Rot[Idx].Map);
			const FString Mark   = !bImported ? FString(TEXT("not imported yet"))
			                                  : (bLast ? FString(TEXT("last opened")) : FString());

			Rows->AddSlot().AutoHeight().Padding(0, 0, 0, 2)
			[
				SNew(SButton).ButtonStyle(&GhostStyle()).ContentPadding(FMargin(10, 6))
				.ToolTipText(FText::FromString(bImported
					? FString::Printf(TEXT("Open %s, slot %d of this experience's map rotation."), *Name, Idx + 1)
					: FString::Printf(TEXT("%s is in the rotation but is not on disk yet. Picking it imports this experience."), *Name)))
				.OnClicked_Lambda([Id, Idx]
				{
					FSlateApplication::Get().DismissAllMenus();
					OpenMapSlot(Id, Idx);
					return FReply::Handled();
				})
				[
					SNew(SHorizontalBox)
					+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0, 0, 8, 0)
					[ Line(FString::Printf(TEXT("%d"), Idx + 1), 9, BF6Theme::TextDim) ]
					+ SHorizontalBox::Slot().FillWidth(1.f).VAlign(VAlign_Center)
					[ Line(Name, 10, bImported ? BF6Theme::Text : BF6Theme::TextDim, bLast) ]
					+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(10, 0, 0, 0)
					[ Line(Mark, 8, bImported ? BF6Theme::Accent : BF6Theme::TextDim) ]
				]
			];
		}

		return SNew(SBorder).BorderImage(PanelBrush()).Padding(FMargin(12, 10))
			[
				SNew(SBox).MinDesiredWidth(320.f)
				[
					SNew(SVerticalBox)
					+ SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 2)
					[ Line(TEXT("CHOOSE A MAP"), 10, BF6Theme::Text, true) ]
					+ SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 8)
					[ Line(FString::Printf(TEXT("%d maps in this experience's rotation"), Rot.Num()), 8, BF6Theme::TextDim) ]
					+ SVerticalBox::Slot().AutoHeight()[ Rows ]
				]
			];
	}

	void OpenForEditing(const FString& Id)
	{
		if (!IsUuid(Id)) return;
		// Whatever happens next, the sign-in surface has no business being over
		// it: this is a click that leaves the screen it was made on.
		BF6PortalWeb::EndSignIn();

		const TArray<FRotationRow> Rot = RotationFor(Id);

		// Nothing to choose between: exactly the old behaviour.
		if (Rot.Num() <= 1)
		{
			if (Rot.Num() == 0 || Rot[0].SaveName.IsEmpty())
			{
				// Not imported yet: import it, and open it when that finishes.
				if (GJob.bActive) { BF6Api::Toast(TEXT("An import is already running.")); return; }
				TArray<FString> One; One.Add(Id);
				StartQueue(One, Id);
				return;
			}
			OpenMapSlot(Id, 0);
			return;
		}

		// Nothing imported at all: there is still nothing to choose between, so
		// the import comes first and the picker follows it.
		bool bAny = false;
		for (const FRotationRow& R : Rot) if (!R.SaveName.IsEmpty()) bAny = true;
		if (!bAny)
		{
			if (GJob.bActive) { BF6Api::Toast(TEXT("An import is already running.")); return; }
			TArray<FString> One; One.Add(Id);
			StartQueue(One, Id);
			return;
		}

		const int32 Last = LastMapFor(Id);
		TSharedPtr<SWindow> Win = FSlateApplication::IsInitialized()
			? FSlateApplication::Get().GetActiveTopLevelWindow() : nullptr;
		if (!Win.IsValid())
		{
			// No window to hang a menu on. Asking is better than guessing, but
			// not asking is better than doing nothing at all.
			const int32 Pick = Rot.IsValidIndex(Last) && !Rot[Last].SaveName.IsEmpty() ? Last : 0;
			UE_LOG(LogBF6Portal, Warning,
				TEXT("Portal profile: no window to show the map picker in, opening slot %d instead."), Pick + 1);
			OpenMapSlot(Id, Pick);
			return;
		}

		FSlateApplication::Get().PushMenu(
			Win.ToSharedRef(), FWidgetPath(),
			MakeMapPicker(Id, Rot, Last),
			FSlateApplication::Get().GetCursorPos(),
			FPopupTransitionEffect(FPopupTransitionEffect::ContextMenu));
		UE_LOG(LogBF6Portal, Display,
			TEXT("Portal profile: asking which of %d rotation maps to open (last opened: %s)."),
			Rot.Num(), Rot.IsValidIndex(Last) ? *BF6Api::DisplayName(Rot[Last].Map) : TEXT("none"));
	}

	void SwitchToMap(const FString& Id, int32 MapIdx)
	{
		const TArray<FRotationRow> Rot = RotationFor(Id);
		if (!Rot.IsValidIndex(MapIdx)) return;
		if (Rot[MapIdx].SaveName.IsEmpty())
		{
			BF6Api::Toast(TEXT("That map is in the rotation but has not been imported yet. Use IMPORT on the experience."));
			return;
		}
		if (BF6Api::IsEditing()) BF6Api::SaveCurrent(true);   // the tool's normal save path
		BF6Api::OpenMapWorld(Rot[MapIdx].Map, Rot[MapIdx].SaveName);
		UE_LOG(LogBF6Portal, Display, TEXT("Portal profile: switched to map %d of %d (%s)"),
			MapIdx + 1, Rot.Num(), *Rot[MapIdx].Map);
	}

	// ---- the experience thumbnail -------------------------------------------

	FString ThumbnailPath()   { return GThumb.Path; }
	FString ThumbnailStatus() { return GThumb.Status; }

	// SCREENSHOT MAP.
	//
	// FViewport::ReadPixels on the active editor viewport, NOT
	// FSlateApplication::TakeScreenshot. TakeScreenshot reads the window's
	// Slate backbuffer for a widget, which means it captures the tool's own
	// build HUD, the radial and this panel along with the map, and every one of
	// them would have to be hidden and restored around the capture. ReadPixels
	// reads the viewport's own render target, which holds the rendered 3D view
	// and nothing Slate drew on top of it, so the picture is clean with no
	// hiding at all. The Portal panel is hidden anyway while the capture runs,
	// because in its overlay placement it covers the part of the map the user
	// is most likely aiming at.
	bool CaptureViewportForThumbnail()
	{
		if (!GEditor) { GThumb.Status = TEXT("No editor to capture."); return false; }
		FViewport* Vp = GEditor->GetActiveViewport();
		if (!Vp) { GThumb.Status = TEXT("No active viewport. Click in the 3D view first, then try again."); Bump(); return false; }
		const FIntPoint Size = Vp->GetSizeXY();
		if (Size.X < 16 || Size.Y < 16) { GThumb.Status = TEXT("The viewport is too small to capture."); Bump(); return false; }
		Vp->Draw();   // a fresh frame, so what is captured is what is on screen
		TArray<FColor> Pixels;
		if (!Vp->ReadPixels(Pixels) || Pixels.Num() < Size.X * Size.Y)
		{
			GThumb.Status = TEXT("The viewport did not give up its pixels. Use UPLOAD IMAGE instead.");
			UE_LOG(LogBF6Portal, Warning, TEXT("Portal thumbnail: ReadPixels returned %d pixels for a %d x %d viewport"),
				Pixels.Num(), Size.X, Size.Y);
			Bump();
			return false;
		}
		AdoptSource(MoveTemp(Pixels), Size.X, Size.Y, TEXT("the map viewport"));
		UE_LOG(LogBF6Portal, Display, TEXT("Portal thumbnail: captured the viewport at %d x %d"), Size.X, Size.Y);
		return true;
	}

	bool LoadImageForThumbnail(const FString& Path)
	{
		TArray<uint8> File;
		if (!FFileHelper::LoadFileToArray(File, *Path) || File.Num() == 0)
		{
			GThumb.Status = FString::Printf(TEXT("Could not read %s"), *Path);
			Bump();
			return false;
		}
		IImageWrapperModule& M = FModuleManager::LoadModuleChecked<IImageWrapperModule>(TEXT("ImageWrapper"));
		const EImageFormat Fmt = M.DetectImageFormat(File.GetData(), File.Num());
		if (Fmt == EImageFormat::Invalid)
		{
			GThumb.Status = TEXT("That file is not an image the tool can read. Use a jpg, png or bmp.");
			Bump();
			return false;
		}
		TSharedPtr<IImageWrapper> W = M.CreateImageWrapper(Fmt);
		TArray64<uint8> Raw;
		if (!W.IsValid() || !W->SetCompressed(File.GetData(), File.Num()) || !W->GetRaw(ERGBFormat::BGRA, 8, Raw))
		{
			GThumb.Status = TEXT("That image could not be decoded.");
			Bump();
			return false;
		}
		const int32 W2 = W->GetWidth(), H2 = W->GetHeight();
		TArray<FColor> Pixels;
		Pixels.SetNumUninitialized(W2 * H2);
		FMemory::Memcpy(Pixels.GetData(), Raw.GetData(), (SIZE_T)W2 * H2 * sizeof(FColor));
		AdoptSource(MoveTemp(Pixels), W2, H2, FPaths::GetCleanFilename(Path));
		UE_LOG(LogBF6Portal, Display, TEXT("Portal thumbnail: loaded %s at %d x %d"), *Path, W2, H2);
		return true;
	}

	bool PickThumbnailFile()
	{
		IDesktopPlatform* DP = FDesktopPlatformModule::Get();
		if (!DP) { GThumb.Status = TEXT("File dialog unavailable."); return false; }
		TArray<FString> Picked;
		const void* Parent = FSlateApplication::IsInitialized()
			? FSlateApplication::Get().FindBestParentWindowHandleForDialogs(nullptr) : nullptr;
		if (!DP->OpenFileDialog(Parent, TEXT("Pick an image for the experience thumbnail"), FPaths::ProjectSavedDir(), TEXT(""),
			TEXT("Images (*.jpg;*.jpeg;*.png;*.bmp)|*.jpg;*.jpeg;*.png;*.bmp"), EFileDialogFlags::None, Picked)
			|| Picked.Num() == 0) return false;
		return LoadImageForThumbnail(Picked[0]);
	}

	bool BuildThumbnail()
	{
		if (GThumb.Src.Num() == 0 || GThumb.SrcW <= 0)
		{
			GThumb.Status = TEXT("Nothing to build from yet. SCREENSHOT MAP or UPLOAD IMAGE first.");
			Bump();
			return false;
		}
		// 1. crop, at the site's aspect
		int32 CX, CY, CW, CH;
		CropRect(CX, CY, CW, CH);
		TArray<FColor> Cropped;
		Cropped.SetNumUninitialized(CW * CH);
		for (int32 Row = 0; Row < CH; Row++)
			FMemory::Memcpy(Cropped.GetData() + Row * CW,
				GThumb.Src.GetData() + (CY + Row) * GThumb.SrcW + CX, (SIZE_T)CW * sizeof(FColor));

		// 2. scale to exactly 352 x 248
		TArray<FColor> Out;
		Out.SetNumUninitialized(kThumbW * kThumbH);
		FImageUtils::ImageResize(CW, CH, Cropped, kThumbW, kThumbH, Out, false);
		for (FColor& C : Out) C.A = 255;

		// 3. the quality ladder, exactly as the template does it: start at 90
		//    and come down until it is under the cap.
		IImageWrapperModule& M = FModuleManager::LoadModuleChecked<IImageWrapperModule>(TEXT("ImageWrapper"));
		TSharedPtr<IImageWrapper> W = M.CreateImageWrapper(EImageFormat::JPEG);
		if (!W.IsValid()) { GThumb.Status = TEXT("The JPEG encoder is not available."); Bump(); return false; }
		if (!W->SetRaw(Out.GetData(), (int64)Out.Num() * sizeof(FColor), kThumbW, kThumbH, ERGBFormat::BGRA, 8))
		{
			GThumb.Status = TEXT("The JPEG encoder refused the image.");
			Bump();
			return false;
		}
		static const int32 Ladder[] = { 90, 85, 80, 75, 70, 60, 50, 40, 30, 20 };
		TArray64<uint8> Best;
		int32 BestQ = 0;
		for (const int32 Q : Ladder)
		{
			TArray64<uint8> Try = W->GetCompressed(Q);
			if (Try.Num() == 0) continue;
			Best = MoveTemp(Try);
			BestQ = Q;
			if (Best.Num() <= kThumbMaxBytes) break;
		}
		if (Best.Num() == 0) { GThumb.Status = TEXT("The image could not be encoded as JPEG."); Bump(); return false; }
		GThumb.Jpeg.SetNumUninitialized((int32)Best.Num());
		FMemory::Memcpy(GThumb.Jpeg.GetData(), Best.GetData(), (SIZE_T)Best.Num());
		GThumb.Quality = BestQ;

		// 4. on disk, so it can be handed to the site's own file dialog
		const FString Id = CurrentExperienceId();
		IFileManager::Get().MakeDirectory(*ThumbDir(), true);
		GThumb.Path = FPaths::Combine(ThumbDir(), (Id.IsEmpty() ? FString(TEXT("thumbnail")) : Id) + TEXT(".jpg"));
		FFileHelper::SaveArrayToFile(GThumb.Jpeg, *GThumb.Path);
		MakeBrushFor(Out, kThumbW, kThumbH, GThumb.OutTex, GThumb.OutBrush, TEXT("preview"));

		const bool bUnder = GThumb.Jpeg.Num() <= kThumbMaxBytes;
		GThumb.Status = FString::Printf(TEXT("%d x %d JPEG, quality %d, %.1f KB%s"),
			kThumbW, kThumbH, BestQ, GThumb.Jpeg.Num() / 1024.f,
			bUnder ? TEXT(" - within the site's 78 KB limit.") : TEXT(" - STILL OVER 78 KB, pick a simpler picture."));
		UE_LOG(LogBF6Portal, Display, TEXT("Portal thumbnail: %s -> %s"), *GThumb.Status, *GThumb.Path);
		Bump();
		return bUnder;
	}

	void CopyThumbnailPath()
	{
		if (GThumb.Path.IsEmpty()) { BF6Api::Toast(TEXT("Build the thumbnail first.")); return; }
		FString Win = FPaths::ConvertRelativePathToFull(GThumb.Path);
		FPaths::MakePlatformFilename(Win);
		FPlatformApplicationMisc::ClipboardCopy(*Win);
		GThumb.Status = FString::Printf(TEXT("Copied: %s. Paste it into the site's file picker."), *Win);
		BF6Api::Toast(TEXT("Thumbnail path copied."));
		Bump();
	}

	void SendThumbnailToSite()
	{
		if (GThumb.Jpeg.Num() == 0 && !BuildThumbnail()) return;
		const FString Id = CurrentExperienceId();
		if (Id.IsEmpty())
		{
			GThumb.Status = TEXT("Open an experience first: the tool has to know which one to set.");
			Bump();
			return;
		}
		if (!LooksSignedIn())
		{
			GThumb.Status = TEXT("Portal signed you out. Sign in on the panel and press SEND TO SITE again.");
			Bump();
			return;
		}
		// NO CONSTRUCTED SECTION ADDRESS. The site holds the open experience in
		// app state, so the tool cannot jump to publish step two by url. It puts
		// the panel on the experience through the site's own card and does the
		// upload from wherever the page already is when that is an experience
		// page; the site's Image Select is then reached by its own menu.
		if (GLastKind == TEXT("editor") || GLastKind == TEXT("blocks"))
		{
			const FString B64 = FBase64::Encode(GThumb.Jpeg);
			GThumb.Status = TEXT("Uploading through the page...");
			UE_LOG(LogBF6Portal, Display, TEXT("Portal thumbnail: asking the page to upload %d bytes"), GThumb.Jpeg.Num());
			// The bytes go into a JS string literal, so they are base64 and
			// nothing else; no header, no cookie, no token is involved.
			BF6PortalWeb::Exec(FString::Printf(
				TEXT("try { window.BF6PortalCapture.uploadThumbnail('%s', 'image/jpeg'); } catch (e) { console.log('BF6CAPTURE upload error ' + e); }"), *B64));
			Bump();
			return;
		}
		GoToExperience(Id);
		GThumb.Status = TEXT("Opening the experience on the site. Go to its publish step, then press SEND TO SITE again.");
		Bump();
	}

	void LogThumbnail()
	{
		UE_LOG(LogBF6Portal, Display, TEXT("---- Portal thumbnail ----"));
		UE_LOG(LogBF6Portal, Display, TEXT("  requirement    : %d x %d, JPEG or PNG, at most %d bytes"), kThumbW, kThumbH, kThumbMaxBytes);
		UE_LOG(LogBF6Portal, Display, TEXT("  source         : %s"), GThumb.SrcWhat.IsEmpty() ? TEXT("(nothing captured)") : *FString::Printf(TEXT("%s %d x %d"), *GThumb.SrcWhat, GThumb.SrcW, GThumb.SrcH));
		UE_LOG(LogBF6Portal, Display, TEXT("  crop           : centre %.2f, %.2f  zoom %.2f"), GThumb.CenterU, GThumb.CenterV, GThumb.Zoom);
		UE_LOG(LogBF6Portal, Display, TEXT("  built          : %s"), GThumb.Jpeg.Num() ? *FString::Printf(TEXT("%d bytes at quality %d"), GThumb.Jpeg.Num(), GThumb.Quality) : TEXT("(not built)"));
		UE_LOG(LogBF6Portal, Display, TEXT("  file           : %s"), GThumb.Path.IsEmpty() ? TEXT("(none)") : *GThumb.Path);
		UE_LOG(LogBF6Portal, Display, TEXT("  uploaded url   : %s"), GThumb.UploadedUrl.IsEmpty() ? TEXT("(not uploaded)") : *GThumb.UploadedUrl);
		UE_LOG(LogBF6Portal, Display, TEXT("  verification   : %s"), GThumb.VerifyState.IsEmpty() ? TEXT("(none)") : *GThumb.VerifyState);
		UE_LOG(LogBF6Portal, Display, TEXT("  set on element : %s"), GThumb.SetState.IsEmpty() ? TEXT("(not set)") : *GThumb.SetState);
		UE_LOG(LogBF6Portal, Display, TEXT("  site's images  : %d pre-approved option(s) seen"), GThumb.SiteOptions.Num());
		for (const TPair<FString, FString>& O : GThumb.SiteOptions)
			UE_LOG(LogBF6Portal, Display, TEXT("    %s  %s"), *O.Value, *O.Key);
		UE_LOG(LogBF6Portal, Display, TEXT("  status         : %s"), *GThumb.Status);
		UE_LOG(LogBF6Portal, Display, TEXT("--------------------------"));
	}

	TSharedRef<SWidget> MakeColumn()         { return SNew(SBF6ProfileColumn); }
	TSharedRef<SWidget> MakeExperienceGrid() { return SNew(SBF6ExperienceGrid); }

	// ---- the progress strip -------------------------------------------------
	//
	// The site does its work off screen now, so this one line is what tells a
	// person the tool is doing something rather than hanging. It says what step
	// it is on, offers SHOW SITE to anyone who wants to watch, and CANCEL where
	// there is something to cancel. It collapses to nothing when there is no
	// work, so a quiet tool looks exactly as it always did.
	TSharedRef<SWidget> MakeWorkStrip()
	{
		return SNew(SBorder)
			.BorderImage(PanelBrush())
			.Padding(FMargin(12, 8))
			.Visibility_Lambda([]{ return BF6PortalProfile::IsWorking() ? EVisibility::Visible : EVisibility::Collapsed; })
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0, 0, 10, 0)
				[ Line(TEXT("PORTAL"), 8, BF6Theme::Accent, true) ]
				+ SHorizontalBox::Slot().FillWidth(1.f).VAlign(VAlign_Center)
				[
					SNew(STextBlock).AutoWrapText(false).OverflowPolicy(ETextOverflowPolicy::Ellipsis)
					.Font(FontReg(9)).ColorAndOpacity(FSlateColor(BF6Theme::Text))
					.Text_Lambda([]{ return FText::FromString(BF6PortalProfile::ProgressLine()); })
				]
				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(10, 0, 0, 0)
				[
					SNew(SButton).ButtonStyle(&GhostStyle()).ContentPadding(FMargin(10, 4))
					.ToolTipText(FText::FromString(TEXT("Bring the Portal site on screen so you can watch what the tool is doing, or do something by hand. The work carries on either way.")))
					.Visibility_Lambda([]{ return BF6PortalWeb::IsShown() ? EVisibility::Collapsed : EVisibility::Visible; })
					.OnClicked_Lambda([]{ BF6PortalWeb::ShowSite(); return FReply::Handled(); })
					[ Line(TEXT("SHOW SITE"), 8, BF6Theme::Text, true) ]
				]
				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(6, 0, 0, 0)
				[
					SNew(SButton).ButtonStyle(&GhostStyle()).ContentPadding(FMargin(10, 4))
					.ToolTipText(FText::FromString(TEXT("Stop after the map the tool is on. Everything already imported stays.")))
					.Visibility_Lambda([]{ return BF6PortalProfile::CanCancelWork() ? EVisibility::Visible : EVisibility::Collapsed; })
					.OnClicked_Lambda([]{ BF6PortalProfile::CancelImport(); return FReply::Handled(); })
					[ Line(TEXT("CANCEL"), 8, BF6Theme::TextDim, true) ]
				]
			];
	}
	TSharedRef<SWidget> MakeMapSwitcher()    { return SNew(SBF6MapSwitcher); }
	TSharedRef<SWidget> MakeThumbnailPanel() { return SNew(SBF6ThumbnailPanel); }

	void LogStatus()
	{
		UE_LOG(LogBF6Portal, Display, TEXT("---- Portal profile ----"));
		UE_LOG(LogBF6Portal, Display, TEXT("  state          : %s"), *StateLabel());
		UE_LOG(LogBF6Portal, Display, TEXT("  account         : %s"), GAccount.IsEmpty() ? TEXT("(the page has not shown one)") : *GAccount);
		UE_LOG(LogBF6Portal, Display, TEXT("  last page kind  : %s"), GLastKind.IsEmpty() ? TEXT("(none yet)") : *GLastKind);
		UE_LOG(LogBF6Portal, Display, TEXT("  last page url   : %s"), GLastUrl.IsEmpty() ? TEXT("(none yet)") : *GLastUrl);
		// The two lines that diagnose a failed import on their own.
		UE_LOG(LogBF6Portal, Display, TEXT("  last asked for  : %s"), GNavTarget.IsEmpty() ? TEXT("(nothing yet)") : *GNavTarget);
		UE_LOG(LogBF6Portal, Display, TEXT("  actually landed : %s"), GNavLanded.IsEmpty() ? (GLastUrl.IsEmpty() ? TEXT("(nothing yet)") : *GLastUrl) : *GNavLanded);
		UE_LOG(LogBF6Portal, Display, TEXT("  address shape   : %s"),
			GUrlPattern.IsEmpty() ? TEXT("(not learned yet; the tool clicks the site's own card and never builds a url)") : *GUrlPattern);
		UE_LOG(LogBF6Portal, Display, TEXT("  looks signed in : %s"), LooksSignedIn() ? TEXT("yes") : TEXT("no"));
		UE_LOG(LogBF6Portal, Display, TEXT("  page check      : %s"), *GPageStatus);
		UE_LOG(LogBF6Portal, Display, TEXT("  owned list seen : %s"), GHasOwnedList ? TEXT("yes") : TEXT("no"));
		UE_LOG(LogBF6Portal, Display, TEXT("  experiences     : %d"), GExps.Num());
		for (const FExp& E : GExps)
		{
			UE_LOG(LogBF6Portal, Display, TEXT("    %s  %s  maps: %s  %s"),
				*E.Id.Left(8), *E.Name,
				E.Maps.Num() ? *FString::Join(E.Maps, TEXT(",")) : TEXT("not imported yet"),
				IsImported(E.Id) ? TEXT("imported") : TEXT(""));
		}
		UE_LOG(LogBF6Portal, Display, TEXT("  cache           : %s"), *PortalRoot());
		UE_LOG(LogBF6Portal, Display, TEXT("  api             : %s"), *ApiStatus());
		UE_LOG(LogBF6Portal, Display, TEXT("  reads go through: %s"),
			ApiReady() ? TEXT("the site's own api, replayed from inside the page")
			           : TEXT("the site's pages, until it has made one call the tool can imitate"));
		UE_LOG(LogBF6Portal, Display, TEXT("  watch the site  : %s"), *WatchStatus());
		UE_LOG(LogBF6Portal, Display, TEXT("  auto-sync       : %s%s"), GAutoSync ? TEXT("on") : TEXT("off"),
			GPendingSyncId.IsEmpty() ? TEXT("") : *FString::Printf(TEXT(" (waiting to refresh %s)"), *GPendingSyncId.Left(8)));
		UE_LOG(LogBF6Portal, Display, TEXT("  search          : %s"), GSearch.IsEmpty() ? TEXT("(none)") : *GSearch);
		// The map screen's cost, as a number. A rebuild is 38 cards and their
		// thumbnails torn down and made again; this used to happen on every
		// general bump and now happens only when a card would look different.
		UE_LOG(LogBF6Portal, Display, TEXT("  card grid       : %d rebuild(s) this session, %d general bump(s)"),
			GGridRebuilds, (int32)GFingerprint);
		if (!GPushStatus.IsEmpty()) UE_LOG(LogBF6Portal, Display, TEXT("  last push       : %s"), *GPushStatus);
		if (!GLastFileImport.Name.IsEmpty())
			UE_LOG(LogBF6Portal, Display,
				TEXT("  last file import: '%s' - %d setting(s), %d team(s), %d map(s), %d spatial, %d script, %d strings, %d blacklist, %d block(s), %d save(s)"),
				*GLastFileImport.Name, GLastFileImport.Mutators, GLastFileImport.Teams, GLastFileImport.Rotation,
				GLastFileImport.Spatial, GLastFileImport.Script, GLastFileImport.Strings, GLastFileImport.Blacklist,
				GLastFileImport.WorkspaceBlocks, GLastFileImport.Saves);
		UE_LOG(LogBF6Portal, Display, TEXT("  importer        : %s"), GJob.bActive ? *GWorkStatus : TEXT("idle"));
		UE_LOG(LogBF6Portal, Display, TEXT("------------------------"));
	}
}

// ---- the bridge object -------------------------------------------------------

void UBF6PortalBridge::capture(FString Json)
{
	TSharedPtr<FJsonObject> Root;
	TSharedRef<TJsonReader<>> R = TJsonReaderFactory<>::Create(Json);
	if (!FJsonSerializer::Deserialize(R, Root) || !Root.IsValid())
	{
		UE_LOG(LogBF6Portal, Warning, TEXT("Portal capture: the page sent something that is not JSON (%d chars)"), Json.Len());
		return;
	}
	FString Method, Url, B64, ChunkId;
	double Status = 0, Seq = 0, Of = 0;
	Root->TryGetStringField(TEXT("method"), Method);
	Root->TryGetStringField(TEXT("url"), Url);
	Root->TryGetStringField(TEXT("bodyB64"), B64);
	Root->TryGetNumberField(TEXT("status"), Status);
	const bool bChunked = Root->TryGetStringField(TEXT("id"), ChunkId)
		&& Root->TryGetNumberField(TEXT("seq"), Seq) && Root->TryGetNumberField(TEXT("of"), Of);

	// The site's own Export is a text document, not a gRPC body: it arrives in
	// the same chunk envelope but is never base64.
	const bool bText = Method == TEXT("exportExperience") || Method == TEXT("siteScript");
	FString WantId;
	Root->TryGetStringField(TEXT("experienceId"), WantId);
	FString FileName;
	Root->TryGetStringField(TEXT("name"), FileName);

	if (!bChunked)
	{
		if (Method == TEXT("siteScript"))      HandleSiteScript(B64);
		else if (bText)                        HandleExportText(B64, WantId, FileName);
		else                                   HandleBody(Method, (int32)Status, B64);
		return;
	}

	FChunkBuf& Buf = GChunks.FindOrAdd(ChunkId);
	if (Buf.Of == 0) { Buf.Of = (int32)Of; Buf.Parts.SetNum((int32)Of); Buf.Started = FPlatformTime::Seconds(); }
	if (Buf.Parts.IsValidIndex((int32)Seq)) Buf.Parts[(int32)Seq] = B64;
	for (const FString& P : Buf.Parts) if (P.IsEmpty()) return;   // still arriving
	const FString Whole = FString::Join(Buf.Parts, TEXT(""));
	GChunks.Remove(ChunkId);
	UE_LOG(LogBF6Portal, Display, TEXT("Portal capture: %s reassembled from %d chunks (%d characters)"),
		*Method, (int32)Of, Whole.Len());
	if (Method == TEXT("siteScript"))      HandleSiteScript(Whole);
	else if (bText)                        HandleExportText(Whole, WantId, FileName);
	else                                   HandleBody(Method, (int32)Status, Whole);
}

void UBF6PortalBridge::pagestate(FString Json)
{
	TSharedPtr<FJsonObject> Root;
	TSharedRef<TJsonReader<>> R = TJsonReaderFactory<>::Create(Json);
	if (!FJsonSerializer::Deserialize(R, Root) || !Root.IsValid()) return;

	FString Kind, Url, Hint, ExpiredSel, ApiRedirect, KeepAliveResult;
	bool bBlockly = false, bBeat = false;
	Root->TryGetStringField(TEXT("kind"), Kind);
	Root->TryGetStringField(TEXT("url"), Url);
	Root->TryGetStringField(TEXT("signedInHint"), Hint);
	bool bOwnedListShown = false;
	Root->TryGetBoolField(TEXT("ownedListShown"), bOwnedListShown);
	GLastOwnedListShown = bOwnedListShown;
	if (GResumingLink) GResumeReports++;
	Root->TryGetStringField(TEXT("expiredDialog"), ExpiredSel);
	Root->TryGetStringField(TEXT("apiRedirect"), ApiRedirect);
	Root->TryGetStringField(TEXT("keepAlive"), KeepAliveResult);
	Root->TryGetBoolField(TEXT("hasBlockly"), bBlockly);
	Root->TryGetBoolField(TEXT("beat"), bBeat);
	GLastKind = Kind;
	GLastUrl = Url;
	// ANY message from the page is proof the page is alive, heartbeat or not.
	GLastProbeAt = FPlatformTime::Seconds();

	// Which of the site's calls the tool can now ask for directly. This is what
	// decides whether the api path is open or whether the site's pages still
	// have to be driven, and it is reported by the page rather than assumed.
	{
		const TArray<TSharedPtr<FJsonValue>>* Api = nullptr;
		if (Root->TryGetArrayField(TEXT("api"), Api))
		{
			const int32 Was = GApiSeen.Num();
			for (const auto& AV : *Api)
			{
				const FString M = AV->AsString();
				if (!M.IsEmpty()) GApiSeen.Add(M);
			}
			if (GApiSeen.Num() != Was)
				UE_LOG(LogBF6Portal, Display, TEXT("Portal api: %s"), *BF6PortalProfile::ApiStatus());
		}
	}

	// ---- the push, through the site's own import ---------------------------
	{
		FString ImportState, ImportDetail, Verdict, Ready, Transfer;
		Root->TryGetStringField(TEXT("importDetail"), ImportDetail);

		// The handshake answering. Only a document that has the receiver in it
		// can send this, and the token is this push's own.
		if (Root->TryGetStringField(TEXT("pushReady"), Ready) && !Ready.IsEmpty())
		{
			if (!GPushWait.bActive || Ready != GPushWait.Token)
			{
				UE_LOG(LogBF6Portal, Verbose, TEXT("Portal push: a stale ready (%s) was ignored"), *Ready);
				return;
			}
			const FString PageId = FindUuid(Url);
			if (!Url.Contains(TEXT("/bf6/experience/")) || PageId != GPushWait.Id)
			{
				// Still on its way. Keep probing until the deadline rather than
				// importing into whatever happens to be open.
				UE_LOG(LogBF6Portal, Verbose,
					TEXT("Portal push: the page answered from %s, waiting for %s"),
					PageId.IsEmpty() ? *Url : *PageId.Left(8), *GPushWait.Id.Left(8));
				return;
			}
			UE_LOG(LogBF6Portal, Display, TEXT("Portal push: the page is ready on %s [%s]"),
				*GPushWait.Id.Left(8), *ImportDetail);
			const FString Id = GPushWait.Id, Text = GPushWait.Text;
			GPushWait.bActive = false;
			SendImportToPage(Id, Text);
			Bump();
			return;
		}

		// A chunk that found no receiver. Chunks from a previous document are
		// meaningless, so the transfer restarts rather than carrying on.
		if (Root->TryGetStringField(TEXT("transferState"), Transfer) && !Transfer.IsEmpty())
		{
			UE_LOG(LogBF6Portal, Warning, TEXT("Portal push: %s (%s)"), *Transfer, *ImportDetail);
			return;
		}
		if (Root->TryGetStringField(TEXT("importState"), ImportState) && !ImportState.IsEmpty())
		{
			// Every one of these is logged whole, selectors and all, so the
			// first live run pins the control down in one pass.
			UE_LOG(LogBF6Portal, Display, TEXT("Portal push by import: %s (%s)"), *ImportState, *ImportDetail);
			if (ImportState == TEXT("handed") || ImportState == TEXT("dropped"))
			{
				GPushStatus = FString::Printf(
					TEXT("Handed the whole experience to the site's own import (%s). Waiting for the site to save it."), *ImportDetail);
			}
			else if (ImportState == TEXT("wrong-experience"))
			{
				EndPush();
				GPushStatus = FString::Printf(TEXT("Refused: %s. Nothing was imported."), *ImportDetail);
				UE_LOG(LogBF6Portal, Warning, TEXT("Portal push refused: %s"), *ImportDetail);
			}
			else if (ImportState == TEXT("front-page"))
			{
				EndPush();
				GPushStatus = TEXT("The experiences list import makes a new experience, so it is never used. Open the experience first.");
			}
			else if (ImportState == TEXT("no-payload"))
			{
				// The transfer did not survive. Start it again from the
				// beginning, through the handshake, rather than sending the
				// missing pieces into a document that may already be gone.
				const FString Id = GPushExpId;
				const FExp* PE = Find(Id);
				TSharedPtr<FJsonObject> Doc;
				if (PE && GPushWait.Restarts < kMaxPushRestarts && BF6PortalProfile::ExperienceJson(Id, Doc) && Doc.IsValid())
				{
					const int32 Restarts = GPushWait.Restarts + 1;
					GPushWait = FPushWait();
					GPushWait.Id = Id;
					GPushWait.Text = JsonToString(Doc);
					GPushWait.Token = FString::Printf(TEXT("t%lld"), (int64)(FPlatformTime::Seconds() * 1000.0));
					GPushWait.Deadline = FPlatformTime::Seconds() + 20.0;
					GPushWait.Restarts = Restarts;
					GPushWait.bActive = true;
					GPushStatus = FString::Printf(TEXT("The transfer did not arrive whole (%s). Starting it again."), *ImportDetail);
					UE_LOG(LogBF6Portal, Warning, TEXT("Portal push: %s Attempt %d of %d."),
						*GPushStatus, Restarts + 1, kMaxPushRestarts + 1);
				}
				else
				{
					EndPush();
					UE_LOG(LogBF6Portal, Warning, TEXT("Portal push: the transfer never arrived whole (%s)"), *ImportDetail);
					GPushStatus = FString::Printf(TEXT("The transfer never arrived whole (%s). Trying the site's own save message instead."), *ImportDetail);
					BF6PortalProfile::PushByMessage();
				}
				Bump();
				return;
			}
			else if (ImportState == TEXT("no-control") || ImportState == TEXT("no-input")
				|| ImportState == TEXT("not-in-editor") || ImportState == TEXT("no-id-on-page"))
			{
				// No import control the tool could find. Fall to the second
				// route, and to the file after that.
				EndPush();
				UE_LOG(LogBF6Portal, Warning, TEXT("Portal push by import failed (%s): %s"), *ImportState, *ImportDetail);
				GPushStatus = FString::Printf(TEXT("The site's import could not be driven (%s). Trying the site's own save message instead."), *ImportState);
				BF6PortalProfile::PushByMessage();
			}
			else
			{
				GPushStatus = FString::Printf(TEXT("The site's import said: %s"), *ImportState);
			}
			Bump();
			return;
		}
		if (Root->TryGetStringField(TEXT("importVerdict"), Verdict) && !Verdict.IsEmpty())
		{
			// The site writing the experience back is the only thing that means
			// the import worked. A click is not a verdict.
			UE_LOG(LogBF6Portal, Display, TEXT("Portal push verdict: %s %s"), *Verdict, *ImportDetail);
			EndPush();
			const bool bOk = Verdict.Contains(TEXT(":200")) || Verdict.Contains(TEXT(":201")) || Verdict.Contains(TEXT(":204"));
			GPushStatus = bOk
				? FString::Printf(TEXT("The site saved the imported experience (%s %s)."), *Verdict, *ImportDetail)
				: FString::Printf(TEXT("The site answered the import with %s. Nothing was assumed; check the experience on the site."), *Verdict);
			if (bOk)
			{
				// What the tool has is now what the site has.
				if (FExp* E = Find(GPushExpId.IsEmpty() ? CurrentExperienceId() : GPushExpId))
				{
					E->SiteName = E->Name;
					E->SiteDescription = E->Description;
					const FString Blocks = FPaths::Combine(ExpDir(E->Id), TEXT("rules.blocks.json"));
					const FString BT = ReadTextFile(Blocks);
					if (!BT.IsEmpty()) FFileHelper::SaveStringToFile(BT, *(Blocks + TEXT(".orig")), FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
					for (const FAttachment& A : E->Files)
					{
						if (A.Path.IsEmpty()) continue;
						const FString T = ReadTextFile(A.Path);
						if (!T.IsEmpty()) FFileHelper::SaveStringToFile(T, *(A.Path + TEXT(".orig")), FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
					}
					WriteCache(*E);
				}
				BF6Api::Toast(GPushStatus);
			}
			Bump();
			return;
		}
	}

	// The whole-experience push through the site's own save message, and the
	// site's own Export being driven.
	{
		FString Push, ExportState, ListState;
		if (Root->TryGetStringField(TEXT("push"), Push) && !Push.IsEmpty())
		{
			GPushStatus = Push.StartsWith(TEXT("push:refused:"))
				? FString::Printf(TEXT("The site would not take it: %s"), *Push.RightChop(13))
				: FString::Printf(TEXT("The site's own save answered %s."), *Push.RightChop(5));
			EndPush();
			UE_LOG(LogBF6Portal, Display, TEXT("Portal push: %s"), *GPushStatus);
			// A push the site accepted makes what the tool has the new "before"
			// for the next one, so the same change is never sent twice.
			if (Push == TEXT("push:200"))
			{
				if (FExp* E = Find(CurrentExperienceId().IsEmpty() ? FirstFetchedId() : CurrentExperienceId()))
				{
					E->SiteName = E->Name;
					E->SiteDescription = E->Description;
					const FString Blocks = FPaths::Combine(ExpDir(E->Id), TEXT("rules.blocks.json"));
					const FString Text = ReadTextFile(Blocks);
					if (!Text.IsEmpty()) FFileHelper::SaveStringToFile(Text, *(Blocks + TEXT(".orig")), FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
					for (const FAttachment& A : E->Files)
					{
						const FString T = ReadTextFile(A.Path);
						if (!T.IsEmpty()) FFileHelper::SaveStringToFile(T, *(A.Path + TEXT(".orig")), FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
					}
					WriteCache(*E);
				}
			}
			Bump();
			return;
		}
		if (Root->TryGetStringField(TEXT("exportState"), ExportState) && !ExportState.IsEmpty())
		{
			// Every one of these is a reason the site's Export did not run, and
			// each is said in words rather than left as silence.
			if (ExportState == TEXT("need-list"))            GWorkStatus = TEXT("The site's export lives on the experiences list. Opening it.");
			else if (ExportState == TEXT("no-card"))         GWorkStatus = TEXT("No tile for that experience is on the list. Refresh the list and try again.");
			else if (ExportState == TEXT("no-menu-button"))  GWorkStatus = TEXT("The tile's menu button was not where the tool expected it. The site's layout may have changed.");
			else if (ExportState == TEXT("no-export-item"))  GWorkStatus = TEXT("The tile menu opened but had no Export item.");
			else if (ExportState == TEXT("clicked"))         GWorkStatus = TEXT("Export clicked. Waiting for the file the site builds.");
			else                                             GWorkStatus = FString::Printf(TEXT("The site's export said: %s"), *ExportState);
			UE_LOG(LogBF6Portal, Display, TEXT("Portal export from site: %s"), *GWorkStatus);
			Bump();
			return;
		}
		if (Root->TryGetStringField(TEXT("list"), ListState) && !ListState.IsEmpty())
		{
			UE_LOG(LogBF6Portal, Display, TEXT("Portal list refresh: %s"), *ListState);
			if (ListState == TEXT("no-request-observed"))
				GWorkStatus = TEXT("The site has not asked for your list yet this session. Opening the experiences page once is enough.");
			Bump();
			return;
		}
	}

	// THE PAGE'S OWN WARNINGS, kept against the experience that is open on it.
	// Only recorded when we know which experience the page is showing: a
	// warning with nothing to attach it to is a warning nobody can act on.
	{
		const TArray<TSharedPtr<FJsonValue>>* Notices = nullptr;
		if (Root->TryGetArrayField(TEXT("notices"), Notices))
		{
			const FString OnId = CurrentExperienceId();
			if (FExp* E = OnId.IsEmpty() ? nullptr : Find(OnId))
			{
				TArray<FString> Fresh;
				for (const TSharedPtr<FJsonValue>& V : *Notices)
				{
					const TSharedPtr<FJsonObject>* O = nullptr;
					if (!V.IsValid() || !V->TryGetObject(O) || !O) { continue; }
					FString NoticeKind, NoticeText;
					(*O)->TryGetStringField(TEXT("kind"), NoticeKind);
					(*O)->TryGetStringField(TEXT("text"), NoticeText);
					if (NoticeText.IsEmpty()) { continue; }
					Fresh.Add(FString::Printf(TEXT("%s: %s"),
						NoticeKind.IsEmpty() ? TEXT("notice") : *NoticeKind, *NoticeText));
				}
				if (Fresh != E->Notices)
				{
					E->Notices = Fresh;
					E->NoticesAt = FDateTime::UtcNow();
					if (Fresh.Num() > 0)
					{
						UE_LOG(LogBF6Portal, Warning,
							TEXT("The Portal page reports %d problem(s) with '%s'. ")
							TEXT("BF6.Portal.Notices lists them; importing it again clears them."),
							Fresh.Num(), *E->Name);
						for (const FString& N : Fresh)
						{
							UE_LOG(LogBF6Portal, Warning, TEXT("   %s"), *N);
						}
					}
					Bump();
				}
			}
		}
	}

	if (!KeepAliveResult.IsEmpty())
	{
		// DISPLAY, not Verbose. This is the only evidence that the thing
		// keeping the session alive is running at all, and at one line every
		// four minutes it is not spam. Hidden at Verbose, "am I still signed
		// in and why not" was unanswerable from a log.
		UE_LOG(LogBF6Portal, Display, TEXT("Portal keep-alive: %s"), *KeepAliveResult);
		return;   // nothing else on this message is meaningful
	}

	// ---- the site's own card, clicked, and where it landed ------------------
	{
		const TSharedPtr<FJsonObject>* Open = nullptr;
		if (Root->TryGetObjectField(TEXT("open"), Open) && Open->IsValid())
		{
			FString Oid, How, Sel, Before, After;
			(*Open)->TryGetStringField(TEXT("id"), Oid);
			(*Open)->TryGetStringField(TEXT("how"), How);
			(*Open)->TryGetStringField(TEXT("selector"), Sel);
			(*Open)->TryGetStringField(TEXT("before"), Before);
			(*Open)->TryGetStringField(TEXT("after"), After);
			Oid = Oid.ToLower();
			GNavLanded = After;

			if (How == TEXT("click"))
			{
				UE_LOG(LogBF6Portal, Display, TEXT("Portal import: clicked the card for %s matched by '%s'; the site landed on %s"),
					*Oid.Left(8), *Sel, *After);
				// LEARN, do not guess. An address is only reusable when it names
				// the experience: the site keeps the open one in app state, so a
				// generic address would later open whatever was open last.
				if (!Oid.IsEmpty() && After.Contains(Oid))
				{
					FExp& E = FindOrAdd(Oid);
					E.OpenUrl = After;
					WriteCache(E);
					FString Pattern = After;
					Pattern.ReplaceInline(*Oid, TEXT("<id>"));
					if (Pattern != GUrlPattern)
					{
						GUrlPattern = Pattern;
						GConfig->SetString(kIniSection, TEXT("PortalExperienceUrlPattern"), *GUrlPattern, GEditorPerProjectIni);
						GConfig->Flush(false, GEditorPerProjectIni);
					}
					if (!GLoggedPattern)
					{
						GLoggedPattern = true;
						UE_LOG(LogBF6Portal, Display, TEXT("Portal experience address learned from the site: %s"), *GUrlPattern);
					}
				}
				else if (!GLoggedPattern)
				{
					GLoggedPattern = true;
					UE_LOG(LogBF6Portal, Display,
						TEXT("Portal experience address does not carry the experience id (%s), so it is not reusable: the site holds the open experience in app state and every navigation goes through its own card."),
						*After);
				}
			}
			else if (How == TEXT("no-card"))
			{
				GPageStatus = FString::Printf(TEXT("Signed in, but no card for that experience is on the list at %s."), *After);
				UE_LOG(LogBF6Portal, Warning, TEXT("Portal import: no card for %s on %s"), *Oid.Left(8), *After);
			}
			else if (How == TEXT("need-list"))
			{
				UE_LOG(LogBF6Portal, Verbose, TEXT("Portal import: the page was on %s, not the experiences list"), *After);
			}
			Bump();
			return;
		}
	}

	// ---- the thumbnail's page-side steps reporting back ---------------------
	{
		FString Verify, Select, Set, Upload;
		if (Root->TryGetStringField(TEXT("thumbVerify"), Verify) && !Verify.IsEmpty())
		{
			GThumb.VerifyState = Verify;
			UE_LOG(LogBF6Portal, Display, TEXT("Portal thumbnail verification: %s"), *Verify);
			if (Verify.StartsWith(TEXT("ok:")) && !GThumb.UploadedUrl.IsEmpty())
			{
				GThumb.Status = TEXT("Scanned and accepted. Selecting it on the page...");
				FString Esc = GThumb.UploadedUrl; Esc.ReplaceInline(TEXT("'"), TEXT("\\'"));
				BF6PortalWeb::Exec(FString::Printf(
					TEXT("try { window.BF6PortalCapture.selectImage('%s'); } catch (e) {}"), *Esc));
			}
			Bump();
			return;
		}
		if (Root->TryGetStringField(TEXT("thumbSelect"), Select) && !Select.IsEmpty())
		{
			GThumb.SetState = FString::Printf(TEXT("Image Select: %s"), *Select);
			UE_LOG(LogBF6Portal, Display, TEXT("Portal thumbnail %s"), *GThumb.SetState);
			if (Select == TEXT("ok"))
			{
				GThumb.Status = TEXT("Picked on the site's own Image Select. Press the site's save or publish to keep it.");
			}
			else
			{
				// The page could not be driven. Last resort: the site's OWN
				// last updatePlayElement message, resent with only the
				// thumbnail string different, so nothing else can be lost.
				GThumb.Status = TEXT("The site's picker did not take it. Trying the site's own update call...");
				const FExp* E = Find(CurrentExperienceId());
				const FString Old = E ? (E->ScrapedThumbUrl.IsEmpty() ? E->ThumbUrl : E->ScrapedThumbUrl) : FString();
				FString EscOld = Old, EscNew = GThumb.UploadedUrl;
				EscOld.ReplaceInline(TEXT("'"), TEXT("\\'"));
				EscNew.ReplaceInline(TEXT("'"), TEXT("\\'"));
				BF6PortalWeb::Exec(FString::Printf(
					TEXT("try { window.BF6PortalCapture.setThumbnailUrl('%s', '%s'); } catch (e) {}"), *EscOld, *EscNew));
			}
			Bump();
			return;
		}
		if (Root->TryGetStringField(TEXT("thumbSet"), Set) && !Set.IsEmpty())
		{
			GThumb.SetState = Set;
			UE_LOG(LogBF6Portal, Warning, TEXT("Portal thumbnail set: %s"), *Set);
			GThumb.Status = Set.StartsWith(TEXT("refused:"))
				? FString::Printf(TEXT("Could not set it automatically (%s). COPY PATH and pick the file in the site's own dialog."), *Set.RightChop(8))
				: FString::Printf(TEXT("Sent through the site's own update call: %s"), *Set);
			Bump();
			return;
		}
		if (Root->TryGetStringField(TEXT("thumb"), Upload) && !Upload.IsEmpty())
		{
			UE_LOG(LogBF6Portal, Display, TEXT("Portal thumbnail page step: %s"), *Upload);
			if (Upload.Contains(TEXT("no-request-observed")))
				GThumb.Status = TEXT("The site has not uploaded a thumbnail itself yet, so there is no call to imitate. COPY PATH and pick the file in the site's own dialog once; after that the tool can do it.");
			Bump();
			return;
		}
	}

	// the pre-approved generic set, read off the Image Select dialog
	{
		const TArray<TSharedPtr<FJsonValue>>* Opts = nullptr;
		if (Root->TryGetArrayField(TEXT("imageOptions"), Opts) && Opts->Num() > 0)
		{
			TArray<TPair<FString, FString>> Found;
			for (const auto& OV : *Opts)
			{
				const TSharedPtr<FJsonObject> O = OV->AsObject(); if (!O.IsValid()) continue;
				FString Src, Label;
				O->TryGetStringField(TEXT("src"), Src);
				O->TryGetStringField(TEXT("label"), Label);
				if (!Src.IsEmpty()) Found.Emplace(Src, Label);
			}
			if (Found.Num() != GThumb.SiteOptions.Num())
				UE_LOG(LogBF6Portal, Display, TEXT("Portal thumbnail: %d image option(s) on the page"), Found.Num());
			GThumb.SiteOptions = MoveTemp(Found);
		}
	}

	// ---- the four ways the site says it signed the user out -----------------
	if (!ExpiredSel.IsEmpty())
	{
		// The selector is logged so a redesign can be re-anchored from the log
		// rather than from a guess.
		UE_LOG(LogBF6Portal, Warning, TEXT("Portal session dialog matched: %s"), *ExpiredSel);
		NoteSessionLost(TEXT("the site showed its session-expired dialog"));
	}
	if (!ApiRedirect.IsEmpty())
		NoteSessionLost(FString::Printf(TEXT("an API call was redirected to %s"), *ApiRedirect));
	if (!Hint.IsEmpty() && Hint != GAccount) { GAccount = Hint; SaveConfig(); Bump(); }

	// ---- resuming a link the site can only show us, not tell us ------------
	//
	// Linked normally needs a PARSED owned-experiences response, and for
	// everything the user drives that rule stands. It cannot be met by the
	// startup check: the site takes its reference to fetch and XHR before
	// anything the tool injects can run, so on a page that is merely loaded
	// every request goes past unseen. Proven on a live page - the experiences
	// list fully rendered, the capture present, and its record of observed
	// calls empty.
	//
	// So for THAT ONE MOMENT, and only while the startup check is running, the
	// page rendering the signed-in owned list is accepted instead. It is still
	// the site answering, and only a signed-in account sees that list. The
	// moment the user does anything, the ordinary rule is what applies again.
	// Said out loud while the check runs. Every failure in this feature so far
	// has been silent, and silence is what made it take so many attempts.
	// AND FOR A LINK THE USER ASKED FOR, TOO.
	//
	// Unlink deliberately leaves the browser signed in, so the very next press
	// of LINK PORTAL PROFILE lands on a site that never shows a login form and
	// never re-fetches the owned list: it is already there, served from the
	// site's own store. No getOwnedPlayElementsV2 goes past, so the strict
	// rule above could not be met, and the panel sat on "Signing in" for ever.
	// Unlink then link again, and the tool did not notice - observed by the
	// user on 2026-09-06.
	//
	// The evidence accepted here is the same evidence the startup check
	// accepts, and it is not weak: only a signed-in account is ever shown its
	// own experiences list. What made it acceptable there makes it acceptable
	// for a link the user pressed a button to start. Everything OUTSIDE those
	// two moments still needs the parsed response.
	const bool bWatchingForLink = GResumingLink || GLinkInProgress
		|| GState == EState::SigningIn;
	if (bWatchingForLink)
	{
		UE_LOG(LogBF6Portal, Display,
			TEXT("Portal %s: the page answered - kind '%s', signed-in list %s, state %s"),
			GResumingLink ? TEXT("resume") : TEXT("link"),
			Kind.IsEmpty() ? TEXT("none") : *Kind,
			bOwnedListShown ? TEXT("present") : TEXT("not present"),
			*BF6PortalProfile::StateLabel());
	}
	if (bWatchingForLink && bOwnedListShown && Kind == TEXT("experiences")
		&& GState != EState::Linked)
	{
		SetState(EState::Linked, TEXT("the site rendered the signed-in experiences list"));
		GResumingLink = false;
	}

	// The rendered cards: the <img> the browser actually shows is the RESOLVED
	// thumbnail url, which the raw field often is not.
	const TArray<TSharedPtr<FJsonValue>>* Thumbs = nullptr;
	if (Root->TryGetArrayField(TEXT("thumbs"), Thumbs))
	{
		for (const auto& TV : *Thumbs)
		{
			const TSharedPtr<FJsonObject> O = TV->AsObject(); if (!O.IsValid()) continue;
			FString Id, Name, Src;
			O->TryGetStringField(TEXT("id"), Id);
			O->TryGetStringField(TEXT("name"), Name);
			O->TryGetStringField(TEXT("src"), Src);
			if (!IsUuid(Id)) continue;
			FExp& E = FindOrAdd(Id.ToLower());
			if (E.Name.IsEmpty() && !Name.IsEmpty()) E.Name = Name;
			if (Src.StartsWith(TEXT("http")) && Src != E.ScrapedThumbUrl) { E.ScrapedThumbUrl = Src; WriteCache(E); Bump(); }
		}
	}

	// A previously linked profile whose page bounced to login has expired. The
	// site is the only party that can tell us, and this is it telling us.
	if (Kind == TEXT("login") && GState == EState::Linked)
		NoteSessionLost(TEXT("the page went back to the login screen"));

	// AND IT ENDS THE IMPORT, THERE AND THEN. The failure this fixes was an
	// import that kept navigating at a login page and counting the answers as
	// failed experiences.
	if (Kind == TEXT("login") && GJob.bActive && !GJob.bSignedOut)
	{
		GJob.bSignedOut = true;
		UE_LOG(LogBF6Portal, Warning, TEXT("Portal import: the page is the login screen, stopping the run."));
	}

	if (GExpect.bActive)
	{
		if (PageAgrees(GExpect.Key, Kind, Url, bBlockly))
		{
			GPageStatus = FString::Printf(TEXT("On %s."), *GExpect.Key);
			ResolveExpect(true, Url);
		}
		else if (Kind == TEXT("login") && GExpect.Key != TEXT("login"))
		{
			GPageStatus = FString::Printf(TEXT("expected %s got %s"), *GExpect.Key, *Url);
			UE_LOG(LogBF6Portal, Warning, TEXT("Portal page check: expected %s got %s"), *GExpect.Key, *Url);
			// A bounce to login in the middle of a tool-driven navigation is
			// the commonest face of a sign-out, and the page check is where the
			// tool notices it first.
			NoteSessionLost(TEXT("a page the tool asked for bounced to the login screen"));
			ResolveExpect(false, GPageStatus);
		}
	}
	else if (GLinkInProgress)
	{
		// A slow site has to read as loading rather than as broken, so the
		// sign-in overlay says what the page is doing, out of the reports the
		// page is already sending.
		const TCHAR* Doing =
			Kind == TEXT("login")       ? TEXT("the sign-in page is up") :
			Kind == TEXT("experiences") ? TEXT("your experiences are loading") :
			Kind == TEXT("editor")      ? TEXT("an experience is open") :
			Kind.IsEmpty()              ? TEXT("waiting for the site") : TEXT("loading");
		GPageStatus = FString::Printf(
			TEXT("Sign in on the page. The tool never sees your password. Right now: %s."), Doing);
	}
	else if (!bBeat)
	{
		GPageStatus = FString::Printf(TEXT("Page: %s"), Kind.IsEmpty() ? TEXT("unknown") : *Kind);
	}

	// ---- recovery, watching the page it just reloaded -----------------------
	if (GLost && GRecovery == ERecovery::Reloading)
	{
		if (Kind == TEXT("login"))
		{
			// The cookie store did not carry the sign-in. Try again a couple of
			// times, then stop and wait for the user rather than reloading a
			// login page forever.
			if (GRecoveryTries >= 2)
			{
				GRecovery = ERecovery::WaitingForUser;
				GRecoveryResult = TEXT("reloading did not sign in again, waiting for the user");
				GBanner = TEXT("Portal signed you out. Nothing is lost: your blocks, script and saves are kept in the tool. Sign in on the page to continue.");
				UE_LOG(LogBF6Portal, Warning, TEXT("Portal session recovery: %s"), *GRecoveryResult);
			}
		}
		else if (!Kind.IsEmpty() && Kind != TEXT("other"))
		{
			// A real Portal page answered, so the browser is signed in again.
			FinishRecovery(FString::Printf(TEXT("the page reloaded signed in on attempt %d"), FMath::Max(GRecoveryTries, 1)));
		}
	}

	// A heartbeat that changed nothing must not rebuild the panel every five
	// seconds; anything the user could see does.
	if (!bBeat || GLost) Bump();
}

// ---- module hooks ------------------------------------------------------------

// ---- picking the link back up on its own ----------------------------------
//
// Linked is only ever reached by evidence: the experiences page, plus a parsed
// owned-experiences response. That rule is right and it stays. What was
// missing is that nothing ever went and GOT that evidence unless the user
// pressed LINK PORTAL PROFILE, so a browser session that was still perfectly
// good sat there while the tool insisted it was not linked, every launch.
//
// If there is a session on disk, the tool asks the site once, quietly, with
// the panel offscreen. Signed in, it goes Linked and the user never sees a
// thing. Signed out, the page lands on login, the tool stays unlinked and the
// button means what it always meant. Nothing is shown either way, because
// being asked to look at a web page you did not open is worse than a button.
static FTSTicker::FDelegateHandle GResumeTick;

// Set when the check ended because the browser became someone else's, so the
// three minute ticker still releases its hold but does not report a timeout
// that did not happen. A wrong line in the log is worse than no line: the last
// round of this bug was chased through a message that was not describing what
// had actually occurred.
static bool GResumeStoppedEarly = false;

static void ResumeLinkQuietly()
{
	// Only worth doing when there is something to resume and we are not there
	// already. SigningIn means the user is doing it by hand right now.
	if (BF6PortalProfile::State() == BF6PortalProfile::EState::Linked) return;
	if (BF6PortalProfile::State() == BF6PortalProfile::EState::SigningIn) return;
	if (!BF6PortalWeb::HasSavedSession()) return;

	// THERE IS ONE BROWSER, AND THIS CHECK DOES NOT OWN IT.
	//
	// The panel, the block editor and the script editor are all the same
	// SWebBrowser window. This check was written for a fresh launch, where the
	// window does not exist yet and the panel's own start address goes to the
	// experiences list. When the window is ALREADY on some other page, nothing
	// navigates: OpenQuiet on a page that is up is a no-op, so the check sat
	// there interrogating whatever happened to be loaded, waiting for a kind of
	// 'experiences' that could never arrive.
	//
	// Observed on 2026-09-07: the window was on the site's own blocks page, the
	// check asked it 98 times over the full three minutes, and every answer
	// said kind 'blocks'. It then reported the profile unlinked. Worse, each
	// ask re-ran the capture inside the page the user was working in, so the
	// cost landed on the block editor.
	//
	// Navigating away to fix it is not an option - that is the user's editor.
	// So the check simply does not run, and LINK PORTAL PROFILE still works.
	const FString Loaded = BF6PortalWeb::CurrentUrl();
	if (!Loaded.IsEmpty() && !Loaded.Contains(TEXT("/experiences")))
	{
		UE_LOG(LogBF6Portal, Display,
			TEXT("Portal profile: the browser is already on %s, which another part of the tool put there, so the quiet session check is skipped. LINK PORTAL PROFILE still works."),
			*Loaded);
		return;
	}

	UE_LOG(LogBF6Portal, Display,
		TEXT("Portal profile: a browser session is on disk, so the site is being asked once, offscreen, whether it is still good. Nothing is shown."));
	GResumingLink = true;
	BF6PortalWeb::HoldOffscreen(true);
	// No address given on purpose: the panel's own start address already goes
	// to the experiences list when a session is on disk, and that list is the
	// exact page the Linked evidence rule wants.
	BF6PortalWeb::OpenQuiet();

	// THE LIST REQUEST HAS TO BE MADE AGAIN, and this is why.
	//
	// The capture hooks fetch when it is injected, and it is injected after the
	// page has loaded. The site's app asks for the experiences list while it is
	// mounting, which is BEFORE that, so on a fresh page load the one request
	// the Linked rule depends on goes past unseen and never comes again. The
	// offscreen page sat on the experiences list having asked for everything
	// and shown us nothing.
	//
	// So once the capture is up, the app is sent away and brought straight
	// back through its own router. That remounts the list with our hook already
	// in place, and the request it makes this time is seen. It is the app's own
	// navigation, not a reload: no page is fetched again and nothing the user
	// has open is disturbed.
	FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda([](float) -> bool
	{
		BF6PortalWeb::Exec(TEXT(
			"try {"
			"  if (window.BF6PortalCapture && /\\/experiences/.test(location.pathname)) {"
			"    var back = location.pathname + location.search;"
			"    history.pushState({}, '', '/bf6');"
			"    window.dispatchEvent(new PopStateEvent('popstate'));"
			"    setTimeout(function () {"
			"      history.pushState({}, '', back);"
			"      window.dispatchEvent(new PopStateEvent('popstate'));"
			"    }, 400);"
			"  }"
			"} catch (e) { console.log('BF6CAPTURE remount error ' + e); }"));
		return false;
	}), 8.0f);

	// ASK REPEATEDLY, BECAUSE THE PAGE ONLY VOLUNTEERS ONCE.
	//
	// The capture reports its state when it becomes ready, and at that instant
	// the site has loaded but has not yet drawn the experiences list. So the
	// one report that arrives says "not signed in yet" and nothing ever
	// corrects it. Proven on a live page: the bridge was up, the capture was
	// up, the list was on screen, and the tool had been told none of it.
	//
	// So it is asked every two seconds for as long as the check runs, and the
	// first answer that carries the list ends it.
	FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda([](float) -> bool
	{
		if (!GResumingLink) return false;                  // answered, or given up
		if (BF6PortalProfile::State() == BF6PortalProfile::EState::Linked) return false;
		// AND STOP THE MOMENT THE BROWSER IS SOMEONE ELSE'S AGAIN. The guard at
		// the top of this function covers a check that never should have
		// started; this covers the block editor opening while one is already
		// running. Either way, asking a page that cannot give the answer is
		// work charged to whatever the user is actually doing in it.
		{
			const FString Now = BF6PortalWeb::CurrentUrl();
			if (!Now.IsEmpty() && !Now.Contains(TEXT("/experiences")))
			{
				UE_LOG(LogBF6Portal, Display,
					TEXT("Portal profile: the browser moved to %s while the quiet session check was running, so the check stops. LINK PORTAL PROFILE still works."),
					*Now);
				GResumingLink = false;
				// The hold is REF COUNTED and the three minute ticker below
				// always runs and always releases one. Releasing here as well
				// would take someone else's hold down with it, so this path
				// only ends the asking.
				GResumeStoppedEarly = true;
				return false;
			}
		}
		BF6PortalWeb::Exec(TEXT(
			"try { if (window.BF6PortalCapture) window.BF6PortalCapture.report(); } catch (e) {}"));
		return true;                                       // keep asking
	}), 3.0f);

	// Let go of the hold once the answer has had time to arrive, whichever way
	// it went. The panel is left exactly as the user had it.
	FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda([](float) -> bool
	{
		BF6PortalWeb::HoldOffscreen(false);
		GResumingLink = false;
		if (GResumeStoppedEarly)
		{
			GResumeStoppedEarly = false;
			return false;   // already said why, and it was not a timeout
		}
		if (BF6PortalProfile::State() != BF6PortalProfile::EState::Linked)
		{
			UE_LOG(LogBF6Portal, Display,
				TEXT("Portal profile: the site did not confirm a session offscreen after %.0f seconds, so the profile stays unlinked. The page answered %d time(s); its last answer was kind '%s', signed-in list %s. LINK PORTAL PROFILE still works."),
				180.0, GResumeReports, GLastKind.IsEmpty() ? TEXT("none") : *GLastKind,
				GLastOwnedListShown ? TEXT("present") : TEXT("not present"));
		}
		return false;
		// THREE MINUTES, NOT THIRTY SECONDS. The page is running offscreen and
		// a hidden renderer is throttled by the browser, so the experiences
		// list can take far longer to draw there than it does on screen. The
		// old window closed while the site was still working, and the failure
		// said nothing about why, which is why it now reports what the page
		// actually answered.
	}), 180.0f);
}

void BF6PortalProfile::Register()
{
	// The editor is still assembling itself at this point, and the browser
	// widget is not there yet, so the question is asked a few seconds in
	// rather than now.
	GResumeTick = FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda([](float) -> bool
	{
		ResumeLinkQuietly();
		return false;
	}), 6.0f);

	// ---- BF6Script / BF6Blocks ----
	// Both editors pause their pushes and journal locally while the site has
	// signed us out, and re-apply what they journaled once it is back. Wired
	// here, once, on the profile's own delegates; both calls are safe to repeat.
	OnSessionLost().AddLambda([]
	{
		BF6Script::NotifySessionLost(TEXT("the site signed us out"));
		BF6Blocks::NoteSessionLost();
	});
	OnSessionRestored().AddLambda([]
	{
		BF6Script::NotifySessionRestored();
		BF6Blocks::NoteSessionRestored();
		// A sync that was deferred because the session had gone is picked up
		// here, and it is announced rather than done quietly.
		if (GAutoSync && IsUuid(GPendingSyncId))
		{
			const FString Id = GPendingSyncId;
			BF6Api::Toast(TEXT("Portal is back. Bringing your experience back in step."));
			RequestSync(Id, TEXT("the session came back and a save was waiting"));
		}
	});
	// ---- end BF6Script / BF6Blocks ----
	LoadConfig();
	IFileManager::Get().MakeDirectory(*PortalRoot(), true);
	ReadCacheAll();

	GBridge.Reset(NewObject<UBF6PortalBridge>(GetTransientPackage(), FName(TEXT("BF6PortalBridge"))));
	BF6PortalWeb::RegisterBridgeObject(kBridgeName, GBridge.Get());

	// The page half, off disk, so it can be iterated on with BF6.Portal.Inject
	// without a recompile.
	const FString JsPath = FPaths::Combine(g_pluginDir, TEXT("Resources"), TEXT("portal"), TEXT("capture.js"));
	FString Js;
	if (FFileHelper::LoadFileToString(Js, *JsPath))
	{
		BF6PortalWeb::RegisterInjectedScript(kScriptId, Js);
		UE_LOG(LogBF6Portal, Display, TEXT("Portal capture script injected from %s (%d chars)"), *JsPath, Js.Len());
	}
	else
	{
		UE_LOG(LogBF6Portal, Error, TEXT("Portal capture script missing: %s. The profile cannot read the site."), *JsPath);
	}

	GUrlHandle = BF6PortalWeb::OnUrlChanged().AddLambda([](const FString& Url)
	{
		GLastUrl = Url;
		// A navigation replaces the document and the observers with it, so a
		// watch that is on is re-armed on the new one rather than silently
		// stopping. The page half is idempotent.
		if (GWatch) BF6PortalWeb::Exec(TEXT("try { window.BF6PortalCapture.setWatch(true); } catch (e) {}"));
		// The probe reports on its own timer too; this makes a router-only move
		// answer immediately rather than up to half a second later.
		BF6PortalWeb::Exec(TEXT("try { if (window.BF6PortalCapture) window.BF6PortalCapture.report(); } catch (e) {}"));
	});

	GTick = FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda([](float) -> bool
	{
		const double Now = FPlatformTime::Seconds();
		if (GExpect.bActive && Now > GExpect.Deadline)
		{
			GPageStatus = FString::Printf(TEXT("expected %s got %s"), *GExpect.Key,
				GLastUrl.IsEmpty() ? TEXT("nothing") : *GLastUrl);
			UE_LOG(LogBF6Portal, Warning, TEXT("Portal page check timed out: expected %s got %s"),
				*GExpect.Key, GLastUrl.IsEmpty() ? TEXT("nothing") : *GLastUrl);
			ResolveExpect(false, GPageStatus);
			Bump();
		}
		// A chunked body whose middle never arrived must not sit in memory.
		for (TMap<FString, FChunkBuf>::TIterator It(GChunks); It; ++It)
			if (Now - It.Value().Started > 60.0) It.RemoveCurrent();

		// ---- the fifth way a session goes: the page stops answering ---------
		//
		// SILENCE IS NOT A SIGN OUT. An import drives the page hard - eleven
		// map reads, a replayed request per map, a navigation between each -
		// and the probe goes quiet while it works. Reading that as "Portal
		// signed you out" told the user they had been signed out in the middle
		// of an import that was in fact succeeding, which is worse than saying
		// nothing at all.
		//
		// Two things have to be true now. The tool must not be the reason the
		// page is busy, and the quiet must last long enough to mean something
		// on an idle page. A page that really has signed us out also lands on
		// the login address, and that is caught elsewhere, immediately, with
		// evidence rather than by inference from a timer.
		// An import job running, or anything else that has just asked the page
		// to do something and is still waiting for it.
		const bool bToolIsWorking = GJob.bActive || GExpect.bActive || GBusyUntil > Now;
		const double QuietFor = Now - GLastProbeAt;
		if (!GLost && !bToolIsWorking && GLastProbeAt > 0.0 && QuietFor > 45.0 && !GLastUrl.IsEmpty())
		{
			// SILENCE IS NOT EVIDENCE. ASK.
			//
			// The page's heartbeat is a setInterval, and this panel runs
			// offscreen, which is exactly the state browsers throttle timers in:
			// a hidden page's five second interval becomes one a minute, and
			// after a few minutes hidden it can be slower still. Forty-five
			// seconds of quiet then "proved" a sign-out on a page that was
			// perfectly signed in, three minutes after it was opened, over and
			// over.
			//
			// So the tool asks the page directly. An Exec is not a timer and is
			// not throttled: a living page answers within a moment. Only a page
			// that stays silent AFTER being asked is treated as gone, and a real
			// sign-out is still caught immediately and with evidence when the
			// address lands on the login screen.
			if (GPokeSentAt <= GLastProbeAt)
			{
				GPokeSentAt = Now;
				UE_LOG(LogBF6Portal, Verbose,
					TEXT("Portal: the page has been quiet for %d s; asking it directly before concluding anything."),
					(int32)QuietFor);
				BF6PortalWeb::Exec(TEXT("try{window.BF6PortalCapture&&window.BF6PortalCapture.beat&&window.BF6PortalCapture.beat()}catch(e){}"));
			}
			else if (Now - GPokeSentAt > 20.0)
			{
				GLastProbeAt = Now;   // one report, not one every quarter second
				GPokeSentAt = 0.0;
				NoteSessionLost(FString::Printf(
					TEXT("the page stopped answering for %d seconds and did not reply when asked directly"),
					(int32)QuietFor));
			}
		}

		// ---- recovery: reload, twice, ten seconds apart ---------------------
		if (GLost && GRecovery == ERecovery::Reloading && Now >= GNextRetryAt)
		{
			if (GRecoveryTries >= 2)
			{
				if (Now > GRecoveryDeadline)
				{
					GRecovery = ERecovery::WaitingForUser;
					GRecoveryResult = TEXT("two reloads did not sign in again, waiting for the user");
					GBanner = TEXT("Portal signed you out. Nothing is lost: your blocks, script and saves are kept in the tool. Sign in on the page to continue.");
					UE_LOG(LogBF6Portal, Warning, TEXT("Portal session recovery: %s"), *GRecoveryResult);
					Bump();
				}
			}
			else
			{
				GRecoveryTries++;
				GNextRetryAt = Now + 10.0;
				GRecoveryDeadline = Now + 10.0;
				GRecoveryResult = FString::Printf(TEXT("reload attempt %d of 2"), GRecoveryTries);
				// NO CREDENTIALS ANYWHERE IN THIS. The browser's own persistent
				// cookie store usually still carries the EA session, so simply
				// asking for the page again is the whole recovery.
				UE_LOG(LogBF6Portal, Display, TEXT("Portal session recovery: %s, reloading %s"), *GRecoveryResult, *GReturnUrl);
				BF6PortalWeb::OpenQuiet(GReturnUrl);
				GLastProbeAt = Now;   // a reloading page is allowed to be quiet
				Bump();
			}
		}

		// ---- keep-alive -----------------------------------------------------
		//
		// A BROWSER TAB DOES NOT STOP REFRESHING BECAUSE YOU STOPPED CLICKING.
		// The site holds a signed-in session for hours on a page nobody touches,
		// because its own timer keeps the token fresh. This used to stop pinging
		// after FIVE MINUTES without a Slate interaction - so reading the site
		// in a real browser, alt-tabbing to another app, or simply thinking, all
		// silently ended the session the tool was keeping alive. That is why
		// signing in lasted hours in a browser and far less here.
		//
		// It also required the web panel to be up or a save to be open, and the
		// panel is deliberately sign-in-only, so most of the time the whole
		// thing hung on having a save open.
		//
		// Now it pings for as long as the editor is up and the profile is
		// linked, and gives up only after a genuinely long idle - the original
		// intent of "a tool left open overnight pings nothing", at a scale that
		// matches how long the site's own session lasts.
		if (GKeepAlive && !GLost && GState == EState::Linked && !GLastUrl.IsEmpty()
			&& (Now - GLastKeepAliveAt) > kKeepAliveEverySecs)
		{
			const double IdleFor = FSlateApplication::IsInitialized()
				? (FPlatformTime::Seconds() - FSlateApplication::Get().GetLastUserInteractionTime())
				: 0.0;
			if (IdleFor < kKeepAliveGiveUpSecs)
			{
				GLastKeepAliveAt = Now;
				BF6PortalWeb::Exec(TEXT("try { if (window.BF6PortalCapture) window.BF6PortalCapture.keepAlive(); } catch (e) {}"));
			}
			else if (!GKeepAliveGaveUpSaid)
			{
				// Said once, so a session that lapses after a long absence is
				// something you can find in the log rather than a mystery.
				GKeepAliveGaveUpSaid = true;
				UE_LOG(LogBF6Portal, Display,
					TEXT("Portal keep-alive stopped after %.0f minutes idle. Touch the editor to resume it."),
					IdleFor / 60.0);
			}
		}
		else if (GKeepAliveGaveUpSaid && FSlateApplication::IsInitialized()
			&& (FPlatformTime::Seconds() - FSlateApplication::Get().GetLastUserInteractionTime()) < 60.0)
		{
			GKeepAliveGaveUpSaid = false;
		}

		// ---- a push waiting for the page to answer --------------------------
		// Probed, not assumed. The exec throws harmlessly inside the page while
		// the capture script is not there yet, so the answer only ever comes
		// from a document that can actually receive the transfer.
		if (GPushWait.bActive)
		{
			if (Now > GPushWait.Deadline)
			{
				const FString Id = GPushWait.Id, Text = GPushWait.Text;
				PushFallbackToFile(Id, Text,
					TEXT("The experience page never answered, so nothing was sent."));
			}
			else if (Now >= GPushWait.NextProbeAt)
			{
				GPushWait.NextProbeAt = Now + 0.5;
				BF6PortalWeb::Exec(FString::Printf(
					TEXT("try { window.BF6PortalCapture.pushReady('%s'); } catch (e) {}"), *GPushWait.Token));
			}
		}
		// A push handed over but never answered must not hold auto-sync down
		// for the rest of the session.
		else if (GPushInFlight && GPushStartedAt > 0.0 && (Now - GPushStartedAt) > 120.0)
		{
			EndPush();
			GPushStatus = TEXT("The site never said what it did with the import. Check the experience on the site before pushing again.");
			UE_LOG(LogBF6Portal, Warning, TEXT("Portal push: %s"), *GPushStatus);
			Bump();
		}

		// ---- the tool saved a map that belongs to an experience -------------
		// Noticed by the session file's timestamp rather than by a hook in the
		// save path, so this file owns the whole behaviour and nothing else has
		// to know it exists. One line in the save path would make it immediate:
		//     BF6PortalProfile::NoteToolSaved(Level, Save);
		// The scan keeps running while a run is suspended, so the saves the run
		// makes become the new baseline; only the notification is held back.
		if (GAutoSync && (Now - GLastSaveScanAt) > 2.0)
		{
			GLastSaveScanAt = Now;
			const FString Lv = BF6Api::CurrentLevel();
			const FString Sv = BF6Api::CurrentSave();
			if (!Lv.IsEmpty() && !Sv.IsEmpty())
			{
				const FString Path = ToolSessionPath(Lv, Sv);
				if (!Path.IsEmpty())
				{
					const FDateTime Stamp = IFileManager::Get().GetTimeStamp(*Path);
					const FString Key = Lv + TEXT("|") + Sv;
					if (FDateTime* Was = GSaveStamps.Find(Key))
					{
						if (Stamp > *Was)
						{
							*Was = Stamp;
							// A save made while the tool is writing is the
							// tool's own. The stamp is taken as the new
							// baseline and nothing is fired.
							if (!SyncSuspended()) BF6PortalProfile::NoteToolSaved(Lv, Sv);
						}
					}
					else
					{
						GSaveStamps.Add(Key, Stamp);   // first sight is not a save
					}
				}
			}
		}

		TickJob();
		return true;
	}), 0.25f);

	IConsoleManager& CM = IConsoleManager::Get();
	GCmds.Add(CM.RegisterConsoleCommand(TEXT("BF6.Portal.Profile"),
		TEXT("Print the Portal profile to the Output Log: state, account name as the page showed it, page check, experiences."),
		FConsoleCommandDelegate::CreateLambda([]{ BF6PortalProfile::LogStatus(); })));
	GCmds.Add(CM.RegisterConsoleCommand(TEXT("BF6.Portal.Unlink"),
		TEXT("Clear the saved Portal session and forget the profile."),
		FConsoleCommandDelegate::CreateLambda([]{ BF6PortalProfile::Unlink(); })));
	GCmds.Add(CM.RegisterConsoleCommand(TEXT("BF6.Portal.ImportAll"),
		TEXT("Import every experience in your Portal list as custom maps, one save per map in each rotation."),
		FConsoleCommandDelegate::CreateLambda([]{ BF6PortalProfile::ImportAll(); })));
	GCmds.Add(CM.RegisterConsoleCommand(TEXT("BF6.Portal.Import"),
		TEXT("Import one Portal experience: BF6.Portal.Import <name, short id or uuid>. Says what it matched."),
		FConsoleCommandWithArgsDelegate::CreateLambda([](const TArray<FString>& A)
		{
			if (A.Num() < 1) { UE_LOG(LogBF6Portal, Warning, TEXT("BF6.Portal.Import <name, short id or uuid>")); return; }
			FString What;
			const FString Id = BF6PortalProfile::Resolve(FString::Join(A, TEXT(" ")), What);
			if (Id.IsEmpty()) { UE_LOG(LogBF6Portal, Warning, TEXT("BF6.Portal.Import: %s"), *What); return; }
			UE_LOG(LogBF6Portal, Display, TEXT("BF6.Portal.Import: matched %s"), *What);
			BF6PortalProfile::ImportOne(Id);
		})));
	GCmds.Add(CM.RegisterConsoleCommand(TEXT("BF6.Portal.Find"),
		TEXT("Search your Portal experiences by name, short id or map codename and print the matches with their uuids: BF6.Portal.Find <text>"),
		FConsoleCommandWithArgsDelegate::CreateLambda([](const TArray<FString>& A)
		{
			BF6PortalProfile::LogFind(FString::Join(A, TEXT(" ")));
		})));
	GCmds.Add(CM.RegisterConsoleCommand(TEXT("BF6.Portal.Refresh"),
		TEXT("Ask the site for your experiences list again, without leaving the page you are on."),
		FConsoleCommandDelegate::CreateLambda([]{ BF6PortalProfile::RefreshList(); })));
	GCmds.Add(CM.RegisterConsoleCommand(TEXT("BF6.Portal.ImportFile"),
		TEXT("Read a whole Portal experience export off disk, with no site and no network: BF6.Portal.ImportFile <path>. No path opens a file picker."),
		FConsoleCommandWithArgsDelegate::CreateLambda([](const TArray<FString>& A)
		{
			BF6PortalProfile::ImportExperienceFile(FString::Join(A, TEXT(" ")));
		})));
	GCmds.Add(CM.RegisterConsoleCommand(TEXT("BF6.Portal.ExportFile"),
		TEXT("Write the open experience out in the site's own export format: BF6.Portal.ExportFile <path>. No path opens a save dialog."),
		FConsoleCommandWithArgsDelegate::CreateLambda([](const TArray<FString>& A)
		{
			BF6PortalProfile::ExportExperienceFile(FString::Join(A, TEXT(" ")));
		})));
	GCmds.Add(CM.RegisterConsoleCommand(TEXT("BF6.Portal.ExportFromSite"),
		TEXT("Have the site export the experience through its own tile menu and read the result straight into the tool, without it touching your downloads folder: BF6.Portal.ExportFromSite [name or uuid]"),
		FConsoleCommandWithArgsDelegate::CreateLambda([](const TArray<FString>& A)
		{
			FString Id;
			if (A.Num() > 0)
			{
				FString What;
				Id = BF6PortalProfile::Resolve(FString::Join(A, TEXT(" ")), What);
				if (Id.IsEmpty()) { UE_LOG(LogBF6Portal, Warning, TEXT("BF6.Portal.ExportFromSite: %s"), *What); return; }
				UE_LOG(LogBF6Portal, Display, TEXT("BF6.Portal.ExportFromSite: matched %s"), *What);
			}
			BF6PortalProfile::ExportFromSite(Id);
		})));
	GCmds.Add(CM.RegisterConsoleCommand(TEXT("BF6.Portal.Notices"),
		TEXT("What the Portal page is warning about, per experience: BF6.Portal.Notices [clear]. ")
		TEXT("They are cleared automatically when an experience is imported again, because a warning ")
		TEXT("about the version that was on the site is not a warning about the one that just came down."),
		FConsoleCommandWithArgsDelegate::CreateLambda([](const TArray<FString>& A)
		{
			const bool bClear = A.Num() > 0 && A[0].ToLower() == TEXT("clear");
			int32 Shown = 0;
			for (FExp& E : GExps)
			{
				if (E.Notices.Num() == 0) { continue; }
				Shown++;
				if (bClear)
				{
					UE_LOG(LogBF6Portal, Display, TEXT("Cleared %d warning(s) on '%s'."),
						E.Notices.Num(), *E.Name);
					E.Notices.Reset();
					E.NoticesAt = FDateTime(0);
					continue;
				}
				UE_LOG(LogBF6Portal, Display, TEXT("'%s' (%s), seen %s:"),
					*E.Name, *E.Id.Left(8), *E.NoticesAt.ToString());
				for (const FString& N : E.Notices)
				{
					UE_LOG(LogBF6Portal, Display, TEXT("   %s"), *N);
				}
			}
			if (Shown == 0)
			{
				UE_LOG(LogBF6Portal, Display,
					TEXT("The Portal page has not warned about anything. Open an experience on the ")
					TEXT("panel to let it report."));
			}
			if (bClear) { Bump(); }
		})));
	GCmds.Add(CM.RegisterConsoleCommand(TEXT("BF6.Portal.ToTemplate"),
		TEXT("Give an experience the scripting template, converting its block workspace into ")
		TEXT("TypeScript under src/ when it has one: BF6.Portal.ToTemplate [name or uuid]. ")
		TEXT("No name uses the open experience. Import does this on its own; this is for ")
		TEXT("an experience imported before it did, or to redo a conversion."),
		FConsoleCommandWithArgsDelegate::CreateLambda([](const TArray<FString>& A)
		{
			FString Id;
			if (A.Num() > 0)
			{
				FString What;
				Id = BF6PortalProfile::Resolve(FString::Join(A, TEXT(" ")), What);
				if (Id.IsEmpty())
				{
					UE_LOG(LogBF6Portal, Warning, TEXT("BF6.Portal.ToTemplate: %s"), *What);
					return;
				}
				UE_LOG(LogBF6Portal, Display, TEXT("BF6.Portal.ToTemplate: matched %s"), *What);
			}
			else
			{
				Id = CurrentExperienceId();
			}
			if (Id.IsEmpty())
			{
				UE_LOG(LogBF6Portal, Warning,
					TEXT("BF6.Portal.ToTemplate: no experience is open, so name one."));
				return;
			}
			const FExp* E = Find(Id);
			if (!E)
			{
				UE_LOG(LogBF6Portal, Warning,
					TEXT("BF6.Portal.ToTemplate: %s has not been imported."), *Id.Left(8));
				return;
			}
			const FString Blocks = FPaths::Combine(ExpDir(Id), TEXT("rules.blocks.json"));
			FString Why, Dir;
			const bool bOk = FPaths::FileExists(Blocks)
				? BF6Script::ConvertBlocksToTemplate(Id, E->Name, Blocks, Why)
				: BF6Script::EnsureTemplateProject(Id, E->Name, Dir, Why);
			if (!bOk)
			{
				UE_LOG(LogBF6Portal, Warning, TEXT("BF6.Portal.ToTemplate: %s"), *Why);
			}
		})));
	GCmds.Add(CM.RegisterConsoleCommand(TEXT("BF6.Portal.Verify"),
		TEXT("Compare what the tool would write against an export the site produced, field by field: BF6.Portal.Verify <path to an experience json>"),
		FConsoleCommandWithArgsDelegate::CreateLambda([](const TArray<FString>& A)
		{
			if (A.Num() < 1) { UE_LOG(LogBF6Portal, Warning, TEXT("BF6.Portal.Verify <path to an experience json>")); return; }
			BF6PortalProfile::VerifyAgainstFile(FString::Join(A, TEXT(" ")).TrimQuotes());
		})));
	GCmds.Add(CM.RegisterConsoleCommand(TEXT("BF6.Portal.Push"),
		TEXT("Send the whole experience back through the site's own import inside the experience, which replaces that experience's contents. Never the front page import, which would make a new one, and never a message composed from scratch."),
		FConsoleCommandDelegate::CreateLambda([]{ BF6PortalProfile::PushExperience(); })));
	GCmds.Add(CM.RegisterConsoleCommand(TEXT("BF6.Portal.Watch"),
		TEXT("Follow what you change on the site, live: BF6.Portal.Watch 0 | 1. Off by default. It watches the settings controls on whatever page the panel is on and the site's script editor, and it only ever observes: it never clicks, sets a value or sends a request."),
		FConsoleCommandWithArgsDelegate::CreateLambda([](const TArray<FString>& A)
		{
			if (A.Num() == 0) { UE_LOG(LogBF6Portal, Display, TEXT("%s"), *BF6PortalProfile::WatchStatus()); return; }
			BF6PortalProfile::SetWatchEnabled(A[0].ToLower() != TEXT("off") && A[0] != TEXT("0"));
			UE_LOG(LogBF6Portal, Display, TEXT("%s"), *BF6PortalProfile::WatchStatus());
		})));
	GCmds.Add(CM.RegisterConsoleCommand(TEXT("BF6.Portal.Watch.Adopt"),
		TEXT("Take the script the site last showed into the tool's own script project. Watching only journals it; this is the step that overwrites what the tool has."),
		FConsoleCommandDelegate::CreateLambda([]{ BF6PortalProfile::AdoptWatchedScript(); })));
	GCmds.Add(CM.RegisterConsoleCommand(TEXT("BF6.Portal.Controls"),
		TEXT("Print every button, menu item and file input on the page the panel is on, so the site's import control can be pinned down."),
		FConsoleCommandDelegate::CreateLambda([]
		{
			BF6PortalWeb::Exec(TEXT(
				"try { window.ue.bf6portal.pagestate(JSON.stringify({v:2,kind:'other',url:location.href,"
				"importState:'controls',importDetail:window.BF6PortalCapture.describeControls()})); } catch (e) {}"));
		})));
	GCmds.Add(CM.RegisterConsoleCommand(TEXT("BF6.Portal.AutoSync"),
		TEXT("Keep the tool and the site in step after a save: BF6.Portal.AutoSync 0 | 1. On by default; it stands down while you are signed out and picks up again when you are back."),
		FConsoleCommandWithArgsDelegate::CreateLambda([](const TArray<FString>& A)
		{
			if (A.Num() == 0) { UE_LOG(LogBF6Portal, Display, TEXT("Portal auto-sync is %s."), BF6PortalProfile::AutoSyncEnabled() ? TEXT("on") : TEXT("off")); return; }
			BF6PortalProfile::SetAutoSyncEnabled(A[0].ToLower() != TEXT("off") && A[0] != TEXT("0"));
		})));
	GCmds.Add(CM.RegisterConsoleCommand(TEXT("BF6.Portal.Session"),
		TEXT("Print the Portal session to the Output Log: held or lost, why it was lost, what the last recovery did, and whether the keep-alive is on."),
		FConsoleCommandDelegate::CreateLambda([]{ BF6PortalProfile::LogSession(); })));
	GCmds.Add(CM.RegisterConsoleCommand(TEXT("BF6.Portal.Session.Simulate"),
		TEXT("Pretend the Portal session was lost or restored, so the editors' journaling can be tested: BF6.Portal.Session.Simulate lost | restored"),
		FConsoleCommandWithArgsDelegate::CreateLambda([](const TArray<FString>& A)
		{
			const FString What = A.Num() ? A[0].ToLower() : FString();
			if (What == TEXT("lost"))          { BF6PortalProfile::SimulateSession(true);  return; }
			if (What == TEXT("restored"))      { BF6PortalProfile::SimulateSession(false); return; }
			UE_LOG(LogBF6Portal, Warning, TEXT("BF6.Portal.Session.Simulate lost | restored"));
		})));
	GCmds.Add(CM.RegisterConsoleCommand(TEXT("BF6.Portal.Thumbnail.Screenshot"),
		TEXT("Take the 3D view as the experience thumbnail, fitted and cropped to the site's 352 x 248 and under its 78 KB limit."),
		FConsoleCommandDelegate::CreateLambda([]
		{
			const bool bWasUp = BF6PortalWeb::IsShown();
			if (bWasUp) BF6PortalWeb::Hide();
			if (BF6PortalProfile::CaptureViewportForThumbnail()) BF6PortalProfile::BuildThumbnail();
			if (bWasUp) BF6PortalWeb::Open();
			BF6PortalProfile::LogThumbnail();
		})));
	GCmds.Add(CM.RegisterConsoleCommand(TEXT("BF6.Portal.Thumbnail.Upload"),
		TEXT("Use an image file as the experience thumbnail and send it to the site: BF6.Portal.Thumbnail.Upload <path to jpg, png or bmp>. No path opens a file picker."),
		FConsoleCommandWithArgsDelegate::CreateLambda([](const TArray<FString>& A)
		{
			const FString Path = FString::Join(A, TEXT(" ")).TrimQuotes();
			const bool bGot = Path.IsEmpty() ? BF6PortalProfile::PickThumbnailFile() : BF6PortalProfile::LoadImageForThumbnail(Path);
			if (!bGot) { BF6PortalProfile::LogThumbnail(); return; }
			BF6PortalProfile::BuildThumbnail();
			BF6PortalProfile::SendThumbnailToSite();
		})));
	GCmds.Add(CM.RegisterConsoleCommand(TEXT("BF6.Portal.Thumbnail.Status"),
		TEXT("Print the experience thumbnail's state: source, crop, encoded size and quality, the file on disk, the upload and what the site did with it."),
		FConsoleCommandDelegate::CreateLambda([]{ BF6PortalProfile::LogThumbnail(); })));
	GCmds.Add(CM.RegisterConsoleCommand(TEXT("BF6.Portal.KeepAlive"),
		TEXT("Turn the Portal keep-alive on or off: BF6.Portal.KeepAlive on | off. On by default; it pings the site every 4 minutes while you are working so it does not sign you out."),
		FConsoleCommandWithArgsDelegate::CreateLambda([](const TArray<FString>& A)
		{
			if (A.Num() == 0) { UE_LOG(LogBF6Portal, Display, TEXT("Portal keep-alive is %s."), BF6PortalProfile::KeepAliveEnabled() ? TEXT("on") : TEXT("off")); return; }
			BF6PortalProfile::SetKeepAliveEnabled(A[0].ToLower() != TEXT("off") && A[0] != TEXT("0"));
		})));

	UE_LOG(LogBF6Portal, Display, TEXT("Portal profile ready. State: %s. Keep-alive: %s. Cache: %s"),
		*StateLabel(), GKeepAlive ? TEXT("on") : TEXT("off"), *PortalRoot());
}

void BF6PortalProfile::Unregister()
{
	for (IConsoleObject* C : GCmds) if (C) IConsoleManager::Get().UnregisterConsoleObject(C);
	GCmds.Reset();
	if (GTick.IsValid()) { FTSTicker::GetCoreTicker().RemoveTicker(GTick); GTick.Reset(); }
	BF6PortalWeb::OnUrlChanged().Remove(GUrlHandle);
	BF6PortalWeb::UnregisterBridgeObject(kBridgeName);
	GBridge.Reset();
	GExpect.bActive = false;
	GExpect.Done = nullptr;
	// A subscriber holding a lambda into a module that is going away is a crash
	// waiting for the next page load.
	GOnLost.Clear();
	GOnRestored.Clear();
	GLost = false;
	GRecovery = ERecovery::Idle;
	GBanner.Reset();
	GJob = FImportJob();
	GChunks.Reset();
	GThumbs.Reset();
	GExps.Reset();
	GExpIdx.Reset();
}
