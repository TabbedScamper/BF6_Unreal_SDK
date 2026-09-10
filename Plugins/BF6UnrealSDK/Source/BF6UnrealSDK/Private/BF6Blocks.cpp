#include "BF6Blocks.h"
#include "BF6BlocksExport.h"
#include "BF6GameLog.h"   // the Portal log the game writes while a mod runs
#include "BF6BlocksBridge.h"
#include "BF6EditorOverlay.h"    // ---- BF6EditorOverlay ---- the full-screen host
#include "BF6PortalWeb.h"
#include "BF6PortalProfile.h"     // BF6_ImportSpatialFile, and the experience model
#include "BF6BuildMode.h"        // BF6Api: the ObjId registry and actor props
#include "BF6Project.h"        // the experience folder that owns the workspace
#include "BF6Script.h"        // TemplateDir: the one finder for the Portal template
#include "BF6SDKExtension.h"     // BF6Ext: selection, plugin and saved folders

#include "SWebBrowser.h"
#include "WebBrowserModule.h"

#include "DesktopPlatformModule.h"       // the native file dialogs for import and export
#include "IDesktopPlatform.h"
#include "Framework/Application/SlateApplication.h"
#include "HAL/PlatformApplicationMisc.h" // the clipboard, for the send-to-site fallback

#include "Containers/Ticker.h"
#include "Dom/JsonObject.h"
#include "Framework/Docking/TabManager.h"
#include "GameFramework/Actor.h"
#include "HAL/FileManager.h"
#include "HAL/IConsoleManager.h"
#include "Misc/Base64.h"
#include "Misc/ConfigCacheIni.h"
#include "UObject/Package.h"
#include "HAL/PlatformProcess.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/SecureHash.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
// The writer's default is the PRETTY policy, one tab per nesting level, and
// blocks nest hundreds deep. See the exportFile handler for what that cost.
#include "Policies/CondensedJsonPrintPolicy.h"
#include "Widgets/Docking/SDockTab.h"
#include "Editor.h"
#include "LevelEditorViewport.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/SNullWidget.h"
#include "Widgets/Text/STextBlock.h"
#include "WorkspaceMenuStructure.h"
#include "WorkspaceMenuStructureModule.h"
#if PLATFORM_WINDOWS
#include "Windows/WindowsHWrapper.h"
#endif

DEFINE_LOG_CATEGORY(LogBF6Blocks);

// ============================================================================
// State
// ============================================================================
namespace
{
	const FName kTabId(TEXT("BF6Blocks"));
	const TCHAR* kBridgeName = TEXT("bf6blocks");
	const int32  kChunk = 512 * 1024;          // one Exec beyond this is asking for trouble

	TSharedPtr<SWebBrowser> GBrowser;          // the tool's own editor page
	// ---- BF6EditorOverlay ----
	// The page outlives both of its homes. GHost is what the dock tab shows;
	// the page sits in it while docked and moves out of it while the
	// full-screen host has it, and is destroyed by neither.
	TSharedPtr<SBox>        GHost;
	TWeakPtr<SDockTab>      GTab;
	// ---- end BF6EditorOverlay ----
	TWeakObjectPtr<UBF6BlocksBridge> GBridge;
	TArray<IConsoleObject*> GCmds;
	FTSTicker::FDelegateHandle GTicker;
	FDelegateHandle GPageLoadedHandle;
	FDelegateHandle GMapOpenedHandle;
	TMap<FString, FString> GProjectRecoveryDirs;
	TFunction<void(bool)> GUpdateSaved;
	FString GUpdateSaveToken;
	FTSTicker::FDelegateHandle GUpdateSaveTimeout;

	bool    GToolReady = false;                // our page said hello
	FString GPendingLootBinding, GPendingLootSave, GPendingLootLevel;
	bool    GSiteReady = false;                // site_sync.js said hello
	bool    GWantWorkspace = false;            // pull as soon as the site is up
	int32   GSeqToSite = 0;
	int32   GSeqToTool = 0;
	int32   GFromSite = 0;
	int32   GFromTool = 0;
	FString GLastError;
	// The folder the last script import came from. A converted block records its
	// source path relative to that folder, so this is what turns "weapons.ts:412"
	// back into a file on disk.
	FString GLastImportDir;
	FString GSiteUrl;
	FString GDefsSource;                       // live | cache | none
	int32   GDefTypes = 0;
	FString GStyleSource;                      // live | cache | default | none
	FString GStyleSummary;                     // the one line the page reported
	FString GStyleAt;                          // when it was captured
	FString GSaveSelector;
	TArray<int32> GLastSceneSel;

	// Chunked messages arriving from a page, keyed by the page's chunk id.
	TMap<FString, TArray<FString>> GInbound;

	// ---- paths -------------------------------------------------------------
	FString BlocksResDir()
	{
		return FPaths::Combine(BF6Ext::ToolPluginDir(), TEXT("Resources"), TEXT("blocks"));
	}
	FString CacheDir()
	{
		return FPaths::Combine(BF6Ext::ToolSavedDir(), TEXT("portal"), TEXT("blockly"));
	}

	// THE SITE HALF, READ FROM DISK EVERY TIME IT IS NEEDED.
	//
	// This used to be built once inside Register(), which meant the script the
	// site page runs was whatever was on disk when the editor started. Editing
	// measure.js and capturing again then measured the site with the OLD tape,
	// silently: the capture succeeded, the numbers looked plausible, and they
	// described code that no longer exists. One capture came back with field
	// classes mangled by a bug that had already been fixed on disk.
	//
	// Reading the files here costs a hundred kilobytes off a warm disk once per
	// capture, which is nothing next to loading the page it is being injected
	// into, and it removes a whole class of wrong answers.
	void RefreshSiteScript()
	{
		FString SiteScript;
		const FString ScriptPath = FPaths::Combine(BlocksResDir(), TEXT("site_sync.js"));
		if (FFileHelper::LoadFileToString(SiteScript, *ScriptPath))
		{
			// ONE TAPE MEASURE, USED ON BOTH PAGES. measure.js is loaded by our own
			// editor through a script tag and prepended here for the site, so the
			// site and the editor are measured by the same code. Two measuring
			// routines written separately give two numbers that differ for reasons
			// nobody can pin down, and the difference that matters is then lost
			// among the ones that do not.
			FString Measure;
			if (FFileHelper::LoadFileToString(Measure, *FPaths::Combine(BlocksResDir(), TEXT("measure.js"))))
			{
				SiteScript = Measure + TEXT("\n") + SiteScript;
			}
			else
			{
				UE_LOG(LogBF6Blocks, Warning,
					TEXT("measure.js is missing, so the site's toolbox cannot be measured. Everything else still runs."));
			}
			BF6PortalWeb::RegisterInjectedScript(TEXT("bf6blocks"), SiteScript);
		}
		else
		{
			UE_LOG(LogBF6Blocks, Warning, TEXT("site_sync.js missing at %s: the site half will not run."), *ScriptPath);
		}
	}

	FString EditorPageUrl()
	{
		FString Path = FPaths::ConvertRelativePathToFull(
			FPaths::Combine(BlocksResDir(), TEXT("editor.html")));
		Path.ReplaceInline(TEXT("\\"), TEXT("/"));
		Path.ReplaceInline(TEXT(" "), TEXT("%20"));
		return TEXT("file:///") + Path;
	}

	bool    GSessionLost = false;
	bool    GReconcileNext = false;            // the next site workspace is a reconcile
	FString GLastPortalVerdict;
	FString GExperienceId;                     // the uuid in the site URL
	int32   GAutosaves = 0;
	FString GLastAutosave;

	// The experience a block page belongs to: ...?id=<uuid>&teams=...
	FString ExperienceFromUrl(const FString& Url)
	{
		int32 At = INDEX_NONE;
		if (!Url.FindLastChar(TEXT('?'), At)) return FString();
		const FString Query = Url.Mid(At + 1);
		TArray<FString> Pairs;
		Query.ParseIntoArray(Pairs, TEXT("&"), true);
		for (const FString& P : Pairs)
		{
			FString K, V;
			if (P.Split(TEXT("="), &K, &V) && K == TEXT("id")) return V;
		}
		return FString();
	}

	FString ExperienceDir()
	{
		const FString Id = GExperienceId.IsEmpty() ? TEXT("unlinked") : GExperienceId;
		return FPaths::Combine(BF6Ext::ToolSavedDir(), TEXT("portal"), TEXT("experiences"),
			Id, TEXT("blocks"));
	}

	// ---- JSON helpers ------------------------------------------------------
	TSharedPtr<FJsonObject> Parse(const FString& Text)
	{
		TSharedPtr<FJsonObject> Obj;
		TSharedRef<TJsonReader<>> R = TJsonReaderFactory<>::Create(Text);
		if (!FJsonSerializer::Deserialize(R, Obj) || !Obj.IsValid()) return nullptr;
		return Obj;
	}
	FString Write(const TSharedRef<FJsonObject>& Obj)
	{
		FString Out;
		TSharedRef<TJsonWriter<>> W = TJsonWriterFactory<>::Create(&Out);
		FJsonSerializer::Serialize(Obj, W);
		return Out;
	}

	// A JSON document, already text, going into a JavaScript string literal.
	FString EscapeForJs(const FString& In)
	{
		FString Out;
		Out.Reserve(In.Len() + 32);
		for (const TCHAR C : In)
		{
			switch (C)
			{
			case TEXT('\\'): Out += TEXT("\\\\"); break;
			case TEXT('\''): Out += TEXT("\\'");  break;
			case TEXT('"'):  Out += TEXT("\\\""); break;
			case TEXT('\n'): Out += TEXT("\\n");  break;
			case TEXT('\r'): Out += TEXT("\\r");  break;
			case TEXT('\t'): Out += TEXT("\\t");  break;
			default:
				// Control characters, and the two separators JavaScript treats as
				// line breaks inside a string literal.
				if (C < 0x20 || C == 0x2028 || C == 0x2029)
					Out += FString::Printf(TEXT("\\u%04x"), (int32)C);
				else Out.AppendChar(C);
			}
		}
		return Out;
	}

	// ---- pushing to a page -------------------------------------------------
	void ExecTool(const FString& Js)
	{
		if (GBrowser.IsValid()) GBrowser->ExecuteJavascript(Js);
	}

	void DeliverToPage(bool bTool, const FString& JsonText)
	{
		const TCHAR* Recv = bTool ? TEXT("window.BF6Blocks&&window.BF6Blocks.recv")
		                          : TEXT("window.BF6SiteSync&&window.BF6SiteSync.recv");
		// TIMED, BECAUSE THIS IS WHERE THE EDITOR STOPS.
		//
		// Loading a 199KB workspace freezes Unreal with nothing logged on
		// either side: the page never reports receiving it, so the message dies
		// somewhere between here and there. Three candidates - escaping,
		// formatting, and the ExecuteJavascript itself - and no way to tell
		// them apart without saying which one was entered. A step that logs its
		// start and never its end is the one that hung.
		//
		// Only large payloads are timed. Ordinary traffic is small and constant
		// and would drown the answer.
		auto Fire = [bTool, Recv](const FString& Payload)
		{
			const bool bBig = Payload.Len() > 50000;
			const double T0 = FPlatformTime::Seconds();
			if (bBig)
			{
				UE_LOG(LogBF6Blocks, Display,
					TEXT("deliver: escaping %d chars for the %s page"),
					Payload.Len(), bTool ? TEXT("editor") : TEXT("site"));
			}
			const FString Escaped = EscapeForJs(Payload);
			if (bBig)
			{
				UE_LOG(LogBF6Blocks, Display, TEXT("deliver: escaped to %d chars in %.0f ms, formatting"),
					Escaped.Len(), (FPlatformTime::Seconds() - T0) * 1000.0);
			}
			const FString Js = FString::Printf(TEXT("try{%s('%s');}catch(e){console.warn(e);}"),
				Recv, *Escaped);
			if (bBig)
			{
				UE_LOG(LogBF6Blocks, Display, TEXT("deliver: formatted %d chars in %.0f ms, running it"),
					Js.Len(), (FPlatformTime::Seconds() - T0) * 1000.0);
			}
			if (bTool) ExecTool(Js); else BF6PortalWeb::Exec(Js);
			if (bBig)
			{
				UE_LOG(LogBF6Blocks, Display, TEXT("deliver: handed to the browser, %.0f ms total"),
					(FPlatformTime::Seconds() - T0) * 1000.0);
			}
		};

		if (JsonText.Len() <= kChunk) { Fire(JsonText); return; }

		// Too big for one call: send it in pieces the page glues back together.
		static int32 ChunkSeq = 0;
		const FString Cid = FString::Printf(TEXT("cpp%d_%lld"), ++ChunkSeq, (long long)FDateTime::UtcNow().GetTicks());
		const int32 Total = FMath::DivideAndRoundUp(JsonText.Len(), kChunk);
		for (int32 i = 0; i < Total; i++)
		{
			TSharedRef<FJsonObject> Piece = MakeShared<FJsonObject>();
			Piece->SetStringField(TEXT("op"), TEXT("chunk"));
			Piece->SetStringField(TEXT("cid"), Cid);
			Piece->SetNumberField(TEXT("i"), i);
			Piece->SetNumberField(TEXT("n"), Total);
			Piece->SetStringField(TEXT("part"), JsonText.Mid(i * kChunk, kChunk));
			Fire(Write(Piece));
		}
	}

	void ToTool(const TSharedRef<FJsonObject>& Obj)
	{
		Obj->SetNumberField(TEXT("seq"), ++GSeqToTool);
		DeliverToPage(true, Write(Obj));
	}
	void ToSite(const TSharedRef<FJsonObject>& Obj)
	{
		Obj->SetNumberField(TEXT("seq"), ++GSeqToSite);
		DeliverToPage(false, Write(Obj));
	}
	void ToToolRaw(const FString& JsonText) { DeliverToPage(true, JsonText); }
	void ToSiteRaw(const FString& JsonText) { DeliverToPage(false, JsonText); }

	void ToolNote(const FString& Op, const FString& Key, const FString& Value)
	{
		TSharedRef<FJsonObject> M = MakeShared<FJsonObject>();
		M->SetStringField(TEXT("op"), Op);
		if (!Key.IsEmpty()) M->SetStringField(Key, Value);
		ToTool(M);
	}

	// ---- the cache ---------------------------------------------------------
	void WriteCacheField(const TSharedPtr<FJsonObject>& Msg, const TCHAR* Field, const TCHAR* File)
	{
		if (!Msg.IsValid() || !Msg->HasField(Field)) return;
		FString Text;
		TSharedRef<TJsonWriter<>> W = TJsonWriterFactory<>::Create(&Text);
		FJsonSerializer::Serialize(Msg->TryGetField(Field).ToSharedRef(), FString(), W);

		// AN EMPTY ANSWER NEVER REPLACES A FULL ONE.
		//
		// The mod catalogue is announced by the site's bundle exactly once, as
		// it loads, and the capture reads it by listening. A capture taken on a
		// page whose bundle was ALREADY running therefore hears nothing and
		// sends {} - a true report of what it heard, and a complete loss if it
		// is written over four megabytes of catalogue that was captured
		// properly. That is what a second capture did on 2026-09-06: it brought
		// back better readings for all 604 block types and destroyed the
		// catalogue in the same breath.
		//
		// So a field that came back empty leaves what is on disk alone. The
		// worst case is a stale file, which the next real capture replaces.
		// EMPTY MEANS EMPTY OF CONTENT, NOT EQUAL TO A PARTICULAR STRING.
		//
		// This compared against the two characters {} while the serializer writes
		// an open brace, a newline and a close brace. So the test never fired
		// once, and four megabytes of captured catalogue was replaced by an empty
		// object regardless. The guard existed, was reasoned about, was written
		// down in a comment, and did nothing at all: it cost the catalogue a
		// SECOND time on 2026-09-06, after being added to stop exactly that.
		//
		// Whitespace is dropped before the comparison now, and the size floor is
		// past anything an empty document can be.
		const FString Path = FPaths::Combine(CacheDir(), File);
		FString Bare;
		for (int32 i = 0; i < Text.Len() && Bare.Len() < 8; ++i)
		{
			if (!FChar::IsWhitespace(Text[i])) Bare.AppendChar(Text[i]);
		}
		const bool bEmpty = Bare.IsEmpty() || Bare == TEXT("{}") || Bare == TEXT("[]")
			|| Bare == TEXT("null");
		if (bEmpty && IFileManager::Get().FileSize(*Path) > 16)
		{
			UE_LOG(LogBF6Blocks, Display,
				TEXT("The site sent nothing for %s this time, so the copy already cached is kept."),
				Field);
			return;
		}
		FFileHelper::SaveStringToFile(Text, *Path,
			FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
	}

	void CacheCapture(const TSharedPtr<FJsonObject>& Msg)
	{
		IFileManager::Get().MakeDirectory(*CacheDir(), true);
		WriteCacheField(Msg, TEXT("definitions"), TEXT("definitions.json"));
		WriteCacheField(Msg, TEXT("synthesized"), TEXT("definitions_synth.json"));
		WriteCacheField(Msg, TEXT("toolbox"), TEXT("toolbox.json"));
		WriteCacheField(Msg, TEXT("theme"), TEXT("theme.json"));
		WriteCacheField(Msg, TEXT("options"), TEXT("options.json"));
		WriteCacheField(Msg, TEXT("tooltips"), TEXT("tooltips.json"));
		WriteCacheField(Msg, TEXT("icons"), TEXT("icons.json"));
		WriteCacheField(Msg, TEXT("categoryIcons"), TEXT("category_icons.json"));
		WriteCacheField(Msg, TEXT("helpUrls"), TEXT("help_urls.json"));
		WriteCacheField(Msg, TEXT("helpLinks"), TEXT("help_links.json"));
		WriteCacheField(Msg, TEXT("contextMenuIds"), TEXT("context_menu.json"));
		WriteCacheField(Msg, TEXT("examples"), TEXT("examples.json"));

		FString Renderer;
		const TSharedPtr<FJsonObject>* Opts = nullptr;
		if (Msg->TryGetObjectField(TEXT("options"), Opts) && Opts && (*Opts).IsValid())
		{
			(*Opts)->TryGetStringField(TEXT("renderer"), Renderer);
		}
		if (!Renderer.IsEmpty())
		{
			FFileHelper::SaveStringToFile(Renderer, *FPaths::Combine(CacheDir(), TEXT("renderer.txt")),
				FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
		}

		// Examples are saved as snippets so they show up beside our own.
		const TArray<TSharedPtr<FJsonValue>>* Examples = nullptr;
		if (Msg->TryGetArrayField(TEXT("examples"), Examples) && Examples && Examples->Num())
		{
			const FString Dir = FPaths::Combine(BlocksResDir(), TEXT("snippets"), TEXT("site"));
			IFileManager::Get().MakeDirectory(*Dir, true);
			int32 n = 0;
			for (const TSharedPtr<FJsonValue>& V : *Examples)
			{
				const TSharedPtr<FJsonObject> E = V->AsObject();
				if (!E.IsValid()) continue;
				FString Name;
				E->TryGetStringField(TEXT("name"), Name);
				FString Text;
				TSharedRef<TJsonWriter<>> W = TJsonWriterFactory<>::Create(&Text);
				FJsonSerializer::Serialize(E.ToSharedRef(), W);
				FFileHelper::SaveStringToFile(Text,
					*FPaths::Combine(Dir, FString::Printf(TEXT("site_%02d_%s.json"), ++n,
						*Name.Replace(TEXT(" "), TEXT("_")))),
					FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
			}
			UE_LOG(LogBF6Blocks, Display, TEXT("Captured %d example entries from the site toolbox."), n);
		}
		else
		{
			UE_LOG(LogBF6Blocks, Display, TEXT("The site toolbox carries no example or template entries."));
		}
	}

	FString ReadFileOr(const FString& Path, const TCHAR* Fallback)
	{
		FString Text;
		if (FFileHelper::LoadFileToString(Text, *Path)) return Text;
		return Fallback;
	}

	// Hand the tool page whatever was captured last session, so it looks right
	// before the site is even open.
	// THE DEFINITIONS THAT SHIP WITH THE TOOL.
	//
	// A first run has no capture, so the page fell back to types_fallback.js:
	// 629 types with their shapes and field names and nothing else. No colours,
	// no output checks, no real tooltips. The editor opened, worked, and did not
	// look like Portal, and the only way to fix it was to sign in and let a
	// capture happen. "Connect once before it looks right" is not a reasonable
	// thing to ask of somebody who has just installed the plugin.
	//
	// So the last good capture is baked into the plugin and used when there is
	// no local one. This is the same distilled block data the tool already ships
	// as types_fallback.js, just complete: block definitions the tool derived,
	// NOT a copy of EA's site mirror, which stays in Saved and ships to nobody.
	FString OfflineDefsDir()
	{
		return FPaths::Combine(BF6Ext::ToolPluginDir(), TEXT("Resources"), TEXT("blocks"), TEXT("offline"));
	}

	// Is this file actually JSON we can hand to the page? Emptiness was the only
	// test here, which let a half-written or truncated cache beat the shipped
	// copy and then get pasted into the payload as-is: the page received
	// malformed JSON and drew no blocks at all, with a good offline set sitting
	// unused on disk. Anything that does not parse is treated as damage.
	bool UsableJson(const FString& Text)
	{
		if (Text.IsEmpty()) { return false; }
		TSharedPtr<FJsonObject> O;
		const TSharedRef<TJsonReader<>> R = TJsonReaderFactory<>::Create(Text);
		return FJsonSerializer::Deserialize(R, O) && O.IsValid();
	}

	bool SendCachedDefinitions()
	{
		// The captured one wins: it is this user's own build of the site and it
		// is newer than anything we shipped. It only wins if it is readable.
		FString From = CacheDir();
		const TCHAR* Source = TEXT("cache");
		FString Defs = ReadFileOr(FPaths::Combine(From, TEXT("definitions.json")), TEXT(""));
		if (!UsableJson(Defs))
		{
			if (!Defs.IsEmpty())
			{
				// Move the damaged file aside rather than delete it, so the next
				// launch does not read it again and the original is still there
				// if it turns out to matter.
				const FString Bad = FPaths::Combine(From, TEXT("definitions.json"));
				const FString Aside = Bad + TEXT(".damaged");
				IFileManager::Get().Delete(*Aside, false, true, true);
				IFileManager::Get().Move(*Aside, *Bad, true, true);
				UE_LOG(LogBF6Blocks, Warning,
					TEXT("The cached block definitions could not be read, so the copy shipped ")
					TEXT("with the tool is being used instead. The damaged file was kept as %s. ")
					TEXT("Signing in to Portal once will rebuild the cache."), *Aside);
			}
			From = OfflineDefsDir();
			Source = TEXT("shipped");
			Defs = ReadFileOr(FPaths::Combine(From, TEXT("definitions.json")), TEXT(""));
		}
		if (!UsableJson(Defs))
		{
			UE_LOG(LogBF6Blocks, Warning,
				TEXT("No readable block definitions in either the cache or %s."), *OfflineDefsDir());
			return false;
		}
		FString Payload = FString::Printf(TEXT("{\"op\":\"defs\",\"source\":\"%s\",\"definitions\":"), Source) + Defs;
		// Each companion is checked on its own. One unreadable file used to make
		// the whole payload unparseable; now it is simply left out, and the
		// editor opens with everything that did survive.
		auto Add = [&Payload, &From](const TCHAR* Field, const FString& File)
		{
			const FString Text = ReadFileOr(FPaths::Combine(From, File), TEXT(""));
			if (Text.IsEmpty()) { return; }
			if (!UsableJson(Text))
			{
				UE_LOG(LogBF6Blocks, Warning,
					TEXT("Skipping %s: it is not readable JSON."), *FPaths::Combine(From, File));
				return;
			}
			Payload += FString::Printf(TEXT(",\"%s\":%s"), Field, *Text);
		};
		Add(TEXT("synthesized"), TEXT("definitions_synth.json"));
		Add(TEXT("toolbox"), TEXT("toolbox.json"));
		Add(TEXT("theme"), TEXT("theme.json"));
		Add(TEXT("options"), TEXT("options.json"));
		Add(TEXT("tooltips"), TEXT("tooltips.json"));
		Add(TEXT("icons"), TEXT("icons.json"));
		Add(TEXT("categoryIcons"), TEXT("category_icons.json"));
		Add(TEXT("helpUrls"), TEXT("help_urls.json"));
		Payload += TEXT("}");
		GDefsSource = Source;
		// The number the status line reports is the number of BLOCK types, and
		// they are in the synthesized file. Counting the catalogue's eight
		// sections instead is what made a full cache read "8 types cached".
		{
			const FString Synth = ReadFileOr(FPaths::Combine(From, TEXT("definitions_synth.json")), TEXT(""));
			TSharedPtr<FJsonObject> O;
			const TSharedRef<TJsonReader<>> R = TJsonReaderFactory<>::Create(Synth);
			if (!Synth.IsEmpty() && FJsonSerializer::Deserialize(R, O) && O.IsValid())
				GDefTypes = O->Values.Num();
		}
		ToToolRaw(Payload);
		return true;
	}

	// ---- the site's look ---------------------------------------------------
	// One file, because the four parts of it are only meaningful together: a
	// theme without the constants it was measured against is half a look.
	FString StyleCachePath() { return FPaths::Combine(CacheDir(), TEXT("style.json")); }

	// Written by the mirror of the site's public bundle. Loaded when nothing
	// has been captured from a signed-in page yet.
	//
	// TWO SHIPPED LEVELS, NOT ONE.
	//
	// site_style_default.json is the old floor: enough to draw something, and
	// visibly not Portal. offline/style.json is a real capture baked in at
	// release time by BF6.Blocks.BakeOffline, so a fresh install gets the look
	// somebody actually signed in and measured, rather than the approximation.
	// The site is public, so this is ours to ship; nothing from the game is.
	FString StyleDefaultPath()
	{
		const FString Baked = FPaths::Combine(OfflineDefsDir(), TEXT("style.json"));
		if (FPaths::FileExists(Baked)) { return Baked; }
		return FPaths::Combine(BlocksResDir(), TEXT("site_style_default.json"));
	}

	// THE PULL-OUT, MEASURED ON THE SITE RATHER THAN GUESSED AT.
	//
	// Everything else about the look is captured by NAME: css rules whose
	// selector says "blockly", constants the provider happens to hold. The
	// site's toolbox is not built out of Blockly's parts - it registers its own
	// PortalToolboxCategory and dresses the whole thing in its own class names -
	// so a name filter drops exactly the rules that make the pull-out a
	// pull-out and keeps only the ones that were already ours. The capture then
	// reports "nothing missing" while the flyout comes out the size of a
	// tooltip.
	bool AskSiteForToolbox()
	{
		if (!GSiteReady)
		{
			UE_LOG(LogBF6Blocks, Warning, TEXT(
				"Reading the site's toolbox needs its BLOCKS page open in the Portal panel."));
			return false;
		}
		TSharedRef<FJsonObject> M = MakeShared<FJsonObject>();
		M->SetStringField(TEXT("op"), TEXT("captureToolbox"));
		ToSite(M);
		return true;
	}

	// ========================================================================
	// The local mirror of the site's own public assets
	//
	// The style downloader keeps a copy of the Portal front end under
	// Saved/BF6UnrealSDK/portalstyle. Inside it, assets/blockly holds the very
	// files the site draws its toolbox with: one svg per category and per value
	// type, named exactly as the toolbox names them, plus the three little
	// pictures Blockly loads out of its media folder. The typefaces sit beside
	// them under fonts, and the site's own stylesheet says which file is which
	// family.
	//
	// The join is BY NAME and never by hash: a category's cssconfig.icon is
	// "toolbox-actions-ai" and the file is toolbox-actions-ai.svg. A site update
	// that rehashes its bundles does not move that.
	//
	// EA'S ART STAYS WHERE IT IS. Nothing is copied into the plugin. The files
	// are read from the mirror at runtime, inlined into the page's own style
	// payload, and the built set is cached under Saved beside the rest of the
	// block cache. Only derived facts ship.
	// ========================================================================
	FString GMirrorDir;                        // ...portalstyle/portal.battlefield.com/bf6/<build>
	FString GMirrorBuild;                      // the build id that mirror holds
	FString GIconSource = TEXT("none");        // live | mirror | fallback | none
	FString GIconSummary;
	FString GFontSummary;
	bool    GMirrorChecked = false;
	bool    GSaidNoMirror = false;
	bool    GSaidFontFallback = false;
	TSharedPtr<FJsonObject> GMirrorPack;       // the built set, kept for the session

	FString MirrorStyleRoot()
	{
		return FPaths::ConvertRelativePathToFull(
			FPaths::Combine(BF6Ext::ToolSavedDir(), TEXT("portalstyle")));
	}

	// The built set, cached so the svgs are read and encoded once per build.
	// Not icons.json: that name is already the live capture's block icon map.
	// THE PACK'S OWN FORMAT VERSION, not the site's build. Bump it whenever
	// the pack gains or loses anything it carries.
	//
	// The cache used to be trusted on the site build id alone, so a pack that
	// learned to carry a new image went on serving a cache written before that
	// image existed. Adding the zoom sprite sheet changed nothing on screen for
	// exactly that reason: the code was right and the answer came from a file
	// written hours earlier.
	static const int32 kMirrorPackVersion = 5;   // 5: portable fonts and media

	// Captured pack first, then the one baked in at release, so a fresh install
	// draws the category and value icons instead of falling back to nothing.
	// WHERE THE ICON PACK IS READ FROM, which is not where it is written to.
	//
	// Reading falls back to the copy shipped inside the plugin so a user who has
	// never been online still gets icons. Writing must never follow that
	// fallback: the first rebuild would then save over the packaged resource,
	// changing files the installer put there, and on a plugin installed
	// somewhere read-only it would simply fail and leave no cache at all. So
	// the two directions are separate functions rather than one path used both
	// ways.
	FString MirrorPackPath()
	{
		const FString Live = FPaths::Combine(CacheDir(), TEXT("mirror_icons.json"));
		if (FPaths::FileExists(Live)) { return Live; }
		return FPaths::Combine(OfflineDefsDir(), TEXT("mirror_icons.json"));
	}

	// Always the user's own cache under Saved. Never the shipped copy.
	FString MirrorPackWritePath()
	{
		return FPaths::Combine(CacheDir(), TEXT("mirror_icons.json"));
	}

	FString ToFileUrl(const FString& Path)
	{
		FString P = FPaths::ConvertRelativePathToFull(Path);
		P.ReplaceInline(TEXT("\\"), TEXT("/"));
		P.ReplaceInline(TEXT("%"), TEXT("%25"));
		P.ReplaceInline(TEXT(" "), TEXT("%20"));
		P.ReplaceInline(TEXT("#"), TEXT("%23"));
		P.ReplaceInline(TEXT("?"), TEXT("%3F"));
		return TEXT("file:///") + P;
	}

	// Which build the mirror holds. The manifest is the first word; a build
	// folder that actually carries the icons is the second, so a mirror whose
	// manifest was lost still works.
	bool FindMirror(FString& OutDir, FString& OutBuild)
	{
		const FString Bf6 = FPaths::Combine(MirrorStyleRoot(),
			TEXT("portal.battlefield.com"), TEXT("bf6"));
		if (!FPaths::DirectoryExists(Bf6)) return false;

		auto HasIcons = [](const FString& Dir)
		{
			return FPaths::DirectoryExists(
				FPaths::Combine(Dir, TEXT("assets"), TEXT("blockly"), TEXT("icons")));
		};

		FString Text;
		if (FFileHelper::LoadFileToString(Text,
			*FPaths::Combine(MirrorStyleRoot(), TEXT("mirror_manifest.json"))))
		{
			TSharedPtr<FJsonObject> M;
			TSharedRef<TJsonReader<>> R = TJsonReaderFactory<>::Create(Text);
			FString Build;
			if (FJsonSerializer::Deserialize(R, M) && M.IsValid() &&
				M->TryGetStringField(TEXT("build_id"), Build) && !Build.IsEmpty())
			{
				const FString Dir = FPaths::Combine(Bf6, Build);
				if (HasIcons(Dir)) { OutDir = Dir; OutBuild = Build; return true; }
			}
		}

		TArray<FString> Dirs;
		IFileManager::Get().FindFiles(Dirs, *(Bf6 / TEXT("*")), false, true);
		Dirs.Sort();
		for (int32 i = Dirs.Num() - 1; i >= 0; --i)
		{
			const FString Dir = FPaths::Combine(Bf6, Dirs[i]);
			if (HasIcons(Dir)) { OutDir = Dir; OutBuild = Dirs[i]; return true; }
		}
		return false;
	}

	// An svg inlined as a data URL. Percent encoded rather than base64: it stays
	// readable, and it is what the site's own bundle does with the same files.
	FString SvgDataUrl(const FString& Svg)
	{
		FTCHARToUTF8 Utf8(*Svg);
		const uint8* Bytes = reinterpret_cast<const uint8*>(Utf8.Get());
		const int32 Len = Utf8.Length();
		static const TCHAR* Hex = TEXT("0123456789ABCDEF");
		FString Out;
		Out.Reserve(Len * 2 + 32);
		Out += TEXT("data:image/svg+xml,");
		for (int32 i = 0; i < Len; ++i)
		{
			const uint8 C = Bytes[i];
			const bool bSafe =
				(C >= 'A' && C <= 'Z') || (C >= 'a' && C <= 'z') || (C >= '0' && C <= '9') ||
				C == '-' || C == '_' || C == '.' || C == '~' || C == '!' || C == '$' ||
				C == '&' || C == '*' || C == '+' || C == ',' || C == '/' || C == ':' ||
				C == ';' || C == '=' || C == '?' || C == '@' || C == '(' || C == ')';
			if (bSafe)
			{
				Out.AppendChar(static_cast<TCHAR>(C));
			}
			else
			{
				Out.AppendChar(TEXT('%'));
				Out.AppendChar(Hex[C >> 4]);
				Out.AppendChar(Hex[C & 0xF]);
			}
		}
		return Out;
	}

	// The faces the site declares, pointed at the mirrored files.
	//
	// The family names are not invented here: they are read out of the site's
	// own stylesheet, which declares each @font-face with the file it wants. The
	// urls in it are absolute site paths (/bf6/<build>/fonts/X), and each one is
	// rewritten to the mirrored file or dropped if that file is not there.
	//
	// THERE ARE NO SUBSTITUTE FACES, AND THERE SHOULD NOT BE.
	//
	// Two used to be declared here, each mapping a family the mirror does not
	// hold onto the nearest one it does, so that a rule asking for it 'still
	// draws something of the right weight instead of dropping to the system
	// sans'. Both were wrong, and both were measured rather than argued:
	//
	//   Purista-Semibold -> BF_TITLE_SEMI-BOLD. The site was believed to serve
	//   Purista from a Typekit kit we cannot mirror. The kit does load - the
	//   page ends with 46 faces, including seravek-web and oswald - and Purista
	//   is not among them. On the site 'Purista-Semibold, sans-serif' measures
	//   identical to plain 'sans-serif', 32.88 for Mod and 83.54 for
	//   Conditions. Ours resolved the name instead, so every header was drawn
	//   in a narrow title face 40% under the site's width.
	//
	//   BFHead-Light -> BFText-Light, wanted by ._blocklyHelp_ h1..h6. Same
	//   answer: the site declares no such face and 'BFHead-Light, sans-serif'
	//   measures exactly 'sans-serif' there, 31.13 and 104.95. BFText-Light is
	//   a different width again, 28.02 and 101.97, so ours would have drawn the
	//   help headings wrong - latently, because that face only loads once the
	//   help panel is opened, which is why nothing looked wrong until it was
	//   measured directly.
	//
	// THE RULE: a face declared under a name the page does not otherwise have
	// does not fill a gap, it hides one. The fallback the site lands on is the
	// answer, and the only way to know it is to measure the stack against its
	// own fallback on the site itself.


	FString FontFormatFor(const FString& File)
	{
		if (File.EndsWith(TEXT(".woff2"))) return TEXT("woff2");
		if (File.EndsWith(TEXT(".woff")))  return TEXT("woff");
		if (File.EndsWith(TEXT(".otf")))   return TEXT("opentype");
		return TEXT("truetype");
	}

	// All captured faces are inlined. A file page
	// is its own opaque origin and a font fetched across folders can be refused
	// without a word; a src list falls through to the next source when one
	// fails, so the inlined copy is the one that always lands.
	bool ShouldInlineFace(const FString& File)
	{
		return true;
	}

	FString BuildFontCss(const FString& MirrorDir, TArray<FString>& OutFamilies,
		TArray<FString>& OutMissing, TSharedRef<FJsonObject> OutAliases)
	{
		const FString FontDir = FPaths::Combine(MirrorDir, TEXT("fonts"));
		if (!FPaths::DirectoryExists(FontDir)) return FString();

		// The site's stylesheet: assets/index-<hash>.css, matched by prefix
		// because the hash moves with every build.
		FString SheetPath;
		{
			TArray<FString> Sheets;
			IFileManager::Get().FindFiles(Sheets,
				*(FPaths::Combine(MirrorDir, TEXT("assets")) / TEXT("index-*.css")), true, false);
			if (Sheets.Num()) SheetPath = FPaths::Combine(MirrorDir, TEXT("assets"), Sheets[0]);
		}
		FString Sheet;
		if (!SheetPath.IsEmpty()) FFileHelper::LoadFileToString(Sheet, *SheetPath);

		FString Css;
		TSet<FString> Seen;

		int32 At = 0;
		while (!Sheet.IsEmpty())
		{
			const int32 Start = Sheet.Find(TEXT("@font-face"), ESearchCase::IgnoreCase,
				ESearchDir::FromStart, At);
			if (Start == INDEX_NONE) break;
			const int32 Open = Sheet.Find(TEXT("{"), ESearchCase::CaseSensitive, ESearchDir::FromStart, Start);
			const int32 Close = Open == INDEX_NONE ? INDEX_NONE
				: Sheet.Find(TEXT("}"), ESearchCase::CaseSensitive, ESearchDir::FromStart, Open);
			if (Open == INDEX_NONE || Close == INDEX_NONE) break;
			const FString Body = Sheet.Mid(Open + 1, Close - Open - 1);
			At = Close + 1;

			FString Family;
			{
				int32 F = Body.Find(TEXT("font-family:"), ESearchCase::IgnoreCase);
				if (F == INDEX_NONE) continue;
				F += 12;
				int32 End = F;
				while (End < Body.Len() && Body[End] != TEXT(';')) ++End;
				Family = Body.Mid(F, End - F).TrimStartAndEnd()
					.Replace(TEXT("\""), TEXT("")).Replace(TEXT("'"), TEXT(""));
			}
			if (Family.IsEmpty() || Seen.Contains(Family)) continue;

			// Every url in the declaration, in the order the site wrote them.
			TArray<FString> Sources;
			int32 U = 0;
			while (true)
			{
				const int32 P = Body.Find(TEXT("url("), ESearchCase::IgnoreCase, ESearchDir::FromStart, U);
				if (P == INDEX_NONE) break;
				const int32 Q = Body.Find(TEXT(")"), ESearchCase::CaseSensitive, ESearchDir::FromStart, P);
				if (Q == INDEX_NONE) break;
				U = Q + 1;
				FString Ref = Body.Mid(P + 4, Q - P - 4).TrimStartAndEnd()
					.Replace(TEXT("\""), TEXT("")).Replace(TEXT("'"), TEXT(""));
				const FString File = FPaths::GetCleanFilename(Ref);
				const FString Full = FPaths::Combine(FontDir, File);
				if (File.IsEmpty() || !FPaths::FileExists(Full)) continue;

				const FString Fmt = FontFormatFor(File);
				if (ShouldInlineFace(File))
				{
					TArray<uint8> Bytes;
					if (FFileHelper::LoadFileToArray(Bytes, *Full) && Bytes.Num())
					{
						Sources.Add(FString::Printf(TEXT("url(\"data:font/%s;base64,%s\") format(\"%s\")"),
							*Fmt, *FBase64::Encode(Bytes), *Fmt));
					}
				}
			}

			Seen.Add(Family);
			if (!Sources.Num())
			{
				OutMissing.Add(Family);
				continue;
			}
			OutFamilies.Add(Family);
			Css += FString::Printf(TEXT("@font-face{font-family:\"%s\";src:%s;font-style:normal;font-display:swap}\n"),
				*Family, *FString::Join(Sources, TEXT(",")));
		}

		return Css;
	}

	// Read the mirror and build the set the page needs. Every svg under
	// assets/blockly/icons, the three block pictures beside them, and the faces.
	TSharedPtr<FJsonObject> BuildMirrorPack(const FString& MirrorDir, const FString& Build)
	{
		const FString IconDir = FPaths::Combine(MirrorDir, TEXT("assets"), TEXT("blockly"), TEXT("icons"));
		TArray<FString> Files;
		IFileManager::Get().FindFiles(Files, *(IconDir / TEXT("*.svg")), true, false);
		if (!Files.Num()) return nullptr;

		TSharedRef<FJsonObject> Pack = MakeShared<FJsonObject>();
		TSharedRef<FJsonObject> Icons = MakeShared<FJsonObject>();
		TSharedRef<FJsonObject> Types = MakeShared<FJsonObject>();
		int32 Skipped = 0;

		for (const FString& File : Files)
		{
			FString Svg;
			if (!FFileHelper::LoadFileToString(Svg, *FPaths::Combine(IconDir, File)) || Svg.IsEmpty())
			{
				++Skipped;
				continue;
			}
			const FString Key = FPaths::GetBaseFilename(File);
			const FString Url = SvgDataUrl(Svg);
			if (Key.StartsWith(TEXT("type-"))) Types->SetStringField(Key, Url);
			else Icons->SetStringField(Key, Url);
		}

		// The three the site's own blocks reference. Blockly loads them out of
		// its media folder, so the folder is handed over as well and the inlined
		// copies stand in wherever a file url is refused.
		TSharedRef<FJsonObject> Images = MakeShared<FJsonObject>();
		const FString BlocklyDir = FPaths::Combine(MirrorDir, TEXT("assets"), TEXT("blockly"));
		// sprites.png is the fourth and it was the one missing. Blockly draws
		// its zoom in, zoom out and show-everything buttons out of that single
		// sheet, so without it those three controls render as blank squares -
		// which is exactly what they were doing above the minimap. It is the
		// site's own sheet, so taking it makes those buttons the site's icons
		// rather than something drawn to look like them.
		static const TCHAR* kImages[] = {
			TEXT("1x1.png"), TEXT("quote0.png"), TEXT("quote1.png"), TEXT("sprites.png")
		};
		// The mirror first, then the plugin's own vendored Blockly media.
		//
		// The three quote and spacer images are the SITE's and only the mirror
		// has them. sprites.png is Blockly's own, identical in the 10.3.0
		// package we vendor, and it is shipped with the plugin so the zoom
		// controls have their icons on a machine that has never mirrored the
		// site at all. Whichever copy is found first wins, so a mirror that
		// does have it still takes precedence.
		const FString VendorMedia = FPaths::Combine(BlocksResDir(), TEXT("vendor"), TEXT("media"));
		for (const TCHAR* Name : kImages)
		{
			TArray<uint8> Bytes;
			if (!FFileHelper::LoadFileToArray(Bytes, *FPaths::Combine(BlocklyDir, Name)) || !Bytes.Num())
			{
				Bytes.Reset();
				FFileHelper::LoadFileToArray(Bytes, *FPaths::Combine(VendorMedia, Name));
			}
			if (Bytes.Num())
			{
				Images->SetStringField(Name, TEXT("data:image/png;base64,") + FBase64::Encode(Bytes));
			}
		}

		TArray<FString> Families, Missing;
		TSharedRef<FJsonObject> Aliases = MakeShared<FJsonObject>();
		const FString FontCss = BuildFontCss(MirrorDir, Families, Missing, Aliases);

		Pack->SetNumberField(TEXT("v"), kMirrorPackVersion);
		Pack->SetStringField(TEXT("buildId"), Build);
		Pack->SetStringField(TEXT("mirrorDir"), MirrorDir);
		Pack->SetStringField(TEXT("builtUtc"), FDateTime::UtcNow().ToIso8601());
		// Images below are data URLs; keep the pack portable between installs.
		Pack->SetObjectField(TEXT("categoryIcons"), Icons);
		Pack->SetObjectField(TEXT("valueTypeIcons"), Types);
		Pack->SetObjectField(TEXT("blockImages"), Images);
		Pack->SetStringField(TEXT("fontCss"), FontCss);
		Pack->SetObjectField(TEXT("fontAliases"), Aliases);
		{
			TArray<TSharedPtr<FJsonValue>> Fam, Miss;
			for (const FString& F : Families) Fam.Add(MakeShared<FJsonValueString>(F));
			for (const FString& F : Missing)  Miss.Add(MakeShared<FJsonValueString>(F));
			Pack->SetArrayField(TEXT("fontFamilies"), Fam);
			Pack->SetArrayField(TEXT("fontsMissing"), Miss);
		}
		if (Skipped)
		{
			UE_LOG(LogBF6Blocks, Warning, TEXT("%d icon file(s) in the mirror could not be read."), Skipped);
		}
		return Pack;
	}

	int32 CountField(const TSharedPtr<FJsonObject>& O, const TCHAR* Field)
	{
		const TSharedPtr<FJsonObject>* S = nullptr;
		return (O.IsValid() && O->TryGetObjectField(Field, S) && S && (*S).IsValid())
			? (*S)->Values.Num() : 0;
	}

	// The built set for this session: the cache when it was built from the same
	// build id, and a fresh build otherwise.
	TSharedPtr<FJsonObject> MirrorPack()
	{
		if (GMirrorChecked) return GMirrorPack;
		GMirrorChecked = true;

		if (!FindMirror(GMirrorDir, GMirrorBuild))
		{
			// A fresh installation has no downloaded mirror. Its shipped pack
			// is self-contained and must still supply the site's icons/fonts.
			FString PackText;
			if (FFileHelper::LoadFileToString(PackText,
				*FPaths::Combine(OfflineDefsDir(), TEXT("mirror_icons.json"))))
			{
				TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(PackText);
				TSharedPtr<FJsonObject> Pack;
				double Version = 0;
				if (FJsonSerializer::Deserialize(Reader, Pack) && Pack.IsValid() &&
					Pack->TryGetNumberField(TEXT("v"), Version) && Version == kMirrorPackVersion &&
					CountField(Pack, TEXT("categoryIcons")) > 0)
				{
					GMirrorPack = Pack;
					GMirrorPack->RemoveField(TEXT("mediaPath"));
					GFontSummary = TEXT("bundled Portal faces");
					UE_LOG(LogBF6Blocks, Display, TEXT("Using bundled Portal icons and fonts (no local site mirror)."));
					return GMirrorPack;
				}
			}
			// Nothing is changed and nothing is guessed: the tool draws with
			// what it already has until the mirror exists.
			if (!GSaidNoMirror)
			{
				GSaidNoMirror = true;
				UE_LOG(LogBF6Blocks, Display, TEXT(
					"No local mirror of the Portal site under %s: the editor uses the icons the "
					"offline default file carries. The style downloader fills the icons in."),
					*MirrorStyleRoot());
			}
			return nullptr;
		}

		FString Text;
		if (FFileHelper::LoadFileToString(Text, *MirrorPackPath()) && !Text.IsEmpty())
		{
			TSharedPtr<FJsonObject> Cached;
			TSharedRef<TJsonReader<>> R = TJsonReaderFactory<>::Create(Text);
			FString Build;
			if (FJsonSerializer::Deserialize(R, Cached) && Cached.IsValid() &&
				Cached->TryGetStringField(TEXT("buildId"), Build) && Build == GMirrorBuild &&
				(int32)Cached->GetNumberField(TEXT("v")) == kMirrorPackVersion &&
				CountField(Cached, TEXT("categoryIcons")) > 0)
			{
				GMirrorPack = Cached;
			}
		}

		if (!GMirrorPack.IsValid())
		{
			GMirrorPack = BuildMirrorPack(GMirrorDir, GMirrorBuild);
			if (GMirrorPack.IsValid())
			{
				IFileManager::Get().MakeDirectory(*CacheDir(), true);
				FString Out;
				TSharedRef<TJsonWriter<>> W = TJsonWriterFactory<>::Create(&Out);
				FJsonSerializer::Serialize(GMirrorPack.ToSharedRef(), W);
				// Reported rather than ignored: a cache that silently failed to
				// write means every launch pays to rebuild it, which looks like
				// the editor being slow for no reason.
				const FString To = MirrorPackWritePath();
				if (!FFileHelper::SaveStringToFile(Out, *To,
					FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
				{
					UE_LOG(LogBF6Blocks, Warning,
						TEXT("The block icons were rebuilt but could not be cached to %s. ")
						TEXT("They still work; they will just be rebuilt again next time."), *To);
				}
			}
		}

		if (!GMirrorPack.IsValid())
		{
			UE_LOG(LogBF6Blocks, Warning, TEXT(
				"The mirror at %s holds no readable block icons."), *GMirrorDir);
			return nullptr;
		}

		const int32 NIcons = CountField(GMirrorPack, TEXT("categoryIcons"));
		const int32 NTypes = CountField(GMirrorPack, TEXT("valueTypeIcons"));
		const int32 NImages = CountField(GMirrorPack, TEXT("blockImages"));
		const TArray<TSharedPtr<FJsonValue>>* Fam = nullptr;
		const TArray<TSharedPtr<FJsonValue>>* Miss = nullptr;
		GMirrorPack->TryGetArrayField(TEXT("fontFamilies"), Fam);
		GMirrorPack->TryGetArrayField(TEXT("fontsMissing"), Miss);
		const int32 NFonts = Fam ? Fam->Num() : 0;

		GFontSummary = FString::Printf(TEXT("%d face(s) from the mirror"), NFonts);
		UE_LOG(LogBF6Blocks, Display, TEXT(
			"Portal site mirror build %s: %d category icons, %d value type icons, %d block images, "
			"%d font face(s). Read from %s, nothing copied."),
			*GMirrorBuild, NIcons, NTypes, NImages, NFonts, *GMirrorDir);

		if (Miss && Miss->Num() && !GSaidFontFallback)
		{
			GSaidFontFallback = true;
			TArray<FString> Names;
			for (const TSharedPtr<FJsonValue>& V : *Miss)
			{
				FString N;
				if (V.IsValid() && V->TryGetString(N)) Names.Add(N);
			}
			const TSharedPtr<FJsonObject>* Al = nullptr;
			FString Pairs;
			if (GMirrorPack->TryGetObjectField(TEXT("fontAliases"), Al) && Al && (*Al).IsValid())
			{
				for (const TPair<FString, TSharedPtr<FJsonValue>>& P : (*Al)->Values)
				{
					FString To;
					if (P.Value.IsValid() && P.Value->TryGetString(To))
					{
						if (!Pairs.IsEmpty()) Pairs += TEXT(", ");
						Pairs += P.Key + TEXT(" drawn with ") + To;
					}
				}
			}
			UE_LOG(LogBF6Blocks, Display, TEXT(
				"Not every face the site uses can be mirrored: %s comes from a Typekit kit rather "
				"than from the site, so it is not in the mirror. %s. The block text itself is "
				"BFText, which is mirrored."),
				*FString::Join(Names, TEXT(", ")),
				Pairs.IsEmpty() ? TEXT("The nearest mirrored face stands in") : *Pairs);
		}
		return GMirrorPack;
	}

	// The offline default file's own copy of the icons, read once. This is the
	// last tier: recovered from the site's public bundle rather than from a
	// signed-in page or from the mirror, and it is what the tool falls back to
	// on a machine with no mirror at all.
	TSharedPtr<FJsonObject> GDefaultIcons;
	bool GDefaultIconsRead = false;

	TSharedPtr<FJsonObject> DefaultIcons()
	{
		if (GDefaultIconsRead) return GDefaultIcons;
		GDefaultIconsRead = true;
		FString Text;
		if (!FFileHelper::LoadFileToString(Text, *StyleDefaultPath()) || Text.IsEmpty()) return nullptr;
		TSharedPtr<FJsonObject> Obj;
		TSharedRef<TJsonReader<>> R = TJsonReaderFactory<>::Create(Text);
		if (!FJsonSerializer::Deserialize(R, Obj) || !Obj.IsValid()) return nullptr;
		GDefaultIcons = Obj;
		return GDefaultIcons;
	}

	// Fold the mirror into a style payload, and work out which tier each icon
	// came from. The page merges by the same rule, so both agree: a live capture
	// wins, the mirror fills whatever it missed, and the offline default is the
	// last word.
	void AddMirrorToStyle(const TSharedRef<FJsonObject>& Style)
	{
		const TSharedPtr<FJsonObject> Pack = MirrorPack();

		// What a signed-in capture already supplied. Only entries that draw
		// count: a capture records a row's colours in the same map.
		TSet<FString> Live;
		const TSharedPtr<FJsonObject>* Cap = nullptr;
		if (Style->TryGetObjectField(TEXT("categoryIcons"), Cap) && Cap && (*Cap).IsValid())
		{
			for (const TPair<FString, TSharedPtr<FJsonValue>>& P : (*Cap)->Values)
			{
				if (P.Key.StartsWith(TEXT("row:")) || P.Key.StartsWith(TEXT("name:"))) continue;
				const TSharedPtr<FJsonObject>* Rec = nullptr;
				FString AsText;
				bool bDraws = false;
				if (P.Value.IsValid() && P.Value->TryGetObject(Rec) && Rec && (*Rec).IsValid())
				{
					FString V;
					bDraws = ((*Rec)->TryGetStringField(TEXT("backgroundImage"), V) && !V.IsEmpty() && V != TEXT("none"))
						|| ((*Rec)->TryGetStringField(TEXT("maskImage"), V) && !V.IsEmpty() && V != TEXT("none"))
						|| ((*Rec)->TryGetStringField(TEXT("svg"), V) && !V.IsEmpty());
				}
				else if (P.Value.IsValid() && P.Value->TryGetString(AsText))
				{
					bDraws = !AsText.IsEmpty() && AsText != TEXT("none");
				}
				if (bDraws) Live.Add(P.Key.StartsWith(TEXT("class:")) ? P.Key.Mid(6) : P.Key);
			}
		}

		int32 NMirror = 0;
		if (Pack.IsValid())
		{
			Style->SetStringField(TEXT("mirrorBuild"), GMirrorBuild);
			const TSharedPtr<FJsonObject>* Sub = nullptr;
			if (Pack->TryGetObjectField(TEXT("categoryIcons"), Sub) && Sub && (*Sub).IsValid())
			{
				Style->SetObjectField(TEXT("mirrorIcons"), *Sub);
				for (const TPair<FString, TSharedPtr<FJsonValue>>& P : (*Sub)->Values)
				{
					if (!Live.Contains(P.Key)) ++NMirror;
				}
			}
			if (Pack->TryGetObjectField(TEXT("valueTypeIcons"), Sub) && Sub && (*Sub).IsValid())
			{
				Style->SetObjectField(TEXT("mirrorValueTypeIcons"), *Sub);
				for (const TPair<FString, TSharedPtr<FJsonValue>>& P : (*Sub)->Values)
				{
					if (!Live.Contains(P.Key)) ++NMirror;
				}
			}
			if (Pack->TryGetObjectField(TEXT("blockImages"), Sub) && Sub && (*Sub).IsValid())
			{
				Style->SetObjectField(TEXT("blockImages"), *Sub);
			}
			if (Pack->TryGetObjectField(TEXT("fontAliases"), Sub) && Sub && (*Sub).IsValid())
			{
				Style->SetObjectField(TEXT("fontAliases"), *Sub);
			}
			FString S;
			if (Pack->TryGetStringField(TEXT("mediaPath"), S)) Style->SetStringField(TEXT("mediaPath"), S);
			if (Pack->TryGetStringField(TEXT("fontCss"), S) && !S.IsEmpty())
			{
				Style->SetStringField(TEXT("fontCss"), S);
			}
			const TArray<TSharedPtr<FJsonValue>>* Arr = nullptr;
			if (Pack->TryGetArrayField(TEXT("fontFamilies"), Arr)) Style->SetArrayField(TEXT("fonts"), *Arr);
			if (Pack->TryGetArrayField(TEXT("fontsMissing"), Arr)) Style->SetArrayField(TEXT("fontsMissing"), *Arr);
		}

		// The last tier: what the offline default file itself carries. It is
		// attached to every payload, not only to its own, so a live capture with
		// no mirror on the machine still has something behind it.
		int32 NFallback = 0;
		{
			TSet<FString> Covered = Live;
			if (Pack.IsValid())
			{
				const TSharedPtr<FJsonObject>* Sub = nullptr;
				if (Pack->TryGetObjectField(TEXT("categoryIcons"), Sub) && Sub && (*Sub).IsValid())
				{
					for (const TPair<FString, TSharedPtr<FJsonValue>>& P : (*Sub)->Values) Covered.Add(P.Key);
				}
				if (Pack->TryGetObjectField(TEXT("valueTypeIcons"), Sub) && Sub && (*Sub).IsValid())
				{
					for (const TPair<FString, TSharedPtr<FJsonValue>>& P : (*Sub)->Values) Covered.Add(P.Key);
				}
			}

			const bool bHasOwn = Style->HasField(TEXT("icons")) || Style->HasField(TEXT("valueTypeIcons"));
			TSharedPtr<FJsonObject> Fallback = bHasOwn
				? TSharedPtr<FJsonObject>(Style) : DefaultIcons();
			if (Fallback.IsValid())
			{
				if (!bHasOwn)
				{
					TSharedRef<FJsonObject> Flat = MakeShared<FJsonObject>();
					const TCHAR* Fields[] = { TEXT("icons"), TEXT("valueTypeIcons") };
					for (const TCHAR* F : Fields)
					{
						const TSharedPtr<FJsonObject>* Sub = nullptr;
						if (!Fallback->TryGetObjectField(F, Sub) || !Sub || !(*Sub).IsValid()) continue;
						for (const TPair<FString, TSharedPtr<FJsonValue>>& P : (*Sub)->Values)
						{
							Flat->SetField(P.Key, P.Value);
						}
					}
					if (Flat->Values.Num()) Style->SetObjectField(TEXT("fallbackIcons"), Flat);
				}
				const TCHAR* Fields[] = { TEXT("icons"), TEXT("valueTypeIcons") };
				for (const TCHAR* F : Fields)
				{
					const TSharedPtr<FJsonObject>* Sub = nullptr;
					if (!Fallback->TryGetObjectField(F, Sub) || !Sub || !(*Sub).IsValid()) continue;
					for (const TPair<FString, TSharedPtr<FJsonValue>>& P : (*Sub)->Values)
					{
						if (!Covered.Contains(P.Key)) ++NFallback;
					}
				}
			}
		}

		GIconSource = Live.Num() ? TEXT("live") : NMirror ? TEXT("mirror")
			: NFallback ? TEXT("fallback") : TEXT("none");
		GIconSummary = FString::Printf(
			TEXT("%d from the site, %d from the mirror, %d from the offline default"),
			Live.Num(), NMirror, NFallback);
	}

	// The counts, for the Output Log, so a gap in a capture is visible without
	// opening the file.
	FString StyleCounts(const TSharedPtr<FJsonObject>& S)
	{
		if (!S.IsValid()) return TEXT("nothing");
		auto CountOf = [&S](const TCHAR* Field) -> int32
		{
			const TSharedPtr<FJsonObject>* O = nullptr;
			return (S->TryGetObjectField(Field, O) && O && (*O).IsValid()) ? (*O)->Values.Num() : 0;
		};
		FString Renderer, ThemeName;
		const TSharedPtr<FJsonObject>* R = nullptr;
		if (S->TryGetObjectField(TEXT("renderer"), R) && R && (*R).IsValid())
		{
			(*R)->TryGetStringField(TEXT("name"), Renderer);
		}
		int32 BlockStyles = 0, CategoryStyles = 0, ComponentStyles = 0;
		const TSharedPtr<FJsonObject>* T = nullptr;
		if (S->TryGetObjectField(TEXT("theme"), T) && T && (*T).IsValid())
		{
			(*T)->TryGetStringField(TEXT("name"), ThemeName);
			const TSharedPtr<FJsonObject>* Sub = nullptr;
			if ((*T)->TryGetObjectField(TEXT("blockStyles"), Sub) && Sub && (*Sub).IsValid()) BlockStyles = (*Sub)->Values.Num();
			if ((*T)->TryGetObjectField(TEXT("categoryStyles"), Sub) && Sub && (*Sub).IsValid()) CategoryStyles = (*Sub)->Values.Num();
			if ((*T)->TryGetObjectField(TEXT("componentStyles"), Sub) && Sub && (*Sub).IsValid()) ComponentStyles = (*Sub)->Values.Num();
		}
		int32 CssBytes = 0;
		const TSharedPtr<FJsonObject>* C = nullptr;
		if (S->TryGetObjectField(TEXT("css"), C) && C && (*C).IsValid())
		{
			FString Text;
			(*C)->TryGetStringField(TEXT("text"), Text);
			CssBytes = Text.Len();
		}

		TArray<FString> Missing;
		if (Renderer.IsEmpty()) Missing.Add(TEXT("renderer"));
		if (CountOf(TEXT("constants")) == 0) Missing.Add(TEXT("constants"));
		if (BlockStyles + CategoryStyles + ComponentStyles == 0) Missing.Add(TEXT("theme"));
		if (CountOf(TEXT("blockColours")) == 0) Missing.Add(TEXT("blockColours"));
		if (CssBytes == 0) Missing.Add(TEXT("css"));
		if (CountOf(TEXT("categoryIcons")) == 0) Missing.Add(TEXT("categoryIcons"));

		return FString::Printf(TEXT(
			"renderer %s, theme %s, %d constants, %d block styles, %d category styles, "
			"%d component colours, %d typed colours, %d icons, %d bytes of CSS. %s"),
			Renderer.IsEmpty() ? TEXT("not captured") : *Renderer,
			ThemeName.IsEmpty() ? TEXT("not captured") : *ThemeName,
			CountOf(TEXT("constants")), BlockStyles, CategoryStyles, ComponentStyles,
			CountOf(TEXT("blockColours")), CountOf(TEXT("categoryIcons")), CssBytes,
			Missing.Num() ? *(TEXT("NOT CAPTURED: ") + FString::Join(Missing, TEXT(", ")))
			              : TEXT("Nothing missing."));
	}

	// IS THIS THE SITE'S LOOK, OR OUR OWN FALLBACK COMING BACK AT US?
	//
	// The capture reads whatever Blockly is currently drawing. Read on the
	// Portal blocks page that is the site's look, which is the point. Read on
	// our own page while it is running the built-in fallback, it is geras and
	// the classic theme with no site CSS at all - and that used to be written
	// straight over the real capture.
	//
	// It happened on 2026-09-07 and cost the whole captured look: 144 constants
	// and 49,053 bytes of site CSS replaced by 85 and 28,245, with the icon set
	// emptied. Nothing warned, because from here one capture looks like another.
	//
	// A stock renderer name is the tell. The site registers its own; geras,
	// zelos, minimalist and thrasos are Blockly's.
	bool LooksLikeOurFallback(const TSharedPtr<FJsonObject>& Msg)
	{
		if (!Msg.IsValid()) return false;
		FString Name;
		const TSharedPtr<FJsonObject>* R = nullptr;
		if (Msg->TryGetObjectField(TEXT("renderer"), R) && R && (*R).IsValid())
		{
			(*R)->TryGetStringField(TEXT("name"), Name);
		}
		else
		{
			Msg->TryGetStringField(TEXT("renderer"), Name);
		}
		const bool bStockRenderer =
			Name == TEXT("geras") || Name == TEXT("zelos") ||
			Name == TEXT("minimalist") || Name == TEXT("thrasos");

		int32 CssLen = 0;
		const TSharedPtr<FJsonObject>* C = nullptr;
		if (Msg->TryGetObjectField(TEXT("css"), C) && C && (*C).IsValid())
		{
			FString Text;
			(*C)->TryGetStringField(TEXT("text"), Text);
			CssLen = Text.Len();
		}
		return bStockRenderer || CssLen == 0;
	}

	void CacheStyle(const TSharedPtr<FJsonObject>& Msg, const FString& Raw)
	{
		if (!Msg.IsValid()) return;

		// Refused rather than merged: a half capture written over a good one is
		// worse than no capture, and re-taking it means opening the site's
		// blocks page, which is a thing the user can be told to do.
		if (LooksLikeOurFallback(Msg) && FPaths::FileExists(StyleCachePath()))
		{
			UE_LOG(LogBF6Blocks, Warning, TEXT(
				"Site style NOT cached: what was read is our own fallback look, not the site's, "
				"and a real capture is already on disk. Open the experience's BLOCKS page in the "
				"Portal panel and run BF6.Blocks.CaptureStyle to refresh it."));
			return;
		}

		IFileManager::Get().MakeDirectory(*CacheDir(), true);
		FFileHelper::SaveStringToFile(Raw, *StyleCachePath(),
			FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
		Msg->TryGetStringField(TEXT("capturedAt"), GStyleAt);
		GStyleSource = TEXT("live");
		GStyleSummary = StyleCounts(Msg);
		UE_LOG(LogBF6Blocks, Display, TEXT("Site style captured: %s"), *GStyleSummary);
	}

	// The look the editor should start with: last capture first, the offline
	// default second, nothing third. Sent as its own message so the editor can
	// restyle without rebuilding its definitions.
	bool SendStyle()
	{
		FString Text;
		const TCHAR* Source = nullptr;
		if (FFileHelper::LoadFileToString(Text, *StyleCachePath()) && !Text.IsEmpty())
		{
			Source = TEXT("cache");
		}
		else if (FFileHelper::LoadFileToString(Text, *StyleDefaultPath()) && !Text.IsEmpty())
		{
			Source = TEXT("default");
		}

		TSharedPtr<FJsonObject> Obj;
		if (Source)
		{
			TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Text);
			if (!FJsonSerializer::Deserialize(Reader, Obj) || !Obj.IsValid())
			{
				GStyleSource = TEXT("none");
				GLastError = FString::Printf(TEXT("style file is not valid JSON: %s"),
					FCString::Strcmp(Source, TEXT("cache")) == 0 ? *StyleCachePath() : *StyleDefaultPath());
				UE_LOG(LogBF6Blocks, Warning, TEXT("%s"), *GLastError);
				return false;
			}
		}
		else if (MirrorPack().IsValid())
		{
			// No capture and no offline file, but the site's own assets are on
			// this machine: the icons and the faces still go over, on their own.
			Obj = MakeShared<FJsonObject>();
			Source = TEXT("mirror");
		}
		else
		{
			GStyleSource = TEXT("none");
			GStyleSummary = TEXT("nothing captured and no offline default file");
			UE_LOG(LogBF6Blocks, Display, TEXT(
				"No site style cached and no %s: the editor draws with Blockly's geras renderer "
				"until CAPTURE STYLE is pressed with the Portal blocks page open."),
				*StyleDefaultPath());
			return false;
		}

		AddMirrorToStyle(Obj.ToSharedRef());
		Obj->SetStringField(TEXT("op"), TEXT("style"));
		Obj->SetStringField(TEXT("source"), Source);
		Obj->TryGetStringField(TEXT("capturedAt"), GStyleAt);
		GStyleSource = Source;
		GStyleSummary = StyleCounts(Obj);
		ToTool(Obj.ToSharedRef());
		UE_LOG(LogBF6Blocks, Display, TEXT("Site style from the %s: %s"), Source, *GStyleSummary);
		UE_LOG(LogBF6Blocks, Display, TEXT("Block icons in use: %s (%s)."), *GIconSource, *GIconSummary);
		return true;
	}

	// Ask the page for a fresh one. Nothing is thrown away if it fails: the
	// cached look stays on screen.
	bool AskSiteForStyle(const TCHAR* Reason)
	{
		if (!GSiteReady)
		{
			GLastError = TEXT("style capture needs the Portal blocks page open in the panel");
			UE_LOG(LogBF6Blocks, Warning, TEXT(
				"CAPTURE STYLE: open your experience's BLOCKS page in the Portal panel first."));
			ToolNote(TEXT("status"), TEXT("text"),
				TEXT("Open your experience's BLOCKS page in the Portal panel, then press CAPTURE STYLE."));
			return false;
		}
		TSharedRef<FJsonObject> M = MakeShared<FJsonObject>();
		M->SetStringField(TEXT("op"), TEXT("captureStyle"));
		M->SetStringField(TEXT("reason"), Reason ? Reason : TEXT("asked"));
		ToSite(M);
		return true;
	}

	// ========================================================================
	// Import and export
	//
	// Two formats, both verified against real files rather than guessed. The
	// page owns the schemas (editor.js documents them field by field, and the
	// node harness proves a read-then-write against a real published export);
	// this side owns the file dialogs, the disk, and where an attachment goes
	// once it is out of the file.
	// ========================================================================
	FString ImportInboxDir()
	{
		return FPaths::Combine(BF6Ext::ToolSavedDir(), TEXT("portal"), TEXT("blocks"), TEXT("import"));
	}

	const void* DialogParent()
	{
		return FSlateApplication::IsInitialized()
			? FSlateApplication::Get().FindBestParentWindowHandleForDialogs(nullptr)
			: nullptr;
	}

	// The TypeScript compiler API, which BF6Convert.tsToBlocks cannot parse a
	// site script export without. Not vendored: it is 9 MB and every Portal
	// script project already has one.
	//   1. [BF6UnrealSDK] TypeScriptLib   the full path to typescript.js
	//   2. the plugin's own Resources/convert/vendor/typescript.js
	//   3. [BF6UnrealSDK] ScriptTemplateDir, the same key BF6Script uses
	FString TypeScriptLibPath()
	{
		FString Configured;
		if (GConfig && GConfig->GetString(TEXT("BF6UnrealSDK"), TEXT("TypeScriptLib"), Configured, GEngineIni)
			&& !Configured.IsEmpty() && FPaths::FileExists(Configured))
		{
			return Configured;
		}
		const FString Vendored = FPaths::Combine(BF6Ext::ToolPluginDir(), TEXT("Resources"),
			TEXT("convert"), TEXT("vendor"), TEXT("typescript.js"));
		if (FPaths::FileExists(Vendored)) return Vendored;

		// ASK THE MODULE THAT ALREADY FOUND THE TEMPLATE.
		//
		// This read [BF6UnrealSDK] ScriptTemplateDir out of the ini and nothing
		// else. BF6Script does not need that key: it scans the fixed drives for
		// a PortalSDK root and finds the template on its own, and had already
		// logged 'Script template: .../_template-v1.7.0' at startup - with a 9MB
		// typescript.js sitting in its node_modules. So the compiler was on disk,
		// found, and reported missing, and script import stayed switched off for
		// want of an ini key nobody had a reason to set.
		//
		// BF6Script::TemplateDir() is that one finder, and it looks the key up
		// itself as its own first choice, so this covers both.
		const FString Template = BF6Script::TemplateDir();
		if (!Template.IsEmpty())
		{
			const FString FromTemplate = FPaths::Combine(Template, TEXT("node_modules"),
				TEXT("typescript"), TEXT("lib"), TEXT("typescript.js"));
			if (FPaths::FileExists(FromTemplate)) return FromTemplate;
		}
		// Export for Portal installs a compiler without requiring a separate
		// community template. Reuse that completed installation for imports.
		const FString Tools = FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir() / TEXT("BF6UnrealSDK/portal-build-tools"));
		TArray<FString> Versions;
		IFileManager::Get().FindFiles(Versions, *(Tools / TEXT("*")), false, true);
		for (const FString& Version : Versions)
		{
			const FString Root = Tools / Version;
			const FString Compiler = Root / TEXT("node_modules/typescript/lib/typescript.js");
			if (FPaths::FileExists(Root / TEXT(".ready")) && FPaths::FileExists(Compiler)) return Compiler;
		}
		return FString();
	}

	// A path the page can put in a script tag. The page is a file:// document,
	// so a file:// script from anywhere on disk loads beside it.
	FString FileUrl(const FString& Path)
	{
		FString P = Path;
		P.ReplaceInline(TEXT("\\"), TEXT("/"));
		P.ReplaceInline(TEXT(" "), TEXT("%20"));
		return TEXT("file:///") + P;
	}

	// Hand the page a file's text. Chunked on the way, like everything else.
	void SendFileToTool(const FString& Path, const FString& Text, const FString& Reason)
	{
		TSharedRef<FJsonObject> M = MakeShared<FJsonObject>();
		M->SetStringField(TEXT("op"), TEXT("importFile"));
		M->SetStringField(TEXT("path"), Path);
		M->SetStringField(TEXT("name"), FPaths::GetCleanFilename(Path));
		M->SetStringField(TEXT("text"), Text);
		M->SetStringField(TEXT("reason"), Reason);
		const FString Lib = TypeScriptLibPath();
		if (!Lib.IsEmpty()) M->SetStringField(TEXT("typescriptUrl"), FileUrl(Lib));
		ToTool(M);
		UE_LOG(LogBF6Blocks, Display, TEXT("Handed %s to the editor (%d characters)."), *Path, Text.Len());
	}

	bool OpenImportDialog(FString& OutPath)
	{
		IDesktopPlatform* DP = FDesktopPlatformModule::Get();
		if (!DP)
		{
			GLastError = TEXT("the file dialog is not available");
			return false;
		}
		TArray<FString> Picked;
		const FString Start = FPaths::DirectoryExists(ImportInboxDir())
			? ImportInboxDir() : FPlatformProcess::UserDir();
		const bool bOk = DP->OpenFileDialog(DialogParent(),
			TEXT("Import a Portal workspace, experience or script export"), Start, TEXT(""),
			TEXT("Portal files (*.json;*.ts)|*.json;*.ts|Experience or workspace (*.json)|*.json|Script export (*.ts)|*.ts|All files (*.*)|*.*"),
			EFileDialogFlags::None, Picked);
		if (!bOk || Picked.Num() == 0) return false;
		OutPath = Picked[0];
		return true;
	}

	bool SaveDialog(const FString& Title, const FString& Suggested, FString& OutPath)
	{
		IDesktopPlatform* DP = FDesktopPlatformModule::Get();
		if (!DP)
		{
			GLastError = TEXT("the file dialog is not available");
			return false;
		}
		TArray<FString> Picked;
		const FString Start = FPaths::Combine(BF6Ext::ToolSavedDir(), TEXT("portal"), TEXT("blocks"));
		IFileManager::Get().MakeDirectory(*Start, true);
		const bool bOk = DP->SaveFileDialog(DialogParent(), Title, Start, Suggested,
			TEXT("Portal JSON (*.json)|*.json"), EFileDialogFlags::None, Picked);
		if (!bOk || Picked.Num() == 0) return false;
		OutPath = Picked[0];
		if (!OutPath.EndsWith(TEXT(".json"))) OutPath += TEXT(".json");
		return true;
	}

	// A FOLDER, BECAUSE A SCRIPT IS NOT ONE FILE.
	//
	// The converter splits a workspace into a small project - rules grouped by
	// family, subroutines, variables, a runtime and an index - so exporting it
	// asks for somewhere to put eleven files rather than a name for one. The
	// same picker serves the way back in.
	bool PickFolder(const FString& Title, FString& OutPath)
	{
		IDesktopPlatform* DP = FDesktopPlatformModule::Get();
		if (!DP)
		{
			GLastError = TEXT("the folder dialog is not available");
			return false;
		}
		const FString Start = FPaths::Combine(BF6Ext::ToolSavedDir(), TEXT("portal"), TEXT("script"));
		IFileManager::Get().MakeDirectory(*Start, true);
		return DP->OpenDirectoryDialog(DialogParent(), Title, Start, OutPath);
	}

	// Read a file and hand it to the editor, which decides what it is.
	bool ImportPath(const FString& Path, const FString& Reason)
	{
		FString Text;
		if (!FFileHelper::LoadFileToString(Text, *Path))
		{
			GLastError = FString::Printf(TEXT("cannot read %s"), *Path);
			UE_LOG(LogBF6Blocks, Warning, TEXT("%s"), *GLastError);
			ToolNote(TEXT("status"), TEXT("text"), GLastError);
			return false;
		}
		BF6Blocks::Open();
		SendFileToTool(Path, Text, Reason);
		return true;
	}

	// One attachment out of a full experience, put where the tool can use it.
	//   spatial      written out and imported as a save through the base tool's
	//                own importer, exactly as the profile module does it
	//   ts, strings, blacklist
	//                written into the import inbox and named in the log. The
	//                script editor has no import seam yet; one line in
	//                BF6Script.h would close that:
	//                    bool BF6Script::ImportFile(const FString& Path);
	//                and this function would call it instead of stopping at the
	//                inbox.
	FString RouteAttachment(const FString& Filename, int32 AttachmentType,
		const FString& Text, int32 MapIdx, FString& OutWhat)
	{
		IFileManager::Get().MakeDirectory(*ImportInboxDir(), true);
		FString Clean = Filename;
		Clean.ReplaceInline(TEXT("\\"), TEXT("_"));
		Clean.ReplaceInline(TEXT("/"), TEXT("_"));
		if (Clean.IsEmpty()) Clean = FString::Printf(TEXT("attachment_%d.txt"), AttachmentType);
		const FString Path = FPaths::Combine(ImportInboxDir(), Clean);
		if (!FFileHelper::SaveStringToFile(Text, *Path, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
		{
			OutWhat = TEXT("could not be written");
			return FString();
		}
		switch (AttachmentType)
		{
		case 1:
		{
			const FString SaveName = FPaths::GetBaseFilename(Clean).Replace(TEXT(".spatial"), TEXT(""));
			const bool bOk = BF6_ImportSpatialFile(Path, SaveName, GSiteUrl, MapIdx);
			OutWhat = bOk
				? FString::Printf(TEXT("imported as the save \"%s\" for rotation slot %d"), *SaveName, MapIdx)
				: TEXT("written out, but the level importer refused it");
			break;
		}
		case 2:  OutWhat = TEXT("written out for the script editor (TypeScript bundle)"); break;
		case 4:  OutWhat = TEXT("written out for the script editor (strings bundle)"); break;
		case 3:  OutWhat = TEXT("written out (blacklist)"); break;
		default: OutWhat = TEXT("written out"); break;
		}
		return Path;
	}

	// The converter's catalogue and event table. The page cannot fetch a file://
	// JSON of its own, so they go across the bridge once, at startup. Without
	// them tsToBlocks still runs; it just falls back to a name pattern where it
	// would have looked an event up.
	void SendConvertData()
	{
		const FString Dir = FPaths::Combine(BF6Ext::ToolPluginDir(), TEXT("Resources"), TEXT("convert"));
		FString Catalog, Events;
		FFileHelper::LoadFileToString(Catalog, *FPaths::Combine(Dir, TEXT("catalog.json")));
		FFileHelper::LoadFileToString(Events, *FPaths::Combine(Dir, TEXT("events.json")));
		const FString Lib = TypeScriptLibPath();
		if (Catalog.IsEmpty() && Events.IsEmpty() && Lib.IsEmpty()) return;
		FString Payload = TEXT("{\"op\":\"convertData\"");
		if (!Catalog.IsEmpty()) Payload += TEXT(",\"catalog\":") + Catalog;
		if (!Events.IsEmpty()) Payload += TEXT(",\"events\":") + Events;
		if (!Lib.IsEmpty())
		{
			FString Url = FileUrl(Lib);
			Url.ReplaceInline(TEXT("\\"), TEXT("\\\\"));
			Url.ReplaceInline(TEXT("\""), TEXT("\\\""));
			Payload += TEXT(",\"typescriptUrl\":\"") + Url + TEXT("\"");
		}
		Payload += TEXT("}");
		ToToolRaw(Payload);
		UE_LOG(LogBF6Blocks, Display, TEXT(
			"Converter data sent to the editor: catalogue %d chars, events %d chars, TypeScript %s"),
			Catalog.Len(), Events.Len(),
			Lib.IsEmpty() ? TEXT("NOT FOUND (a script export cannot be imported until "
				"[BF6UnrealSDK] TypeScriptLib points at a typescript.js)") : *Lib);
	}

	// ---- autosave ----------------------------------------------------------
	// Replace only after the temporary file has been written completely.
	// On Windows use replacement rename, without deleting the previous save first.
	bool WriteAtomic(const FString& Path, const FString& Text)
	{
		const FString Temp = Path + TEXT(".tmp");
		if (!FFileHelper::SaveStringToFile(Text, *Temp, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
		{
			return false;
		}
#if PLATFORM_WINDOWS
		FString From = FPaths::ConvertRelativePathToFull(Temp), To = FPaths::ConvertRelativePathToFull(Path);
		FPaths::MakePlatformFilename(From); FPaths::MakePlatformFilename(To);
		return !!::MoveFileExW(*From, *To, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
#else
		return IFileManager::Get().Move(*Path, *Temp, true, false);
#endif
	}

	// Ten timestamped copies, oldest first out.
	void RotateAutosaves(const FString& Dir, const FString& Text)
	{
		const FString Stamp = FDateTime::Now().ToString(TEXT("%Y%m%d_%H%M%S"));
		WriteAtomic(FPaths::Combine(Dir, FString::Printf(TEXT("autosave_%s.json"), *Stamp)), Text);
		TArray<FString> Old;
		IFileManager::Get().FindFiles(Old, *FPaths::Combine(Dir, TEXT("autosave_*.json")), true, false);
		Old.Sort();
		while (Old.Num() > 10)
		{
			IFileManager::Get().Delete(*FPaths::Combine(Dir, Old[0]), false, true, true);
			Old.RemoveAt(0);
		}
	}

	void WriteAutosave(const FString& Raw)
	{
		const FString Dir = ExperienceDir();
		IFileManager::Get().MakeDirectory(*Dir, true);
		const FString Path = FPaths::Combine(Dir, TEXT("autosave.json"));
		if (!WriteAtomic(Path, Raw))
		{
			GLastError = FString::Printf(TEXT("autosave failed: %s"), *Path);
			UE_LOG(LogBF6Blocks, Warning, TEXT("%s"), *GLastError);
			return;
		}
		GAutosaves++;
		GLastAutosave = FDateTime::Now().ToString();
		// One timestamped copy every tenth save keeps a history without turning
		// a busy hour into a thousand files.
		if (GAutosaves == 1 || (GAutosaves % 10) == 0) RotateAutosaves(Dir, Raw);
	}

	bool WriteProjectRecovery(const TSharedPtr<FJsonObject>& M, bool bCheckpoint)
	{
		const TSharedPtr<FJsonObject>* Json = nullptr;
		if (!M->TryGetObjectField(TEXT("json"), Json) || !Json || !Json->IsValid()) return false;
		FString Project; M->TryGetStringField(TEXT("project"), Project);
		const FString* Dir = GProjectRecoveryDirs.Find(Project);
		const FString Text = Write(M.ToSharedRef());
		// A unique update copy is retained even for a workspace imported before
		// a map was chosen. Never overwrite the imported workspace on this path.
		if (bCheckpoint)
		{
			const FString Checkpoints = BF6Ext::ToolSavedDir() / TEXT("blocks/update-backups");
			IFileManager::Get().MakeDirectory(*Checkpoints, true);
			const FString Checkpoint = Checkpoints / (FGuid::NewGuid().ToString(EGuidFormats::Digits) + TEXT(".json"));
			if (!WriteAtomic(Checkpoint, Text)) return false;
			FString Verified;
			if (!FFileHelper::LoadFileToString(Verified, *Checkpoint) || Verified != Text) return false;
		}
		if (!Dir) return bCheckpoint;
		IFileManager::Get().MakeDirectory(**Dir, true);
		return WriteAtomic(*Dir / TEXT("recovery.json"), Text);
	}

	// The autosave, if it is ahead of what the site just handed back. Ahead
	// means it still has edits the site never acknowledged, or it was written
	// after the last workspace we pulled.
	TSharedPtr<FJsonObject> AutosaveAheadOfSite()
	{
		const FString Path = FPaths::Combine(ExperienceDir(), TEXT("autosave.json"));
		FString Text;
		if (!FFileHelper::LoadFileToString(Text, *Path)) return nullptr;
		TSharedPtr<FJsonObject> Doc = Parse(Text);
		if (!Doc.IsValid()) return nullptr;
		const TSharedPtr<FJsonObject>* Meta = nullptr;
		bool bAhead = false;
		if (Doc->TryGetObjectField(TEXT("meta"), Meta) && Meta && (*Meta).IsValid())
		{
			double Pending = 0;
			(*Meta)->TryGetNumberField(TEXT("pending"), Pending);
			bool bLost = false;
			(*Meta)->TryGetBoolField(TEXT("sessionLost"), bLost);
			bAhead = (Pending > 0) || bLost;
			if (!bAhead)
			{
				// No pending edits, but written since the last pull: still ours.
				const FDateTime Stamp = IFileManager::Get().GetTimeStamp(*Path);
				FString LastPullText;
				if (FFileHelper::LoadFileToString(LastPullText,
					*FPaths::Combine(ExperienceDir(), TEXT("lastpull.txt"))))
				{
					FDateTime LastPull;
					if (FDateTime::Parse(LastPullText, LastPull)) bAhead = Stamp > LastPull;
				}
				else bAhead = true;
			}
		}
		return bAhead ? Doc : nullptr;
	}

	void NoteSitePull()
	{
		FFileHelper::SaveStringToFile(FDateTime::UtcNow().ToIso8601(),
			*FPaths::Combine(ExperienceDir(), TEXT("lastpull.txt")),
			FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
	}

	// The community FAQ, if the script editor's build step has produced one.
	void SendFaq()
	{
		const FString Path = FPaths::Combine(BF6Ext::ToolPluginDir(), TEXT("Resources"),
			TEXT("script"), TEXT("faq.json"));
		FString Text;
		if (!FFileHelper::LoadFileToString(Text, *Path)) return;
		Text.TrimStartAndEndInline();
		// The file is either a bare array or an object with an "entries" array.
		FString Payload;
		if (Text.StartsWith(TEXT("[")))
			Payload = TEXT("{\"op\":\"faq\",\"entries\":") + Text + TEXT("}");
		else
			Payload = TEXT("{\"op\":\"faq\",\"entries\":") + Text + TEXT("}");
		ToToolRaw(Payload);
		UE_LOG(LogBF6Blocks, Display, TEXT("Community FAQ handed to the editor from %s"), *Path);
	}

	// ========================================================================
	// The curated answers
	//
	// Resources/faq/answers/*.json: written out, one line each, with the
	// gotchas, a block example that has been loaded headlessly and checked, and
	// the same thing again in TypeScript. They are handed over as their own
	// message and the page keeps them in their own list, because a curated
	// answer and a mined forum thread are two different things and must never
	// be mistaken for each other.
	//
	// The wider mined corpus (Resources/script/faq.json) still goes over as
	// "faq" and still sits underneath them in the panel.
	// ========================================================================
	FString AnswersDir()
	{
		return FPaths::Combine(BF6Ext::ToolPluginDir(), TEXT("Resources"),
			TEXT("faq"), TEXT("answers"));
	}

	void SendAnswers()
	{
		const FString Dir = AnswersDir();
		TArray<FString> Files;
		IFileManager::Get().FindFiles(Files, *(Dir / TEXT("*.json")), true, false);
		if (!Files.Num())
		{
			UE_LOG(LogBF6Blocks, Display, TEXT(
				"No curated answer sets at %s: the editor shows the mined threads alone."), *Dir);
			return;
		}
		Files.Sort();

		FString Payload = TEXT("{\"op\":\"answers\",\"sets\":[");
		int32 Sets = 0, Entries = 0;
		for (const FString& File : Files)
		{
			FString Text;
			if (!FFileHelper::LoadFileToString(Text, *FPaths::Combine(Dir, File))) continue;
			Text.TrimStartAndEndInline();
			if (Text.IsEmpty()) continue;

			// Counted, not parsed twice: the page owns the schema, this side
			// owns the disk. The count is for the log so a set that failed to
			// build is visible without opening the file.
			TSharedPtr<FJsonObject> Obj;
			TSharedRef<TJsonReader<>> R = TJsonReaderFactory<>::Create(Text);
			if (!FJsonSerializer::Deserialize(R, Obj) || !Obj.IsValid())
			{
				UE_LOG(LogBF6Blocks, Warning, TEXT("Answer set %s is not valid JSON, skipped."), *File);
				continue;
			}
			const TArray<TSharedPtr<FJsonValue>>* Arr = nullptr;
			if (Obj->TryGetArrayField(TEXT("entries"), Arr) && Arr) Entries += Arr->Num();

			if (Sets) Payload += TEXT(",");
			Payload += Text;
			++Sets;
		}
		Payload += TEXT("],\"note\":\"curated\"}");
		if (!Sets) return;
		ToToolRaw(Payload);
		UE_LOG(LogBF6Blocks, Display, TEXT(
			"Curated answers handed to the editor: %d set(s), %d entries, from %s"),
			Sets, Entries, *Dir);
	}

	// ========================================================================
	// One panel state, remembered
	//
	// The key list is CLOSED: a page that sends anything else is refused rather
	// than given a new line in the editor ini. Written where the rest of the
	// tool's panel states live, and flushed, which is what the scene tree's
	// symbol legend already does.
	// ========================================================================
	const TCHAR* kPrefSection = TEXT("BF6UnrealSDK");

	bool IsKnownBlocksPref(const FString& Name)
	{
		return Name == TEXT("shelf") || Name == TEXT("shelfOpen") || Name == TEXT("shelfPinned") ||
			Name == TEXT("textFloor") || Name == TEXT("simplifiedOverview") || Name == TEXT("redVariables") || Name == TEXT("compactFields") || Name == TEXT("categoryColors");
	}

	FString BlocksPrefIniKey(const FString& Name) { return TEXT("BlocksUi_") + Name; }

	void SetBlocksPref(const FString& Name, const FString& Value)
	{
		if (!IsKnownBlocksPref(Name))
		{
			UE_LOG(LogBF6Blocks, Warning, TEXT(
				"%s is not a panel state this editor keeps, so it was not written."), *Name);
			return;
		}
		if (!GConfig) return;
		if (Name == TEXT("categoryColors"))
		{
			TSharedPtr<FJsonObject> Palette;
			if (Value.Len() > 4096 || !FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Value), Palette) || !Palette.IsValid()) return;
			for (const auto& Pair : Palette->Values)
			{
				double Hue = 0;
				if (Pair.Key.Len() > 80 || !FString(*Pair.Key).EndsWith(TEXT("-block-style")) ||
					!Pair.Value.IsValid() || !Pair.Value->TryGetNumber(Hue) || !FMath::IsFinite(Hue) || Hue < 0 || Hue >= 360) return;
			}
		}
		GConfig->SetString(kPrefSection, *BlocksPrefIniKey(Name), *Value, GEditorPerProjectIni);
		GConfig->Flush(false, GEditorPerProjectIni);
	}

	// ---- the site's own block definitions ----------------------------------
	//
	// WITHOUT THESE, NOTHING LOOKS RIGHT. A block's shape, its fields, their
	// order and its wording all come from the site's own Blockly definition.
	// With none cached the editor falls back to the mined catalogue, which
	// knows a type's name and roughly its inputs and nothing else, so a
	// workspace loads with the right blocks in the right places and every one
	// of them drawn wrong. That is what "it does not match the site at all"
	// is: 191 real types rendered from an approximation.
	//
	// They were only ever captured by the user driving the panel to a blocks
	// page and pressing PULL FROM SITE, which nobody would guess they had to
	// do. Now the panel fetches them itself, offscreen, the first time a block
	// editor opens without them.
	bool HasCachedDefinitions()
	{
		const FString P = FPaths::Combine(CacheDir(), TEXT("definitions.json"));
		return FPaths::FileExists(P) && IFileManager::Get().FileSize(*P) > 1024;
	}

	// Tried once per editor session, whatever the answer.
	bool GTriedDefsFetch = false;

	void FetchDefinitions(bool bForce)
	{
		if (HasCachedDefinitions() && !bForce) return;

		// OFFLINE IS A FIRST-CLASS WAY TO WORK, NOT A DEGRADED ONE.
		//
		// Building with blocks must never wait on the site, so this asks for
		// nothing unless the site is already known to be reachable: the
		// profile has to be Linked, which only happens when the site has
		// answered. With no network, no account, or no experience, the editor
		// opens straight away on the mined catalogue and the workspace on
		// disk, which is the whole offline path and is unchanged.
		//
		// And it is attempted ONCE per session. A failed reach must not become
		// a browser opening itself every time a block editor opens.
		if (GTriedDefsFetch && !bForce) return;
		if (BF6PortalProfile::State() != BF6PortalProfile::EState::Linked)
		{
			if (bForce)
				UE_LOG(LogBF6Blocks, Display,
					TEXT("The site cannot be read for block definitions while the Portal profile is not linked. LINK PORTAL PROFILE first."));
			return;
		}

		// The blocks page of the experience this save belongs to. No experience
		// means no page to read them from, and that is not an error: an offline
		// project still opens, on the mined catalogue, exactly as before.
		const FString Url = BF6PortalWeb::ExperienceForSave(
			BF6Api::CurrentLevel(), BF6Api::CurrentSave());
		// ANY EXPERIENCE WILL DO.
		//
		// The definitions are the SITE's - the same 800-odd block types on
		// every experience anybody owns - so this does not need the one the
		// open save belongs to. It only needs a blocks page to read.
		//
		// Insisting on the open save's own experience is why this never ran:
		// opening the block editor without a map open leaves no save, no
		// experience and no address, and it gave up rather than using any of
		// the 38 experiences the profile already knows about.
		FString Id;
		{
			const int32 At = Url.Find(TEXT("id="));
			if (At != INDEX_NONE)
			{
				Id = Url.Mid(At + 3);
				int32 Amp; if (Id.FindChar(TEXT('&'), Amp)) Id.LeftInline(Amp);
			}
		}
		if (Id.Len() < 32)
		{
			for (const BF6PortalProfile::FExperienceRow& R : BF6PortalProfile::ListAll())
			{
				if (R.Id.Len() >= 32) { Id = R.Id; break; }
			}
		}
		if (Id.Len() < 32)
		{
			UE_LOG(LogBF6Blocks, Display,
				TEXT("No block definitions cached, and the profile knows no experience to read them from. The blocks are drawn from the mined catalogue."));
			return;
		}
		GTriedDefsFetch = true;

		// BUILT FROM THE ID, NOT PATCHED FROM THE STORED ADDRESS.
		//
		// This used to swap one section of the recorded url for another, which
		// assumed that url had a section in it at all. The one actually on disk
		// is ".../bf6/bf6/experience?id=<uuid>" - a doubled segment and no
		// section - so every replacement missed, the check below refused it,
		// and the fetch returned without a word. It never ran once.
		//
		// The uuid is the only part worth keeping. The address is rebuilt in
		// the shape the site really uses, which is the same one the panel
		// navigates to and is verified against the live site.
		const FString Blocks = BF6PortalWeb::BaseUrl() +
			TEXT("/experience/rules/blocks?id=") + Id + TEXT("&teams=1%2C2");

		UE_LOG(LogBF6Blocks, Display,
			TEXT("%s"), bForce
				? TEXT("Reading the site's blocks page again, offscreen, for its block definitions. Nothing is shown.")
				: TEXT("No block definitions cached yet, so the site's blocks page is being read once, offscreen, to get them. Nothing is shown."));
		// The tape measure the site page is about to be read with, taken off
		// disk now rather than at editor startup. See RefreshSiteScript.
		RefreshSiteScript();
		BF6PortalWeb::HoldOffscreen(true);
		BF6PortalWeb::OpenQuiet(Blocks);

		FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda([](float) -> bool
		{
			BF6PortalWeb::HoldOffscreen(false);
			UE_LOG(LogBF6Blocks, Display,
				TEXT("Block definitions after reading the site: %s"),
				HasCachedDefinitions()
					? TEXT("captured. Reopen the block editor and the blocks will be drawn the way the site draws them.")
					: TEXT("still none. The blocks page did not hand them over; PULL FROM SITE with the panel on that page still works."));
			return false;
		}), 90.0f);
	}

	// ---- the workspace the open save already owns --------------------------
	//
	// THE LAST LINK. The import downloads the blocks and writes them into the
	// experience's project folder, and the block editor never asked for them.
	// It opened with definitions, style, snippets and help, and an empty
	// canvas, so an experience with 5,192 blocks on disk looked like an
	// experience with no blocks at all. Both halves worked; nothing joined
	// them.
	//
	// Sent on ready, which is the moment the page can accept it. The page owns
	// the format: its own unwrap already understands the site's {"mod":{...}}
	// wrapper, which is exactly how the file is written.
	void SendProjectWorkspace()
	{
		const FString Save = BF6Api::CurrentSave();
		const FString Dir = Save.IsEmpty() ? FString() : BF6Project::DirFor(Save);

		const FString Path = FPaths::Combine(Dir, TEXT("unreal"), TEXT("blockly"), TEXT("workspace.json"));
		FString Text;
		const bool bHasWorkspace = !Dir.IsEmpty() && FPaths::FileExists(Path);
		if (bHasWorkspace && !FFileHelper::LoadFileToString(Text, *Path))
		{
			ToolNote(TEXT("status"), TEXT("text"), TEXT("Could not read this experience's block workspace."));
			return;
		}
		if (!bHasWorkspace) Text = TEXT("{\"blocks\":{\"languageVersion\":0,\"blocks\":[]},\"variables\":[]}");

		TSharedPtr<FJsonObject> Ws;
		TSharedRef<TJsonReader<>> R = TJsonReaderFactory<>::Create(Text);
		if (!FJsonSerializer::Deserialize(R, Ws) || !Ws.IsValid())
		{
			UE_LOG(LogBF6Blocks, Warning, TEXT("%s is not readable as JSON, so it was not opened"), *Path);
			ToolNote(TEXT("status"), TEXT("text"), TEXT("This experience's block workspace is invalid JSON. Its original file has been kept."));
			return;
		}

		TSharedRef<FJsonObject> M = MakeShared<FJsonObject>();
		M->SetStringField(TEXT("op"), TEXT("projectWorkspace"));
		M->SetStringField(TEXT("project"), Dir.IsEmpty()
			? TEXT("unnamed:") + BF6Api::CurrentLevel()
			: FPaths::ConvertRelativePathToFull(Dir).ToLower());
		M->SetStringField(TEXT("revision"), FMD5::HashAnsiString(*Text));
		M->SetStringField(TEXT("name"), Save);
		const FString Project = M->GetStringField(TEXT("project"));
		const FString RecoveryDir = Dir.IsEmpty()
			? BF6Ext::ToolSavedDir() / TEXT("blocks/local") / FMD5::HashAnsiString(*Project)
			: Dir / TEXT("unreal/blockly");
		GProjectRecoveryDirs.Add(Project, RecoveryDir);
		FString RecoveryText;
		if (FFileHelper::LoadFileToString(RecoveryText, *(RecoveryDir / TEXT("recovery.json"))))
		{
			if (TSharedPtr<FJsonObject> Recovery = Parse(RecoveryText)) M->SetObjectField(TEXT("recovery"), Recovery);
		}
		M->SetObjectField(TEXT("json"), Ws);
		ToTool(M);
		UE_LOG(LogBF6Blocks, Display,
			TEXT("Opened the workspace '%s' owns (%d characters from %s)"),
			*Save, Text.Len(), *Path);
	}

	void SendPrefs()
	{
		TSharedRef<FJsonObject> Values = MakeShared<FJsonObject>();
		static const TCHAR* kKeys[] = { TEXT("shelf"), TEXT("shelfOpen"), TEXT("shelfPinned"),
			TEXT("textFloor"), TEXT("simplifiedOverview"), TEXT("redVariables"), TEXT("compactFields"), TEXT("categoryColors") };
		int32 Found = 0;
		for (const TCHAR* Key : kKeys)
		{
			FString Value;
			if (GConfig && GConfig->GetString(kPrefSection, *BlocksPrefIniKey(Key), Value, GEditorPerProjectIni))
			{
				Values->SetStringField(Key, Value);
				++Found;
			}
		}
		// A key that was never written does not come back, which is how the
		// page tells a first run from a return visit.
		TSharedRef<FJsonObject> M = MakeShared<FJsonObject>();
		M->SetStringField(TEXT("op"), TEXT("prefs"));
		M->SetObjectField(TEXT("values"), Values);
		ToTool(M);
		UE_LOG(LogBF6Blocks, Verbose, TEXT("Blocks panel state: %d key(s) remembered."), Found);
	}

	// ---- the object side ---------------------------------------------------
	// Band bases for a new id, by kind. Convention only, and overridable in
	// DefaultEngine.ini under [BF6UnrealSDK] as BlocksObjIdBand_<Kind>=<n>.
	int32 BandFor(const FString& Kind)
	{
		int32 Band = 0;
		if (GConfig && GConfig->GetInt(TEXT("BF6UnrealSDK"),
			*(TEXT("BlocksObjIdBand_") + Kind), Band, GEngineIni) && Band > 0) return Band;
		if (Kind == TEXT("CapturePoint")) return 100;
		if (Kind == TEXT("SpawnPoint"))   return 200;
		if (Kind == TEXT("HQ"))           return 300;
		if (Kind == TEXT("MCOM"))         return 400;
		if (Kind == TEXT("VehicleSpawner")) return 500;
		if (Kind == TEXT("Sector"))       return 600;
		if (Kind == TEXT("AreaTrigger"))  return 700;
		if (Kind == TEXT("SpatialObject")) return 800;
		return 900;
	}

	int32 NextFreeObjId(const FString& Kind)
	{
		TSet<int32> Taken;
		for (const BF6Api::FObjIdRow& R : BF6Api::GatherObjIds())
		{
			if (R.Id >= 0) Taken.Add(R.Id);
		}
		const int32 Base = BandFor(Kind);
		for (int32 i = Base; i < Base + 999; i++) if (!Taken.Contains(i)) return i;
		return Base;
	}
}

// ============================================================================
// The object seams
// ============================================================================
int32 BF6Blocks::SelectActorsByObjId(const TArray<int32>& ObjIds)
{
	if (!ObjIds.Num()) return 0;
	TArray<AActor*> Hits;
	for (const BF6Api::FObjIdRow& R : BF6Api::GatherObjIds())
	{
		if (R.Id >= 0 && ObjIds.Contains(R.Id) && R.Actor.IsValid()) Hits.Add(R.Actor.Get());
	}
	if (!Hits.Num()) return 0;
	// SelectOnly is the tool's own exclusive select, so the first hit lands the
	// same way a click in the scene tree does.
	BF6Api::SelectOnly(Hits[0]);
	return Hits.Num();
}

bool BF6Blocks::SelectedObjId(const FString& Kind, int32& OutObjId, FString& OutName, bool bAssignIfMissing)
{
	TArray<AActor*> Sel;
	BF6Ext::Selection(Sel);
	if (!Sel.Num()) return false;
	AActor* A = Sel[0];
	if (!A) return false;
	OutName = BF6Ext::ObjectLinkName(A);
	if (OutName.IsEmpty()) OutName = A->GetActorNameOrLabel();
	const FString Cur = BF6Api::GetActorProp(A, TEXT("ObjId"));
	const int32 CurId = FCString::Atoi(*Cur);
	if (!Cur.IsEmpty() && CurId > 0) { OutObjId = CurId; return true; }
	if (!bAssignIfMissing) return false;
	OutObjId = NextFreeObjId(Kind);
	BF6Api::SetActorProp(A, TEXT("ObjId"), FString::FromInt(OutObjId));
	UE_LOG(LogBF6Blocks, Display, TEXT("Assigned ObjId %d to %s for a %s block."),
		OutObjId, *OutName, *Kind);
	return true;
}

bool BF6Blocks::AssignObjIdToSelected(int32 ObjId, FString& OutName)
{
	TArray<AActor*> Sel;
	BF6Ext::Selection(Sel);
	if (!Sel.Num() || !Sel[0]) return false;
	AActor* A = Sel[0];
	OutName = BF6Ext::ObjectLinkName(A);
	if (OutName.IsEmpty()) OutName = A->GetActorNameOrLabel();
	BF6Api::SetActorProp(A, TEXT("ObjId"), FString::FromInt(ObjId));
	return true;
}

// ============================================================================
// Messages
// ============================================================================
namespace
{
	// The Blocks editor's own game-log subscription. Held per consumer so that
	// switching Watch off here cannot stop the LOG section's watch.
	FGuid GBlocksLogWatch;

	void SendObjIds()
	{
		TSharedRef<FJsonObject> M = MakeShared<FJsonObject>();
		M->SetStringField(TEXT("op"), TEXT("objIds"));
		TArray<TSharedPtr<FJsonValue>> Rows;
		for (const BF6Api::FObjIdRow& R : BF6Api::GatherObjIds())
		{
			TSharedRef<FJsonObject> Row = MakeShared<FJsonObject>();
			Row->SetNumberField(TEXT("id"), R.Id);
			Row->SetStringField(TEXT("name"), R.Name);
			Row->SetStringField(TEXT("type"), R.Type);
			Rows.Add(MakeShared<FJsonValueObject>(Row));
		}
		M->SetArrayField(TEXT("rows"), Rows);
		ToTool(M);
	}

	void SendSnippetList()
	{
		TArray<FString> Files;
		const FString Dir = FPaths::Combine(BlocksResDir(), TEXT("snippets"));
		IFileManager::Get().FindFilesRecursive(Files, *Dir, TEXT("*.json"), true, false);
		TSharedRef<FJsonObject> M = MakeShared<FJsonObject>();
		M->SetStringField(TEXT("op"), TEXT("snippetList"));
		TArray<TSharedPtr<FJsonValue>> Names;
		for (const FString& F : Files)
		{
			FString Rel = F;
			FPaths::MakePathRelativeTo(Rel, *(Dir + TEXT("/")));
			Names.Add(MakeShared<FJsonValueString>(Rel));
		}
		M->SetArrayField(TEXT("names"), Names);
		ToTool(M);
	}

	void SendSnippet(const FString& Name)
	{
		const FString Path = FPaths::Combine(BlocksResDir(), TEXT("snippets"), Name);
		FString Text;
		if (!FFileHelper::LoadFileToString(Text, *Path))
		{
			GLastError = FString::Printf(TEXT("snippet not found: %s"), *Name);
			ToolNote(TEXT("status"), TEXT("text"), GLastError);
			return;
		}
		ToToolRaw(FString::Printf(TEXT("{\"op\":\"snippet\",\"name\":\"%s\",\"json\":%s}"),
			*Name.Replace(TEXT("\\"), TEXT("/")), *Text));
	}

	void AskSiteForWorkspace()
	{
		TSharedRef<FJsonObject> M = MakeShared<FJsonObject>();
		M->SetStringField(TEXT("op"), TEXT("getWorkspace"));
		ToSite(M);
	}

	// The site page has to be ON the blocks page for any of this to work. If it
	// is somewhere else, take it there and wait for site_sync.js to say hello.
	void EnsureSiteOnBlocksPage()
	{
		const FString Url = BF6PortalWeb::CurrentUrl();
		if (Url.Contains(TEXT("/rules/blocks"))) return;
		// Same experience, different section: swap the section and keep the id.
		FString Target = Url.Replace(TEXT("/rules/typescript"), TEXT("/rules/blocks"));
		if (!Target.Contains(TEXT("/rules/blocks")))
		{
			ToolNote(TEXT("status"), TEXT("text"),
				TEXT("Open your experience's BLOCKS page in the Portal panel, then press PULL FROM SITE."));
			return;
		}
		BF6PortalWeb::Open(Target);
	}

	void HandleFromTool(const TSharedPtr<FJsonObject>& M, const FString& Op, const FString& Raw)
	{
		GFromTool++;
		if (Op == TEXT("ready"))
		{
			GToolReady = true;
			if (!SendCachedDefinitions()) GDefsSource = TEXT("none");
			SendStyle();
			SendConvertData();
			SendAnswers();
			SendFaq();
			SendSnippetList();
			SendObjIds();
			SendPrefs();
			SendProjectWorkspace();
			if (!GPendingLootBinding.IsEmpty())
			{
				if (GPendingLootSave == BF6Api::CurrentSave() && GPendingLootLevel == BF6Api::CurrentLevel()) ToToolRaw(GPendingLootBinding);
				else BF6Ext::Notify(TEXT("The map changed before Blocks opened. Select the spawner and connect it again."));
				GPendingLootBinding.Reset(); GPendingLootSave.Reset(); GPendingLootLevel.Reset();
			}
			FetchDefinitions(false);
			TSharedRef<FJsonObject> C = MakeShared<FJsonObject>();
			C->SetStringField(TEXT("op"), TEXT("connected"));
			C->SetBoolField(TEXT("connected"), GSiteReady);
			// TWO DIFFERENT THINGS, AND THE PILL USED TO SHOW ONLY ONE.
			//
			// "connected" means the Portal blocks page has said hello INSIDE the
			// tool's own web panel. It says nothing about whether the user is
			// signed in, so somebody signed in to Portal with the panel closed,
			// or open on a different page, was told "SITE OFFLINE" and reasonably
			// read it as a lost connection. The sign-in state travels with it now
			// so the page can say which of the two is actually missing.
			C->SetStringField(TEXT("signIn"), BF6PortalProfile::StateLabel());
			ToTool(C);
			if (GSiteReady) AskSiteForWorkspace();
			return;
		}
		if (Op == TEXT("perfmine"))
		{
			IFileManager::Get().MakeDirectory(*CacheDir(), true);
			FFileHelper::SaveStringToFile(Raw, *FPaths::Combine(CacheDir(), TEXT("perf_ours.json")),
				FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
			return;
		}
		if (Op == TEXT("lootBindingResult"))
		{
			FString Message; M->TryGetStringField(TEXT("message"), Message); BF6Ext::Notify(Message); return;
		}
		if (Op == TEXT("diagnosemine"))
		{
			IFileManager::Get().MakeDirectory(*CacheDir(), true);
			FFileHelper::SaveStringToFile(Raw, *FPaths::Combine(CacheDir(), TEXT("diagnose_ours.json")),
				FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
			UE_LOG(LogBF6Blocks, Display, TEXT("Our diagnosis written (%d chars)."), Raw.Len());
			return;
		}
		if (Op == TEXT("fieldmine"))
		{
			IFileManager::Get().MakeDirectory(*CacheDir(), true);
			FFileHelper::SaveStringToFile(Raw,
				*FPaths::Combine(CacheDir(), TEXT("fields_ours.json")),
				FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
			UE_LOG(LogBF6Blocks, Display, TEXT("How we paint the same fields: %s (%d chars)."),
				*FPaths::Combine(CacheDir(), TEXT("fields_ours.json")), Raw.Len());
			return;
		}
		if (Op == TEXT("sweepmine"))
		{
			IFileManager::Get().MakeDirectory(*CacheDir(), true);
			FFileHelper::SaveStringToFile(Raw,
				*FPaths::Combine(CacheDir(), TEXT("sweep_ours.json")),
				FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
			UE_LOG(LogBF6Blocks, Display,
				TEXT("Every block on our canvas was measured into %s (%d chars)."),
				*FPaths::Combine(CacheDir(), TEXT("sweep_ours.json")), Raw.Len());
			return;
		}
		if (Op == TEXT("layoutmine"))
		{
			IFileManager::Get().MakeDirectory(*CacheDir(), true);
			FFileHelper::SaveStringToFile(Raw,
				*FPaths::Combine(CacheDir(), TEXT("layout_ours.json")),
				FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
			UE_LOG(LogBF6Blocks, Display,
				TEXT("How we measure the same blocks was written to %s (%d chars)."),
				*FPaths::Combine(CacheDir(), TEXT("layout_ours.json")), Raw.Len());
			return;
		}
		if (Op == TEXT("toolboxmine"))
		{
			// OUR side of the same measurement. Written next to the site's so the
			// two can be put side by side: same tape measure, same schema, and the
			// only differences left in the pair are real ones.
			IFileManager::Get().MakeDirectory(*CacheDir(), true);
			FFileHelper::SaveStringToFile(Raw,
				*FPaths::Combine(CacheDir(), TEXT("toolbox_ours.json")),
				FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
			UE_LOG(LogBF6Blocks, Display,
				TEXT("Our own toolbox was measured the same way and written to %s (%d chars)."),
				*FPaths::Combine(CacheDir(), TEXT("toolbox_ours.json")), Raw.Len());
			return;
		}
		// ---- sounds -------------------------------------------------------
		//
		// A block that picks a sound is a dropdown of the same names the script
		// editor searches, so the same two paths serve both: the game itself
		// through the High Poly add-on, or a recorded clip handed back for the
		// page to play. Neither is duplicated here - BF6Script owns both.
		if (Op == TEXT("sfxsource"))
		{
			TSharedRef<FJsonObject> O = MakeShared<FJsonObject>();
			O->SetStringField(TEXT("op"), TEXT("sfxsource"));
			O->SetStringField(TEXT("source"), BF6Script::SoundPreviewSource());
			ToTool(O);
			return;
		}
		if (Op == TEXT("sfxplay"))
		{
			FString Name;
			M->TryGetStringField(TEXT("name"), Name);
			FString Why, B64;
			const bool bAddon = BF6Script::SoundPreviewSource() == TEXT("addon");
			const bool bOk = bAddon
				? BF6Script::PlayPlaceableSound(Name, Why)
				: BF6Script::LoadSoundClipBase64(Name, B64, Why);
			TSharedRef<FJsonObject> O = MakeShared<FJsonObject>();
			O->SetStringField(TEXT("op"), TEXT("sfxplayed"));
			O->SetStringField(TEXT("name"), Name);
			O->SetBoolField(TEXT("ok"), bOk);
			O->SetStringField(TEXT("why"), Why);
			// Empty when the add-on played it itself: there is nothing for the
			// page to do in that case, and shipping the audio anyway would send a
			// megabyte across the bridge for nothing.
			O->SetStringField(TEXT("audio"), B64);
			ToTool(O);
			return;
		}
		if (Op == TEXT("sfxstop"))
		{
			BF6Script::StopPlaceableSound();
			return;
		}

		if (Op == TEXT("autosave"))
		{
			FString Project; M->TryGetStringField(TEXT("project"), Project);
			if (GProjectRecoveryDirs.Contains(Project) && !WriteProjectRecovery(M, false))
				ToolNote(TEXT("status"), TEXT("text"), TEXT("Could not save block recovery data. Export the workspace before closing."));
			WriteAutosave(Raw);
			return;
		}
		if (Op == TEXT("projectRecovery"))
		{
			if (!WriteProjectRecovery(M, false)) ToolNote(TEXT("status"), TEXT("text"), TEXT("Could not save block recovery data. Export the workspace before closing."));
			return;
		}
		if (Op == TEXT("updateWorkspace"))
		{
			FString Token; M->TryGetStringField(TEXT("token"), Token);
			if (!GUpdateSaved || Token != GUpdateSaveToken) return;
			const bool bSaved = WriteProjectRecovery(M, true);
			FTSTicker::GetCoreTicker().RemoveTicker(GUpdateSaveTimeout); GUpdateSaveTimeout.Reset();
			auto Done = MoveTemp(GUpdateSaved); GUpdateSaveToken.Reset(); Done(bSaved);
			return;
		}
		if (Op == TEXT("pref"))
		{
			FString Name, Value;
			M->TryGetStringField(TEXT("name"), Name);
			M->TryGetStringField(TEXT("value"), Value);
			SetBlocksPref(Name, Value);
			return;
		}
		if (Op == TEXT("captureStyle"))
		{
			AskSiteForStyle(TEXT("button"));
			return;
		}
		// ---- import and export ---------------------------------------------
		// BLOCKS OUT AS A SCRIPT PROJECT. The page has the converter and does the
		// conversion; this only chooses a folder and writes what it was handed,
		// so there is one converter and it is the same one the tests drive.
		// WATCH THE GAME'S LOG, FROM THE BLOCKS SIDE TOO.
		//
		// The Script editor has had this button since it shipped. Somebody
		// working in blocks has exactly the same question after a playtest -
		// what did my mod actually print - and had to change tabs to ask it.
		//
		// One watcher, not two: BF6GameLog owns the tail and this just
		// subscribes, so turning it on in both places does not read the file
		// twice or double up the lines.
		if (Op == TEXT("gameLogWatch"))
		{
			bool bOn = true;
			M->TryGetBoolField(TEXT("on"), bOn);
			if (!bOn)
			{
				// Drop only this editor's subscription. Stopping the shared
				// poll here used to switch off the LOG section's watch too.
				BF6GameLog::RemoveWatcher(GBlocksLogWatch);
				GBlocksLogWatch.Invalidate();
				ToolNote(TEXT("status"), TEXT("text"), TEXT("stopped watching the game's log"));
				return;
			}
			if (GBlocksLogWatch.IsValid()) { return; }   // already watching from here
			const FString Folder = BF6GameLog::LocateFolder();
			GBlocksLogWatch = BF6GameLog::AddWatcher(BF6GameLog::FOnLines::CreateLambda(
				[](const TArray<BF6GameLog::FEntry>& Entries, bool bNewSession)
				{
					TArray<TSharedPtr<FJsonValue>> Rows;
					for (const BF6GameLog::FEntry& E : Entries)
					{
						TSharedRef<FJsonObject> R = MakeShared<FJsonObject>();
						R->SetStringField(TEXT("at"), E.TimestampUtc);
						R->SetStringField(TEXT("kind"), E.Kind);
						R->SetStringField(TEXT("text"), E.Text);
						Rows.Add(MakeShared<FJsonValueObject>(R));
					}
					TSharedRef<FJsonObject> O = MakeShared<FJsonObject>();
					O->SetStringField(TEXT("op"), TEXT("gameLogLines"));
					O->SetBoolField(TEXT("newSession"), bNewSession);
					O->SetArrayField(TEXT("entries"), Rows);
					ToTool(O);
				}), 1.0f);
			ToolNote(TEXT("status"), TEXT("text"), Folder.IsEmpty()
				? TEXT("watching for the game's log. It appears once a mod runs on Host Locally.")
				: FString::Printf(TEXT("watching %s"), *(Folder / TEXT("PortalLog.txt"))));
			return;
		}

		if (Op == TEXT("gameLogPull"))
		{
			int32 Want = 200;
			double D = 0.0;
			if (M->TryGetNumberField(TEXT("lines"), D)) { Want = FMath::Clamp((int32)D, 1, 20000); }
			const BF6GameLog::FResult R = BF6GameLog::ReadLocal(Want);
			TArray<TSharedPtr<FJsonValue>> Rows;
			for (const BF6GameLog::FEntry& E : R.Entries)
			{
				TSharedRef<FJsonObject> Row = MakeShared<FJsonObject>();
				Row->SetStringField(TEXT("at"), E.TimestampUtc);
				Row->SetStringField(TEXT("kind"), E.Kind);
				Row->SetStringField(TEXT("text"), E.Text);
				Rows.Add(MakeShared<FJsonValueObject>(Row));
			}
			TSharedRef<FJsonObject> O = MakeShared<FJsonObject>();
			O->SetStringField(TEXT("op"), TEXT("gameLogLines"));
			O->SetBoolField(TEXT("replace"), true);
			O->SetBoolField(TEXT("found"), R.bFound);
			O->SetStringField(TEXT("path"), R.Path);
			O->SetStringField(TEXT("why"), R.Why);
			O->SetArrayField(TEXT("entries"), Rows);
			ToTool(O);
			return;
		}

		if (Op == TEXT("exportPortalScript"))
		{
			const TSharedPtr<FJsonObject>* Files=nullptr;
			if (!M->TryGetObjectField(TEXT("files"),Files) || !Files || !Files->IsValid())
			{ ToolNote(TEXT("status"),TEXT("text"),TEXT("The block compiler produced no source files.")); return; }
			FString Dir; M->TryGetStringField(TEXT("dir"),Dir);
			if (Dir.IsEmpty() && !PickFolder(TEXT("Choose where to put the Portal upload files"),Dir))
			{ ToolNote(TEXT("status"),TEXT("text"),TEXT("Portal export cancelled.")); return; }
			FString Why;
			if (!BF6BlocksExport::Start(Files->ToSharedRef(),Dir,[](const FString& Text,bool Bad)
			{
				if (Bad) GLastError=Text;
				ToolNote(TEXT("status"),TEXT("text"),Text);
			},Why)) { GLastError=Why; ToolNote(TEXT("status"),TEXT("text"),Why); BF6Ext::Notify(Why); }
			return;
		}
		if (Op == TEXT("exportScript"))
		{
			const TSharedPtr<FJsonObject>* Files = nullptr;
			if (!M->TryGetObjectField(TEXT("files"), Files) || !Files || !(*Files).IsValid()
				|| (*Files)->Values.Num() == 0)
			{
				GLastError = TEXT("the script export produced no files");
				ToolNote(TEXT("status"), TEXT("text"), GLastError);
				return;
			}
			FString Dir;
			M->TryGetStringField(TEXT("dir"), Dir);
			if (Dir.IsEmpty() && !PickFolder(TEXT("Export the blocks as a script project"), Dir))
			{
				ToolNote(TEXT("status"), TEXT("text"), TEXT("script export cancelled"));
				return;
			}
			IFileManager::Get().MakeDirectory(*Dir, true);

			// NOTHING THIS TOOL DID NOT WRITE GETS OVERWRITTEN.
			//
			// The export used to write its files into the chosen folder and
			// count them. Export into a project that already has a handwritten
			// src/index.ts and that file was replaced by generated output, with
			// the only trace a number in a status line. The project contract is
			// explicit that generated files and handwritten ones have different
			// owners and that regenerating must never rewrite the latter.
			//
			// A manifest beside the output records what this tool wrote, so a
			// second export over its own previous output is fine and an export
			// over somebody's work is refused.
			//
			// Refused as a WHOLE, before anything is written. A partial export
			// is a project that compiles into a mixture of two programs, which
			// is harder to recover from than one that never started.
			// A NAME IS NOT CONTINUING PERMISSION. The manifest used to record
			// only what this tool had written, by filename, so a generated file
			// the user had since edited was still counted as ours and silently
			// replaced. Being generated once does not make somebody's later work
			// disposable. Each entry therefore carries a hash of what we wrote,
			// and a file whose content no longer matches is treated as theirs.
			const FString ManifestPath = FPaths::Combine(Dir, TEXT(".bf6-generated.json"));
			TMap<FString, FString> OursHash;   // relative path -> hash we wrote, "" when unknown
			auto HashOf = [](const FString& Text)
			{
				return FMD5::HashAnsiString(*Text);
			};
			auto HashOfFile = [&HashOf](const FString& Path)
			{
				FString Text;
				return FFileHelper::LoadFileToString(Text, *Path) ? HashOf(Text) : FString();
			};
			{
				FString ManifestText;
				if (FFileHelper::LoadFileToString(ManifestText, *ManifestPath))
				{
					TSharedPtr<FJsonObject> MO;
					TSharedRef<TJsonReader<>> R = TJsonReaderFactory<>::Create(ManifestText);
					const TArray<TSharedPtr<FJsonValue>>* Wrote0 = nullptr;
					if (FJsonSerializer::Deserialize(R, MO) && MO.IsValid() &&
						MO->TryGetArrayField(TEXT("files"), Wrote0) && Wrote0)
					{
						for (const TSharedPtr<FJsonValue>& V : *Wrote0)
						{
							if (!V.IsValid()) { continue; }
							// Manifests written before hashes existed hold plain
							// strings. They are honoured once, and this export
							// rewrites them with hashes, so the protection starts
							// applying from here on. That one export is the only
							// window, and it is the price of not refusing every
							// project that already has a manifest.
							FString S;
							if (V->TryGetString(S)) { OursHash.Add(S, FString()); continue; }
							const TSharedPtr<FJsonObject> O = V->AsObject();
							if (!O.IsValid()) { continue; }
							FString P2, H2;
							if (O->TryGetStringField(TEXT("path"), P2) && !P2.IsEmpty())
							{
								O->TryGetStringField(TEXT("sha"), H2);
								OursHash.Add(P2, H2);
							}
						}
					}
				}
			}

			TArray<FString> Clobber;     // files this tool never wrote
			TArray<FString> Edited;      // files it wrote that have since been changed
			for (const TPair<FString, TSharedPtr<FJsonValue>>& P : (*Files)->Values)
			{
				FString Rel0 = P.Key;
				Rel0.ReplaceInline(TEXT("\\"), TEXT("/"));
				const FString Full0 = FPaths::Combine(Dir, Rel0);
				if (const FString* Known = OursHash.Find(Rel0))
				{
					if (Known->IsEmpty()) { continue; }          // legacy entry, honoured once
					if (!FPaths::FileExists(Full0)) { continue; } // ours and since deleted
					if (HashOfFile(Full0) != *Known) { Edited.Add(Rel0); }
					continue;
				}
				if (FPaths::FileExists(Full0)) { Clobber.Add(Rel0); }
			}
			if (Clobber.Num() || Edited.Num())
			{
				GLastError = TEXT("Nothing was exported. ");
				if (Clobber.Num())
				{
					GLastError += FString::Printf(
						TEXT("That folder has %d file(s) this tool did not write: %s. "),
						Clobber.Num(), *FString::Join(Clobber, TEXT(", ")));
				}
				if (Edited.Num())
				{
					GLastError += FString::Printf(
						TEXT("%d file(s) this tool generated have been edited since: %s. "),
						Edited.Num(), *FString::Join(Edited, TEXT(", ")));
				}
				GLastError += TEXT("Move them aside, or export somewhere else, if you want them replaced.");
				UE_LOG(LogBF6Blocks, Warning, TEXT("script export refused: %s"), *GLastError);
				ToolNote(TEXT("status"), TEXT("text"), GLastError);
				return;
			}

			int32 Wrote = 0;
			TArray<FString> Failed;
			TArray<FString> WroteRel;
			for (const TPair<FString, TSharedPtr<FJsonValue>>& P : (*Files)->Values)
			{
				FString Body;
				if (!P.Value.IsValid() || !P.Value->TryGetString(Body)) continue;

				// A NAME FROM THE PAGE STILL NEVER ESCAPES THE CHOSEN FOLDER.
				//
				// This used to keep only the leaf, which made that guarantee
				// trivially true and also made it impossible to write the one
				// layout the template needs: src/index.ts and src/strings.json
				// both collapsed onto the project root, and a project scaffolded
				// that way does not build.
				//
				// So a relative subpath is now allowed and the guarantee is
				// enforced properly instead of by mutilation: reject anything
				// rooted or containing a parent segment, collapse what is left,
				// and then verify the resolved path really is under Dir. The
				// last check is the one that counts, because it holds even if
				// the earlier ones miss a spelling.
				FString Rel = P.Key;
				Rel.ReplaceInline(TEXT("\\"), TEXT("/"));
				while (Rel.StartsWith(TEXT("/"))) Rel.RightChopInline(1);
				if (Rel.IsEmpty() || FPaths::IsRelative(Rel) == false ||
					Rel.Contains(TEXT("..")) || Rel.Contains(TEXT(":")))
				{
					Failed.Add(P.Key);
					continue;
				}
				const FString Full = FPaths::ConvertRelativePathToFull(FPaths::Combine(Dir, Rel));
				const FString Root = FPaths::ConvertRelativePathToFull(Dir);
				if (!FPaths::IsUnderDirectory(Full, Root))
				{
					UE_LOG(LogBF6Blocks, Warning,
						TEXT("refused a script export path outside the chosen folder: %s"), *P.Key);
					Failed.Add(P.Key);
					continue;
				}
				IFileManager::Get().MakeDirectory(*FPaths::GetPath(Full), true);
				if (FFileHelper::SaveStringToFile(Body, *Full,
					FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
				{
					Wrote++;
					WroteRel.Add(Rel);
					// The hash of what we actually wrote, so the next export can
					// tell our own output from a file somebody has since edited.
					OursHash.Add(Rel, HashOf(Body));
				}
				else Failed.Add(Rel);
			}
			// Record what we wrote, so exporting again over our own output is
			// allowed and exporting over anything else is not.
			{
				TArray<TSharedPtr<FJsonValue>> Vals;
				TSet<FString> Seen;
				auto AddEntry = [&Vals, &Seen, &OursHash](const FString& Rel2)
				{
					if (Seen.Contains(Rel2)) { return; }
					Seen.Add(Rel2);
					TSharedRef<FJsonObject> E = MakeShared<FJsonObject>();
					E->SetStringField(TEXT("path"), Rel2);
					const FString* H = OursHash.Find(Rel2);
					E->SetStringField(TEXT("sha"), H ? *H : FString());
					Vals.Add(MakeShared<FJsonValueObject>(E));
				};
				for (const FString& R2 : WroteRel) { AddEntry(R2); }
				for (const TPair<FString, FString>& It : OursHash) { AddEntry(It.Key); }
				TSharedRef<FJsonObject> MO = MakeShared<FJsonObject>();
				MO->SetStringField(TEXT("writtenBy"), TEXT("BF6 Unreal SDK block export"));
				MO->SetNumberField(TEXT("format"), 2);   // 1 was bare filenames
				MO->SetArrayField(TEXT("files"), Vals);
				FString Out2;
				TSharedRef<TJsonWriter<>> W2 = TJsonWriterFactory<>::Create(&Out2);
				FJsonSerializer::Serialize(MO, W2);
				// Checked: a manifest that failed to write leaves the next export
				// unable to tell its own output from the user's, and the safe
				// reading of that is to say so now rather than discover it later.
				if (!FFileHelper::SaveStringToFile(Out2, *ManifestPath,
					FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
				{
					UE_LOG(LogBF6Blocks, Warning,
						TEXT("The files were written but %s could not be. The next export will not be able ")
						TEXT("to tell which files it generated, and will refuse rather than overwrite them."),
						*ManifestPath);
					Failed.Add(TEXT(".bf6-generated.json"));
				}
			}
			UE_LOG(LogBF6Blocks, Display, TEXT("Blocks exported as a script project: %d file(s) into %s%s"),
				Wrote, *Dir, Failed.Num() ? *(TEXT(" - could not write ") + FString::Join(Failed, TEXT(", "))) : TEXT(""));
			ToolNote(TEXT("status"), TEXT("text"), Failed.Num()
				? FString::Printf(TEXT("Source export incomplete: wrote %d file(s); failed: %s. Check folder access and available disk space."), Wrote, *FString::Join(Failed, TEXT(", ")))
				: FString::Printf(TEXT("Wrote %d source file(s) to %s. Use Export for Portal for the uploadable script."), Wrote, *Dir));
			return;
		}
		// AND A WHOLE FOLDER BACK IN, because the export it reads is eleven
		// files and a script that only ever sees one of them is not the script.
		if (Op == TEXT("openImportScript"))
		{
			FString Dir;
			if (!PickFolder(TEXT("Import a script project folder"), Dir))
			{
				ToolNote(TEXT("status"), TEXT("text"), TEXT("script import cancelled"));
				return;
			}
			TArray<FString> Found;
			// Prune dependency/build directories before walking them. A normal npm
			// project contains thousands of declarations that are not mode logic.
			TArray<FString> Pending { Dir };
			int32 Visited = 0;
			int64 TotalBytes = 0;
			FString ImportError;
			while (Pending.Num() && ImportError.IsEmpty())
			{
				const FString Folder = Pending.Pop();
				if (++Visited > 4096) { ImportError = TEXT("Too many folders. Select the mode's src folder instead."); break; }
				TArray<FString> Children;
				IFileManager::Get().FindFiles(Children, *(Folder / TEXT("*")), false, true);
				for (const FString& Child : Children)
				{
					const FString Name = Child.ToLower();
					if (Name.StartsWith(TEXT(".")) || Name == TEXT("node_modules") || Name == TEXT("dist") ||
						Name == TEXT("build") || Name == TEXT("coverage")) continue;
					Pending.Add(Folder / Child);
				}
				TArray<FString> Scripts;
				IFileManager::Get().FindFiles(Scripts, *(Folder / TEXT("*.ts")), true, false);
				for (const FString& Script : Scripts)
				{
					if (Script.EndsWith(TEXT(".d.ts"))) continue;
					const FString Full = Folder / Script;
					const int64 Size = IFileManager::Get().FileSize(*Full);
					TotalBytes += FMath::Max<int64>(0, Size);
					if (Size < 0 || Found.Num() >= 512 || TotalBytes > 32 * 1024 * 1024)
					{ ImportError = TEXT("The source project is unreadable or exceeds 512 files / 32 MB. Select the mode's src folder."); break; }
					Found.Add(Full);
				}
			}
			if (!ImportError.IsEmpty())
			{ GLastError = ImportError; ToolNote(TEXT("status"), TEXT("text"), GLastError); return; }
			if (Found.Num() == 0)
			{
				GLastError = FString::Printf(TEXT("no .ts files under %s"), *Dir);
				ToolNote(TEXT("status"), TEXT("text"), GLastError);
				return;
			}
			TSharedRef<FJsonObject> Files = MakeShared<FJsonObject>();
			int32 Read = 0;
			for (const FString& F : Found)
			{
				FString Body;
				if (!FFileHelper::LoadFileToString(Body, *F))
				{
					GLastError = FString::Printf(TEXT("Could not read %s. Import stopped to avoid losing mode logic."), *F);
					ToolNote(TEXT("status"), TEXT("text"), GLastError); return;
				}
				FString Rel = F;
				FPaths::MakePathRelativeTo(Rel, *(Dir / TEXT("")));
				Files->SetStringField(Rel.IsEmpty() ? FPaths::GetCleanFilename(F) : Rel, Body);
				Read++;
			}
			TSharedRef<FJsonObject> R = MakeShared<FJsonObject>();
			R->SetStringField(TEXT("op"), TEXT("scriptFolder"));
			R->SetStringField(TEXT("dir"), Dir);
			GLastImportDir = Dir;
			R->SetObjectField(TEXT("files"), Files);
			ToTool(R);
			UE_LOG(LogBF6Blocks, Display, TEXT("Script project handed to the editor: %d file(s) from %s"),
				Read, *Dir);
			return;
		}
		if (Op == TEXT("openImport"))
		{
			FString Path;
			if (!OpenImportDialog(Path))
			{
				ToolNote(TEXT("status"), TEXT("text"), TEXT("import cancelled"));
				return;
			}
			ImportPath(Path, TEXT("dialog"));
			return;
		}
		if (Op == TEXT("needExperience"))
		{
			// What the tool honestly knows about the linked experience, for the
			// fields a full export has to fill. Everything here came out of a
			// response the panel already received; nothing is invented.
			TSharedRef<FJsonObject> R = MakeShared<FJsonObject>();
			R->SetStringField(TEXT("op"), TEXT("experience"));
			R->SetStringField(TEXT("id"), GExperienceId);
			bool bFound = false;
			if (!GExperienceId.IsEmpty())
			{
				for (const BF6PortalProfile::FExperienceRow& E : BF6PortalProfile::List())
				{
					if (E.Id != GExperienceId) continue;
					R->SetStringField(TEXT("name"), E.Name);
					R->SetStringField(TEXT("description"), E.Description);
					bFound = true;
					break;
				}
				TArray<TSharedPtr<FJsonValue>> Maps;
				for (const BF6PortalProfile::FRotationRow& Row : BF6PortalProfile::RotationFor(GExperienceId))
				{
					TSharedRef<FJsonObject> M = MakeShared<FJsonObject>();
					M->SetStringField(TEXT("map"), Row.Map);
					M->SetNumberField(TEXT("index"), Row.MapIdx);
					M->SetStringField(TEXT("save"), Row.SaveName);
					M->SetBoolField(TEXT("hasSpatial"), Row.bHasSpatial);
					// The tool's own most recent export for that map, which is
					// what a full export should attach.
					const FString Spatial = FPaths::Combine(FPaths::ProjectSavedDir(),
						TEXT("BF6UnrealSDK"), TEXT("export"), Row.Map + TEXT(".spatial.json"));
					if (FPaths::FileExists(Spatial))
					{
						FString Text;
						if (FFileHelper::LoadFileToString(Text, *Spatial))
						{
							M->SetStringField(TEXT("spatialPath"), Spatial);
							M->SetStringField(TEXT("spatialText"), Text);
							M->SetStringField(TEXT("spatialFilename"), Row.Map + TEXT(".spatial.json"));
						}
					}
					Maps.Add(MakeShared<FJsonValueObject>(M));
				}
				R->SetArrayField(TEXT("maps"), Maps);
			}
			R->SetBoolField(TEXT("known"), bFound);
			// Settings live in BF6PortalSettings, which decodes the experience's
			// mutators but hands nothing out. One accessor would fill the
			// mutators and assetRestrictions of a full export from the live
			// experience instead of from whatever file was imported:
			//     bool BF6PortalSettings::ExperienceMutators(
			//         const FString& ExperienceId, TSharedPtr<FJsonObject>& Out);
			// Until then a full export writes what the imported experience had,
			// or the site's defaults, and the export summary says which.
			R->SetBoolField(TEXT("hasSettings"), false);
			ToTool(R);
			return;
		}
		if (Op == TEXT("exportFile"))
		{
			FString Format, Path, Suggested, Text;
			M->TryGetStringField(TEXT("format"), Format);
			M->TryGetStringField(TEXT("path"), Path);
			M->TryGetStringField(TEXT("suggested"), Suggested);
			const TSharedPtr<FJsonValue> Value = M->TryGetField(TEXT("json"));
			if (Value.IsValid())
			{
				// CONDENSED, AND THE REASON IS NOT TIDINESS.
				//
				// TJsonWriterFactory<> defaults to the pretty policy, which
				// indents one tab PER NESTING LEVEL. Blocks nest through inputs
				// and next, so a real mod is hundreds of levels deep and every
				// line at that depth carries hundreds of tabs. Saving Undead
				// Ground Zero wrote a 34 MB file to hold 358 KB of JSON, which
				// is what made SaveFile look broken. Nothing reads this by eye,
				// and the site's own export is condensed too.
				TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> W =
					TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Text);
				FJsonSerializer::Serialize(Value.ToSharedRef(), FString(), W);
			}
			else
			{
				M->TryGetStringField(TEXT("text"), Text);
			}
			if (Text.IsEmpty())
			{
				GLastError = TEXT("the export produced nothing");
				ToolNote(TEXT("status"), TEXT("text"), GLastError);
				return;
			}
			if (Path.IsEmpty())
			{
				if (Suggested.IsEmpty()) Suggested = (Format == TEXT("experience"))
					? TEXT("experience.json") : TEXT("workspace.json");
				if (!SaveDialog(Format == TEXT("experience")
					? TEXT("Export the full experience") : TEXT("Export the workspace"),
					Suggested, Path))
				{
					ToolNote(TEXT("status"), TEXT("text"), TEXT("export cancelled"));
					return;
				}
			}
			const bool bOk = FFileHelper::SaveStringToFile(Text, *Path,
				FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
			UE_LOG(LogBF6Blocks, Display, TEXT("%s %s (%s, %d characters)"),
				bOk ? TEXT("Wrote") : TEXT("FAILED to write"), *Path,
				Format.IsEmpty() ? TEXT("workspace") : *Format, Text.Len());
			TSharedRef<FJsonObject> R = MakeShared<FJsonObject>();
			R->SetStringField(TEXT("op"), TEXT("exportResult"));
			R->SetBoolField(TEXT("ok"), bOk);
			R->SetStringField(TEXT("path"), Path);
			R->SetStringField(TEXT("format"), Format);
			R->SetNumberField(TEXT("bytes"), Text.Len());
			ToTool(R);
			if (!bOk) GLastError = FString::Printf(TEXT("could not write %s"), *Path);
			return;
		}
		if (Op == TEXT("importAttachments"))
		{
			const TArray<TSharedPtr<FJsonValue>>* List = nullptr;
			TArray<TSharedPtr<FJsonValue>> Done;
			int32 n = 0;
			if (M->TryGetArrayField(TEXT("attachments"), List) && List)
			{
				for (const TSharedPtr<FJsonValue>& V : *List)
				{
					const TSharedPtr<FJsonObject> A = V->AsObject();
					if (!A.IsValid()) continue;
					FString Filename, Text;
					double Type = 0, MapIdx = 0;
					A->TryGetStringField(TEXT("filename"), Filename);
					A->TryGetStringField(TEXT("text"), Text);
					A->TryGetNumberField(TEXT("attachmentType"), Type);
					A->TryGetNumberField(TEXT("mapIdx"), MapIdx);
					FString What;
					const FString Written = RouteAttachment(Filename, (int32)Type, Text, (int32)MapIdx, What);
					TSharedRef<FJsonObject> R = MakeShared<FJsonObject>();
					R->SetStringField(TEXT("filename"), Filename);
					R->SetStringField(TEXT("path"), Written);
					R->SetStringField(TEXT("what"), What);
					Done.Add(MakeShared<FJsonValueObject>(R));
					UE_LOG(LogBF6Blocks, Display, TEXT("Attachment %s: %s"), *Filename, *What);
					n++;
				}
			}
			TSharedRef<FJsonObject> R = MakeShared<FJsonObject>();
			R->SetStringField(TEXT("op"), TEXT("attachmentsDone"));
			R->SetArrayField(TEXT("results"), Done);
			R->SetStringField(TEXT("folder"), ImportInboxDir());
			ToTool(R);
			UE_LOG(LogBF6Blocks, Display, TEXT("%d attachment(s) taken out of the experience into %s"),
				n, *ImportInboxDir());
			return;
		}
		if (Op == TEXT("sendToSite"))
		{
			// Write it first, always: whatever happens to the site's own import
			// control, the file exists and its path is on the clipboard.
			FString Format, Text, Suggested;
			M->TryGetStringField(TEXT("format"), Format);
			M->TryGetStringField(TEXT("suggested"), Suggested);
			const TSharedPtr<FJsonValue> Value = M->TryGetField(TEXT("json"));
			if (Value.IsValid())
			{
				// Condensed, for the reason spelled out on the exportFile
				// handler above. This file goes to the site's own importer, so
				// a hundred tabs a line is worse here than anywhere.
				TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> W =
					TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Text);
				FJsonSerializer::Serialize(Value.ToSharedRef(), FString(), W);
			}
			IFileManager::Get().MakeDirectory(*ImportInboxDir(), true);
			if (Suggested.IsEmpty()) Suggested = TEXT("send_to_portal.json");
			const FString Path = FPaths::Combine(ImportInboxDir(), Suggested);
			const bool bWrote = FFileHelper::SaveStringToFile(Text, *Path,
				FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
			const FString Win = FPaths::ConvertRelativePathToFull(Path).Replace(TEXT("/"), TEXT("\\"));
			if (bWrote) FPlatformApplicationMisc::ClipboardCopy(*Win);
			if (GSiteReady)
			{
				TSharedRef<FJsonObject> S = MakeShared<FJsonObject>();
				S->SetStringField(TEXT("op"), TEXT("findImport"));
				S->SetStringField(TEXT("path"), Win);
				ToSite(S);
			}
			TSharedRef<FJsonObject> R = MakeShared<FJsonObject>();
			R->SetStringField(TEXT("op"), TEXT("sendToSiteResult"));
			R->SetBoolField(TEXT("ok"), bWrote);
			R->SetStringField(TEXT("path"), Win);
			R->SetBoolField(TEXT("siteReady"), GSiteReady);
			R->SetStringField(TEXT("text"), bWrote
				? (GSiteReady
					? FString::Printf(TEXT("Written to %s and copied to the clipboard. Looking for the site's import control."), *Win)
					: FString::Printf(TEXT("Written to %s and copied to the clipboard. Open the experience's page in the Portal panel and paste the path into its import picker."), *Win))
				: TEXT("Could not write the file."));
			ToTool(R);
			UE_LOG(LogBF6Blocks, Display, TEXT("Send to the site: %s"), *Win);
			return;
		}
		if (Op == TEXT("replaceTop") || Op == TEXT("deleteTop") || Op == TEXT("variables"))
		{
			// Signed out, or no page: the edit stays in the editor's journal and
			// on disk, and goes out when the session comes back. Nothing is lost
			// and nothing is silently dropped.
			if (GSessionLost) { GLastError = TEXT("held: Portal signed the user out"); return; }
			if (!GSiteReady) { GLastError = TEXT("edit not mirrored: the site page is not connected"); return; }
			ToSiteRaw(Raw);
			return;
		}
		if (Op == TEXT("pullWorkspace"))
		{
			GWantWorkspace = true;
			if (GSiteReady) AskSiteForWorkspace(); else EnsureSiteOnBlocksPage();
			return;
		}
		if (Op == TEXT("pushWorkspace"))
		{
			if (!GSiteReady) { EnsureSiteOnBlocksPage(); return; }
			ToSiteRaw(Raw.Replace(TEXT("\"op\":\"pushWorkspace\""), TEXT("\"op\":\"setWorkspace\"")));
			return;
		}
		if (Op == TEXT("savePortal"))
		{
			if (!GSiteReady) { EnsureSiteOnBlocksPage(); return; }
			TSharedRef<FJsonObject> S = MakeShared<FJsonObject>();
			S->SetStringField(TEXT("op"), TEXT("clickSave"));
			ToSite(S);
			return;
		}
		if (Op == TEXT("blockSelected"))
		{
			TArray<int32> Ids;
			const TArray<TSharedPtr<FJsonValue>>* Arr = nullptr;
			if (M->TryGetArrayField(TEXT("ids"), Arr) && Arr)
			{
				for (const TSharedPtr<FJsonValue>& V : *Arr) Ids.Add((int32)V->AsNumber());
			}
			const int32 n = BF6Blocks::SelectActorsByObjId(Ids);
			if (Ids.Num() && !n)
			{
				ToolNote(TEXT("status"), TEXT("text"),
					TEXT("No placed object in this level carries that ObjId."));
			}
			return;
		}
		if (Op == TEXT("useSelected"))
		{
			FString Kind;
			M->TryGetStringField(TEXT("kind"), Kind);
			int32 Id = 0; FString Name;
			TSharedRef<FJsonObject> R = MakeShared<FJsonObject>();
			R->SetStringField(TEXT("op"), TEXT("useSelected"));
			if (BF6Blocks::SelectedObjId(Kind, Id, Name, true))
			{
				R->SetBoolField(TEXT("ok"), true);
				R->SetNumberField(TEXT("objId"), Id);
				R->SetStringField(TEXT("name"), Name);
			}
			else
			{
				R->SetBoolField(TEXT("ok"), false);
				R->SetStringField(TEXT("text"), TEXT("Select one placed object in the level first."));
			}
			ToTool(R);
			return;
		}
		if (Op == TEXT("assignToSelected"))
		{
			double Id = 0;
			M->TryGetNumberField(TEXT("objId"), Id);
			FString Name;
			const bool bOk = BF6Blocks::AssignObjIdToSelected((int32)Id, Name);
			ToolNote(TEXT("status"), TEXT("text"), bOk
				? FString::Printf(TEXT("ObjId %d written to %s"), (int32)Id, *Name)
				: TEXT("Select one placed object in the level first."));
			if (bOk) SendObjIds();
			return;
		}
		if (Op == TEXT("snippetLoad"))
		{
			FString Name;
			M->TryGetStringField(TEXT("name"), Name);
			SendSnippet(Name);
			return;
		}
		// ---- the two the editor's new block actions need -----------------------
		//
		// Added with the actions themselves rather than after them. An op the page
		// sends that nothing here handles is silent: the click appears to work and
		// nothing happens, which is how a dead save path went unnoticed in this
		// file for a long time.

		// GO TO THE TYPESCRIPT THIS BLOCK CAME FROM.
		//
		// Every converted top level block carries the file and line it was made
		// from. The path is relative to the project that was imported, so it is
		// resolved against the folder the import came from and handed to whatever
		// the machine opens .ts with.
		if (Op == TEXT("openSource"))
		{
			FString Rel; int32 Line = 0;
			M->TryGetStringField(TEXT("path"), Rel);
			{ double D = 0; if (M->TryGetNumberField(TEXT("line"), D)) { Line = (int32)D; } }
			if (Rel.IsEmpty()) return;
			FString Full = Rel;
			if (FPaths::IsRelative(Full) && !GLastImportDir.IsEmpty())
			{
				Full = FPaths::Combine(GLastImportDir, Rel);
			}
			if (!FPaths::FileExists(Full))
			{
				UE_LOG(LogBF6Blocks, Warning,
					TEXT("Go to source: %s is not on disk (import folder %s)."),
					*Full, GLastImportDir.IsEmpty() ? TEXT("not recorded") : *GLastImportDir);
				ToolNote(TEXT("status"), TEXT("text"),
					FString::Printf(TEXT("could not find %s"), *Rel));
				return;
			}
			UE_LOG(LogBF6Blocks, Display, TEXT("Go to source: %s line %d"), *Full, Line);
			FPlatformProcess::LaunchURL(*(TEXT("file:///") + Full.Replace(TEXT("\\"), TEXT("/"))),
				nullptr, nullptr);
			return;
		}

		// SAVE THE SELECTED BLOCKS AS A SNIPPET.
		//
		// The editor has loaded snippets from this folder since it was written and
		// nothing has ever put one there, so the library could only ever hold what
		// shipped with the tool.
		if (Op == TEXT("saveSnippet"))
		{
			FString Name;
			M->TryGetStringField(TEXT("name"), Name);
			const TSharedPtr<FJsonValue> Value = M->TryGetField(TEXT("json"));
			if (Name.IsEmpty() || !Value.IsValid()) return;
			FString Safe;
			for (const TCHAR C : Name)
			{
				Safe.AppendChar(FChar::IsAlnum(C) || C == TEXT('_') || C == TEXT('-')
					? C : TEXT('_'));
			}
			FString Text;
			TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> W =
				TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Text);
			FJsonSerializer::Serialize(Value.ToSharedRef(), FString(), W);
			const FString Dir = FPaths::Combine(BlocksResDir(), TEXT("snippets"), TEXT("mine"));
			IFileManager::Get().MakeDirectory(*Dir, true);
			const FString Path = FPaths::Combine(Dir, Safe + TEXT(".json"));
			const bool bOk = FFileHelper::SaveStringToFile(Text, *Path,
				FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
			UE_LOG(LogBF6Blocks, Display, TEXT("Snippet %s: %s (%d chars)"),
				bOk ? TEXT("saved") : TEXT("COULD NOT BE SAVED"), *Path, Text.Len());
			ToolNote(TEXT("status"), TEXT("text"), bOk
				? FString::Printf(TEXT("saved snippet \"%s\""), *Name)
				: FString::Printf(TEXT("could not save \"%s\""), *Name));
			if (bOk) { SendSnippetList(); }
			return;
		}

		if (Op == TEXT("openUrl"))
		{
			FString Url;
			M->TryGetStringField(TEXT("url"), Url);
			if (Url.IsEmpty()) return;
			if (Url.Contains(TEXT("portal.battlefield.com"))) BF6PortalWeb::Open(Url);
			else FPlatformProcess::LaunchURL(*Url, nullptr, nullptr);
			return;
		}
		if (Op == TEXT("saveFileResult"))
		{
			// The page serialized its workspace; write it in the shape the site
			// exports, so the file opens on Portal as well as here.
			FString Path;
			M->TryGetStringField(TEXT("path"), Path);
			const TSharedPtr<FJsonValue> Value = M->TryGetField(TEXT("json"));
			if (Path.IsEmpty() || !Value.IsValid()) { GLastError = TEXT("save produced nothing"); return; }
			FString Inner;
			TSharedRef<TJsonWriter<>> W = TJsonWriterFactory<>::Create(&Inner);
			FJsonSerializer::Serialize(Value.ToSharedRef(), FString(), W);
			const FString Doc = TEXT("{\"mod\":") + Inner + TEXT("}");
			const bool bOk = FFileHelper::SaveStringToFile(Doc, *Path,
				FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
			UE_LOG(LogBF6Blocks, Display, TEXT("%s %s (%d characters)"),
				bOk ? TEXT("Wrote") : TEXT("FAILED to write"), *Path, Doc.Len());
			ToolNote(TEXT("status"), TEXT("text"),
				bOk ? FString::Printf(TEXT("Saved to %s"), *Path) : TEXT("Could not write that path"));
			return;
		}
		if (Op == TEXT("log"))
		{
			FString Text;
			M->TryGetStringField(TEXT("text"), Text);
			UE_LOG(LogBF6Blocks, Display, TEXT("[editor] %s"), *Text);
			// NO FLUSH HERE. IT DEADLOCKS THE EDITOR.
			//
			// This once called GLog->Flush() for messages beginning "trail:",
			// so that breadcrumbs reached disk before a hang could swallow
			// them. It froze the editor solid, and took a long time to see
			// because every message the diagnostic page sent began with that
			// exact word: the instrument was producing the fault it was
			// installed to find.
			//
			// Proven by elimination on a page carrying nothing else. Same page,
			// same events, same deferred timing:
			//   no bridge call at all              smooth
			//   bridge call, text begins "trail:"  frozen
			//   bridge call, any other text        smooth
			//
			// A synchronous log flush inside a bridge callback is not safe;
			// the callback does not arrive on a thread that may take that
			// lock. If breadcrumbs need to survive a hang again, buffer them
			// somewhere that costs nothing to write and read them back after a
			// restart. Never flush from here.
			return;
		}
	}

	void HandleFromSite(const TSharedPtr<FJsonObject>& M, const FString& Op, const FString& Raw)
	{
		GFromSite++;
		if (Op == TEXT("ready"))
		{
			M->TryGetStringField(TEXT("url"), GSiteUrl);
			double Blocks = 0;
			M->TryGetNumberField(TEXT("blocks"), Blocks);
			GSiteReady = (Blocks >= 0);
			TSharedRef<FJsonObject> C = MakeShared<FJsonObject>();
			C->SetStringField(TEXT("op"), TEXT("connected"));
			C->SetBoolField(TEXT("connected"), GSiteReady);
			// TWO DIFFERENT THINGS, AND THE PILL USED TO SHOW ONLY ONE.
			//
			// "connected" means the Portal blocks page has said hello INSIDE the
			// tool's own web panel. It says nothing about whether the user is
			// signed in, so somebody signed in to Portal with the panel closed,
			// or open on a different page, was told "SITE OFFLINE" and reasonably
			// read it as a lost connection. The sign-in state travels with it now
			// so the page can say which of the two is actually missing.
			C->SetStringField(TEXT("signIn"), BF6PortalProfile::StateLabel());
			ToTool(C);
			GExperienceId = ExperienceFromUrl(GSiteUrl);
			UE_LOG(LogBF6Blocks, Display, TEXT("Site page ready at %s (%d blocks)"), *GSiteUrl, (int32)Blocks);
			// The blocks page answering IS the session being back.
			if (GSiteReady && GSessionLost) BF6Blocks::NoteSessionRestored();
			if (GSiteReady && (GWantWorkspace || GToolReady))
			{
				GWantWorkspace = false;
				AskSiteForWorkspace();
			}
			return;
		}
		if (Op == TEXT("perflook"))
		{
			IFileManager::Get().MakeDirectory(*CacheDir(), true);
			FFileHelper::SaveStringToFile(Raw, *FPaths::Combine(CacheDir(), TEXT("perf_site.json")),
				FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
			return;
		}
		if (Op == TEXT("diagnoselook"))
		{
			IFileManager::Get().MakeDirectory(*CacheDir(), true);
			FFileHelper::SaveStringToFile(Raw, *FPaths::Combine(CacheDir(), TEXT("diagnose_site.json")),
				FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
			UE_LOG(LogBF6Blocks, Display, TEXT("Site diagnosis written (%d chars)."), Raw.Len());
			return;
		}
		if (Op == TEXT("fieldlook"))
		{
			IFileManager::Get().MakeDirectory(*CacheDir(), true);
			FFileHelper::SaveStringToFile(Raw,
				*FPaths::Combine(CacheDir(), TEXT("fields_site.json")),
				FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
			UE_LOG(LogBF6Blocks, Display, TEXT("How the site paints its fields: %s (%d chars)."),
				*FPaths::Combine(CacheDir(), TEXT("fields_site.json")), Raw.Len());
			return;
		}
		if (Op == TEXT("sweeplook"))
		{
			IFileManager::Get().MakeDirectory(*CacheDir(), true);
			FFileHelper::SaveStringToFile(Raw,
				*FPaths::Combine(CacheDir(), TEXT("sweep_site.json")),
				FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
			UE_LOG(LogBF6Blocks, Display,
				TEXT("Every block on the site was measured into %s (%d chars)."),
				*FPaths::Combine(CacheDir(), TEXT("sweep_site.json")), Raw.Len());
			return;
		}
		if (Op == TEXT("layoutlook"))
		{
			IFileManager::Get().MakeDirectory(*CacheDir(), true);
			FFileHelper::SaveStringToFile(Raw,
				*FPaths::Combine(CacheDir(), TEXT("layout_site.json")),
				FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
			UE_LOG(LogBF6Blocks, Display,
				TEXT("How the site measures its blocks was written to %s (%d chars)."),
				*FPaths::Combine(CacheDir(), TEXT("layout_site.json")), Raw.Len());
			return;
		}
		if (Op == TEXT("socketlook"))
		{
			IFileManager::Get().MakeDirectory(*CacheDir(), true);
			FFileHelper::SaveStringToFile(Raw,
				*FPaths::Combine(CacheDir(), TEXT("socket_look.json")),
				FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
			UE_LOG(LogBF6Blocks, Display,
				TEXT("What the site draws for an empty socket was written to %s (%d chars)."),
				*FPaths::Combine(CacheDir(), TEXT("socket_look.json")), Raw.Len());
			return;
		}
		if (Op == TEXT("toolboxlook"))
		{
			// Written whole, because the point of it is to be READ: the css
			// rules that really apply, the measured sizes and the class names
			// are evidence, and evidence that is summarised is evidence lost.
			IFileManager::Get().MakeDirectory(*CacheDir(), true);
			FFileHelper::SaveStringToFile(Raw,
				*FPaths::Combine(CacheDir(), TEXT("toolbox_look.json")),
				FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
			UE_LOG(LogBF6Blocks, Display,
				TEXT("The site's toolbox and pull-out were measured and written to %s (%d chars)."),
				*FPaths::Combine(CacheDir(), TEXT("toolbox_look.json")), Raw.Len());
			return;
		}
		if (Op == TEXT("defs"))
		{
			CacheCapture(M);
			// COUNT THE BLOCKS, NOT THE CATALOGUE'S SECTIONS.
			//
			// 'definitions' is the site's mod catalogue: eight arrays called
			// objects, events, values, actions and so on. Counting its keys
			// made the status read "8 types cached" while 604 block readings
			// sat next to it under 'synthesized', so the one line a person
			// looks at to decide whether the definitions are there said no
			// when the answer was yes.
			int32 Cat = 0, Blocks = 0;
			const TSharedPtr<FJsonObject>* Defs = nullptr;
			if (M->TryGetObjectField(TEXT("definitions"), Defs) && Defs && (*Defs).IsValid())
				Cat = (*Defs)->Values.Num();
			const TSharedPtr<FJsonObject>* Synth = nullptr;
			if (M->TryGetObjectField(TEXT("synthesized"), Synth) && Synth && (*Synth).IsValid())
				Blocks = (*Synth)->Values.Num();
			GDefTypes = Blocks;
			GDefsSource = TEXT("live");
			ToToolRaw(Raw);
			UE_LOG(LogBF6Blocks, Display,
				TEXT("Read from the site: %d block types, and a mod catalogue of %d section(s)."),
				Blocks, Cat);
			return;
		}
		if (Op == TEXT("importControl"))
		{
			// What the page found, logged so the selector can be re-anchored
			// without guessing when the site's next build renames it.
			FString Selector, Text, How;
			bool bDrove = false;
			M->TryGetStringField(TEXT("selector"), Selector);
			M->TryGetStringField(TEXT("text"), Text);
			M->TryGetStringField(TEXT("how"), How);
			M->TryGetBoolField(TEXT("drove"), bDrove);
			if (Selector.IsEmpty())
			{
				UE_LOG(LogBF6Blocks, Display, TEXT(
					"No import control found on the site's page. The file is written and its path "
					"is on the clipboard: press the site's own IMPORT and paste the path."));
			}
			else
			{
				UE_LOG(LogBF6Blocks, Display, TEXT(
					"Site import control: %s  text \"%s\"  (%s). %s"),
					*Selector, *Text, *How,
					bDrove ? TEXT("Opened it for you; paste the path the tool put on your clipboard.")
					       : TEXT("It could not be driven from here, so paste the path yourself."));
			}
			ToToolRaw(Raw);
			return;
		}
		if (Op == TEXT("style"))
		{
			bool bFailed = false;
			M->TryGetBoolField(TEXT("failed"), bFailed);
			if (bFailed)
			{
				FString Note;
				M->TryGetStringField(TEXT("note"), Note);
				GLastError = FString::Printf(TEXT("style capture failed: %s"), *Note);
				UE_LOG(LogBF6Blocks, Warning, TEXT("%s"), *GLastError);
				ToolNote(TEXT("status"), TEXT("text"),
					TEXT("That page has no Blockly workspace: open the BLOCKS page and press CAPTURE STYLE again."));
				return;
			}
			CacheStyle(M, Raw);
			// A capture off the live page wins, but it is only what that page
			// had on screen: a category whose row was never drawn carries no
			// icon in it. The mirror fills those in by name rather than being
			// dropped whole because the capture had some of them.
			TSharedRef<FJsonObject> Out = M.ToSharedRef();
			AddMirrorToStyle(Out);
			Out->SetStringField(TEXT("source"), TEXT("live"));
			ToTool(Out);
			UE_LOG(LogBF6Blocks, Display, TEXT("Block icons in use: %s (%s)."),
				*GIconSource, *GIconSummary);
			return;
		}
		if (Op == TEXT("workspace"))
		{
			GExperienceId = ExperienceFromUrl(GSiteUrl);
			// Reconciling after a sign-out: the editor diffs, we do not offer a
			// choice, because the user never left the editor.
			if (GReconcileNext)
			{
				GReconcileNext = false;
				NoteSitePull();
				ToToolRaw(Raw.Replace(TEXT("{\"op\":\"workspace\""),
					TEXT("{\"reconcile\":true,\"op\":\"workspace\"")));
				return;
			}
			TSharedPtr<FJsonObject> Local = AutosaveAheadOfSite();
			if (Local.IsValid())
			{
				// Hand both to the editor: it counts the blocks and rules of each
				// and lets the user pick.
				FString LocalText;
				TSharedRef<TJsonWriter<>> W = TJsonWriterFactory<>::Create(&LocalText);
				FJsonSerializer::Serialize(Local.ToSharedRef(), W);
				FString Merged = Raw;
				Merged.RemoveFromEnd(TEXT("}"));
				Merged += TEXT(",\"local\":") + LocalText + TEXT("}");
				NoteSitePull();
				ToToolRaw(Merged);
				UE_LOG(LogBF6Blocks, Display,
					TEXT("Local autosave is ahead of the site for experience %s: offering the choice."),
					*GExperienceId);
				return;
			}
			NoteSitePull();
			ToToolRaw(Raw);
			return;
		}
		if (Op == TEXT("replaceTop") || Op == TEXT("deleteTop") ||
			Op == TEXT("variables") || Op == TEXT("saveResult") || Op == TEXT("applied") ||
			Op == TEXT("portalResult"))
		{
			if (Op == TEXT("saveResult")) M->TryGetStringField(TEXT("selector"), GSaveSelector);
			if (Op == TEXT("portalResult"))
			{
				double St = 0;
				const bool bHas = M->TryGetNumberField(TEXT("status"), St);
				FString Message, Sel;
				M->TryGetStringField(TEXT("message"), Message);
				M->TryGetStringField(TEXT("dialogSelector"), Sel);
				const TArray<TSharedPtr<FJsonValue>>* Blocks = nullptr;
				const int32 nBlocks = M->TryGetArrayField(TEXT("blocks"), Blocks) && Blocks ? Blocks->Num() : 0;
				GLastPortalVerdict = FString::Printf(TEXT("grpc-status %s %s (%d block warning(s))%s"),
					bHas ? *FString::FromInt((int32)St) : TEXT("unknown"), *Message, nBlocks,
					Sel.IsEmpty() ? TEXT("") : *(TEXT(" dialog ") + Sel));
				UE_LOG(LogBF6Blocks, Display, TEXT("Portal save verdict: %s"), *GLastPortalVerdict);
			}
			ToToolRaw(Raw);
			return;
		}
		if (Op == TEXT("log"))
		{
			FString Text;
			M->TryGetStringField(TEXT("text"), Text);
			UE_LOG(LogBF6Blocks, Display, TEXT("[site] %s"), *Text);
			return;
		}
	}
}

void BF6Blocks::HandleMessage(const FString& Json)
{
	TSharedPtr<FJsonObject> M = Parse(Json);
	if (!M.IsValid()) { GLastError = TEXT("a page sent something that is not JSON"); return; }
	FString From, Op;
	M->TryGetStringField(TEXT("from"), From);
	M->TryGetStringField(TEXT("op"), Op);

	// A message too big for one binding call arrives in pieces.
	if (Op == TEXT("chunk"))
	{
		FString Cid;
		double Index = 0, Total = 0;
		FString Part;
		M->TryGetStringField(TEXT("cid"), Cid);
		M->TryGetNumberField(TEXT("i"), Index);
		M->TryGetNumberField(TEXT("n"), Total);
		M->TryGetStringField(TEXT("part"), Part);
		TArray<FString>& Slots = GInbound.FindOrAdd(Cid);
		Slots.SetNum(FMath::Max(Slots.Num(), (int32)Total));
		if ((int32)Index < Slots.Num()) Slots[(int32)Index] = Part;
		for (const FString& S : Slots) if (S.IsEmpty()) return;
		const FString Whole = FString::Join(Slots, TEXT(""));
		GInbound.Remove(Cid);
		HandleMessage(Whole);
		return;
	}

	if (From == TEXT("site")) HandleFromSite(M, Op, Json);
	else HandleFromTool(M, Op, Json);
}

void UBF6BlocksBridge::Msg(const FString& Json)
{
	BF6Blocks::HandleMessage(Json);
}

// ============================================================================
// Session
// ============================================================================
bool BF6Blocks::IsSessionLost() { return GSessionLost; }

void BF6Blocks::NoteSessionLost()
{
	if (GSessionLost) return;
	GSessionLost = true;
	GSiteReady = false;
	TSharedRef<FJsonObject> M = MakeShared<FJsonObject>();
	M->SetStringField(TEXT("op"), TEXT("session"));
	M->SetStringField(TEXT("state"), TEXT("lost"));
	M->SetStringField(TEXT("text"), TEXT(
		"Portal signed you out. Your edits are safe here and will be re-applied when you are back in."));
	ToTool(M);
	UE_LOG(LogBF6Blocks, Warning,
		TEXT("Portal session lost. Editing continues; edits are held in the editor's journal and on disk at %s"),
		*ExperienceDir());
}

void BF6Blocks::NoteSessionRestored()
{
	if (!GSessionLost) return;
	GSessionLost = false;
	TSharedRef<FJsonObject> M = MakeShared<FJsonObject>();
	M->SetStringField(TEXT("op"), TEXT("session"));
	M->SetStringField(TEXT("state"), TEXT("restored"));
	ToTool(M);
	// Only reconcile once the page is actually back on this experience's block
	// page; site_sync's own ready message is the proof of that.
	const FString Url = BF6PortalWeb::CurrentUrl();
	const bool bSamePage = Url.Contains(TEXT("/rules/blocks")) &&
		(GExperienceId.IsEmpty() || ExperienceFromUrl(Url) == GExperienceId);
	if (bSamePage && GSiteReady)
	{
		GReconcileNext = true;
		AskSiteForWorkspace();
		UE_LOG(LogBF6Blocks, Display, TEXT("Portal session restored: reconciling with the site."));
	}
	else
	{
		GReconcileNext = true;      // the next ready message triggers the pull
		UE_LOG(LogBF6Blocks, Display,
			TEXT("Portal session restored, waiting for the blocks page of experience %s."), *GExperienceId);
	}
}

// The gRPC-web trailer on the save call: one flags byte, four length bytes,
// then the payload; the frame with 0x80 set is plain text carrying grpc-status
// and grpc-message.
void BF6Blocks::NoteWebPlayResponse(const FString& Url, const FString& Body)
{
	if (!Url.Contains(TEXT("PlayElement")) && !Url.Contains(TEXT("WebPlay"))) return;
	int32 Status = 0;
	FString Message;
	const int32 At = Body.Find(TEXT("grpc-status:"));
	if (At == INDEX_NONE) return;
	FString Tail = Body.Mid(At);
	Tail.Split(TEXT("grpc-status:"), nullptr, &Tail);
	Status = FCString::Atoi(*Tail.TrimStart());
	int32 MsgAt = Body.Find(TEXT("grpc-message:"));
	if (MsgAt != INDEX_NONE)
	{
		Message = Body.Mid(MsgAt + 13);
		int32 Eol = INDEX_NONE;
		if (Message.FindChar(TEXT('\r'), Eol) || Message.FindChar(TEXT('\n'), Eol)) Message = Message.Left(Eol);
		Message = Message.TrimStartAndEnd();
	}
	GLastPortalVerdict = FString::Printf(TEXT("grpc-status %d %s"), Status, *Message);
	UE_LOG(LogBF6Blocks, Display, TEXT("WebPlay verdict from the profile capture: %s"), *GLastPortalVerdict);
	TSharedRef<FJsonObject> M = MakeShared<FJsonObject>();
	M->SetStringField(TEXT("op"), TEXT("portalResult"));
	M->SetNumberField(TEXT("status"), Status);
	M->SetStringField(TEXT("message"), Message);
	M->SetArrayField(TEXT("blocks"), TArray<TSharedPtr<FJsonValue>>());
	ToTool(M);
}

// ============================================================================
// The tab
// ============================================================================
namespace
{
	bool WebAvailable()
	{
		FModuleManager::Get().LoadModulePtr<IWebBrowserModule>(TEXT("WebBrowser"));
		return IWebBrowserModule::IsAvailable() && IWebBrowserModule::Get().IsWebModuleAvailable();
	}

	TSharedRef<SWidget> MakeUnavailableView()
	{
		return SNew(SBorder)
			.Padding(16.f)
			[
				SNew(STextBlock)
				.AutoWrapText(true)
				.Text(FText::FromString(TEXT(
					"This editor needs the engine's embedded browser, which this build does not have. "
					"Edit blocks on the Portal site instead, or run an editor build with the CEF binaries.")))
			];
	}

	// ---- BF6EditorOverlay ----
	// THE PAGE IS MADE ONCE. It used to be built by the tab spawner and thrown
	// away by the tab's X, which meant a workspace was lost every time the tab
	// was closed - and would have been lost on every move between the tab and
	// the full-screen host. Now the browser is created here, on the first ask,
	// and lives until the module shuts down.
	TSharedRef<SWidget> EnsurePage()
	{
		if (GBrowser.IsValid()) return GBrowser.ToSharedRef();
		if (!WebAvailable()) return MakeUnavailableView();

		GBrowser = SNew(SWebBrowser)
			.BrowserFrameRate(60)
			.InitialURL(EditorPageUrl())
			.ShowControls(false)
			.ShowAddressBar(false)
			.ShowErrorMessage(true)
			.ShowInitialThrobber(true)
			.SupportsTransparency(false)
			.BackgroundColor(FColor(0x0D, 0x0F, 0x10));

		if (GBridge.IsValid())
		{
			GBrowser->BindUObject(kBridgeName, GBridge.Get(), true);
		}
		return GBrowser.ToSharedRef();
	}
	// ---- end BF6EditorOverlay ----

	// ---- the 3D scene behind the block editor ------------------------------
	//
	// The block editor is a browser widget composited over the level viewport,
	// and the viewport keeps redrawing the whole scene underneath it at its own
	// realtime rate whether any of it can be seen or not. On a big project the
	// page is asking for every frame it can get, so the two are fighting over
	// the same GPU for a picture only one of them is showing.
	//
	// While the blocks tab is up, the viewports are told to stand down: a
	// non-realtime viewport still draws, it just stops redrawing a scene that
	// has not changed. It is an OVERRIDE rather than a mode change, so whatever
	// the user had set comes back untouched when the tab closes, and anything
	// that genuinely needs realtime (a running PIE, a Sequencer scrub) applies
	// its own override on top.
	static const TCHAR* kBlocksQuiet = TEXT("BF6 block editor is open");
	bool GSceneStoodDown = false;

	void StandDownTheScene(bool bQuiet)
	{
		if (bQuiet == GSceneStoodDown) return;
		GSceneStoodDown = bQuiet;
		int32 Touched = 0;
		for (FLevelEditorViewportClient* VC : GEditor ? GEditor->GetLevelViewportClients()
			: TArray<FLevelEditorViewportClient*>())
		{
			if (!VC) continue;
			if (bQuiet) VC->AddRealtimeOverride(false, FText::FromString(kBlocksQuiet));
			else VC->RemoveRealtimeOverride(FText::FromString(kBlocksQuiet), false);
			Touched++;
		}
		UE_LOG(LogBF6Blocks, Display,
			TEXT("The 3D scene behind the block editor was %s on %d viewport(s)."),
			bQuiet ? TEXT("stood down") : TEXT("let go"), Touched);
	}

	// THE TAB NEVER GAVE THE PAGE THE KEYBOARD, AND THE OVERLAY ALWAYS DID.
	//
	// Opened as the full screen overlay the block editor has full control;
	// opened as a dock tab nothing reaches it. Not one mouse press arrived at
	// the page across a whole session of trying, and Space went to the level
	// editor's radial menu instead of the canvas.
	//
	// The difference is one call. BF6EditorOverlay::FocusPage hands the keyboard
	// to the browser once the widget is in a window; the tab path set its
	// content and stopped, so the browser was drawn but never focused, and CEF
	// does not take input it was not given.
	//
	// Next tick, for the same reason the overlay does it that way: focusing a
	// widget that has no parent window yet does nothing at all.
	// DO NOT ASK SLATE TO FIND THIS WIDGET. IT IS IN THE TREE TWICE.
	//
	// This used to call SetKeyboardFocus(GBrowser) so the tab took the keyboard
	// on open. SetKeyboardFocus goes to FSlateApplication::SetUserFocus, which
	// goes to FSlateWindowHelper::FindPathToWidget, which walks the widget tree
	// looking for a path to the widget it was given.
	//
	// The browser is ONE widget shared by both placements, as the comment in
	// SpawnBlocksTab below says. A widget reachable by more than one route makes
	// that walk run away: captured from a hung editor on 2026-09-07, the game
	// thread stack held roughly three hundred recursive FindPathToWidget frames
	// under this lambda, with the editor at 90% of a core and never returning.
	//
	//   FTSTicker::Tick
	//     BF6UnrealSDK!FocusTabPage::<lambda_1>
	//       FSlateApplication::SetUserFocus
	//         FSlateWindowHelper::FindPathToWidget   x ~300
	//
	// That is the freeze that made the block editor unusable: opening a
	// category, loading a workspace and clicking about all reached it, which is
	// why it looked like three different bugs.
	//
	// Focus is not worth this. Clicking the page gives it the keyboard, the
	// same as any other embedded browser. If it is wanted again it must be done
	// without asking Slate to search for a widget that has two parents: give
	// each placement its own browser, or focus the tab's own content widget
	// after confirming which placement actually holds the page.
	void FocusTabPage()
	{
	}

	TSharedRef<SDockTab> SpawnBlocksTab(const FSpawnTabArgs&)
	{
		TSharedRef<SDockTab> Tab = SNew(SDockTab).TabRole(ETabRole::NomadTab);
		// ---- BF6EditorOverlay ----
		// A tab asking for the page takes it off the full-screen host: the two
		// placements share one widget, and whichever one is asked for wins.
		BF6EditorOverlay::HideIfShowing(BF6EditorOverlay::EEditor::Blocks);
		if (!GHost.IsValid()) GHost = SNew(SBox);
		GHost->SetContent(EnsurePage());
		Tab->SetContent(GHost.ToSharedRef());
		GTab = Tab;
		StandDownTheScene(true);

		// On open, and again every time it is brought forward: clicking another
		// tab and coming back must not leave the canvas dead.
		FocusTabPage();
		Tab->SetOnTabActivated(SDockTab::FOnTabActivatedCallback::CreateLambda(
			[](TSharedRef<SDockTab>, ETabActivationCause)
			{
				FocusTabPage();
			}));
		Tab->SetOnTabClosed(SDockTab::FOnTabClosedCallback::CreateLambda([](TSharedRef<SDockTab>)
		{
			// Closed by its own X: the page keeps running, unparented, and
			// comes straight back with everything on it.
			if (GHost.IsValid()) GHost->SetContent(SNullWidget::NullWidget);
			GTab.Reset();
			StandDownTheScene(false);
		}));
		// ---- end BF6EditorOverlay ----
		return Tab;
	}

	bool PollSelection(float)
	{
		if (!GToolReady) return true;
		TArray<AActor*> Sel;
		BF6Ext::Selection(Sel);
		TArray<int32> Ids;
		for (AActor* A : Sel)
		{
			if (!A) continue;
			const FString V = BF6Api::GetActorProp(A, TEXT("ObjId"));
			if (V.IsEmpty()) continue;
			const int32 Id = FCString::Atoi(*V);
			if (Id > 0) Ids.AddUnique(Id);
		}
		if (Ids == GLastSceneSel) return true;
		GLastSceneSel = Ids;
		TSharedRef<FJsonObject> M = MakeShared<FJsonObject>();
		M->SetStringField(TEXT("op"), TEXT("sceneSelected"));
		TArray<TSharedPtr<FJsonValue>> Arr;
		for (int32 Id : Ids) Arr.Add(MakeShared<FJsonValueNumber>(Id));
		M->SetArrayField(TEXT("objIds"), Arr);
		ToTool(M);
		return true;
	}
}

// ============================================================================
// Console
// ============================================================================
void BF6Blocks::Open()
{
	// ---- BF6EditorOverlay ----
	// Asking for the tab moves the page into it, so the host must let go first.
	BF6EditorOverlay::HideIfShowing(BF6EditorOverlay::EEditor::Blocks);
	// ---- end BF6EditorOverlay ----
	FGlobalTabmanager::Get()->TryInvokeTab(FTabId(kTabId));
	// A tab that was already open does not spawn again, so fill it here too.
	if (GHost.IsValid() && GTab.IsValid()) GHost->SetContent(EnsurePage());
}

// ---- BF6EditorOverlay ----
TSharedRef<SWidget> BF6Blocks::Widget()
{
	TSharedRef<SWidget> W = EnsurePage();
	// One parent only: the dock tab gives it up while the host holds it, and
	// the tab is left empty rather than closed, so it is still there to go
	// back into.
	if (GHost.IsValid()) GHost->SetContent(SNullWidget::NullWidget);
	return W;
}

void BF6Blocks::ReleaseWidget()
{
	if (GHost.IsValid() && GTab.IsValid()) GHost->SetContent(EnsurePage());
}
// ---- end BF6EditorOverlay ----

FString BF6Blocks::Status()
{
	return FString::Printf(TEXT(
		"BF6 Blocks\n"
		"  editor page   : %s\n"
		"  site page     : %s%s\n"
		"  definitions   : %s (%d types cached at %s)\n"
		"  site style    : %s%s\n"
		"                  %s\n"
		"  block icons   : %s (%s)\n"
		"  site mirror   : %s\n"
		"  messages      : %d in from the editor, %d in from the site, %d out to the editor, %d out to the site\n"
		"  session       : %s\n"
		"  experience    : %s\n"
		"  autosave      : %d written, last at %s, folder %s\n"
		"  save control  : %s\n"
		"  portal verdict: %s\n"
		"  last error    : %s"),
		GToolReady ? TEXT("open") : TEXT("closed"),
		GSiteReady ? TEXT("connected") : TEXT("not connected"),
		GSiteUrl.IsEmpty() ? TEXT("") : *(TEXT(" at ") + GSiteUrl),
		GDefsSource.IsEmpty() ? TEXT("none") : *GDefsSource, GDefTypes, *CacheDir(),
		GStyleSource.IsEmpty() ? TEXT("not loaded yet") : *GStyleSource,
		GStyleAt.IsEmpty() ? TEXT("") : *(TEXT(", captured ") + GStyleAt),
		GStyleSummary.IsEmpty() ? TEXT("press CAPTURE STYLE with the Portal blocks page open") : *GStyleSummary,
		*GIconSource, GIconSummary.IsEmpty() ? TEXT("not worked out yet") : *GIconSummary,
		GMirrorDir.IsEmpty()
			? TEXT("none on this machine, the style downloader fills the icons in")
			: *FString::Printf(TEXT("build %s, %s, %s"), *GMirrorBuild,
				GFontSummary.IsEmpty() ? TEXT("no faces") : *GFontSummary, *GMirrorDir),
		GFromTool, GFromSite, GSeqToTool, GSeqToSite,
		GSessionLost ? TEXT("signed out, edits held locally") : TEXT("signed in"),
		GExperienceId.IsEmpty() ? TEXT("unknown") : *GExperienceId,
		GAutosaves, GLastAutosave.IsEmpty() ? TEXT("never") : *GLastAutosave, *ExperienceDir(),
		GSaveSelector.IsEmpty() ? TEXT("not looked up yet") : *GSaveSelector,
		GLastPortalVerdict.IsEmpty() ? TEXT("none yet") : *GLastPortalVerdict,
		GLastError.IsEmpty() ? TEXT("none") : *GLastError);
}

void BF6Blocks::PrepareForUpdate(TFunction<void(bool)> Done)
{
	if (!GBrowser.IsValid()) { Done(true); return; }
	if (!GToolReady || GUpdateSaved) { Done(false); return; }
	GUpdateSaved = MoveTemp(Done);
	GUpdateSaveToken = FGuid::NewGuid().ToString(EGuidFormats::Digits);
	GUpdateSaveTimeout = FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda([](float)
	{
		GUpdateSaveTimeout.Reset(); GUpdateSaveToken.Reset();
		if (GUpdateSaved) { auto Callback = MoveTemp(GUpdateSaved); Callback(false); }
		return false;
	}), 30.f);
	TSharedRef<FJsonObject> M = MakeShared<FJsonObject>();
	M->SetStringField(TEXT("op"), TEXT("prepareUpdate"));
	M->SetStringField(TEXT("token"), GUpdateSaveToken);
	ToTool(M);
}

void BF6Blocks::ApplyLootBinding(const FString& Json)
{
	TSharedPtr<FJsonObject> Binding;
	if (!FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Json), Binding) || !Binding) return;
	Binding->SetStringField(TEXT("op"), TEXT("lootBinding"));
	FString Message; FJsonSerializer::Serialize(Binding.ToSharedRef(), TJsonWriterFactory<>::Create(&Message));
	Open();
	if (GToolReady) ToToolRaw(Message);
	else { GPendingLootBinding = Message; GPendingLootSave = BF6Api::CurrentSave(); GPendingLootLevel = BF6Api::CurrentLevel(); }
}

bool BF6Blocks::LoadFile(const FString& Path)
{
	FString Target = Path;
	if (Target.IsEmpty() && !OpenImportDialog(Target)) return false;
	// The page decides what the file is, because the page owns both schemas
	// and the converter. Everything arrives the same way.
	return ImportPath(Target, Path.IsEmpty() ? TEXT("dialog") : TEXT("console"));
}

bool BF6Blocks::SaveFile(const FString& Path, const FString& Format)
{
	if (!GToolReady) { GLastError = TEXT("the editor tab is not open"); return false; }
	FString Kind = Format.ToLower();
	if (Kind.IsEmpty()) Kind = TEXT("workspace");
	if (Kind != TEXT("workspace") && Kind != TEXT("experience"))
	{
		GLastError = FString::Printf(TEXT("unknown export format \"%s\": use workspace or experience"), *Format);
		UE_LOG(LogBF6Blocks, Warning, TEXT("%s"), *GLastError);
		return false;
	}
	// The page writes the file itself through a save message; asking it here
	// keeps one serializer in the whole feature.
	TSharedRef<FJsonObject> M = MakeShared<FJsonObject>();
	M->SetStringField(TEXT("op"), TEXT("saveToFile"));
	M->SetStringField(TEXT("path"), Path);
	M->SetStringField(TEXT("format"), Kind);
	ToTool(M);
	return true;
}

// ============================================================================
// Register
// ============================================================================
void BF6Blocks::Register()
{
	UBF6BlocksBridge* Bridge = NewObject<UBF6BlocksBridge>(GetTransientPackage(),
		TEXT("BF6BlocksBridge"), RF_Transient);
	Bridge->AddToRoot();
	GBridge = Bridge;

	// The same object on both pages: that is what makes this one conversation.
	BF6PortalWeb::RegisterBridgeObject(kBridgeName, Bridge);

	// The site's own toolbox art, if the mirror is on this machine: read and
	// inlined once here so the first page the user opens already draws the
	// site's icons. Cached against the mirror's build id, so this is a file read
	// on every later start and a rebuild only when the site's build moves.
	MirrorPack();

	RefreshSiteScript();

	FGlobalTabmanager::Get()->RegisterNomadTabSpawner(kTabId, FOnSpawnTab::CreateStatic(&SpawnBlocksTab))
		.SetDisplayName(FText::FromString(TEXT("BF6 Blocks")))
		.SetTooltipText(FText::FromString(TEXT(
			"The Portal rules editor inside the tool. Edits mirror to your experience's block page as you work.")))
		.SetGroup(WorkspaceMenu::GetMenuStructure().GetToolsCategory());

	IConsoleManager& CM = IConsoleManager::Get();
	GCmds.Add(CM.RegisterConsoleCommand(TEXT("BF6.Blocks.Open"),
		TEXT("Open the block editor tab."),
		FConsoleCommandDelegate::CreateStatic(&BF6Blocks::Open)));
	// RELOAD THE PAGE ITSELF.
	//
	// EnsurePage keeps the browser for the life of the editor session, so
	// closing the tab and opening it again shows the SAME page: the editor's
	// html, its javascript and its stylesheet are read once at startup and
	// never again. Everything under Resources/blocks is therefore invisible
	// until Unreal is restarted, which is a slow way to change a stylesheet
	// and an easy thing to be wrong about - a change can look like it did
	// nothing when it was simply never loaded.
	//
	// This re-reads them in place. The workspace is held by the tool rather
	// than the page, so nothing in progress is lost; the page asks for it back
	// as soon as it says it is ready.
	GCmds.Add(CM.RegisterConsoleCommand(TEXT("BF6.Blocks.CaptureToolbox"),
		TEXT("Measure the Portal site's own toolbox and pull-out: its classes, its sizes "
			"and every css rule that really applies to them. Needs the BLOCKS page open."),
		FConsoleCommandDelegate::CreateLambda([]{ AskSiteForToolbox(); })));
	GCmds.Add(CM.RegisterConsoleCommand(TEXT("BF6.Blocks.Perf"),
		TEXT("BF6.Blocks.Perf [zoom]  Measure the block editor: svg nodes, and the frame times of a "
			"scripted pan. A zoom measures at that scale, which is where a big project is slowest."),
		FConsoleCommandWithArgsDelegate::CreateLambda([](const TArray<FString>& Args)
		{
			if (!GToolReady) { UE_LOG(LogBF6Blocks, Warning, TEXT("The block editor is not open.")); return; }
			TSharedRef<FJsonObject> M = MakeShared<FJsonObject>();
			M->SetStringField(TEXT("op"), TEXT("perf"));
			if (Args.Num()) M->SetNumberField(TEXT("scale"), FCString::Atod(*Args[0]));
			ToTool(M);
		})));
	GCmds.Add(CM.RegisterConsoleCommand(TEXT("BF6.Blocks.PerfTry"),
		TEXT("BF6.Blocks.PerfTry [name] [off]  Turn one performance experiment on, so BF6.Blocks.Perf can "
			"measure what it was worth. No name lists them; the name off clears them all."),
		FConsoleCommandWithArgsDelegate::CreateLambda([](const TArray<FString>& Args)
		{
			if (!GToolReady) { UE_LOG(LogBF6Blocks, Warning, TEXT("The block editor is not open.")); return; }
			TSharedRef<FJsonObject> M = MakeShared<FJsonObject>();
			M->SetStringField(TEXT("op"), TEXT("perfTry"));
			if (Args.Num()) M->SetStringField(TEXT("name"), Args[0]);
			if (Args.Num() > 1 && Args[1].Equals(TEXT("off"), ESearchCase::IgnoreCase))
				M->SetBoolField(TEXT("on"), false);
			ToTool(M);
		})));
	GCmds.Add(CM.RegisterConsoleCommand(TEXT("BF6.Blocks.PerfSite"),
		TEXT("The same measurement on the Portal page, for comparison. Needs the BLOCKS page open."),
		FConsoleCommandDelegate::CreateLambda([]
		{
			if (!GSiteReady) { UE_LOG(LogBF6Blocks, Warning, TEXT("The site blocks page is not open.")); return; }
			TSharedRef<FJsonObject> M = MakeShared<FJsonObject>();
			M->SetStringField(TEXT("op"), TEXT("capturePerf"));
			ToSite(M);
		})));
	GCmds.Add(CM.RegisterConsoleCommand(TEXT("BF6.Blocks.Diagnose"),
		TEXT("Read the icons, the odd label colours and the warnings off the Portal page."),
		FConsoleCommandDelegate::CreateLambda([]
		{
			if (!GSiteReady) { UE_LOG(LogBF6Blocks, Warning, TEXT("Diagnosing needs the BLOCKS page open in the Portal panel.")); return; }
			TSharedRef<FJsonObject> M = MakeShared<FJsonObject>();
			M->SetStringField(TEXT("op"), TEXT("captureDiagnose"));
			ToSite(M);
		})));
	GCmds.Add(CM.RegisterConsoleCommand(TEXT("BF6.Blocks.CaptureFields"),
		TEXT("Read how the Portal site paints its fields and what icons it hangs on a block. Needs the BLOCKS page open."),
		FConsoleCommandDelegate::CreateLambda([]
		{
			if (!GSiteReady) { UE_LOG(LogBF6Blocks, Warning, TEXT("Reading the fields needs the BLOCKS page open in the Portal panel.")); return; }
			TSharedRef<FJsonObject> M = MakeShared<FJsonObject>();
			M->SetStringField(TEXT("op"), TEXT("captureFields"));
			ToSite(M);
		})));
	GCmds.Add(CM.RegisterConsoleCommand(TEXT("BF6.Blocks.CaptureSweep"),
		TEXT("Measure EVERY block on the Portal page. The same experience is open on both sides, so the two sweeps line up by id."),
		FConsoleCommandDelegate::CreateLambda([]
		{
			if (!GSiteReady)
			{
				UE_LOG(LogBF6Blocks, Warning, TEXT(
					"Sweeping the site needs its BLOCKS page open in the Portal panel."));
				return;
			}
			TSharedRef<FJsonObject> M = MakeShared<FJsonObject>();
			M->SetStringField(TEXT("op"), TEXT("captureSweep"));
			ToSite(M);
		})));
	GCmds.Add(CM.RegisterConsoleCommand(TEXT("BF6.Blocks.CaptureLayout"),
		TEXT("Measure how the Portal site lays a block out, row by row and element by element. Needs the BLOCKS page open."),
		FConsoleCommandDelegate::CreateLambda([]
		{
			if (!GSiteReady)
			{
				UE_LOG(LogBF6Blocks, Warning, TEXT(
					"Measuring the site's block layout needs its BLOCKS page open in the Portal panel."));
				return;
			}
			TSharedRef<FJsonObject> M = MakeShared<FJsonObject>();
			M->SetStringField(TEXT("op"), TEXT("captureLayout"));
			ToSite(M);
		})));
	GCmds.Add(CM.RegisterConsoleCommand(TEXT("BF6.Blocks.CaptureSockets"),
		TEXT("Dump the svg the Portal site really draws for an empty socket. Needs the BLOCKS page open."),
		FConsoleCommandDelegate::CreateLambda([]
		{
			if (!GSiteReady)
			{
				UE_LOG(LogBF6Blocks, Warning, TEXT(
					"Reading the site's sockets needs its BLOCKS page open in the Portal panel."));
				return;
			}
			TSharedRef<FJsonObject> M = MakeShared<FJsonObject>();
			M->SetStringField(TEXT("op"), TEXT("captureSockets"));
			ToSite(M);
		})));
	// THE RELEASE CHECK.
	//
	// Asks the page to audit ITSELF: overlapping controls, buttons wired to
	// nothing, modules loaded but not installed, guards left switched off by a
	// bulk load, a canvas with no size. Those are the faults this project keeps
	// shipping, and none of them throw, so nothing catches them today except a
	// person noticing.
	//
	// The answers come back through the log rather than a return value, because
	// ExecuteJavascript has no return channel and the log is already where the
	// page talks.
	// BAKE THE CURRENT CAPTURE INTO THE PLUGIN.
	//
	// Run before a release, with a good capture in hand. Everything a fresh
	// install needs to draw Portal blocks correctly is copied out of Saved and
	// into Resources/blocks/offline, where it ships.
	//
	// A command rather than a copy done once by hand, because the site changes
	// and a baked copy that nobody can refresh is worse than none: it goes stale
	// silently and every new user gets last season's blocks.
	GCmds.Add(CM.RegisterConsoleCommand(TEXT("BF6.Blocks.BakeOffline"),
		TEXT("Copy the current captured definitions into the plugin, so a fresh install looks right with no Portal sign in."),
		FConsoleCommandDelegate::CreateLambda([]
		{
			static const TCHAR* kFiles[] = {
				TEXT("definitions.json"), TEXT("definitions_synth.json"), TEXT("toolbox.json"),
				TEXT("theme.json"), TEXT("options.json"), TEXT("tooltips.json"),
				TEXT("icons.json"), TEXT("category_icons.json"), TEXT("help_urls.json"),
				// The look, so a fresh install is 1:1 and not merely functional.
				TEXT("style.json"), TEXT("mirror_icons.json")
			};
			const FString Src = CacheDir();
			const FString Dst = OfflineDefsDir();
			if (ReadFileOr(FPaths::Combine(Src, TEXT("definitions.json")), TEXT("")).IsEmpty())
			{
				UE_LOG(LogBF6Blocks, Warning,
					TEXT("bake: there is no capture to bake. Open the Portal blocks page and press CAPTURE STYLE first."));
				return;
			}
			IFileManager::Get().MakeDirectory(*Dst, true);
			int32 Copied = 0, Skipped = 0;
			int64 Bytes = 0;
			for (const TCHAR* Name : kFiles)
			{
				const FString In = FPaths::Combine(Src, Name);
				if (!FPaths::FileExists(In)) { Skipped++; continue; }
				if (IFileManager::Get().Copy(*FPaths::Combine(Dst, Name), *In, true, true) == COPY_OK)
				{ Copied++; Bytes += IFileManager::Get().FileSize(*In); }
				else { Skipped++; }
			}
			UE_LOG(LogBF6Blocks, Display,
				TEXT("bake: %d file(s), %.1f MB into %s. %d missing. A fresh install now draws Portal blocks without signing in."),
				Copied, (double)Bytes / (1024.0 * 1024.0), *Dst, Skipped);
		})));
	GCmds.Add(CM.RegisterConsoleCommand(TEXT("BF6.Blocks.SelfTest"),
		TEXT("Audit the block editor page: overlapping controls, unwired buttons, missing modules."),
		FConsoleCommandDelegate::CreateLambda([]
		{
			if (!GToolReady)
			{
				UE_LOG(LogBF6Blocks, Warning,
					TEXT("selftest: the block editor is not open. BF6.Blocks.Open first."));
				return;
			}
			TSharedRef<FJsonObject> M = MakeShared<FJsonObject>();
			M->SetStringField(TEXT("op"), TEXT("selfTest"));
			ToTool(M);
		})));
	GCmds.Add(CM.RegisterConsoleCommand(TEXT("BF6.Blocks.Probe"),
		TEXT("Ask the block editor what its toolbox is doing: how many categories, "
			"and what happens when one is opened."),
		FConsoleCommandDelegate::CreateLambda([]
		{
			if (!GToolReady)
			{
				UE_LOG(LogBF6Blocks, Warning,
					TEXT("The block editor is not open, so there is no toolbox to ask about."));
				return;
			}
			TSharedRef<FJsonObject> M = MakeShared<FJsonObject>();
			M->SetStringField(TEXT("op"), TEXT("probe"));
			ToTool(M);
		})));
	// The same export the toolbar button runs, with the folder given instead of
	// asked for. A native folder dialog needs a hand on the mouse, so without
	// this the whole blocks-to-script path could only ever be tried by a person
	// - and it is the half most worth having a machine check.
	GCmds.Add(CM.RegisterConsoleCommand(TEXT("BF6.Blocks.ExportScript"),
		TEXT("BF6.Blocks.ExportScript <folder>  Build one Portal upload script and its strings file. ")
		TEXT("No folder opens the picker, the same as the toolbar button."),
		FConsoleCommandWithArgsDelegate::CreateLambda([](const TArray<FString>& Args)
		{
			if (!GToolReady)
			{
				UE_LOG(LogBF6Blocks, Warning,
					TEXT("The block editor is not open, so there is nothing to export."));
				return;
			}
			const FString Dir = Args.Num() ? FString::Join(Args, TEXT(" ")).TrimQuotes() : FString();
			TSharedRef<FJsonObject> M = MakeShared<FJsonObject>();
			M->SetStringField(TEXT("op"), TEXT("exportScriptTo"));
			M->SetStringField(TEXT("dir"), Dir);
			ToTool(M);
		})));
	GCmds.Add(CM.RegisterConsoleCommand(TEXT("BF6.Blocks.Reload"),
		TEXT("Re-read the block editor's own html, javascript and css from disk. Use after editing anything in Resources/blocks."),
		FConsoleCommandDelegate::CreateLambda([]
		{
			if (!GBrowser.IsValid())
			{
				UE_LOG(LogBF6Blocks, Warning,
					TEXT("The block editor page has not been opened yet, so there is nothing to reload."));
				return;
			}
			GToolReady = false;
			GBrowser->Reload();
			UE_LOG(LogBF6Blocks, Display,
				TEXT("Block editor page reloading from %s"), *EditorPageUrl());
		})));

	GCmds.Add(CM.RegisterConsoleCommand(TEXT("BF6.Blocks.Checkpoint"),
		TEXT("Back up the complete block project using the update save barrier, without installing an update."),
		FConsoleCommandDelegate::CreateLambda([]
		{
			BF6Blocks::PrepareForUpdate([](bool bOk)
			{
				UE_LOG(LogBF6Blocks, Display, TEXT("Block update checkpoint: %s"), bOk ? TEXT("ready") : TEXT("failed; restart blocked"));
			});
		})));
	GCmds.Add(CM.RegisterConsoleCommand(TEXT("BF6.Blocks.Status"),
		TEXT("What the block editor is connected to and what it has cached."),
		FConsoleCommandDelegate::CreateLambda([]
		{
			UE_LOG(LogBF6Blocks, Display, TEXT("%s"), *BF6Blocks::Status());
		})));
	GCmds.Add(CM.RegisterConsoleCommand(TEXT("BF6.Blocks.CaptureStyle"),
		TEXT("Read the Portal blocks page's renderer, constants, theme and stylesheet, "
			"cache them, and restyle the editor without a restart."),
		FConsoleCommandDelegate::CreateLambda([]
		{
			AskSiteForStyle(TEXT("console"));
		})));
	GCmds.Add(CM.RegisterConsoleCommand(TEXT("BF6.Blocks.CaptureDefs"),
		TEXT("Read the Portal blocks page again for its block definitions, even when some are already cached."),
		FConsoleCommandDelegate::CreateLambda([]
		{
			// THE CACHE IS THE WHOLE POINT, WHICH IS WHY THIS HAS TO EXIST.
			// The definitions are read once and then never again, so a fix to
			// what we ask the page for reaches nothing until the cache is
			// refused. Without a way to say "read it again" the only route was
			// to delete files by hand, which nobody would guess.
			FetchDefinitions(true);
		})));
	GCmds.Add(CM.RegisterConsoleCommand(TEXT("BF6.Blocks.StyleStatus"),
		TEXT("What of the site's look is cached, when it was captured, and what is missing."),
		FConsoleCommandDelegate::CreateLambda([]
		{
			const bool bCache = FPaths::FileExists(StyleCachePath());
			const bool bDefault = FPaths::FileExists(StyleDefaultPath());
			const TSharedPtr<FJsonObject> Pack = MirrorPack();
			const bool bPack = FPaths::FileExists(MirrorPackPath());
			FString Missing;
			if (Pack.IsValid())
			{
				const TArray<TSharedPtr<FJsonValue>>* Miss = nullptr;
				if (Pack->TryGetArrayField(TEXT("fontsMissing"), Miss) && Miss)
				{
					TArray<FString> Names;
					for (const TSharedPtr<FJsonValue>& V : *Miss)
					{
						FString N;
						if (V.IsValid() && V->TryGetString(N)) Names.Add(N);
					}
					Missing = FString::Join(Names, TEXT(", "));
				}
			}
			UE_LOG(LogBF6Blocks, Display, TEXT(
				"BF6 Blocks style\n"
				"  in use        : %s\n"
				"  captured      : %s\n"
				"  what is there : %s\n"
				"  icons in use  : %s (%s)\n"
				"                  a live capture wins, the mirror fills what it missed, "
				"the offline default is last\n"
				"  site mirror   : %s\n"
				"  icon set      : %s%s\n"
				"  faces         : %s%s\n"
				"  cache file    : %s%s\n"
				"  offline file  : %s%s\n"
				"  to make exact : open the experience's BLOCKS page in the Portal panel, "
				"then BF6.Blocks.CaptureStyle or the CAPTURE STYLE button."),
				GStyleSource.IsEmpty() ? TEXT("not loaded yet") : *GStyleSource,
				GStyleAt.IsEmpty() ? TEXT("never") : *GStyleAt,
				GStyleSummary.IsEmpty() ? TEXT("nothing") : *GStyleSummary,
				*GIconSource, GIconSummary.IsEmpty() ? TEXT("not worked out yet") : *GIconSummary,
				GMirrorDir.IsEmpty()
					? TEXT("none on this machine, the style downloader fills the icons in")
					: *FString::Printf(TEXT("build %s at %s"), *GMirrorBuild, *GMirrorDir),
				*MirrorPackPath(), bPack ? TEXT("") : TEXT("  (not built yet)"),
				GFontSummary.IsEmpty() ? TEXT("none") : *GFontSummary,
				Missing.IsEmpty() ? TEXT("")
					: *FString::Printf(TEXT(", not mirrorable: %s, drawn with the nearest mirrored face"), *Missing),
				*StyleCachePath(), bCache ? TEXT("") : TEXT("  (not written yet)"),
				*StyleDefaultPath(), bDefault ? TEXT("") : TEXT("  (not present)"));
		})));
	GCmds.Add(CM.RegisterConsoleCommand(TEXT("BF6.Blocks.LoadFile"),
		TEXT("BF6.Blocks.LoadFile [path]  Import a Portal file: a workspace, a full experience, "
			"or the site's script export. The format is read off the content, not the name. "
			"No path opens the file picker."),
		FConsoleCommandWithArgsDelegate::CreateLambda([](const TArray<FString>& Args)
		{
			BF6Blocks::LoadFile(Args.Num() ? FString::Join(Args, TEXT(" ")).TrimQuotes() : FString());
		})));
	GCmds.Add(CM.RegisterConsoleCommand(TEXT("BF6.Blocks.SaveFile"),
		TEXT("BF6.Blocks.SaveFile [path] [workspace|experience]  Export what the editor holds. "
			"workspace is the default; experience writes the file the site itself imports. "
			"No path opens the save dialog."),
		FConsoleCommandWithArgsDelegate::CreateLambda([](const TArray<FString>& Args)
		{
			TArray<FString> A = Args;
			FString Format;
			if (A.Num())
			{
				const FString Last = A.Last().ToLower().TrimQuotes();
				if (Last == TEXT("workspace") || Last == TEXT("experience"))
				{
					Format = Last;
					A.Pop();
				}
			}
			BF6Blocks::SaveFile(A.Num() ? FString::Join(A, TEXT(" ")).TrimQuotes() : FString(), Format);
		})));

	// An experience owns its blocks across its entire map rotation. Send that
	// workspace when an already-open page changes projects, as well as on ready.
	// Never clear first: clearing raised deletion events and never loaded the save.
	GMapOpenedHandle = BF6Ext::OnMapOpened().AddLambda([](const FString& MapName, const FString&)
	{
		if (GToolReady) { SendProjectWorkspace(); SendObjIds(); }
	});

	// A navigation away from the blocks page drops the connection until
	// site_sync.js says hello again.
	GPageLoadedHandle = BF6PortalWeb::OnPageLoaded().AddLambda([](const FString& Url)
	{
		GSiteUrl = Url;
		if (!Url.Contains(TEXT("/rules/blocks")))
		{
			GSiteReady = false;
			TSharedRef<FJsonObject> C = MakeShared<FJsonObject>();
			C->SetStringField(TEXT("op"), TEXT("connected"));
			C->SetBoolField(TEXT("connected"), false);
			// Navigating away from the blocks page is not signing out, so the
			// pill needs the sign-in state here too or it drops back to the
			// same misleading "offline".
			C->SetStringField(TEXT("signIn"), BF6PortalProfile::StateLabel());
			ToTool(C);
		}
		// Until the profile module hands us a session delegate, a page that
		// turned into a login page is the signal that the session went away.
		if (Url.Contains(TEXT("/login")) || Url.Contains(TEXT("signin")) ||
			Url.Contains(TEXT("accounts.ea.com")))
		{
			BF6Blocks::NoteSessionLost();
		}
	});

	GTicker = FTSTicker::GetCoreTicker().AddTicker(
		FTickerDelegate::CreateStatic(&PollSelection), 0.25f);

	UE_LOG(LogBF6Blocks, Display, TEXT("BF6 Blocks ready. BF6.Blocks.Open, or Window > Tools > BF6 Blocks."));
}

void BF6Blocks::Unregister()
{
	if (GUpdateSaveTimeout.IsValid()) { FTSTicker::GetCoreTicker().RemoveTicker(GUpdateSaveTimeout); GUpdateSaveTimeout.Reset(); }
	GUpdateSaved = nullptr; GUpdateSaveToken.Reset(); GProjectRecoveryDirs.Reset();
	BF6BlocksExport::Stop();
	if (GTicker.IsValid()) { FTSTicker::GetCoreTicker().RemoveTicker(GTicker); GTicker.Reset(); }
	if (GPageLoadedHandle.IsValid()) { BF6PortalWeb::OnPageLoaded().Remove(GPageLoadedHandle); GPageLoadedHandle.Reset(); }
	if (GMapOpenedHandle.IsValid()) { BF6Ext::OnMapOpened().Remove(GMapOpenedHandle); GMapOpenedHandle.Reset(); }

	for (IConsoleObject* C : GCmds) if (C) IConsoleManager::Get().UnregisterConsoleObject(C);
	GCmds.Reset();

	// The built icon set is per session: a reload re-reads the mirror, which is
	// what makes a mirror updated while the editor ran take effect.
	GMirrorPack.Reset();
	GMirrorChecked = false;
	GDefaultIcons.Reset();
	GDefaultIconsRead = false;

	BF6PortalWeb::RegisterInjectedScript(TEXT("bf6blocks"), FString());
	BF6PortalWeb::UnregisterBridgeObject(kBridgeName);

	FGlobalTabmanager::Get()->UnregisterNomadTabSpawner(kTabId);
	// ---- BF6EditorOverlay ----
	if (GHost.IsValid()) GHost->SetContent(SNullWidget::NullWidget);
	GHost.Reset();
	GTab.Reset();
	// ---- end BF6EditorOverlay ----
	GBrowser.Reset();

	if (GBridge.IsValid()) { GBridge->RemoveFromRoot(); GBridge.Reset(); }
	GInbound.Reset();
	GToolReady = GSiteReady = false;
}
